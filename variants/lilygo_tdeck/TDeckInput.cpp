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

#define TB_IDLE_RESET_MILLIS   250   // partial movement older than this is dropped
#define TB_AXIS_LOCK_MILLIS    300   // after a vertical step, ignore sideways drift for a moment

static bool probeKeyboard() {
  Wire.beginTransmission(TDECK_KB_ADDR);
  if (Wire.endTransmission() == 0) return true;
  // some keyboard firmwares only answer reads
  return Wire.requestFrom((uint8_t)TDECK_KB_ADDR, (uint8_t)1) == 1 && Wire.read() >= 0;
}

void TDeckInput::setSensitivity(int level) {
  if (level < 1) level = 1;
  if (level > 5) level = 5;
  static const int v[] = { 4, 3, 2, 1, 1 };
  static const int h[] = { 6, 5, 4, 3, 1 };   // sideways switches tabs/leaves chats, so it takes more
  _thresh_v = v[level - 1];
  _thresh_h = h[level - 1];
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
  int up = tb_up, down = tb_down, left = tb_left, right = tb_right;
  tb_up = tb_down = tb_left = tb_right = 0;
  portEXIT_CRITICAL(&tb_mux);

  unsigned long now = millis();
  if (up || down || left || right) {
    if (now - _last_pulse > TB_IDLE_RESET_MILLIS) _acc_v = _acc_h = 0;
    _last_pulse = now;
    _acc_v += down - up;
    if (now - _last_v_step > TB_AXIS_LOCK_MILLIS) _acc_h += right - left;

    while (_acc_v >= _thresh_v)  { _acc_v -= _thresh_v; _steps_v++; }
    while (_acc_v <= -_thresh_v) { _acc_v += _thresh_v; _steps_v--; }
    while (_acc_h >= _thresh_h)  { _acc_h -= _thresh_h; _steps_h++; }
    while (_acc_h <= -_thresh_h) { _acc_h += _thresh_h; _steps_h--; }
    if (_steps_v) { _acc_h = 0; _steps_h = 0; _last_v_step = now; }   // scrolling wins over drift
  }

  // one step per call, so a fast roll scrolls several lines rather than jumping
  if (_steps_v > 0) { _steps_v--; return TKEY_DOWN; }
  if (_steps_v < 0) { _steps_v++; return TKEY_UP; }
  if (_steps_h > 0) { _steps_h--; return TKEY_RIGHT; }
  if (_steps_h < 0) { _steps_h++; return TKEY_LEFT; }

  if ((long)(now - _next_kb_poll) < 0) return TKEY_NONE;

  if (!_kb_present) {
    _next_kb_poll = now + KB_PROBE_MILLIS;
    _kb_present = probeKeyboard();
    return TKEY_NONE;
  }

  _next_kb_poll = now + KB_POLL_MILLIS;
  if (Wire.requestFrom((uint8_t)TDECK_KB_ADDR, (uint8_t)1) != 1) {
    if (++_kb_errors >= 5) {   // a glitch now and then is normal on a shared bus
      _kb_present = false;
      _kb_errors = 0;
    }
    return TKEY_NONE;
  }
  _kb_errors = 0;
  int c = Wire.read();
  return (c > 0) ? c : TKEY_NONE;
}

#endif
