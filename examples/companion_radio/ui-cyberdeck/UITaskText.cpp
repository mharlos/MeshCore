// Text the way people actually send it: UTF-8 with emoji.
//
// Messages are kept exactly as they arrived. When drawn, each character becomes a glyph
// of the 8x16 CP437 font (accented letters and a few symbols map across), an emoji from
// EmojiData.h (16x16, two character cells wide, tinted and anti-aliased), or a placeholder.
// Bytes that aren't valid UTF-8 are drawn as CP437 as before, so the UI's own glyphs
// (arrows, check marks) and old history keep working.
//
// Also here: the emoji picker, and turning a stored message into the line people read
// (mentions shown as "who -> whom", farm codes like !PAGE-ACK as plain words).

#include "UITask.h"
#include "EmojiData.h"
#include <target.h>
#include <helpers/AdvertDataHelpers.h>

#define SCR_W       CYBERDECK_W
#define CHAR_W      8
#define ROW_H       16
#define SMALL_W     6
#define BODY_Y      35
#define INPUT_Y     214

#define TKEY_CLICK   0x85

static const uint8_t PICK_COLS = 15, PICK_ROWS = 7, PICK_CELL = 20;
static const int PICK_X = (SCR_W - PICK_COLS * PICK_CELL) / 2, PICK_Y = BODY_Y + 2;

/* ------------------------------------- decoding ---------------------------------------- */

static int emojiIndex(uint32_t cp) {
  int lo = 0, hi = EMOJI_COUNT - 1;
  while (lo <= hi) {
    int mid = (lo + hi) / 2;
    if (EMOJI[mid].cp == cp) return mid;
    if (EMOJI[mid].cp < cp) lo = mid + 1; else hi = mid - 1;
  }
  return -1;
}

// Latin letters and symbols that the CP437 font has
static uint8_t cp437For(uint32_t cp) {
  static const struct { uint16_t cp; uint8_t g; } MAP[] = {
    {0xC7,0x80},{0xFC,0x81},{0xE9,0x82},{0xE2,0x83},{0xE4,0x84},{0xE0,0x85},{0xE5,0x86},{0xE7,0x87},
    {0xEA,0x88},{0xEB,0x89},{0xE8,0x8A},{0xEF,0x8B},{0xEE,0x8C},{0xEC,0x8D},{0xC4,0x8E},{0xC5,0x8F},
    {0xC9,0x90},{0xE6,0x91},{0xC6,0x92},{0xF4,0x93},{0xF6,0x94},{0xF2,0x95},{0xFB,0x96},{0xF9,0x97},
    {0xFF,0x98},{0xD6,0x99},{0xDC,0x9A},{0xA2,0x9B},{0xA3,0x9C},{0xA5,0x9D},{0xE1,0xA0},{0xED,0xA1},
    {0xF3,0xA2},{0xFA,0xA3},{0xF1,0xA4},{0xD1,0xA5},{0xAA,0xA6},{0xBA,0xA7},{0xBF,0xA8},{0xBD,0xAB},
    {0xBC,0xAC},{0xA1,0xAD},{0xAB,0xAE},{0xBB,0xAF},{0xDF,0xE1},{0xB5,0xE6},{0xB1,0xF1},{0xF7,0xF6},
    {0xB0,0xF8},{0xB7,0xFA},{0xB2,0xFD},{0x2022,0x07},{0x2190,0x1B},{0x2191,0x18},{0x2192,0x1A},
    {0x2193,0x19},{0x2194,0x1D},{0x2195,0x12},{0x2018,'\''},{0x2019,'\''},{0x201C,'"'},{0x201D,'"'},
    {0x2013,'-'},{0x2014,'-'},{0x2026,0xFA},{0x2713,0xFB},{0x2714,0xFB},{0x263A,0x01},{0x263B,0x02},
    {0x2665,0x03},{0x2666,0x04},{0x2663,0x05},{0x2660,0x06},{0x266A,0x0D},{0x266B,0x0E},{0x263C,0x0F},
    {0x25B2,0x1E},{0x25BC,0x1F},{0x25BA,0x10},{0x25C4,0x11},{0x2588,0xDB},{0x2591,0xB0},{0x2592,0xB1},
    {0x2593,0xB2},{0x221A,0xFB},{0x221E,0xEC},{0x2248,0xF7},{0x2264,0xF3},{0x2265,0xF2},{0x03C0,0xE3},
    {0x00B4,'\''},{0x00A0,' '},
  };
  for (const auto& e : MAP) if (e.cp == cp) return e.g;
  return 0;
}

