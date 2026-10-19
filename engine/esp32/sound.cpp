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

struct Voice {
  const std::vector<int16_t>* pcm = nullptr;
  size_t pos = 0;
};

void mixer(void*) {
  Voice voices[kVoices];
  const int frame = static_cast<int>(g_rate) * kFrameMs / 1000;
  std::vector<int16_t> out(static_cast<size_t>(frame));
  bool open = false;
  int ahead = 0;  // frames written beyond real time
  TickType_t last = xTaskGetTickCount();
  for (;;) {
    const bool any = std::any_of(voices, voices + kVoices, [](const Voice& v) { return v.pcm; });
    int n = -1;
    if (xQueueReceive(g_requests, &n, any ? 0 : portMAX_DELAY) == pdTRUE && n >= 0 && n < kSfxCount) {
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
    if (!std::any_of(voices, voices + kVoices, [](const Voice& v) { return v.pcm; })) {
      g_speaker->end();  // what's written plays out; then the speaker rests
      open = false;
      continue;
    }
    // A frame of the voices mixed, clipped to 16 bits.
    for (int k = 0; k < frame; ++k) {
      int32_t s = 0;
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
  g_requests = xQueueCreate(8, sizeof(int));
  if (!g_requests || xTaskCreatePinnedToCore(mixer, "hg-sfx", 3072, nullptr, 5, nullptr, 0) != pdPASS) {
    ESP_LOGW(kTag, "no sound task: carts play silent");
    g_speaker = nullptr;
  }
}

bool available() { return g_speaker != nullptr; }

void set_muted(bool muted) { g_muted = muted; }

void play(int n) {
  if (g_requests && !g_muted) xQueueSend(g_requests, &n, 0);  // a full queue drops it: eight in 10 ms is plenty
}

}  // namespace carts::sound
