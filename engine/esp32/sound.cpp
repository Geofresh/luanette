#include "sound.hpp"

#include <algorithm>
#include <atomic>
#include <vector>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "sfx.hpp"
#include "synth.hpp"

namespace carts::sound {
namespace {

const char* kTag = "hg.sound";
constexpr int kVoices = 4;
constexpr int kFrameMs = 10;
constexpr int kAheadFrames = 4;  // written ahead of what plays: the speaker's task takes it at its pace

hg::AudioOut* g_speaker = nullptr;
uint32_t g_rate = 16000;
bool (*g_voice_busy)() = nullptr;
QueueHandle_t g_requests = nullptr;
std::atomic<bool> g_muted{false};
std::vector<int16_t> g_sounds[kSfxCount];  // rendered once, on first use (PSRAM)

// What play() and music() queue: a built-in's number, or (n < 0) a cart's sound or song, which the
// mixer then owns (new'd here, deleted there). Null pointers stop them.
struct Request {
  int n;
  std::shared_ptr<const Sound>* sound;
  std::shared_ptr<const Song>* song;
};
constexpr int kSound = -1, kSong = -2;

struct Voice {
  const std::vector<int16_t>* pcm = nullptr;
  size_t pos = 0;
};

void mixer(void*) {
  Voice voices[kVoices];
  Synth synth(static_cast<int>(g_rate));
  const int frame = static_cast<int>(g_rate) * kFrameMs / 1000;
  std::vector<int16_t> out(static_cast<size_t>(frame));
  std::vector<int32_t> acc(static_cast<size_t>(frame));
  bool open = false;
  int ahead = 0;  // frames written beyond real time
  TickType_t last = xTaskGetTickCount();
  for (;;) {
    const bool any = synth.busy() || std::any_of(voices, voices + kVoices, [](const Voice& v) { return v.pcm; });
    Request r{-3, nullptr, nullptr};
    const bool got = xQueueReceive(g_requests, &r, any ? 0 : portMAX_DELAY) == pdTRUE;
    if (got && (r.n == kSound || r.n == kSong)) {
      if (r.n == kSound) synth.sfx(std::move(*r.sound));
      else synth.music(std::move(*r.song));
      delete r.sound;
      delete r.song;
      if (synth.busy() && !open) {
        open = !(g_voice_busy && g_voice_busy()) && g_speaker->begin(g_rate);
        ahead = 0;
        last = xTaskGetTickCount();
        if (!open) {  // the voice has the speaker, or it won't open: not heard (as a built-in sound isn't)
          if (!(g_voice_busy && g_voice_busy())) ESP_LOGW(kTag, "the speaker won't open; the cart's sound not heard");
          synth.sfx(nullptr);
          synth.music(nullptr);
        }
      }
    }
    const int n = got ? r.n : -3;
    if (got && n >= 0 && n < kSfxCount) {
      if (g_voice_busy && g_voice_busy() && !open) {
        continue;  // the voice has the speaker: this one isn't heard
      }
      if (g_sounds[n].empty()) g_sounds[n] = sfx_render(n, static_cast<int>(g_rate));
      // A free voice, else the one furthest along (the newest sound matters most).
      Voice* v = std::find_if(voices, voices + kVoices, [](const Voice& x) { return !x.pcm; });
      if (v == voices + kVoices)
        v = std::max_element(voices, voices + kVoices, [](const Voice& a, const Voice& b) { return a.pos < b.pos; });
      v->pcm = &g_sounds[n];
      v->pos = 0;
      if (!open) {
        open = g_speaker->begin(g_rate);
        ahead = 0;
        last = xTaskGetTickCount();
        if (!open) {
          ESP_LOGW(kTag, "the speaker won't open; sfx(%d) not heard", n);
          for (auto& x : voices) x.pcm = nullptr;
          continue;
        }
      }
    }
    if (!open) continue;
    if (!synth.busy() && !std::any_of(voices, voices + kVoices, [](const Voice& v) { return v.pcm; })) {
      g_speaker->end();  // what's written plays out; then the speaker rests
      open = false;
      continue;
    }
    // A frame of the synth and the voices mixed, clipped to 16 bits.
    std::fill(acc.begin(), acc.end(), 0);
    if (synth.busy()) synth.render(acc.data(), frame);
    for (int k = 0; k < frame; ++k) {
      int32_t s = acc[static_cast<size_t>(k)];
      for (auto& v : voices) {
        if (!v.pcm) continue;
        s += (*v.pcm)[v.pos++];
        if (v.pos >= v.pcm->size()) v.pcm = nullptr;
      }
      out[static_cast<size_t>(k)] = static_cast<int16_t>(std::clamp<int32_t>(s, -32768, 32767));
    }
    g_speaker->write(out.data(), out.size());
    // Keep a few frames ahead of what plays, no more (a new sound shouldn't queue behind much).
    if (++ahead >= kAheadFrames) {
      vTaskDelayUntil(&last, pdMS_TO_TICKS(kFrameMs));
      --ahead;
    }
  }
}

}  // namespace

void begin(hg::AudioOut* speaker, uint32_t rate, bool (*voice_busy)()) {
  if (!speaker || g_speaker) return;
  g_speaker = speaker;
  g_rate = rate;
  g_voice_busy = voice_busy;
  g_requests = xQueueCreate(8, sizeof(Request));
  if (!g_requests || xTaskCreatePinnedToCore(mixer, "hg-sfx", 4096, nullptr, 5, nullptr, 0) != pdPASS) {
    ESP_LOGW(kTag, "no sound task: carts play silent");
    g_speaker = nullptr;
  }
}

bool available() { return g_speaker != nullptr; }

void play(int n) {
  const Request r{n, nullptr, nullptr};
  if (g_requests && !g_muted) xQueueSend(g_requests, &r, 0);  // a full queue drops it: eight in 10 ms is plenty
}

void play(std::shared_ptr<const Sound> sound) {
  const bool stop = !sound;
  if (!g_requests || (g_muted && !stop)) return;  // a stop always goes through
  const Request r{kSound, new std::shared_ptr<const Sound>(std::move(sound)), nullptr};
  if (xQueueSend(g_requests, &r, stop ? pdMS_TO_TICKS(50) : 0) != pdTRUE) delete r.sound;
}

void music(std::shared_ptr<const Song> song) {
  if (!g_requests || (g_muted && song)) return;  // a stop always goes through
  const Request r{kSong, nullptr, new std::shared_ptr<const Song>(std::move(song))};
  if (xQueueSend(g_requests, &r, pdMS_TO_TICKS(50)) != pdTRUE) delete r.song;  // a song or a stop matters: wait a little
}

void set_muted(bool muted) {
  g_muted = muted;
  if (muted) {  // a song already playing stops too
    play(std::shared_ptr<const Sound>());
    music(nullptr);
  }
}

}  // namespace carts::sound