// One drawable thing at s: how many bytes it takes, how many 8-px cells it fills (0 for
// joiners and modifiers), and either an emoji index (>= 0), -2 for an emoji we don't
// have, or -1 with the CP437 glyph to draw in ch.
UITask::Gl UITask::glyphAt(const char* str) {
  const uint8_t* s = (const uint8_t*)str;
  Gl g = { 1, 1, -1, s[0] };
  uint8_t b0 = s[0];
  if (b0 < 0x80) return g;
  int n;
  uint32_t cp;
  if ((b0 & 0xE0) == 0xC0) { n = 2; cp = b0 & 0x1F; }
  else if ((b0 & 0xF0) == 0xE0) { n = 3; cp = b0 & 0x0F; }
  else if ((b0 & 0xF8) == 0xF0) { n = 4; cp = b0 & 0x07; }
  else return g;   // not UTF-8: a CP437 byte (our own glyphs, old history)
  for (int i = 1; i < n; i++) {
    if ((s[i] & 0xC0) != 0x80) return g;
    cp = (cp << 6) | (s[i] & 0x3F);
  }
  if (cp < 0x80) return g;
  g.bytes = n;

  if ((cp >= 0xFE00 && cp <= 0xFE0F) || (cp >= 0x200B && cp <= 0x200F) || cp == 0x20E3
      || (cp >= 0x1F3FB && cp <= 0x1F3FF) || (cp >= 0xE0020 && cp <= 0xE007F)) {   // variation, skin tone, tags
    g.cells = 0; g.emoji = -1; g.ch = 0;
    if (cp == 0x200D && s[n]) g.bytes += glyphAt((const char*)s + n).bytes;   // a joined emoji: show the first part
    return g;
  }
  if (cp >= 0x1F1E6 && cp <= 0x1F1FF) {   // flag letters: "US"
    g.ch = 'A' + (cp - 0x1F1E6);
    return g;
  }
  int e = emojiIndex(cp);
  if (e < 0 && cp == 0x2764) e = emojiIndex(0x2764);
  if (e >= 0) { g.cells = 2; g.emoji = e; return g; }
  uint8_t c = cp437For(cp);
  if (c) { g.ch = c; return g; }
  if (cp >= 0x1F000 || (cp >= 0x2600 && cp < 0x2800) || (cp >= 0x2B00 && cp < 0x2C00) || (cp >= 0x2300 && cp < 0x2400)) {
    g.cells = 2; g.emoji = -2;   // an emoji we don't carry
    return g;
  }
  g.ch = 0xFE;   // some other script
  return g;
}

int UITask::cellsOf(const char* s, int nbytes) {
  int cells = 0;
  for (int i = 0; s[i] && (nbytes < 0 || i < nbytes); ) {
    Gl g = glyphAt(s + i);
    cells += g.cells;
    i += g.bytes;
  }
  return cells;
}

// bytes of s that fit in `width` cells, breaking after a space in the second half if it can
int UITask::wrapBytes(const char* s, int nbytes, int width) {
  int i = 0, cells = 0, last_space = -1;
  while (i < nbytes && s[i]) {
    Gl g = glyphAt(s + i);
    if (cells + g.cells > width) break;
    cells += g.cells;
    i += g.bytes;
    if (s[i - 1] == ' ' && cells > width / 2) last_space = i;
  }
  if (i < nbytes && s[i] && last_space > 0) return last_space;
  return i > 0 ? i : (nbytes > 0 ? glyphAt(s).bytes : 0);
}

