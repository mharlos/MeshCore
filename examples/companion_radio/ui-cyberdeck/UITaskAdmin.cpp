// Remote admin console: run a repeater's (or room server's) command line over the mesh,
// the way the phone app's admin screen does.
//
// After an admin login, commands go to the node as encrypted text marked CLI
// (TXT_TYPE_CLI_DATA); the node only acts on them from an admin, and answers the same
// way. Its replay check wants each command's timestamp to be newer than the last, which
// getCurrentTimeUnique() gives us. `clock sync` sets the node's clock from that
// timestamp - the fix for a solar repeater that lost power overnight.

#include "UITask.h"
#include <stdarg.h>
#include <target.h>
#include <helpers/TxtDataHelpers.h>
#include <helpers/AdvertDataHelpers.h>

#define SCR_W       CYBERDECK_W
#define CHAR_W      8
#define ROW_H       16
#define SMALL_W     6
#define TABS_Y      17
#define BODY_Y      35
#define INPUT_Y     214
#define FOOT_Y      232
#define CON_ROWS    9
#define CON_Y       BODY_Y
#define QUICK_Y     (CON_Y + CON_ROWS * 10 + 4)

#define TKEY_CLICK   0x85

struct Quick { const char* label; const char* cmd; bool confirm; };
static const Quick QUICK[] = {
  { "SYNC CLOCK TO DECK", "clock sync",       false },
  { "CLOCK",              "clock",            false },
  { "NEIGHBOURS",         "neighbors",        false },
  { "BATTERY / SOLAR",    "power",            false },
  { "RADIO",              "get radio",        false },
  { "TX POWER",           "get tx",           false },
  { "VERSION",            "ver",              false },
  { "SEND ADVERT",        "advert",           false },
  { "REPEAT ON?",         "get repeat",       false },
  { "REBOOT",             "reboot",           true  },
};
#define NUM_QUICK  (int)(sizeof(QUICK) / sizeof(QUICK[0]))

bool UITask::isAdminOf(const uint8_t* pub) const {
  for (int i = 0; i < _num_admins; i++) if (memcmp(_admins[i], pub, 6) == 0) return true;
  return false;
}

void UITask::adminPrint(const char* fmt, ...) {
  char line[160];
  va_list args;
  va_start(args, fmt);
  vsnprintf(line, sizeof(line), fmt, args);
  va_end(args);
  // wrap at the small font's width, so long replies (neighbour lists) stay readable
  const int w = sizeof(_acon[0]) - 1;
  for (const char* p = line; ; ) {
    if (_acon_len == CD_ADMIN_LINES) {
      memmove(_acon[0], _acon[1], sizeof(_acon[0]) * (CD_ADMIN_LINES - 1));
      _acon_len--;
    }
    snprintf(_acon[_acon_len++], sizeof(_acon[0]), "%.*s", w, p);
    if ((int)strlen(p) <= w) break;
    p += w;
  }
  _acon_scroll = 0;
  _dirty = true;
}

void UITask::openAdmin() {
  _screen = SCR_ADMIN;
  _acmd_len = 0;
  _acmd[0] = 0;
  _aq_sel = 0;
  _aq_confirm = false;
  if (_acon_len == 0) adminPrint("CONNECTED // PICK BELOW OR TYPE A COMMAND");
  _dirty = true;
}

void UITask::adminSend(const char* cmd) {
  ContactInfo* c = the_mesh.lookupContactByPubKey(_conv_peer, 6);
  if (c == NULL) { adminPrint("! NOT IN CONTACTS"); return; }
  uint32_t est = 0;
  uint32_t ts = the_mesh.getRTCClock()->getCurrentTimeUnique();
  adminPrint("> %s", cmd);
  int r = the_mesh.sendCommandData(*c, ts, 0, TXT_TYPE_CLI_DATA, cmd, est);
  if (r == MSG_SEND_FAILED) {
    adminPrint("! NOT SENT (QUEUE FULL)");
    return;
  }
  _admin_wait_until = millis() + est + 6000;   // replies come back after a short delay
}

void UITask::adminReply(const char* text) {
  _admin_wait_until = 0;
  // replies can run to several lines
  char buf[200];
  strncpy(buf, text, sizeof(buf) - 1);
  buf[sizeof(buf) - 1] = 0;
  for (char* line = strtok(buf, "\n"); line; line = strtok(NULL, "\n")) {
    while (*line == ' ') line++;
    char clean[160];
    size_t j = 0;
    for (size_t i = 0; line[i] && j < sizeof(clean) - 1; i++) {
      unsigned char ch = line[i];
      clean[j++] = (ch >= 32 && ch < 127) ? ch : ' ';
    }
    clean[j] = 0;
    adminPrint("  %s", clean);
  }
  if (_screen != SCR_ADMIN) showAlert("REPLY ON THE ADMIN CONSOLE", 2500);
}

