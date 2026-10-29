#ifdef CYBERDECK_UI

#include "TDeckInput.h"
#include <Wire.h>

#define KB_POLL_MILLIS     10
#define KB_PROBE_MILLIS  1000   // the keyboard MCU can boot after us, so keep looking for it

static volatile int tb_up = 0, tb_down = 0, tb_left = 0, tb_right = 0;
static portMUX_TYPE tb_mux = portMUX_INITIALIZER_UNLOCKED;

static void IRAM_ATTR onTrackUp()    { portENTER_CRITICAL_ISR(&tb_mux); tb_up++;    portEXIT_CRITICAL_ISR(&tb_mux); }
static void IRAM_ATTR onTrackDown()  { portENTER_CRITICAL_ISR(&tb_mux); tb_down++;  portEXIT_CRITICAL_ISR(&tb_mux); }
static void IRAM_ATTR onTrackLeft()  { portENTER_CRITICAL_ISR(&tb_mux); tb_left++;  portEXIT_CRITICAL_ISR(&tb_mux); }
static void IRAM_ATTR onTrackRight() { portENTER_CRITICAL_ISR(&tb_mux); tb_right++; portEXIT_CRITICAL_ISR(&tb_mux); }

static bool probeKeyboard() {
  Wire.beginTransmission(TDECK_KB_ADDR);
  return Wire.endTransmission() == 0;
}

void TDeckInput::begin() {
  pinMode(PIN_TRACKBALL_UP, INPUT_PULLUP);
  pinMode(PIN_TRACKBALL_DOWN, INPUT_PULLUP);
  pinMode(PIN_TRACKBALL_LEFT, INPUT_PULLUP);
  pinMode(PIN_TRACKBALL_RIGHT, INPUT_PULLUP);
  attachInterrupt(digitalPinToInterrupt(PIN_TRACKBALL_UP), onTrackUp, FALLING);
  attachInterrupt(digitalPinToInterrupt(PIN_TRACKBALL_DOWN), onTrackDown, FALLING);
  attachInterrupt(digitalPinToInterrupt(PIN_TRACKBALL_LEFT), onTrackLeft, FALLING);
  attachInterrupt(digitalPinToInterrupt(PIN_TRACKBALL_RIGHT), onTrackRight, FALLING);

  _kb_present = probeKeyboard();
  _next_kb_poll = millis();
}

int TDeckInput::read() {
  portENTER_CRITICAL(&tb_mux);
  _pend_up += tb_up;       tb_up = 0;
  _pend_down += tb_down;   tb_down = 0;
  _pend_left += tb_left;   tb_left = 0;
  _pend_right += tb_right; tb_right = 0;
  portEXIT_CRITICAL(&tb_mux);

  // one step per call, so a fast roll scrolls several lines rather than jumping
  if (_pend_up > 0)    { _pend_up--;    return TKEY_UP; }
  if (_pend_down > 0)  { _pend_down--;  return TKEY_DOWN; }
  if (_pend_left > 0)  { _pend_left--;  return TKEY_LEFT; }
  if (_pend_right > 0) { _pend_right--; return TKEY_RIGHT; }

  if ((long)(millis() - _next_kb_poll) < 0) return TKEY_NONE;

  if (!_kb_present) {
    _next_kb_poll = millis() + KB_PROBE_MILLIS;
    _kb_present = probeKeyboard();
    return TKEY_NONE;
  }

  _next_kb_poll = millis() + KB_POLL_MILLIS;
  if (Wire.requestFrom((uint8_t)TDECK_KB_ADDR, (uint8_t)1) != 1) {
    _kb_present = false;
    return TKEY_NONE;
  }
  int c = Wire.read();
  return (c > 0) ? c : TKEY_NONE;
}

#endif