// copy a message as UTF-8, dropping control codes and anything that isn't valid
void UITask::keepText(char* dest, size_t dest_size, const char* src) {
  size_t j = 0;
  for (size_t i = 0; src[i] && j < dest_size - 1; ) {
    unsigned char c = (unsigned char)src[i];
    if (c < 0x80) {
      if (c >= 32 && c < 127) dest[j++] = c;
      else if (c == '\n' || c == '\t') dest[j++] = ' ';
      i++;
      continue;
    }
    Gl g = glyphAt(src + i);
    bool valid = g.bytes > 1;
    if (valid && j + g.bytes < dest_size) {
      // copy the first code point only: joined parts are copied as their own steps
      int n = ((c & 0xE0) == 0xC0) ? 2 : ((c & 0xF0) == 0xE0) ? 3 : 4;
      memcpy(dest + j, src + i, n);
      j += n;
      i += n;
    } else if (valid) {
      break;   // no room for the whole character
    } else {
      i++;     // stray byte
    }
  }
  dest[j] = 0;
}

/* ------------------------------------- drawing ----------------------------------------- */

static inline uint16_t blend565(uint16_t fg, uint16_t bg, int a /* 0..3 */) {
  if (a >= 3) return fg;
  if (a <= 0) return bg;
  int r = (((fg >> 11) & 31) * a + ((bg >> 11) & 31) * (3 - a)) / 3;
  int g = (((fg >> 5) & 63) * a + ((bg >> 5) & 63) * (3 - a)) / 3;
  int b = ((fg & 31) * a + (bg & 31) * (3 - a)) / 3;
  return (r << 11) | (g << 5) | b;
}

void UITask::drawEmoji(int x, int y, int idx) {
  uint16_t* buf = _display->buffer();
  Adafruit_GFX& gfx = _display->gfx();
  if (idx < 0) {   // one we don't have: a small rounded box
    gfx.drawRoundRect(x + 2, y + 2, 12, 12, 3, c_dim);
    return;
  }
  const EmojiGlyph& e = EMOJI[idx];
  for (int r = 0; r < 16; r++) {
    int py = y + r;
    if (py < 0 || py >= CYBERDECK_H) continue;
    for (int c = 0; c < 16; c++) {
      int px = x + c;
      if (px < 0 || px >= CYBERDECK_W) continue;
      int i = r * 16 + c;
      int a = (e.a[i >> 2] >> (6 - 2 * (i & 3))) & 3;
      if (a == 0) continue;
      if (buf) {
        uint16_t* p = buf + py * CYBERDECK_W + px;
        *p = blend565(e.color, *p, a);
      } else if (a >= 2) {
        gfx.drawPixel(px, py, e.color);
      }
    }
  }
}

/* ------------------------------------ the picker --------------------------------------- */

static uint8_t s_recent[PICK_COLS];   // most recently used first (EMOJI indexes + 1; 0 = none)

// what the picker shows: recent ones first, then everything, filtered by name
int UITask::emojiList(uint8_t* out) {
  int n = 0;
  if (_emoji_filter[0] == 0) {
    for (int i = 0; i < PICK_COLS && s_recent[i]; i++) out[n++] = s_recent[i] - 1;
  }
  for (int i = 0; i < EMOJI_COUNT; i++) {
    int idx = EMOJI_PICKER[i];
    if (_emoji_filter[0] && strstr(EMOJI[idx].name, _emoji_filter) == NULL) continue;
    bool dup = false;
    for (int k = 0; k < n && !_emoji_filter[0]; k++) if (out[k] == idx && k < PICK_COLS) { dup = true; break; }
    if (!dup) out[n++] = idx;
  }
  return n;
}