void UITask::handleAdminKey(int key) {
  if (_acmd_len == 0) {
    // an empty prompt: the trackball works the quick menu
    if (key == TKEY_UP) { if (_aq_sel > 0) _aq_sel--; _aq_confirm = false; return; }
    if (key == TKEY_DOWN) { if (_aq_sel < NUM_QUICK - 1) _aq_sel++; _aq_confirm = false; return; }
    if (key == TKEY_LEFT) { _acon_scroll++; return; }    // older output
    if (key == TKEY_RIGHT) { if (_acon_scroll > 0) _acon_scroll--; return; }
    if (key == TKEY_ENTER || key == TKEY_CLICK) {
      const Quick& q = QUICK[_aq_sel];
      if (q.confirm && !_aq_confirm) {
        _aq_confirm = true;
        adminPrint("! PRESS AGAIN TO %s", q.label);
        return;
      }
      _aq_confirm = false;
      adminSend(q.cmd);
      return;
    }
    if (key == TKEY_ESC || key == TKEY_BACKSPACE) { _screen = SCR_NODE; _node_mode = NM_VIEW; return; }
  } else {
    if (key == TKEY_ENTER || key == TKEY_CLICK) {
      _acmd[_acmd_len] = 0;
      adminSend(_acmd);
      _acmd_len = 0;
      _acmd[0] = 0;
      return;
    }
    if (key == TKEY_BACKSPACE) { _acmd[--_acmd_len] = 0; return; }
    if (key == TKEY_ESC) { _acmd_len = 0; _acmd[0] = 0; return; }
  }
  if (key >= 32 && key < 127 && _acmd_len < (int)sizeof(_acmd) - 1) {
    _acmd[_acmd_len++] = key;
    _acmd[_acmd_len] = 0;
    _aq_confirm = false;
  }
}

void UITask::drawAdmin() {
  Adafruit_GFX& g = _display->gfx();
  const Node* n = findNode(_conv_peer);
  textf(0, TABS_Y, c_hi, "ADMIN // %.28s", n ? n->name : "?");
  g.drawFastHLine(0, TABS_Y + ROW_H + 1, SCR_W, c_dim);

  // console, newest at the bottom
  int last = _acon_len - 1 - _acon_scroll;
  for (int r = CON_ROWS - 1, i = last; r >= 0 && i >= 0; r--, i--) {
    const char* l = _acon[i];
    stext(0, CON_Y + r * 10, l[0] == '>' ? c_dim : l[0] == '!' ? c_alert : c_hi, l);
  }
  if (_admin_wait_until && (long)(_admin_wait_until - millis()) > 0) {
    stext(SCR_W - 13 * SMALL_W, CON_Y, c_alert, ((millis() / 400) % 2) ? "WAITING..." : "");
  } else if (_admin_wait_until) {
    _admin_wait_until = 0;
    adminPrint("! NO REPLY - TRY AGAIN");
  }
  if (_acon_scroll > 0) {
    char sc[12];
    snprintf(sc, sizeof(sc), "+%d", _acon_scroll);
    stext(SCR_W - strlen(sc) * SMALL_W - 2, CON_Y + (CON_ROWS - 1) * 10, c_alert, sc);
  }
  g.drawFastHLine(0, QUICK_Y - 3, SCR_W, c_dim);

  // quick menu, two columns
  const int rows = (NUM_QUICK + 1) / 2;
  for (int i = 0; i < NUM_QUICK; i++) {
    int x = (i < rows) ? 0 : SCR_W / 2, y = QUICK_Y + (i % rows) * 15;
    bool sel = _acmd_len == 0 && i == _aq_sel;
    if (sel) g.fillRect(x, y - 2, SCR_W / 2 - 2, 13, QUICK[i].confirm && _aq_confirm ? c_alert : c_fg);
    stext(x + 3, y, sel ? c_bg : (QUICK[i].confirm ? c_alert : c_fg), QUICK[i].label);
  }

  drawInput(INPUT_Y, "rpt#", _acmd, _acmd_len);
  drawFooter(_acmd_len ? "RET SEND  BKSP EDIT  ESC CLEAR"
                       : "\x18\x19 PICK  RET RUN  TYPE A COMMAND  \x1b\x1a OUTPUT  ESC BACK");
}
