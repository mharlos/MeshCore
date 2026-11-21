// FARM tab: big buttons for the things people do every day.
//
//   CLOSE DUCK DOOR    "!DUCK DOOR CLOSE"      on the farm channel, for the duck house
//   DUCK LIGHTS        "!DUCK LIGHTS TOGGLE"   controller to act on (not built yet)
//   CALL DOGS HOME     "!HOME @[D*] ..."       every collar named D<n> plays the recall tone
//   CALL KIDS HOME     "!HOME @[K*] ..."       every kid's pager named K<n>
//   PAGE OFFICE        "!PAGE @[moe] ..."      the office deck alarms (`farm office <name>`)
//   WHERE IS EVERYONE  "!WHERE"                every pager answers with its position
//
// Anything that sets off an alarm or moves the door wants a second tap within a few
// seconds, so a pocket can't do it. Answers ("K1: coming home") show on the button.
//
// The farm channel is the one named "flockandfurrow" (or "farm"), else the first
// channel that isn't Public.

#include "UITask.h"
#include <target.h>

#define SCR_W       CYBERDECK_W
#define CHAR_W      8
#define ROW_H       16
#define SMALL_W     6
#define BODY_Y      35
#define FOOT_Y      232

#define TKEY_CLICK   0x85

#define ARM_MS      4000          // the second tap must come this soon
#define ANSWER_MS   (30 * 60000UL) // answers count toward the last press for this long

struct FarmButton { const char* line1; const char* line2; const char* icon; bool confirm; };
static const FarmButton BUTTONS[CD_FARM_BUTTONS] = {
  { "CLOSE",   "DUCK DOOR",  "\xF0\x9F\xA6\x86", true  },   // 🦆
  { "DUCK",    "LIGHTS",     "\xF0\x9F\x92\xA1", false },   // 💡
  { "CALL DOGS", "HOME",     "\xF0\x9F\x90\xB6", true  },   // 🐶
  { "CALL KIDS", "HOME",     "\xF0\x9F\x8F\xA1", true  },   // 🏡
  { "PAGE",    "OFFICE",     "\xF0\x9F\x93\xA2", true  },   // 📢
  { "WHERE IS", "EVERYONE",  "\xF0\x9F\x93\x8D", false },   // 📍
};

static const int BTN_GAP = 4, BTN_W = (SCR_W - 3 * BTN_GAP) / 2, BTN_H = (FOOT_Y - BODY_Y - 4 * BTN_GAP) / 3;

static void btnRect(int i, int& x, int& y) {
  x = BTN_GAP + (i % 2) * (BTN_W + BTN_GAP);
  y = BODY_Y + BTN_GAP + (i / 2) * (BTN_H + BTN_GAP);
}

const char* UITask::officeName() const {
  return _prefs.office[0] ? _prefs.office : "moe";
}

int UITask::farmChannel() {
  ChannelDetails cd;
  int first = -1;
  for (int i = 0; i < MAX_GROUP_CHANNELS; i++) {
    if (!the_mesh.getChannel(i, cd) || !cd.name[0]) continue;
    const char* nm = cd.name[0] == '#' ? cd.name + 1 : cd.name;
    if (strcasecmp(nm, "flockandfurrow") == 0 || strcasecmp(nm, "farm") == 0) return i;
    if (first < 0 && strcasecmp(nm, "public") != 0) first = i;
  }
  return first;
}

// post on a channel without opening it
bool UITask::sendOnChannel(int idx, const char* text, uint8_t flags) {
  ConvKind kind = _conv_kind;
  uint8_t chan = _conv_chan;
  char name[sizeof(_conv_name)];
  strcpy(name, _conv_name);
  _conv_kind = CONV_CHANNEL;
  _conv_chan = idx;
  bool ok = sendText(text, flags);
  _conv_kind = kind;
  _conv_chan = chan;
  strcpy(_conv_name, name);
  return ok;
}

void UITask::farmPress(int i) {
  if (i < 0 || i >= CD_FARM_BUTTONS) return;
  _farm_sel = i;
  unsigned long now = millis();
  if (BUTTONS[i].confirm && !(_farm_armed == i && now - _farm_armed_at < ARM_MS)) {
    _farm_armed = i;   // first tap: ready, waiting for the second
    _farm_armed_at = now;
    _dirty = true;
    return;
  }
  _farm_armed = -1;

  int ch = farmChannel();
  if (ch < 0) { showAlert("NO FARM CHANNEL // addchan FIRST", 3000); return; }
  char msg[96];
  uint8_t flags = 0;
  switch (i) {
    case 0: strcpy(msg, "!DUCK DOOR CLOSE"); break;
    case 1: strcpy(msg, "!DUCK LIGHTS TOGGLE"); break;
    case 2: strcpy(msg, CD_HOME " @[D*] come home"); flags = MF_PAGE; break;
    case 3: strcpy(msg, CD_HOME " @[K*] come home"); flags = MF_PAGE; break;
    case 4: snprintf(msg, sizeof(msg), CD_PAGE_PREFIX " @[%s] to the office", officeName()); flags = MF_PAGE; break;
    default: strcpy(msg, CD_WHERE); break;
  }
  bool ok = sendOnChannel(ch, msg, flags);
  _farm_sent_at[i] = ok ? rtc_clock.getCurrentTime() : 0;
  _farm_ans[i][0] = 0;
  showAlert(ok ? "SENT" : "NOT SENT // RADIO BUSY", 1500);
  beep(ok ? Tones::DELIVERED : Tones::CLICK);
}