void UITask::openEmojiPicker() {
  _emoji_open = true;
  _emoji_sel = 0;
  _emoji_filter[0] = 0;
}

static int utf8Encode(uint32_t cp, char* out) {
  if (cp < 0x80) { out[0] = cp; return 1; }
  if (cp < 0x800) { out[0] = 0xC0 | (cp >> 6); out[1] = 0x80 | (cp & 0x3F); return 2; }
  if (cp < 0x10000) { out[0] = 0xE0 | (cp >> 12); out[1] = 0x80 | ((cp >> 6) & 0x3F); out[2] = 0x80 | (cp & 0x3F); return 3; }
  out[0] = 0xF0 | (cp >> 18); out[1] = 0x80 | ((cp >> 12) & 0x3F); out[2] = 0x80 | ((cp >> 6) & 0x3F); out[3] = 0x80 | (cp & 0x3F);
  return 4;
}

void UITask::handleEmojiKey(int key, int max_len) {
  static uint8_t list[EMOJI_COUNT + PICK_COLS];
  int n = emojiList(list);
  int fl = strlen(_emoji_filter);
  switch (key) {
    case TKEY_UP:    if (_emoji_sel >= PICK_COLS) _emoji_sel -= PICK_COLS; break;
    case TKEY_DOWN:  if (_emoji_sel + PICK_COLS < n) _emoji_sel += PICK_COLS; else if (n) _emoji_sel = n - 1; break;
    case TKEY_LEFT:  if (_emoji_sel > 0) _emoji_sel--; break;
    case TKEY_RIGHT: if (_emoji_sel < n - 1) _emoji_sel++; break;
    case TKEY_ESC:   _emoji_open = false; break;
    case TKEY_BACKSPACE:
      if (fl) { _emoji_filter[fl - 1] = 0; _emoji_sel = 0; }
      else _emoji_open = false;
      break;
    case TKEY_CLICK: case TKEY_ENTER: {
      if (n == 0) { _emoji_open = false; break; }
      int idx = list[_emoji_sel < n ? _emoji_sel : 0];
      char u[5];
      int ul = utf8Encode(EMOJI[idx].cp, u);
      if (_input_len + ul <= max_len) {
        memcpy(_input_buf + _input_len, u, ul);
        _input_len += ul;
        _input_buf[_input_len] = 0;
      } else {
        showAlert("NO ROOM FOR IT", 1200);
      }
      // to the front of the recent row
      int at = PICK_COLS - 1;
      for (int i = 0; i < PICK_COLS; i++) if (s_recent[i] == idx + 1 || s_recent[i] == 0) { at = i; break; }
      memmove(&s_recent[1], &s_recent[0], at);
      s_recent[0] = idx + 1;
      _emoji_open = false;
      break;
    }
    default:
      if (((key >= 'a' && key <= 'z') || (key >= 'A' && key <= 'Z') || (key >= '0' && key <= '9')) && fl < (int)sizeof(_emoji_filter) - 1) {
        _emoji_filter[fl] = tolower(key);
        _emoji_filter[fl + 1] = 0;
        _emoji_sel = 0;
      }
      break;
  }
}

