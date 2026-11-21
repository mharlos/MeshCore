#include "TDeckTouch.h"
#include <Wire.h>

#define GT911_STATUS   0x814E   // bit 7: a report is ready; low 4 bits: how many fingers
#define GT911_POINT1   0x814F   // id, x lo, x hi, y lo, y hi, size lo, size hi, reserved

#define SCREEN_W  320
#define SCREEN_H  240

bool TDeckTouch::readReg(uint16_t reg, uint8_t* buf, uint8_t len) {
  Wire.beginTransmission(_addr);
  Wire.write(reg >> 8);
  Wire.write(reg & 0xFF);
  if (Wire.endTransmission() != 0) return false;
  if (Wire.requestFrom(_addr, len) != len) return false;
  for (uint8_t i = 0; i < len; i++) buf[i] = Wire.read();
  return true;
}

bool TDeckTouch::writeReg(uint16_t reg, uint8_t val) {
  Wire.beginTransmission(_addr);
  Wire.write(reg >> 8);
  Wire.write(reg & 0xFF);
  Wire.write(val);
  return Wire.endTransmission() == 0;
}

bool TDeckTouch::begin() {
  pinMode(PIN_TOUCH_INT, INPUT);
  for (uint8_t a : { (uint8_t)TDECK_TOUCH_ADDR, (uint8_t)0x14 }) {   // the GT911 answers at one or the other
    Wire.beginTransmission(a);
    if (Wire.endTransmission() == 0) { _addr = a; _present = true; break; }
  }
  return _present;
}

bool TDeckTouch::poll(unsigned long every_ms) {
  if (!_present) return false;
  unsigned long now = millis();
  if ((long)(now - _next_poll) < 0) return _down;
  _next_poll = now + every_ms;

  uint8_t st;
  if (!readReg(GT911_STATUS, &st, 1)) return _down;
  if (!(st & 0x80)) return _down;          // no new report: nothing has changed
  uint8_t n = st & 0x0F;
  if (n > 0 && n <= 5) {
    uint8_t p[8];
    if (readReg(GT911_POINT1, p, sizeof(p))) {
      _raw_x = p[1] | (p[2] << 8);
      _raw_y = p[3] | (p[4] << 8);
      // portrait panel -> landscape screen (as LilyGo's own examples turn it)
      int x = _raw_y, y = SCREEN_H - 1 - _raw_x;
      if (_flip & 1) { int t = x; x = y * SCREEN_W / SCREEN_H; y = t * SCREEN_H / SCREEN_W; }
      if (_flip & 2) x = SCREEN_W - 1 - x;
      if (_flip & 4) y = SCREEN_H - 1 - y;
      _x = constrain(x, 0, SCREEN_W - 1);
      _y = constrain(y, 0, SCREEN_H - 1);
      _down = true;
    }
  } else {
    _down = false;
  }
  writeReg(GT911_STATUS, 0);               // ready for the next report
  return _down;
}
