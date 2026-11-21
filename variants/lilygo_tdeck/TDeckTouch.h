#pragma once

// T-Deck (Plus) capacitive touch panel: a Goodix GT911 on the shared I2C bus.
//
// The panel is mounted for portrait, so its coordinates are turned to match the
// landscape screen; `flip` bits (from the `touch` console command) correct a panel
// that's turned differently: 1 = swap x/y, 2 = mirror x, 4 = mirror y.

#include <Arduino.h>

#ifndef TDECK_TOUCH_ADDR
  #define TDECK_TOUCH_ADDR   0x5D
#endif
#ifndef PIN_TOUCH_INT
  #define PIN_TOUCH_INT      16
#endif

class TDeckTouch {
  bool _present = false;
  uint8_t _addr = TDECK_TOUCH_ADDR;
  bool _down = false;
  int _x = 0, _y = 0, _raw_x = 0, _raw_y = 0;
  uint8_t _flip = 0;
  unsigned long _next_poll = 0;

  bool readReg(uint16_t reg, uint8_t* buf, uint8_t len);
  bool writeReg(uint16_t reg, uint8_t val);

public:
  bool begin();
  bool present() const { return _present; }
  void setFlip(uint8_t f) { _flip = f; }

  // polls the panel (at most every `every_ms`); true while a finger is down
  bool poll(unsigned long every_ms = 15);
  bool down() const { return _down; }
  int x() const { return _x; }   // screen pixels, 0..319
  int y() const { return _y; }   // 0..239
  int rawX() const { return _raw_x; }
  int rawY() const { return _raw_y; }
};