// someone answered: put it on the button it belongs to
void UITask::farmNoteAnswer(const char* from, const char* body) {
  uint32_t now = rtc_clock.getCurrentTime();
  int b = -1;
  bool dog = (from[0] == 'D' || from[0] == 'd') && isdigit((unsigned char)from[1]);
  bool kid = (from[0] == 'K' || from[0] == 'k') && isdigit((unsigned char)from[1]);
  if (strncasecmp(body, "!DUCK", 5) == 0) b = (strstr(body, "LIGHT") || strstr(body, "light")) ? 1 : 0;
  else if (strncasecmp(body, CD_PAGE_ACK, strlen(CD_PAGE_ACK)) == 0) {
    if (dog) b = 2;
    else if (kid) b = 3;
    else if (strcasecmp(from, officeName()) == 0) b = 4;
  } else if (strstr(body, "@ ") || strstr(body, "no GPS fix")) b = 5;   // a !WHERE answer
  if (b < 0 || _farm_sent_at[b] == 0 || now - _farm_sent_at[b] > ANSWER_MS / 1000) return;

  // "K1 OK", "D1 coming home", "K2 NEED HELP", "K1 63%" (plain text: the button's small font has no emoji)
  char word[16] = "OK";
  const char* w = strstr(body, " - ");
  const char* batt = strstr(body, "batt ");
  if (w) snprintf(word, sizeof(word), "%.12s", w + 3);
  else if (b == 5 && batt) snprintf(word, sizeof(word), "%.4s", batt + 5);
  char who[8];
  int k = 0;
  for (const char* p = from; *p && k < (int)sizeof(who) - 1; p++) if ((uint8_t)*p >= 32 && (uint8_t)*p < 127) who[k++] = *p;
  who[k] = 0;
  char add[24];
  snprintf(add, sizeof(add), "%s %s", who, word);
  size_t l = strlen(_farm_ans[b]);
  if (strstr(_farm_ans[b], add)) return;
  snprintf(_farm_ans[b] + l, sizeof(_farm_ans[b]) - l, "%s%s", l ? ", " : "", add);
  _dirty = true;
}

bool UITask::handleFarmKey(int key) {
  switch (key) {
    case TKEY_UP:   if (_farm_sel > 0) _farm_sel--; _farm_armed = -1; return true;
    case TKEY_DOWN: if (_farm_sel < CD_FARM_BUTTONS - 1) _farm_sel++; _farm_armed = -1; return true;
    case TKEY_ENTER: case TKEY_CLICK: farmPress(_farm_sel); return true;
    case TKEY_ESC: _farm_armed = -1; return true;
    default: return false;   // left/right change tabs
  }
}

void UITask::farmTap(int x, int y) {
  for (int i = 0; i < CD_FARM_BUTTONS; i++) {
    int bx, by;
    btnRect(i, bx, by);
    if (x >= bx && x < bx + BTN_W && y >= by && y < by + BTN_H) { farmPress(i); return; }
  }
  _farm_armed = -1;
}

void UITask::drawFarm() {
  Adafruit_GFX& g = _display->gfx();
  if (_farm_armed >= 0 && millis() - _farm_armed_at >= ARM_MS) _farm_armed = -1;
  uint32_t now = rtc_clock.getCurrentTime();
  for (int i = 0; i < CD_FARM_BUTTONS; i++) {
    const FarmButton& b = BUTTONS[i];
    int x, y;
    btnRect(i, x, y);
    bool armed = i == _farm_armed, sel = i == _farm_sel;
    uint16_t edge = armed ? c_alert : sel ? c_hi : c_fg;
    if (armed) g.fillRoundRect(x, y, BTN_W, BTN_H, 6, c_alert);
    g.drawRoundRect(x, y, BTN_W, BTN_H, 6, edge);
    if (sel && !armed) g.drawRoundRect(x + 1, y + 1, BTN_W - 2, BTN_H - 2, 5, edge);
    uint16_t col = armed ? c_bg : (b.confirm && i >= 2 && i <= 4 ? c_alert : c_hi);
    text(x + 6, y + 6, col, b.icon);
    if (armed) {
      text(x + 26, y + 6, col, "TAP AGAIN");
      text(x + 26, y + 6 + ROW_H, col, "TO SEND");
    } else {
      text(x + 26, y + 6, col, b.line1);
      text(x + 26, y + 6 + ROW_H, col, b.line2);
    }
    // last press, and who's answered
    char st[48] = "";
    if (_farm_sent_at[i]) {
      uint32_t ago = now > _farm_sent_at[i] ? now - _farm_sent_at[i] : 0;
      if (_farm_ans[i][0]) snprintf(st, sizeof(st), "%s", _farm_ans[i]);
      else if (ago < 60) snprintf(st, sizeof(st), "SENT // WAITING");
      else snprintf(st, sizeof(st), "SENT %lum AGO", (unsigned long)(ago / 60));
    } else if (i <= 1) {
      strcpy(st, "NO DUCK HOUSE YET");
    } else if (i == 4) {
      snprintf(st, sizeof(st), "TO %s", officeName());
    }
    if (st[0]) {
      int maxc = (BTN_W - 10) / SMALL_W;
      st[maxc < (int)sizeof(st) - 1 ? maxc : sizeof(st) - 1] = 0;
      stext(x + 6, y + BTN_H - 12, armed ? c_bg : (_farm_ans[i][0] ? c_hi : c_dim), st);
    }
  }
  drawFooter(_farm_armed >= 0 ? "TAP AGAIN (OR RET) TO SEND  ESC CANCEL" : "TAP A BUTTON  \x18\x19 PICK  RET PRESS  \x1b\x1a TAB");
}
