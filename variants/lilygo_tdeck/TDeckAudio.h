#pragma once

// Tones on the T-Deck speaker (MAX98357A I2S amp). Patterns play on their own
// FreeRTOS task, so a beep never holds up the mesh loop.

#include <Arduino.h>

#ifndef PIN_I2S_BCK
  #define PIN_I2S_BCK   7
#endif
#ifndef PIN_I2S_WS
  #define PIN_I2S_WS    5
#endif
#ifndef PIN_I2S_DOUT
  #define PIN_I2S_DOUT  6
#endif

// a pattern is pairs of { frequency Hz (0 = rest), milliseconds }, ended by { 0, 0 }
struct Tone { uint16_t hz, ms; };

namespace Tones {
  extern const Tone DM[];
  extern const Tone CHANNEL[];
  extern const Tone KEYWORD[];
  extern const Tone PAGE[];
  extern const Tone DELIVERED[];
  extern const Tone FAILED[];
  extern const Tone BOOT[];
  extern const Tone CLICK[];
}

class TDeckAudio {
  bool _ok;
  volatile uint8_t _volume;     // 0-10
  volatile bool _stop;
  void* _queue;

  static void taskMain(void* arg);
  void playNow(const Tone* pattern);

public:
  TDeckAudio() : _ok(false), _volume(6), _stop(false), _queue(NULL) { }
  bool begin();
  void play(const Tone* pattern);   // queued; returns at once
  void stop();                      // cut short whatever is playing
  void setVolume(uint8_t v) { _volume = v > 10 ? 10 : v; }
  uint8_t getVolume() const { return _volume; }
  bool ready() const { return _ok; }
};
