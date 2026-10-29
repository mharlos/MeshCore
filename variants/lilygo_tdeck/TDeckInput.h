#pragma once

// T-Deck keyboard and trackball.
//
// The keyboard is a separate ESP32-C3 on the shared I2C bus at 0x55; reading one
// byte returns the ASCII code of the last key pressed, or 0 when nothing is
// waiting. The trackball is four hall sensors that each pulse a GPIO as the ball
// rolls, plus a click on GPIO0 (already handled as user_btn).

#include <Arduino.h>

#ifndef TDECK_KB_ADDR
  #define TDECK_KB_ADDR     0x55
#endif
#ifndef PIN_TRACKBALL_UP
  #define PIN_TRACKBALL_UP     3
#endif
#ifndef PIN_TRACKBALL_DOWN
  #define PIN_TRACKBALL_DOWN  15
#endif
#ifndef PIN_TRACKBALL_LEFT
  #define PIN_TRACKBALL_LEFT   1
#endif
#ifndef PIN_TRACKBALL_RIGHT
  #define PIN_TRACKBALL_RIGHT  2
#endif

// key codes returned by TDeckInput::read() besides printable ASCII
#define TKEY_NONE       0
#define TKEY_BACKSPACE  0x08
#define TKEY_ENTER      0x0D
#define TKEY_ESC        0x1B
#define TKEY_UP         0x81   // trackball
#define TKEY_DOWN       0x82
#define TKEY_LEFT       0x83
#define TKEY_RIGHT      0x84

class TDeckInput {
  bool _kb_present;
  uint8_t _kb_errors;
  unsigned long _next_kb_poll;
  int _acc_v, _acc_h;               // pulses since the last step: +down/-up, +right/-left
  int _steps_v, _steps_h;           // steps decided but not yet returned
  unsigned long _last_pulse, _last_v_step;
  int _thresh_v, _thresh_h;

public:
  TDeckInput() : _kb_present(false), _kb_errors(0), _next_kb_poll(0), _acc_v(0), _acc_h(0), _steps_v(0), _steps_h(0),
                 _last_pulse(0), _last_v_step(0) { setSensitivity(3); }

  void begin();
  bool hasKeyboard() const { return _kb_present; }

  // 1 (needs a deliberate roll) .. 5 (every pulse counts)
  void setSensitivity(int level);

  // the next key or trackball step, or TKEY_NONE
  int read();
};
