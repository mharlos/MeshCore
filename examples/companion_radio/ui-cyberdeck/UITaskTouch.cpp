// The touch screen, alongside the keyboard and trackball.
//
//   tap            open what's under the finger (a channel, a node), switch tabs,
//                  pick a chat message (tap it again to @reply), answer a page
//   swipe up/down  scroll lists, step through chat messages
//   swipe sideways change tabs; in a chat, swipe right to go back
//   long press     in a chat: emoji picker; on the map: zoom out
//   drag           on the map: pan
//   double tap     on the map: zoom in
//   tap the footer the same as ESC (back)
//
// Gestures mostly turn into the keys the trackball already sends, so every screen
// behaves the same whichever way it's driven.

#include "UITask.h"
#include <target.h>

#define SCR_W       CYBERDECK_W
#define CHAR_W      8
#define ROW_H       16
#define TABS_Y      17
#define BODY_Y      35
#define INPUT_Y     214
#define FOOT_Y      232
#define BODY_ROWS   11

#define TKEY_CLICK   0x85
#define TKEY_HOLD    0x86

#define TAP_SLOP      12      // px a finger may wander and still be a tap
#define LONG_MS      600
#define DOUBLE_MS    350

static const char* TAB_LABELS[] = { "CHAN", "NODES", "HEARD", "MAP", "RF", "SYS" };

static int listTopFor(int count, int sel, int rows) {   // as the lists draw themselves
  int top = sel - rows / 2;
  if (top > count - rows) top = count - rows;
  return top < 0 ? 0 : top;
}

void UITask::touchBegin() {
  _touch.begin();
  _touch.setFlip(_prefs.touch_flip);
  _t_down = _t_long = _t_drag = false;
  _t_last_tap = 0;
  _touch_debug = false;
}

void UITask::touchTick() {
  if (!_touch.present()) return;
  bool on = _display && _display->isOn();
  bool down = _touch.poll(on ? 15 : 120);   // slower while the screen is dark: a touch still wakes it
  int x = _touch.x(), y = _touch.y();
  unsigned long now = millis();

  if (down && !_t_down) {   // finger down
    _t_down = true;
    _t_long = _t_drag = false;
    _t_x0 = _t_lx = x; _t_y0 = _t_ly = y;
    _t_t0 = now;
    _t_woke = !on;
    if (!on) { wake(); _dirty = true; }
    return;
  }
  if (down && _t_down) {    // finger moving or resting
    if (_t_woke) return;
    if (abs(x - _t_x0) > TAP_SLOP || abs(y - _t_y0) > TAP_SLOP) _t_drag = true;
    if (_t_drag && _screen == SCR_LIST && _tab == TAB_MAP && !_map_pick) {
      mapTouchPan(x - _t_lx, y - _t_ly);
      _dirty = true;
    } else if (!_t_drag && !_t_long && now - _t_t0 > LONG_MS) {
      _t_long = true;
      touchLong(x, y);
    }
    _t_lx = x; _t_ly = y;
    if (_touch_debug) _dirty = true;
    return;
  }
  if (!down && _t_down) {   // finger up
    _t_down = false;
    if (_t_woke || _t_long) return;
    if (_t_drag) {
      if (!(_screen == SCR_LIST && _tab == TAB_MAP && !_map_pick)) touchSwipe(_t_lx - _t_x0, _t_ly - _t_y0);
      return;
    }
    bool dbl = now - _t_last_tap < DOUBLE_MS && abs(x - _t_tap_x) < 25 && abs(y - _t_tap_y) < 25;
    _t_last_tap = dbl ? 0 : now;
    _t_tap_x = x; _t_tap_y = y;
    touchTap(_t_x0, _t_y0, dbl);
  }
}

void UITask::touchSwipe(int dx, int dy) {
  _auto_off = millis() + _prefs.dim_secs * 1000UL;
  if (abs(dx) > abs(dy)) {
    if (_screen == SCR_LIST) handleKey(dx < 0 ? TKEY_RIGHT : TKEY_LEFT);   // the next tab is to the right
    else if (dx > 0) handleKey(TKEY_ESC);                                // swipe right: back
    return;
  }
  int steps = abs(dy) / ROW_H;
  if (steps < 1) steps = 1;
  if (steps > 12) steps = 12;
  // finger down pulls older things into view (up the list); finger up, newer
  int key = dy > 0 ? TKEY_UP : TKEY_DOWN;
  if (_screen == SCR_CHAT && _emoji_open) key = dy > 0 ? TKEY_UP : TKEY_DOWN;
  for (int i = 0; i < steps; i++) handleKey(key);
}

void UITask::touchLong(int x, int y) {
  _auto_off = millis() + _prefs.dim_secs * 1000UL;
  if (_screen == SCR_CHAT && !_emoji_open && y < INPUT_Y - 2) { openEmojiPicker(); _dirty = true; }
  else if (_screen == SCR_LIST && _tab == TAB_MAP && !_map_pick) handleKey('-');
}