void UITask::drawEmojiPicker() {
  Adafruit_GFX& g = _display->gfx();
  static uint8_t list[EMOJI_COUNT + PICK_COLS];
  int n = emojiList(list);
  if (_emoji_sel >= n) _emoji_sel = n > 0 ? n - 1 : 0;

  int h = PICK_ROWS * PICK_CELL + 14;
  g.fillRect(PICK_X - 3, PICK_Y - 2, PICK_COLS * PICK_CELL + 6, h + 4, c_bg);
  g.drawRect(PICK_X - 3, PICK_Y - 2, PICK_COLS * PICK_CELL + 6, h + 4, c_hi);

  int sel_row = _emoji_sel / PICK_COLS;
  int top = sel_row >= PICK_ROWS ? sel_row - PICK_ROWS + 1 : 0;
  for (int r = 0; r < PICK_ROWS; r++) {
    for (int c = 0; c < PICK_COLS; c++) {
      int i = (top + r) * PICK_COLS + c;
      if (i >= n) break;
      int x = PICK_X + c * PICK_CELL, y = PICK_Y + r * PICK_CELL;
      if (i == _emoji_sel) g.drawRect(x, y, PICK_CELL, PICK_CELL, c_alert);
      else if (i < PICK_COLS && !_emoji_filter[0] && s_recent[0] && i < (int)sizeof(s_recent) && s_recent[i])
        g.drawFastHLine(x + 4, y + PICK_CELL - 1, PICK_CELL - 8, c_dim);   // the recent row
      drawEmoji(x + 2, y + 2, list[i]);
    }
  }
  char line[60];
  if (n) snprintf(line, sizeof(line), ":%s:  %s%s", EMOJI[list[_emoji_sel]].name, _emoji_filter[0] ? "FIND " : "", _emoji_filter);
  else snprintf(line, sizeof(line), "NOTHING CALLED \"%s\"", _emoji_filter);
  stext(PICK_X, PICK_Y + PICK_ROWS * PICK_CELL + 2, n ? c_hi : c_alert, line);
}

/* ------------------------------ messages as people read them --------------------------- */

static bool namesEqual(const char* a, int alen, const char* b) {
  return (int)strlen(b) == alen && strncasecmp(a, b, alen) == 0;
}

// the first "@[name]" at s (spaces first are fine): name and its length, and where it ends
static bool leadingMention(const char* s, const char*& name, int& len, const char*& after) {
  while (*s == ' ') s++;
  if (s[0] != '@' || s[1] != '[') return false;
  const char* e = strchr(s + 2, ']');
  if (!e) return false;
  name = s + 2;
  len = e - name;
  after = e + 1;
  while (*after == ' ' || *after == ':' || *after == ',') after++;
  return true;
}

bool UITask::mentionsName(const char* text, const char* name) const {
  char at[40];
  snprintf(at, sizeof(at), "@[%s]", name);
  for (const char* p = text; (p = strchr(p, '@')) != NULL; p++) {
    if (strncasecmp(p, at, strlen(at)) == 0) return true;
  }
  return false;
}

