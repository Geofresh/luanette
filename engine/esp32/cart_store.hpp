// The carts a device has, and how they arrive: the ones the firmware carries (builtin::kAll), the
// ones the host sent (kept in flash, in the `assets` partition), their images, and live images the
// host sends while a cart runs. Also the host's cart messages (cart, cart.images, cart.image).
// ESP32 (esp_partition, NVS).
#pragma once
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "esp_partition.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "hg/json.hpp"

namespace carts::store {

struct Entry {
  std::string name, title, about, src;
  uint32_t seq;  // the store's order: newer is higher
};

// The carts the firmware carries. The firmware defines these (Luanette's: firmware/main/builtin_carts.cpp).
namespace builtin {
struct Builtin {
  const char* name;
  const char* src;
};
extern const Builtin kAll[];
extern const size_t kCount;
std::string_view find(std::string_view name);
}  // namespace builtin

// Sent carts in flash: 36 KB slots at the start of the `assets` partition.
namespace cartstore {
constexpr size_t kSlot = 0x9000, kSlots = 48;
const esp_partition_t* part();
void load_all(std::vector<Entry>& out);
bool save(const Entry& e);
bool erase(const std::string& name);
}  // namespace cartstore

// Carts' image bundles (carts/pack_images.py), in the assets partition after the cart slots.
namespace imagestore {
constexpr size_t kBase = cartstore::kSlot * cartstore::kSlots, kBlock = 0x8000;
struct Header {
  uint32_t magic, seq, len, crc, blocks;
  char name[48];
  uint32_t hcrc;  // of the fields above
};
struct Bundle {
  std::string name;
  uint32_t seq = 0, len = 0, crc = 0;
  int first = 0, blocks = 0;
  size_t data() const { return kBase + static_cast<size_t>(first) * kBlock + sizeof(Header); }  // the bundle's bytes
};
std::vector<Bundle> all();
bool find(const std::string& name, Bundle& out);
bool remove(const std::string& name);
void note_used(const std::string& name);
// `carts`: the carts the device has (a bundle of none of them goes first when room is needed).
bool save(const std::string& name, const uint8_t* data, size_t len, uint32_t crc, uint32_t seq, const std::vector<std::string>& carts);
}  // namespace imagestore

// Live images: asked of the host by name, kept in PSRAM.
namespace live_images {
// How to ask the host for one (a message to it); set once at start.
void set_asker(void (*ask)(const std::string& name));
// The image if it's here (true); else asks the host for it (at most every 10 s) and returns false.
bool get(std::string_view name, std::vector<uint8_t>& out);
void put(const std::string& name, std::vector<uint8_t> data, bool missing);
}  // namespace live_images

// The carts a device can open: the host's (newest first), then the built-in ones.
class Library {
 public:
  void load();  // the sent carts, from flash (a few ms)
  // A cart the host carries for its own use (the console's launcher): opened by name like the
  // others, but not in list(). `src` lives as long as the library (embedded by the build).
  void add_hidden(std::string name, const char* src);
  // A cart's source ("" when there's none by that name).
  std::string source(std::string_view name) const;
  struct Item {
    std::string name, title, about;
  };
  std::vector<Item> list() const;
  // A cart message from the host (carts/push.py): {"name", "lua", "run"?, "images"?}, {"open": name},
  // {"close": true}, {"delete": name}; "cart.images" (a cart's images, in parts, before it);
  // "cart.image" (a live image asked for). True when it was one.
  struct Outcome {
    bool close = false;    // leave the cart that's open
    bool changed = false;  // the list changed
    std::string open;      // a cart to open
  };
  bool on_message(const hg::json::Value& msg, Outcome& out);

 private:
  std::vector<std::string> names() const;
  mutable SemaphoreHandle_t m_ = nullptr;
  std::vector<Entry> pushed_;  // PSRAM strings, newest first
  std::vector<std::pair<std::string, const char*>> hidden_;
};

}  // namespace carts::store