void UITask::touchTap(int x, int y, bool dbl) {
  _auto_off = millis() + _prefs.dim_secs * 1000UL;
  _dirty = true;
  if (_screen == SCR_BOOT || _screen == SCR_PAGE) { handleKey(TKEY_ENTER); return; }   // a page: tapping answers it

  if (y >= FOOT_Y) {   // the footer: back
    if (_screen == SCR_CHAT && _emoji_open) _emoji_open = false;
    else if (_screen != SCR_LIST) handleKey(TKEY_ESC);
    else if (_tab == TAB_MAP && (_map_pick || _map_route_seq || _map_node_sel >= 0)) handleKey(TKEY_ESC);
    return;
  }

  if (_screen == SCR_LIST) {
    if (y >= TABS_Y - 1 && y < BODY_Y - 2) {   // the tab strip: same widths drawTabs uses
      int tx = 0;
      for (int t = 0; t < TAB_COUNT; t++) {
        int w = (strlen(TAB_LABELS[t]) + 2) * CHAR_W + CHAR_W / 2;
        if (x < tx + w + 3) {
          if (t != _tab) {
            int key = t > _tab ? TKEY_RIGHT : TKEY_LEFT;   // via the keys, so leaving a tab tidies up
            for (int n = abs(t - _tab); n > 0 && _tab != t; n--) handleKey(key);
          }
          return;
        }
        tx += w + 3;
      }
      return;
    }
    if (y < BODY_Y) return;
    if (_tab == TAB_MAP) {
      if (_map_pick) { mapPickTap(y); return; }
      if (dbl) handleKey('+');
      else mapTouchTap(x, y);
      return;
    }
    if (_tab == TAB_CHAN || _tab == TAB_NODES || _tab == TAB_HEARD) {
      int count = _list_count;
      int row = (y - BODY_Y) / ROW_H;
      int idx = listTopFor(count, _sel[_tab], BODY_ROWS) + row;
      if (row < 0 || row >= BODY_ROWS || idx >= count) return;
      _sel[_tab] = idx;
      handleKey(TKEY_ENTER);   // straight in
    }
    return;
  }

  if (_screen == SCR_CHAT) {
    if (_emoji_open) { emojiTap(x, y); return; }
    if (y < BODY_Y - 2) { handleKey(TKEY_ESC); return; }   // the title: back
    if (y >= INPUT_Y - 2) return;                          // the input line: type on the keyboard
    int row = (y - BODY_Y) / ROW_H;
    if (row < 0 || row >= BODY_ROWS) return;
    uint32_t seq = _row_seq[row];
    if (seq == 0) return;
    if (_chat_sel >= 0 && seq == _chat_sel_seq) handleChatKey(TKEY_CLICK);   // tapped again: @reply
    else { _chat_sel = 0; _chat_sel_seq = seq; }                            // pick it (the draw finds its place)
    return;
  }

  if (_screen == SCR_NODE || _screen == SCR_GPS || _screen == SCR_MULE) {
    if (y < BODY_Y - 2) handleKey(TKEY_ESC);
  }
}

// a tap on the emoji picker's grid
void UITask::emojiTap(int x, int y) {
  const int cols = 15, cell = 20, px = (SCR_W - cols * cell) / 2, py = BODY_Y + 2, rows = 7;
  int c = (x - px) / cell, r = (y - py) / cell;
  if (x < px || c >= cols || y < py || r >= rows) { _emoji_open = false; return; }
  static uint8_t list[256];
  int n = emojiList(list);
  int sel_row = _emoji_sel / cols;
  int top = sel_row >= rows ? sel_row - rows + 1 : 0;
  int i = (top + r) * cols + c;
  if (i >= n) return;
  _emoji_sel = i;
  int max_len = CD_INPUT_MAX - (_conv_kind == CONV_CHANNEL ? strlen(_node_prefs->node_name) + 2 : 0);
  handleEmojiKey(TKEY_ENTER, max_len);
}

// what the `touch` console command and debug overlay show
void UITask::drawTouchDebug() {
  if (!_touch_debug || !_touch.present()) return;
  Adafruit_GFX& g = _display->gfx();
  int x = _touch.x(), y = _touch.y();
  if (_touch.down()) {
    g.drawFastHLine(x - 8, y, 17, c_alert);
    g.drawFastVLine(x, y - 8, 17, c_alert);
  }
  char l[40];
  snprintf(l, sizeof(l), "RAW %d,%d  SCR %d,%d", _touch.rawX(), _touch.rawY(), x, y);
  g.fillRect(SCR_W - strlen(l) * 6 - 4, BODY_Y, strlen(l) * 6 + 4, 10, 0);
  stext(SCR_W - strlen(l) * 6 - 2, BODY_Y + 1, c_alert, l);
}
