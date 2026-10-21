// The speaker: the ES8311 DAC on I2S (esp_codec_dev does its registers), the NS4150B amplifier on
// a GPIO that this driver turns on only while sound plays. A task writes what's queued at the I2S
// pace; after 2 s with nothing to play the codec powers down (~35 mA idling open) and comes back
// for the next sound.
#include "board.hpp"

#include <algorithm>

#include "esp_codec_dev_defaults.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/task.h"

namespace board {
namespace {
const char* TAG = "board.speaker";
constexpr size_t kBuffer = 16 * 1024;  // ~0.5 s at 16 kHz: the sound mixer keeps only a few frames ahead
constexpr size_t kChunk = 512;         // samples per codec write
}  // namespace

bool Speaker::begin(const CodecConfig& cfg, i2c_master_bus_handle_t bus) {
  if (!bus) return false;
  gpio_reset_pin(static_cast<gpio_num_t>(cfg.pa));
  gpio_set_direction(static_cast<gpio_num_t>(cfg.pa), GPIO_MODE_OUTPUT);
  gpio_set_level(static_cast<gpio_num_t>(cfg.pa), 0);

  // (The board's microphones share this I2S bus through an ES7210; the console doesn't use them.)
  i2s_chan_config_t chan = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_1, I2S_ROLE_MASTER);
  chan.auto_clear = true;  // silence on underrun instead of repeating the last buffer
  if (i2s_new_channel(&chan, &tx_, nullptr) != ESP_OK) return false;
  i2s_std_config_t std_cfg = {};
  std_cfg.clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(kRate);  // MCLK = 256 x fs
  std_cfg.slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_MONO);
  std_cfg.gpio_cfg.mclk = static_cast<gpio_num_t>(cfg.mclk);
  std_cfg.gpio_cfg.bclk = static_cast<gpio_num_t>(cfg.bclk);
  std_cfg.gpio_cfg.ws = static_cast<gpio_num_t>(cfg.ws);
  std_cfg.gpio_cfg.dout = static_cast<gpio_num_t>(cfg.dout);
  std_cfg.gpio_cfg.din = I2S_GPIO_UNUSED;
  if (i2s_channel_init_std_mode(tx_, &std_cfg) != ESP_OK) {
    ESP_LOGE(TAG, "I2S setup failed");
    return false;
  }
  i2s_channel_enable(tx_);

  audio_codec_i2s_cfg_t i2s_cfg = {};
  i2s_cfg.port = I2S_NUM_1;
  i2s_cfg.tx_handle = tx_;
  const audio_codec_data_if_t* data_if = audio_codec_new_i2s_data(&i2s_cfg);
  audio_codec_i2c_cfg_t dac_i2c = {};
  dac_i2c.port = I2C_NUM_0;
  dac_i2c.addr = ES8311_CODEC_DEFAULT_ADDR;
  dac_i2c.bus_handle = bus;
  es8311_codec_cfg_t dac = {};
  dac.ctrl_if = audio_codec_new_i2c_ctrl(&dac_i2c);
  dac.gpio_if = audio_codec_new_gpio();
  dac.codec_mode = ESP_CODEC_DEV_WORK_MODE_DAC;
  dac.pa_pin = -1;  // ours: on only while sound plays
  dac.master_mode = false;
  dac.use_mclk = true;
  dac.hw_gain.pa_voltage = 5.0;
  dac.hw_gain.codec_dac_voltage = 3.3;
  esp_codec_dev_cfg_t out_cfg = {};
  out_cfg.dev_type = ESP_CODEC_DEV_TYPE_OUT;
  out_cfg.codec_if = es8311_codec_new(&dac);
  out_cfg.data_if = data_if;
  dev_ = out_cfg.codec_if ? esp_codec_dev_new(&out_cfg) : nullptr;
  fs_.sample_rate = kRate;
  fs_.channel = 1;
  fs_.bits_per_sample = 16;
  if (!dev_ || esp_codec_dev_open(dev_, &fs_) != ESP_CODEC_DEV_OK) {
    ESP_LOGE(TAG, "no ES8311");
    dev_ = nullptr;
    return false;
  }
  esp_codec_dev_set_out_vol(dev_, volume_);
  esp_codec_dev_set_out_mute(dev_, true);  // unmuted while something plays

  uint8_t* storage = static_cast<uint8_t*>(heap_caps_malloc(kBuffer + 1, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
  static StaticStreamBuffer_t control;
  if (!storage || !(buffer_ = xStreamBufferCreateStatic(kBuffer, 1, storage, &control))) return false;
  const auto pin = static_cast<gpio_num_t>(cfg.pa);
  pa_.emplace([this](void* chunk, size_t bytes) { return xStreamBufferReceive(buffer_, chunk, bytes, 0); },
              [this] { xStreamBufferReset(buffer_); }, [pin](bool on) { gpio_set_level(pin, on); });
  xTaskCreate(&Speaker::task, "board-spk", 4096, this, 7, &task_);
  ESP_LOGI(TAG, "ES8311 ready");
  return true;
}

bool Speaker::begin(uint32_t sample_rate) {
  if (!dev_ || sample_rate != kRate) return false;
  abort();
  open_ = true;
  draining_ = false;
  if (task_) xTaskNotifyGive(task_);
  return true;
}

void Speaker::write(const int16_t* samples, size_t count) {
  if (!open_) return;
  const size_t bytes = count * sizeof(int16_t);
  const size_t fit = std::min(bytes, xStreamBufferSpacesAvailable(buffer_)) & ~static_cast<size_t>(1);
  xStreamBufferSend(buffer_, samples, fit, 0);
}

void Speaker::end() {
  open_ = false;
  draining_ = true;
}

void Speaker::abort() {
  open_ = false;
  draining_ = false;
  if (pa_) pa_->abort();
  flush_ = true;
}

bool Speaker::busy() const { return open_ || draining_ || xStreamBufferBytesAvailable(buffer_) > 0; }

void Speaker::set_volume(uint8_t percent) {
  volume_ = percent > 100 ? 100 : percent;
  if (dev_ && !closed_) esp_codec_dev_set_out_vol(dev_, volume_);
}

// The codec closed (powered down) and opened again at the same format, volume and mute.
void Speaker::set_sleep(bool sleep) {
  if (sleep == closed_) return;
  if (sleep) {
    esp_codec_dev_close(dev_);
  } else {
    // Opening sets the format first, and that disables the I2S channel, which closing already did:
    // ESP-IDF logs an error for a channel that isn't enabled. Enabled first, as begin() has it, the
    // channel is running after the open just the same, and nothing is logged.
    i2s_channel_enable(tx_);
    esp_codec_dev_open(dev_, &fs_);
    esp_codec_dev_set_out_vol(dev_, volume_);
    esp_codec_dev_set_out_mute(dev_, true);
  }
  closed_ = sleep;
}

void Speaker::task(void* arg) {
  auto* self = static_cast<Speaker*>(arg);
  int16_t chunk[kChunk];
  bool playing = false;
  int64_t idle_since = esp_timer_get_time();
  for (;;) {
    // Idle: wait for begin() instead of polling (a dark screen's light sleep isn't cut short).
    if (!playing && !self->open_ && !self->draining_ && !self->flush_ && xStreamBufferBytesAvailable(self->buffer_) == 0) {
      if (!self->closed_ && esp_timer_get_time() - idle_since >= 2000000) self->set_sleep(true);
      ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(1000));
      continue;
    }
    idle_since = esp_timer_get_time();
    if (self->flush_.exchange(false)) {  // abort() emptied the queue and turned the amplifier off
      esp_codec_dev_set_out_mute(self->dev_, true);
      playing = false;
    }
    uint32_t generation = 0;
    const size_t got = self->pa_->receive(chunk, sizeof(chunk), generation);
    if (got == 0) {
      if (playing && !self->open_) {  // everything queued is out: quiet again
        self->draining_ = false;
        esp_codec_dev_set_out_mute(self->dev_, true);
        self->pa_->disable();
        playing = false;
      }
      vTaskDelay(pdMS_TO_TICKS(5));
      continue;
    }
    if (!playing) {
      self->set_sleep(false);
      esp_codec_dev_set_out_mute(self->dev_, false);
      playing = true;
    }
    if (!self->pa_->enable(generation)) continue;  // a chunk taken before an abort(): dropped
    esp_codec_dev_write(self->dev_, chunk, static_cast<int>(got & ~static_cast<size_t>(1)));
  }
}

}  // namespace board
