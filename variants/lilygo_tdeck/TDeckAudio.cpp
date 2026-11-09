#ifdef CYBERDECK_UI

#include "TDeckAudio.h"
#include <driver/i2s.h>
#include <freertos/queue.h>
#include <math.h>

#define SAMPLE_RATE   16000
#define I2S_PORT      I2S_NUM_0
#define CHUNK         256

namespace Tones {
  const Tone DM[]        = { {1760, 70}, {0, 40}, {2349, 110}, {0, 0} };
  const Tone CHANNEL[]   = { {1318, 60}, {0, 0} };
  const Tone KEYWORD[]   = { {2093, 60}, {0, 30}, {2093, 60}, {0, 30}, {2637, 120}, {0, 0} };
  const Tone PAGE[]      = { {988, 160}, {1480, 160}, {988, 160}, {1480, 160}, {988, 160}, {1480, 260}, {0, 0} };
  const Tone DELIVERED[] = { {2637, 35}, {0, 0} };
  const Tone FAILED[]    = { {330, 160}, {0, 40}, {247, 220}, {0, 0} };
  // a nod to a modem handshake
  const Tone BOOT[]      = { {1200, 70}, {2200, 70}, {1200, 70}, {2200, 70}, {0, 60}, {2100, 140}, {0, 50},
                             {1650, 60}, {1850, 60}, {1650, 60}, {1850, 60}, {0, 60}, {980, 90}, {1180, 90}, {0, 0} };
  const Tone CLICK[]     = { {3000, 4}, {0, 0} };
}

bool TDeckAudio::begin() {
  if (_ok) return true;

  i2s_config_t cfg = {};
  cfg.mode = (i2s_mode_t)(I2S_MODE_MASTER | I2S_MODE_TX);
  cfg.sample_rate = SAMPLE_RATE;
  cfg.bits_per_sample = I2S_BITS_PER_SAMPLE_16BIT;
  cfg.channel_format = I2S_CHANNEL_FMT_ONLY_LEFT;
  cfg.communication_format = I2S_COMM_FORMAT_STAND_I2S;
  cfg.intr_alloc_flags = 0;
  cfg.dma_buf_count = 4;
  cfg.dma_buf_len = CHUNK;
  cfg.use_apll = false;
  cfg.tx_desc_auto_clear = true;   // silence, not a stuck tone, if we fall behind
  if (i2s_driver_install(I2S_PORT, &cfg, 0, NULL) != ESP_OK) return false;

  i2s_pin_config_t pins = {};
  pins.mck_io_num = I2S_PIN_NO_CHANGE;
  pins.bck_io_num = PIN_I2S_BCK;
  pins.ws_io_num = PIN_I2S_WS;
  pins.data_out_num = PIN_I2S_DOUT;
  pins.data_in_num = I2S_PIN_NO_CHANGE;
  if (i2s_set_pin(I2S_PORT, &pins) != ESP_OK) {
    i2s_driver_uninstall(I2S_PORT);
    return false;
  }
  i2s_zero_dma_buffer(I2S_PORT);

  _queue = xQueueCreate(4, sizeof(const Tone*));
  if (_queue == NULL) return false;
  xTaskCreatePinnedToCore(taskMain, "deck_audio", 3072, this, 1, NULL, 0);
  _ok = true;
  return true;
}

void TDeckAudio::play(const Tone* pattern) {
  if (!_ok || pattern == NULL || _volume == 0) return;
  xQueueSend((QueueHandle_t)_queue, &pattern, 0);   // drop it if the queue is full
}

void TDeckAudio::stop() {
  if (!_ok) return;
  _stop = true;
  xQueueReset((QueueHandle_t)_queue);
}

void TDeckAudio::taskMain(void* arg) {
  TDeckAudio* self = (TDeckAudio*) arg;
  const Tone* pattern;
  for (;;) {
    if (xQueueReceive((QueueHandle_t)self->_queue, &pattern, portMAX_DELAY) == pdTRUE) {
      self->_stop = false;
      self->playNow(pattern);
    }
  }
}

void TDeckAudio::playNow(const Tone* pattern) {
  static int16_t buf[CHUNK];
  float phase = 0;
  for (const Tone* t = pattern; (t->hz || t->ms) && !_stop; t++) {
    // a soft square wave: louder than a sine on a small speaker, gentler than a hard square
    int32_t amp = 900 * _volume;
    int total = (int)((uint32_t)SAMPLE_RATE * t->ms / 1000);
    int ramp = SAMPLE_RATE / 500;   // 2ms fade at each end, no clicks
    float step = 2.0f * (float)M_PI * t->hz / SAMPLE_RATE;
    for (int done = 0; done < total && !_stop; ) {
      int n = (total - done < CHUNK) ? total - done : CHUNK;
      for (int i = 0; i < n; i++, done++) {
        int16_t v = 0;
        if (t->hz) {
          float s = sinf(phase);
          s = s * 1.6f;
          if (s > 1.0f) s = 1.0f; else if (s < -1.0f) s = -1.0f;
          int32_t a = amp;
          if (done < ramp) a = a * done / ramp;
          else if (total - done < ramp) a = a * (total - done) / ramp;
          v = (int16_t)(s * a);
          phase += step;
          if (phase > 2.0f * (float)M_PI) phase -= 2.0f * (float)M_PI;
        }
        buf[i] = v;
      }
      size_t written;
      i2s_write(I2S_PORT, buf, n * sizeof(int16_t), &written, portMAX_DELAY);
    }
  }
  memset(buf, 0, sizeof(buf));
  size_t written;
  for (int k = 0; k < 4; k++) i2s_write(I2S_PORT, buf, sizeof(buf), &written, portMAX_DELAY);
}

#endif