// who wrote it (and to whom), the words, and how to colour them
void UITask::prettyMsg(const Msg* m, char* who, size_t wn, char* body, size_t bn, uint8_t& style) {
  style = 0;
  const char* me = _node_prefs->node_name;
  const char* t = m->text;
  snprintf(who, wn, "%s", m->from);
  const char* nm; int nl; const char* after;

  if (t[0] == '!') {
    style |= PS_SYSTEM;
    if (strncasecmp(t, CD_PAGE_ACK, strlen(CD_PAGE_ACK)) == 0) {
      const char* w = strstr(t, " - ");
      if (w && strstr(w, "HELP")) style |= PS_ALERT;
      snprintf(body, bn, "\xE2\x9C\x85 answered%s%s", w ? ": " : " the page", w ? w + 3 : "");   // ✅
      return;
    }
    if (strncasecmp(t, "!DUCK", 5) == 0) {   // the duck house controller, and its answers
      snprintf(body, bn, "\xF0\x9F\xA6\x86 duck house: %s", t[5] == '-' ? t + 9 : t + 6);   // 🦆
      for (char* p = body; *p; p++) if (*p >= 'A' && *p <= 'Z') *p += 32;
      return;
    }
    bool page = strncasecmp(t, CD_PAGE_PREFIX, 5) == 0, home = strncasecmp(t, CD_HOME, 5) == 0;
    bool where = strncasecmp(t, CD_WHERE, strlen(CD_WHERE)) == 0;
    if (page || home || where) {
      const char* rest = t + (where ? strlen(CD_WHERE) : 5);
      char targets[64] = "";
      size_t tl = 0;
      while (leadingMention(rest, nm, nl, after)) {
        tl += snprintf(targets + tl, sizeof(targets) > tl ? sizeof(targets) - tl : 0, "%s%.*s", tl ? " " : "", nl, nm);
        if (namesEqual(nm, nl, me)) style |= PS_TO_ME;
        rest = after;
      }
      while (*rest == ' ') rest++;
      if (page) {
        style |= PS_ALERT;
        bool sos = strncasecmp(rest, "SOS", 3) == 0;
        snprintf(body, bn, "%s %s%s%s%s%s", sos ? "\xF0\x9F\x86\x98" : "\xF0\x9F\x9A\xA8", sos ? "" : "PAGE",   // 🆘 / 🚨
                 targets[0] ? " \x1A " : (sos ? "" : " ALL"), targets, *rest ? (sos ? "" : ": ") : "", sos ? rest + 3 : rest);
      } else if (home) {
        style |= PS_ALERT;
        snprintf(body, bn, "\xF0\x9F\x8F\xA0 COME HOME%s%s%s%s", targets[0] ? " \x1A " : " ALL", targets, *rest ? ": " : "", rest);   // 🏠
      } else {
        snprintf(body, bn, "\xF0\x9F\x93\x8D where %s?", targets[0] ? targets : "is everyone");   // 📍
      }
      if (targets[0] == 0 && (page || home)) style |= PS_TO_ME;   // everyone includes us
      return;
    }
  }

  // a reply: "@[name] words" reads as "who -> name: words"
  if (leadingMention(t, nm, nl, after)) {
    bool to_me = namesEqual(nm, nl, me);
    size_t l = strlen(who);
    snprintf(who + l, wn > l ? wn - l : 0, " \x1A %.*s", to_me ? 3 : nl, to_me ? "you" : nm);
    style |= to_me ? PS_TO_ME : (m->outgoing ? 0 : PS_TO_OTHER);
    t = after;
  } else if (!m->outgoing && mentionsName(t, me)) {
    style |= PS_TO_ME;
  }

  // positions get a pin, and M shows them on the map
  const char* at = strstr(t, "@ ");
  bool pos = at && (isdigit((unsigned char)at[2]) || at[2] == '-' || at[2] == '~');
  if (pos) style |= PS_POS;
  snprintf(body, bn, "%s%s", pos ? "\xF0\x9F\x93\x8D " : "", t);   // 📍
}

// the T view: the back-and-forth between two people (or everything to/from one)
bool UITask::inThread(const Msg& m) const {
  if (!_thread_on) return true;
  const char* nm; int nl; const char* after;
  bool has = leadingMention(m.text, nm, nl, after);
  bool fromA = strcasecmp(m.from, _thread_a) == 0, fromB = _thread_b[0] && strcasecmp(m.from, _thread_b) == 0;
  if (_thread_b[0] == 0) return fromA || mentionsName(m.text, _thread_a);
  if (fromA) return !has || namesEqual(nm, nl, _thread_b) || mentionsName(m.text, _thread_b);
  if (fromB) return !has || namesEqual(nm, nl, _thread_a) || mentionsName(m.text, _thread_a);
  return false;
}

void UITask::startThread(const Msg* m) {
  const char* nm; int nl; const char* after;
  strncpy(_thread_a, m->from, sizeof(_thread_a) - 1);
  _thread_a[sizeof(_thread_a) - 1] = 0;
  _thread_b[0] = 0;
  if (leadingMention(m->text, nm, nl, after) && nl < (int)sizeof(_thread_b)) {
    memcpy(_thread_b, nm, nl);
    _thread_b[nl] = 0;
  }
  _thread_on = true;
  _chat_sel = -1;
  _scroll = 0;
}
