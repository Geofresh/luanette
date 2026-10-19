#include "cart_store.hpp"

#include <algorithm>
#include <cstring>
#include <tuple>

#include "cart.hpp"
#include "esp_log.h"
#include "esp_rom_crc.h"
#include "esp_timer.h"
#include "freertos/task.h"
#include "hd.hpp"
#include "hg/crypto.hpp"
#include "nvs.h"

namespace carts::store {
namespace {
const char* kTag = "hg.carts";
}  // namespace

namespace builtin {
std::string_view find(std::string_view name) {
  for (size_t i = 0; i < kCount; ++i)
    if (name == kAll[i].name) return kAll[i].src;
  return {};
}
}  // namespace builtin

// The carts the host sent survive a restart: one 36 KB slot each in the first ~1.7 MB of the
// `assets` partition (nothing else uses it yet), a header and the source. A new cart takes the
// slot of the same name, else an empty one, else the oldest's. Read whole at start (a few carts,
// a few ms); written only when one arrives (the erase pauses both cores ~100 ms: never mid-game).
namespace cartstore {
// "HGC2": 36 KB slots for 32 KB carts. The 20 KB slots of "HGC1" (16 KB carts) read as empty.
constexpr uint32_t kMagic = 0x32434748;
struct Header {
  uint32_t magic, seq, len, crc;
  char name[48];
};
static_assert(sizeof(Header) + carts::kMaxSource <= kSlot, "a slot holds a header and the largest cart");

const esp_partition_t* part() {
  static const esp_partition_t* p = esp_partition_find_first(ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_ANY, "assets");
  return p && p->size >= kSlot * kSlots ? p : nullptr;
}

bool read_header(int i, Header& h) {
  return esp_partition_read(part(), i * kSlot, &h, sizeof h) == ESP_OK && h.magic == kMagic && h.len <= carts::kMaxSource &&
         memchr(h.name, 0, sizeof h.name);
}

void load_all(std::vector<Entry>& out) {
  if (!part()) {
    ESP_LOGW(kTag, "carts: no assets partition; sent carts last until a restart");
    return;
  }
  for (int i = 0; i < static_cast<int>(kSlots); ++i) {
    Header h;
    if (!read_header(i, h)) continue;
    std::string src(h.len, '\0');
    if (esp_partition_read(part(), i * kSlot + sizeof h, src.data(), h.len) != ESP_OK ||
        esp_rom_crc32_le(0, reinterpret_cast<const uint8_t*>(src.data()), h.len) != h.crc) {
      ESP_LOGW(kTag, "carts: slot %d (%s) is damaged; skipped", i, h.name);
      continue;
    }
    out.push_back({h.name, carts::Cart::header(src, "title"), carts::Cart::header(src, "about"), std::move(src), h.seq});
  }
  std::sort(out.begin(), out.end(), [](const Entry& a, const Entry& b) { return a.seq > b.seq; });
}

bool save(const Entry& e) {
  if (!part() || e.name.size() >= sizeof(Header::name)) return false;
  int slot = -1, empty = -1, oldest = -1;
  uint32_t oldest_seq = UINT32_MAX;
  for (int i = 0; i < static_cast<int>(kSlots); ++i) {
    Header h;
    if (!read_header(i, h)) {
      if (empty < 0) empty = i;
    } else if (e.name == h.name) {
      slot = i;
    } else if (h.seq < oldest_seq) {
      oldest_seq = h.seq;
      oldest = i;
    }
  }
  if (slot < 0) slot = empty >= 0 ? empty : oldest;
  Header h{};
  h.magic = kMagic;
  h.seq = e.seq;
  h.len = static_cast<uint32_t>(e.src.size());
  h.crc = esp_rom_crc32_le(0, reinterpret_cast<const uint8_t*>(e.src.data()), h.len);
  std::strncpy(h.name, e.name.c_str(), sizeof h.name - 1);
  // The source first, the header last: a slot cut off mid-write reads as empty.
  esp_err_t err = esp_partition_erase_range(part(), slot * kSlot, kSlot);
  if (err == ESP_OK) err = esp_partition_write(part(), slot * kSlot + sizeof h, e.src.data(), h.len);
  if (err == ESP_OK) err = esp_partition_write(part(), slot * kSlot, &h, sizeof h);
  if (err != ESP_OK) ESP_LOGE(kTag, "carts: saving %s failed: %s", e.name.c_str(), esp_err_to_name(err));
  return err == ESP_OK;
}

bool erase(const std::string& name) {
  if (!part()) return false;
  bool found = false;
  for (int i = 0; i < static_cast<int>(kSlots); ++i) {
    Header h;
    if (read_header(i, h) && name == h.name) {
      found = true;
      if (esp_partition_erase_range(part(), i * kSlot, 0x1000) != ESP_OK) return false;
    }
  }
  return found;
}
}  // namespace cartstore

// Carts' images (carts/pack_images.py bundles), in the assets partition after the cart slots: a
// bundle takes a run of 32 KB blocks, its header at the start of its first. Each cart has at most
// one; when there's no room the oldest go. The header is written last, so a bundle cut off
// mid-write reads as free space.
namespace imagestore {
constexpr uint32_t kMagic = 0x31494748;  // "HGI1"

int count() { return cartstore::part() ? static_cast<int>((cartstore::part()->size - kBase) / kBlock) : 0; }

uint32_t header_crc(const Header& h) { return esp_rom_crc32_le(0, reinterpret_cast<const uint8_t*>(&h), offsetof(Header, hcrc)); }

std::vector<Bundle> all() {
  std::vector<Bundle> out;
  const int n = count();
  for (int i = 0; i < n;) {
    Header h;
    if (esp_partition_read(cartstore::part(), kBase + static_cast<size_t>(i) * kBlock, &h, sizeof h) == ESP_OK && h.magic == kMagic &&
        h.hcrc == header_crc(h) && h.blocks > 0 && i + static_cast<int>(h.blocks) <= n && memchr(h.name, 0, sizeof h.name) &&
        sizeof h + h.len <= h.blocks * kBlock) {
      out.push_back({h.name, h.seq, h.len, h.crc, i, static_cast<int>(h.blocks)});
      i += static_cast<int>(h.blocks);
    } else {
      ++i;
    }
  }
  return out;
}

bool find(const std::string& name, Bundle& out) {
  for (auto& b : all())
    if (b.name == name) {
      out = b;
      return true;
    }
  return false;
}

// A run of blocks erased a 32 KB block at a time, with a breath between: an erase stalls the
// flash (and so both cores' cached code) for as long as it runs.
esp_err_t erase(int first, int blocks) {
  for (int i = 0; i < blocks; ++i) {
    const esp_err_t err = esp_partition_erase_range(cartstore::part(), kBase + static_cast<size_t>(first + i) * kBlock, kBlock);
    if (err != ESP_OK) return err;
    vTaskDelay(1);
  }
  return ESP_OK;
}

bool remove(const std::string& name) {
  bool found = false;
  for (const auto& b : all())
    if (b.name == name) {
      found = true;
      erase(b.first, 1);  // its header gone: the rest reads as free space
    }
  return found;
}

// When each cart's images were last opened (a counter that moves on each open), in NVS: eviction
// takes the least recently opened. NVS keys are at most 15 characters: the name's crc.
std::string use_key(const std::string& name) {
  char k[12];
  std::snprintf(k, sizeof k, "u%08x", static_cast<unsigned>(esp_rom_crc32_le(0, reinterpret_cast<const uint8_t*>(name.data()), name.size())));
  return k;
}
uint32_t last_used(const std::string& name) {
  nvs_handle_t h;
  uint32_t v = 0;
  if (nvs_open("hgimguse", NVS_READONLY, &h) != ESP_OK) return 0;
  nvs_get_u32(h, use_key(name).c_str(), &v);
  nvs_close(h);
  return v;
}
void note_used(const std::string& name) {
  nvs_handle_t h;
  if (nvs_open("hgimguse", NVS_READWRITE, &h) != ESP_OK) return;
  uint32_t n = 0;
  nvs_get_u32(h, "n", &n);
  ++n;
  esp_err_t err = nvs_set_u32(h, "n", n);
  if (err == ESP_OK) err = nvs_set_u32(h, use_key(name).c_str(), n);
  if (err == ESP_OK) err = nvs_commit(h);
  if (err != ESP_OK) ESP_LOGW(kTag, "images: noting %s's use failed: %s", name.c_str(), esp_err_to_name(err));
  nvs_close(h);
}

// `carts`: the carts the board has; a bundle of none of them goes first when room is needed, then
// the least recently opened, then the oldest sent.
bool save(const std::string& name, const uint8_t* data, size_t len, uint32_t crc, uint32_t seq, const std::vector<std::string>& carts) {
  if (!cartstore::part() || name.size() >= sizeof(Header::name)) return false;
  remove(name);
  const int need = static_cast<int>((sizeof(Header) + len + kBlock - 1) / kBlock), n = count();
  if (need > n) return false;
  for (;;) {  // the first free run that fits; else the oldest bundle goes and we look again
    auto bundles = all();
    std::vector<bool> used(static_cast<size_t>(n), false);
    for (const auto& b : bundles)
      for (int i = 0; i < b.blocks; ++i) used[static_cast<size_t>(b.first + i)] = true;
    int at = -1;
    for (int i = 0, run = 0; i < n; ++i) {
      run = used[static_cast<size_t>(i)] ? 0 : run + 1;
      if (run == need) {
        at = i - need + 1;
        break;
      }
    }
    if (at >= 0) {
      Header h{};
      h.magic = kMagic;
      h.seq = seq;
      h.len = static_cast<uint32_t>(len);
      h.crc = crc;
      h.blocks = static_cast<uint32_t>(need);
      std::strncpy(h.name, name.c_str(), sizeof h.name - 1);
      h.hcrc = header_crc(h);
      const size_t base = kBase + static_cast<size_t>(at) * kBlock;
      esp_err_t err = erase(at, need);
      if (err == ESP_OK) err = esp_partition_write(cartstore::part(), base + sizeof h, data, len);
      if (err == ESP_OK) err = esp_partition_write(cartstore::part(), base, &h, sizeof h);
      if (err != ESP_OK) ESP_LOGE(kTag, "images: saving %s's failed: %s", name.c_str(), esp_err_to_name(err));
      return err == ESP_OK;
    }
    if (bundles.empty()) return false;
    struct Rank {
      bool kept;  // its cart is on the board
      uint32_t used, seq;
      bool operator<(const Rank& o) const { return std::tie(kept, used, seq) < std::tie(o.kept, o.used, o.seq); }
    };
    auto rank = [&](const Bundle& b) {
      return Rank{std::find(carts.begin(), carts.end(), b.name) != carts.end(), last_used(b.name), b.seq};
    };
    const auto victim = std::min_element(bundles.begin(), bundles.end(), [&](const Bundle& a, const Bundle& b) { return rank(a) < rank(b); });
    const Rank r = rank(*victim);
    ESP_LOGI(kTag, "images: %s's go to make room for %s's (%s)", victim->name.c_str(), name.c_str(),
             !r.kept ? "its cart is gone" : r.used ? "the least recently opened" : "never opened, the oldest");
    erase(victim->first, 1);
  }
}
}  // namespace imagestore

// Live images (docs/api.md, "hd"): ones the host sends while a cart runs, asked for by name the
// first time a cart draws one it doesn't have (a cover, a generated picture). Kept in PSRAM for all
// carts, the least recently used dropped past kLiveBytes; names the host has none of are noted,
// so they aren't asked for again.
namespace live_images {
constexpr size_t kLiveBytes = 1024 * 1024;
constexpr int64_t kAskAgainUs = 10'000'000;  // a request unanswered (the host away): ask again after this
struct Entry {
  std::string name;
  std::vector<uint8_t> data;  // packed (pack_images.py); empty: asked for, or missing
  bool missing = false;
  int64_t asked_us = 0;
};
SemaphoreHandle_t lock() {
  static SemaphoreHandle_t m = xSemaphoreCreateMutex();  // on first use (not at static init)
  return m;
}
std::vector<Entry> entries;  // newest use last
void (*g_ask)(const std::string& name) = nullptr;

// The image if it's here (true); else asks the host for it (at most every kAskAgainUs) and returns false.
bool get(std::string_view name, std::vector<uint8_t>& out) {
  bool ask = false;
  xSemaphoreTake(lock(), portMAX_DELAY);
  auto it = std::find_if(entries.begin(), entries.end(), [&](const Entry& e) { return e.name == name; });
  if (it != entries.end() && !it->data.empty()) {
    out = it->data;
    std::rotate(it, it + 1, entries.end());
    xSemaphoreGive(lock());
    return true;
  }
  const int64_t now = esp_timer_get_time();
  if (it == entries.end()) {
    entries.push_back({std::string(name), {}, false, now});
    ask = true;
  } else if (!it->missing && now - it->asked_us > kAskAgainUs) {
    it->asked_us = now;
    ask = true;
  }
  xSemaphoreGive(lock());
  if (ask && g_ask) g_ask(std::string(name));
  return false;
}

void put(const std::string& name, std::vector<uint8_t> data, bool missing) {
  xSemaphoreTake(lock(), portMAX_DELAY);
  entries.erase(std::remove_if(entries.begin(), entries.end(), [&](const Entry& e) { return e.name == name; }), entries.end());
  size_t total = data.size();
  for (const auto& e : entries) total += e.data.size();
  while (total > kLiveBytes && !entries.empty()) {  // the least recently used first
    total -= entries.front().data.size();
    entries.erase(entries.begin());
  }
  entries.push_back({name, std::move(data), missing, esp_timer_get_time()});
  xSemaphoreGive(lock());
}
}  // namespace live_images

namespace live_images {
void set_asker(void (*ask)(const std::string& name)) { g_ask = ask; }
}  // namespace live_images

// ---- the library --------------------------------------------------------------------

void Library::load() {
  if (!m_) m_ = xSemaphoreCreateMutex();
  std::vector<Entry> got;
  cartstore::load_all(got);
  xSemaphoreTake(m_, portMAX_DELAY);
  pushed_ = std::move(got);
  xSemaphoreGive(m_);
}

std::string Library::source(std::string_view name) const {
  xSemaphoreTake(m_, portMAX_DELAY);
  for (const auto& e : pushed_)
    if (e.name == name) {
      std::string src = e.src;
      xSemaphoreGive(m_);
      return src;
    }
  xSemaphoreGive(m_);
  for (const auto& [n, src] : hidden_)
    if (n == name) return src;
  return std::string(builtin::find(name));
}

void Library::add_hidden(std::string name, const char* src) { hidden_.emplace_back(std::move(name), src); }

std::vector<Library::Item> Library::list() const {
  std::vector<Item> out;
  xSemaphoreTake(m_, portMAX_DELAY);
  for (const auto& e : pushed_) out.push_back({e.name, e.title.empty() ? e.name : e.title, e.about});
  for (size_t i = 0; i < builtin::kCount; ++i) {
    const auto& b = builtin::kAll[i];
    bool dup = false;
    for (const auto& e : pushed_) dup |= e.name == b.name;
    if (!dup) out.push_back({b.name, carts::Cart::header(b.src, "title"), carts::Cart::header(b.src, "about")});
  }
  xSemaphoreGive(m_);
  return out;
}

std::vector<std::string> Library::names() const {
  std::vector<std::string> out;
  xSemaphoreTake(m_, portMAX_DELAY);
  for (const auto& c : pushed_) out.push_back(c.name);
  xSemaphoreGive(m_);
  return out;
}

bool Library::on_message(const hg::json::Value& msg, Outcome& out) {
  const std::string_view type = msg["type"].str_or("");
  if (type == "cart.image") {  // a live image the board asked for: {"name", "b64"} or {"name", "missing": true}
    const std::string name(msg["name"].str_or(""));
    std::vector<uint8_t> data;
    const bool missing = msg["missing"].as_bool(false);
    int w = 0, h = 0;
    bool alpha = false;
    if (!missing && (!hg::crypto::base64_decode(msg["b64"].str_or(""), data) || !carts::hd_image_info(data.data(), data.size(), &w, &h, &alpha))) {
      ESP_LOGW(kTag, "live image %s: damaged; dropped", name.c_str());
      return true;
    }
    if (missing) ESP_LOGI(kTag, "live image %s: the host has none", name.c_str());
    else ESP_LOGI(kTag, "live image %s: %dx%d, %u bytes", name.c_str(), w, h, static_cast<unsigned>(data.size()));
    live_images::put(name, std::move(data), missing);
    return true;
  }
  if (type == "cart.images") {  // a cart's images, before the cart: {"name", "size", "crc", "off", "b64"} in order
    static struct {
      std::string name;
      std::vector<uint8_t> buf;  // PSRAM: past 64 bytes malloc prefers it (SPIRAM_MALLOC_ALWAYSINTERNAL)
      size_t got = 0;
      uint32_t crc = 0;
    } in;
    const std::string name(msg["name"].str_or(""));
    const size_t size = static_cast<size_t>(msg["size"].as_int(0)), off = static_cast<size_t>(msg["off"].as_int(-1));
    if (off == 0) {
      in.name = name;
      in.crc = static_cast<uint32_t>(msg["crc"].as_int(0));
      in.got = 0;
      in.buf.assign(size <= 512 * 1024 + 64 ? size : 0, 0);
      if (in.buf.empty()) ESP_LOGW(kTag, "images for %s refused: %u bytes", name.c_str(), static_cast<unsigned>(size));
    }
    std::vector<uint8_t> part;
    if (in.buf.empty() || name != in.name || off != in.got || !hg::crypto::base64_decode(msg["b64"].str_or(""), part) ||
        in.got + part.size() > in.buf.size()) {
      if (!in.buf.empty()) ESP_LOGW(kTag, "images for %s: a part out of order or damaged at %u; dropped", name.c_str(), static_cast<unsigned>(off));
      in.buf.clear();
      in.buf.shrink_to_fit();
      return true;
    }
    std::memcpy(in.buf.data() + in.got, part.data(), part.size());
    in.got += part.size();
    if (in.got == in.buf.size()) {
      const uint32_t crc = esp_rom_crc32_le(0, in.buf.data(), in.buf.size());
      uint32_t seq = 1;
      for (const auto& b : imagestore::all()) seq = std::max(seq, b.seq + 1);
      std::vector<std::string> carts = names();
      carts.push_back(in.name);  // its cart arrives next
      const bool kept = crc == in.crc && imagestore::save(in.name, in.buf.data(), in.buf.size(), crc, seq, carts);
      if (crc != in.crc) ESP_LOGW(kTag, "images for %s: damaged (crc %08x, expected %08x)", in.name.c_str(), static_cast<unsigned>(crc), static_cast<unsigned>(in.crc));
      else ESP_LOGI(kTag, "images for %s arrived (%u KB)%s", in.name.c_str(), static_cast<unsigned>(in.buf.size() / 1024), kept ? ", kept in flash" : ", not kept");
      in.buf.clear();
      in.buf.shrink_to_fit();
    }
    return true;
  }
  if (type == "cart") {  // a cart from the host (carts/push.py): {"name", "lua", "run"?}; {"open": name}; {"close": true}
    if (msg["close"].as_bool()) {
      out.close = true;
      return true;
    }
    if (msg["delete"].is_string()) {
      const std::string name(msg["delete"].str_or(""));
      const bool gone = cartstore::erase(name);
      imagestore::remove(name);
      xSemaphoreTake(m_, portMAX_DELAY);
      pushed_.erase(std::remove_if(pushed_.begin(), pushed_.end(), [&](const Entry& p) { return p.name == name; }), pushed_.end());
      xSemaphoreGive(m_);
      out.changed = true;
      ESP_LOGI(kTag, "cart %s deleted%s", name.c_str(), gone ? " from flash" : " (wasn't in flash)");
      return true;
    }
    if (msg["open"].is_string()) {
      out.open = std::string(msg["open"].str_or(""));
      return true;
    }
    const std::string name(msg["name"].str_or(""));
    const std::string& src = msg["lua"].as_string();
    if (name.empty() || src.empty() || src.size() > carts::kMaxSource) {
      ESP_LOGW(kTag, "cart message refused: name '%s', %u bytes", name.c_str(), static_cast<unsigned>(src.size()));
      return true;
    }
    Entry e{name, carts::Cart::header(src, "title"), carts::Cart::header(src, "about"), src, 1};
    xSemaphoreTake(m_, portMAX_DELAY);
    for (const auto& p : pushed_) e.seq = std::max(e.seq, p.seq + 1);
    xSemaphoreGive(m_);
    const bool kept = cartstore::save(e);  // outside the lock: the erase stalls the flash, not the frames' lock
    // Its images came just before ({"images": {"crc"}}), or it has none (any old ones go).
    imagestore::Bundle b;
    const bool have = imagestore::find(name, b);
    if (!msg["images"].is_object()) {
      if (have) imagestore::remove(name);
    } else if (!have || b.crc != static_cast<uint32_t>(msg["images"]["crc"].as_int(0))) {
      ESP_LOGW(kTag, "cart %s: its images didn't arrive; it runs without them", name.c_str());
    }
    xSemaphoreTake(m_, portMAX_DELAY);
    pushed_.erase(std::remove_if(pushed_.begin(), pushed_.end(), [&](const Entry& p) { return p.name == name; }), pushed_.end());
    pushed_.insert(pushed_.begin(), std::move(e));
    xSemaphoreGive(m_);
    out.changed = true;
    ESP_LOGI(kTag, "cart %s arrived (%u bytes)%s", name.c_str(), static_cast<unsigned>(src.size()), kept ? ", kept in flash" : "");
    if (msg["run"].as_bool(true)) out.open = name;
    return true;
  }
  return false;
}

}  // namespace carts::store
