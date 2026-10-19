// BOOT (active low) and PWR (mirrored onto a GPIO, high while pressed), polled with a short
// debounce. PWR's long hold stays the AXP2101's: 6 s powers the board off.
#include "board.hpp"

#include "esp_attr.h"
#include "esp_sleep.h"
#include "hal/gpio_ll.h"

namespace board {
namespace {
constexpr uint8_t kDebounceTicks = 3;  // 3 polls at ~10 ms
TaskHandle_t g_woken = nullptr;
}  // namespace

void Buttons::begin(int boot_gpio, int pwr_gpio) {
  boot_.gpio = boot_gpio;
  pwr_.gpio = pwr_gpio;
  pwr_.active_high = true;
  for (Key* k : {&boot_, &pwr_}) {
    if (k->gpio < 0) continue;
    gpio_config_t io = {};
    io.pin_bit_mask = 1ULL << k->gpio;
    io.mode = GPIO_MODE_INPUT;
    io.pull_up_en = k->active_high ? GPIO_PULLUP_DISABLE : GPIO_PULLUP_ENABLE;  // PWR: the board drives it
    gpio_config(&io);
  }
}

// A key's wake interrupt: off again at once (a level keeps firing while held); the poll arms it
// again after the release.
void IRAM_ATTR Buttons::on_wake_isr(void* arg) {
  gpio_ll_intr_disable(&GPIO, static_cast<uint32_t>(reinterpret_cast<uintptr_t>(arg)));
  if (g_woken) {
    BaseType_t woken = pdFALSE;
    vTaskNotifyGiveFromISR(g_woken, &woken);
    if (woken) portYIELD_FROM_ISR();
  }
}

bool Buttons::idle() const {
  for (const Key* k : {&boot_, &pwr_})
    if (k->gpio >= 0 && (k->pressed || k->stable)) return false;
  return true;
}

void Buttons::set_wake(bool on, TaskHandle_t woken) {
  g_woken = woken;
  wake_wanted_ = on;
}

// Each key a light-sleep wake source while released, so the level it waits for is the press.
void Buttons::apply_wake() {
  auto level = [](const Key& k) { return gpio_get_level(static_cast<gpio_num_t>(k.gpio)) == (k.active_high ? 1 : 0); };
  const bool want = wake_wanted_.load();
  if (want && !isr_ready_) {
    if (!gpio_isr_service()) return;
    for (Key* k : {&boot_, &pwr_}) {
      if (k->gpio < 0) continue;
      gpio_set_intr_type(static_cast<gpio_num_t>(k->gpio), GPIO_INTR_DISABLE);
      gpio_isr_handler_add(static_cast<gpio_num_t>(k->gpio), &Buttons::on_wake_isr,
                           reinterpret_cast<void*>(static_cast<uintptr_t>(k->gpio)));
    }
    esp_sleep_enable_gpio_wakeup();
    isr_ready_ = true;
  }
  if (!isr_ready_) return;
  for (Key* k : {&boot_, &pwr_}) {
    if (k->gpio < 0) continue;
    const auto pin = static_cast<gpio_num_t>(k->gpio);
    if (want) {
      if (k->pressed || k->stable || level(*k)) continue;
      if (!k->wake) {
        gpio_wakeup_enable(pin, k->active_high ? GPIO_INTR_HIGH_LEVEL : GPIO_INTR_LOW_LEVEL);
        k->wake = true;
      }
      gpio_intr_enable(pin);
    } else if (k->wake) {
      gpio_intr_disable(pin);
      gpio_wakeup_disable(pin);
      k->wake = false;
    }
  }
  wake_armed_ = want;
}

void Buttons::poll() {
  if (wake_wanted_.load() || wake_armed_) apply_wake();
  for (Key* k : {&boot_, &pwr_}) {
    if (k->gpio < 0) continue;
    const bool down = gpio_get_level(static_cast<gpio_num_t>(k->gpio)) == (k->active_high ? 1 : 0);
    if (down == k->pressed) {
      k->stable = 0;
      continue;
    }
    if (++k->stable < kDebounceTicks) continue;
    k->pressed = down;
    k->stable = 0;
    if (k == &boot_ && on_boot) on_boot(down);
    if (k == &pwr_ && down && on_pwr) on_pwr();
  }
}

}  // namespace board
