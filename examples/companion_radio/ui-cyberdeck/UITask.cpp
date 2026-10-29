#include "UITask.h"
#include <SPIFFS.h>
#include <stdarg.h>
#include <helpers/TxtDataHelpers.h>
#include <helpers/AdvertDataHelpers.h>
#include <target.h>

#ifndef CD_DEFAULT_DIM_SECS
  #define CD_DEFAULT_DIM_SECS  90
#endif
#define CD_PREFS_FILE   "/cdeck"
#define CD_PREFS_MAGIC  0xCD

#define SCR_W       CYBERDECK_W
#define CHAR_W      6
#define ROW_H       10
#define COLS        (SCR_W / CHAR_W)    // 53
#define BODY_Y      26
#define BODY_ROWS   18
#define INPUT_Y     214
#define FOOT_Y      231

#define TKEY_CLICK   0x85
#define TKEY_HOLD    0x86

#define SEND_ATTEMPTS  3

#define RGB(r, g, b)  (uint16_t)((((r) & 0xF8) << 8) | (((g) & 0xFC) << 3) | ((b) >> 3))

// CP437 glyphs from the built-in font
#define G_LIGHT   "\xB0"
#define G_MED     "\xB1"
#define G_DARK    "\xB2"
#define G_BLOCK   "\xDB"
#define G_CHECK   "\xFB"
#define G_UP      "\x18"
#define G_DOWN    "\x19"
#define G_RIGHT   "\x1A"
#define G_LEFT    "\x1B"

struct Theme {
  const char* name;
  uint16_t fg, dim, hi, alert;
};

static const Theme THEMES[] = {
  { "matrix", RGB(51, 255, 102),  RGB(0, 110, 40),   RGB(200, 255, 210), RGB(255, 60, 60) },
  { "amber",  RGB(255, 176, 0),   RGB(120, 70, 0),   RGB(255, 230, 160), RGB(255, 60, 40) },
  { "gibson", RGB(0, 230, 255),   RGB(0, 80, 120),   RGB(255, 60, 220),  RGB(255, 230, 0) },
  { "redline", RGB(255, 50, 50),  RGB(110, 10, 10),  RGB(255, 200, 200), RGB(255, 255, 255) },
};
#define NUM_THEMES  (sizeof(THEMES) / sizeof(THEMES[0]))

static const char* TAB_NAMES[] = { "CHAN", "NODES", "HEARD", "SYS" };

static AdvertPath heard[16];
static int num_heard = 0;

// copy text for the built-in font: ASCII as-is, each UTF-8 sequence as one small block
static void sanitize(char* dest, size_t dest_size, const char* src) {
  size_t j = 0;
  for (size_t i = 0; src[i] && j < dest_size - 1; i++) {
    unsigned char c = (unsigned char)src[i];
    if (c >= 32 && c < 127) {
      dest[j++] = c;
    } else if (c >= 0xC0) {
      dest[j++] = '\xFE';
      while (src[i + 1] && ((unsigned char)src[i + 1] & 0xC0) == 0x80) i++;
    } else if (c == '\n' || c == '\t') {
      dest[j++] = ' ';
    }
  }
  dest[j] = 0;
}

static const char* typeTag(uint8_t type) {
  switch (type) {
    case ADV_TYPE_CHAT:     return "USR";
    case ADV_TYPE_REPEATER: return "RPT";
    case ADV_TYPE_ROOM:     return "ROOM";
    case ADV_TYPE_SENSOR:   return "SNS";
    default:                return "???";
  }
}

UITask::UITask(mesh::MainBoard* board, MultiSerialInterface* serial) : AbstractUITask(board, serial) {
  _display = NULL;
  _sensors = NULL;
  _node_prefs = NULL;
  _msgs = NULL;
  _max_msgs = 0;
  _nodes = NULL;
  _num_nodes = _num_chans = 0;
  _next_seq = 1;
  memset(_pending, 0, sizeof(_pending));
  memset(_sel, 0, sizeof(_sel));
  _screen = SCR_BOOT;
  _tab = TAB_CHAN;
  _scroll = 0;
  _conv_kind = CONV_NONE;
  _input_len = _cmd_len = _console_len = 0;
  _input_buf[0] = _cmd_buf[0] = 0;
  _alert[0] = 0;
  _msgcount = 0;
  _dirty = true;
  _cursor_on = true;
  _nodes_dirty = true;
  _boot_started = _next_render = _next_blink = _auto_off = _glitch_until = _alert_until = _last_render = _nodes_refresh_at = 0;
}

void UITask::begin(DisplayDriver* display, SensorManager* sensors, NodePrefs* node_prefs) {
  _display = static_cast<CyberDeckDisplay*>(display);
  _sensors = sensors;
  _node_prefs = node_prefs;

  _msgs = (Msg*) ps_calloc(CD_MAX_MSGS, sizeof(Msg));
  _max_msgs = CD_MAX_MSGS;
  if (_msgs == NULL) {   // no PSRAM: keep a short history in internal RAM
    _max_msgs = 24;
    _msgs = (Msg*) calloc(_max_msgs, sizeof(Msg));
    if (_msgs == NULL) _max_msgs = 0;
  }
  _nodes = (Node*) ps_calloc(MAX_CONTACTS, sizeof(Node));

  user_btn.begin();
  _input.begin();
  loadPrefs();
  applyTheme();
  refreshChannels();
  refreshNodes();

  _boot_started = millis();
  _auto_off = millis() + _prefs.dim_secs * 1000UL;
  _screen = SCR_BOOT;
  _dirty = true;
  if (_display) _display->turnOn();
}

/* ---------------------------------------- prefs ---------------------------------------- */

void UITask::loadPrefs() {
  _prefs.magic = CD_PREFS_MAGIC;
  _prefs.theme = 0;
  _prefs.crt = 1;
  _prefs.dim_secs = CD_DEFAULT_DIM_SECS;

  File f = SPIFFS.open(CD_PREFS_FILE, "r");
  if (f) {
    Prefs p;
    if (f.read((uint8_t*)&p, sizeof(p)) == sizeof(p) && p.magic == CD_PREFS_MAGIC) {
      _prefs = p;
      if (_prefs.theme >= NUM_THEMES) _prefs.theme = 0;
    }
    f.close();
  }
}

void UITask::savePrefs() {
  File f = SPIFFS.open(CD_PREFS_FILE, "w");
  if (f) {
    f.write((const uint8_t*)&_prefs, sizeof(_prefs));
    f.close();
  }
}

void UITask::applyTheme() {
  const Theme& t = THEMES[_prefs.theme];
  c_fg = t.fg;
  c_dim = t.dim;
  c_hi = t.hi;
  c_alert = t.alert;
  c_bg = 0;
  _dirty = true;
}

/* ------------------------------------- mesh state -------------------------------------- */

void UITask::refreshChannels() {
  _num_chans = 0;
  ChannelDetails cd;
  for (int i = 0; i < MAX_GROUP_CHANNELS; i++) {
    if (the_mesh.getChannel(i, cd) && cd.name[0]) _chan_idx[_num_chans++] = i;
  }
}

void UITask::refreshNodes() {
  _nodes_dirty = false;
  _nodes_refresh_at = millis() + 2000;
  if (_nodes == NULL) { _num_nodes = 0; return; }

  ContactInfo c;
  int n = 0;
  for (int i = 0; i < the_mesh.getTotalContactSlots() && n < MAX_CONTACTS; i++) {
    if (!the_mesh.getContactByIdx(i, c) || c.type == ADV_TYPE_NONE) continue;
    Node& d = _nodes[n++];
    memcpy(d.pub, c.id.pub_key, sizeof(d.pub));
    sanitize(d.name, sizeof(d.name), c.name);
    d.type = c.type;
    d.path_len = c.out_path_len;
    d.lastmod = c.lastmod;
    d.lat = c.gps_lat;
    d.lon = c.gps_lon;
  }
  _num_nodes = n;
  qsort(_nodes, _num_nodes, sizeof(Node), [](const void* a, const void* b) -> int {   // most recently heard first
    uint32_t la = ((const Node*)a)->lastmod, lb = ((const Node*)b)->lastmod;
    return (la < lb) ? 1 : (la > lb) ? -1 : 0;
  });
  if (_sel[TAB_NODES] >= _num_nodes) _sel[TAB_NODES] = _num_nodes > 0 ? _num_nodes - 1 : 0;
}

bool UITask::msgInConv(const Msg& m, ConvKind kind, uint8_t chan, const uint8_t* peer) const {
  if (m.seq == 0 || m.kind != kind) return false;
  if (kind == CONV_CHANNEL) return m.chan_idx == chan;
  return memcmp(m.peer, peer, sizeof(m.peer)) == 0;
}

UITask::Msg* UITask::addMsg(ConvKind kind, uint8_t chan, const uint8_t* peer, const char* from, const char* text, bool outgoing) {
  if (_max_msgs == 0) return NULL;
  uint32_t seq = _next_seq++;
  Msg& m = _msgs[seq % _max_msgs];

  // the slot being overwritten may still be awaiting an ACK
  for (int i = 0; i < CD_PENDING_SENDS; i++) {
    if (_pending[i].seq == m.seq) _pending[i].seq = 0;
  }

  memset(&m, 0, sizeof(m));
  m.seq = seq;
  m.ts = rtc_clock.getCurrentTime();
  m.kind = kind;
  m.chan_idx = chan;
  if (peer) memcpy(m.peer, peer, sizeof(m.peer));
  m.outgoing = outgoing;
  m.hops = 0xFF;
  sanitize(m.from, sizeof(m.from), from);
  sanitize(m.text, sizeof(m.text), text);
  return &m;
}

UITask::Msg* UITask::findMsg(uint32_t seq) {
  if (seq == 0 || _max_msgs == 0) return NULL;
  Msg& m = _msgs[seq % _max_msgs];
  return (m.seq == seq) ? &m : NULL;
}

int UITask::unreadFor(ConvKind kind, uint8_t chan, const uint8_t* peer) const {
  int n = 0;
  for (int i = 0; i < _max_msgs; i++) {
    if (_msgs[i].unread && msgInConv(_msgs[i], kind, chan, peer)) n++;
  }
  return n;
}

int UITask::totalUnread() const {
  int n = 0;
  for (int i = 0; i < _max_msgs; i++) {
    if (_msgs[i].seq && _msgs[i].unread) n++;
  }
  return n;
}

void UITask::markRead(ConvKind kind, uint8_t chan, const uint8_t* peer) {
  for (int i = 0; i < _max_msgs; i++) {
    if (msgInConv(_msgs[i], kind, chan, peer)) _msgs[i].unread = 0;
  }
}

/* --------------------------------------- sending --------------------------------------- */

void UITask::sendInput() {
  if (_input_len == 0) return;
  _input_buf[_input_len] = 0;

  if (_input_buf[0] == '/') {   // commands work from any chat too
    _console_len = 0;
    runCommand(&_input_buf[1]);
    showAlert(_console_len > 0 ? _console[0] : "OK", 2500);
    _input_len = 0;
    _input_buf[0] = 0;
    return;
  }

  if (_conv_kind == CONV_CHANNEL) {
    ChannelDetails cd;
    if (!the_mesh.getChannel(_conv_chan, cd) || !cd.name[0]) {
      showAlert("CHANNEL GONE", 2000);
      return;
    }
    uint32_t ts = the_mesh.getRTCClock()->getCurrentTimeUnique();
    Msg* m = addMsg(CONV_CHANNEL, _conv_chan, NULL, _node_prefs->node_name, _input_buf, true);
    bool ok = the_mesh.sendGroupMessage(ts, cd.channel, _node_prefs->node_name, _input_buf, _input_len);
    if (m) m->status = ok ? ST_SENT : ST_FAILED;
    if (!ok) showAlert("TX FAILED: QUEUE FULL", 2500);
  } else if (_conv_kind == CONV_DM) {
    Msg* m = addMsg(CONV_DM, 0, _conv_peer, _node_prefs->node_name, _input_buf, true);
    if (m == NULL) return;

    int slot = -1;
    for (int i = 0; i < CD_PENDING_SENDS; i++) {
      if (_pending[i].seq == 0) { slot = i; break; }
    }
    if (slot < 0) {
      m->status = ST_FAILED;
      showAlert("TOO MANY UNACKED MSGS", 2500);
    } else {
      _pending[slot].seq = m->seq;
      _pending[slot].msg_ts = the_mesh.getRTCClock()->getCurrentTimeUnique();
      m->status = ST_SENDING;
      if (!sendDirect(slot, 0)) {
        m->status = ST_FAILED;
        _pending[slot].seq = 0;
        showAlert("TX FAILED", 2500);
      }
    }
  }

  _input_len = 0;
  _input_buf[0] = 0;
  _scroll = 0;
  _dirty = true;
}

bool UITask::sendDirect(int slot, uint8_t attempt) {
  Pending& p = _pending[slot];
  Msg* m = findMsg(p.seq);
  if (m == NULL) return false;
  ContactInfo* contact = the_mesh.lookupContactByPubKey(m->peer, sizeof(m->peer));
  if (contact == NULL) return false;

  // the last try floods, in case the stored direct path has gone stale
  if (attempt == SEND_ATTEMPTS - 1 && contact->out_path_len != OUT_PATH_UNKNOWN) {
    the_mesh.resetPathTo(*contact);
  }

  uint32_t ack = 0, est_timeout = 0;
  int result = the_mesh.sendMessage(*contact, p.msg_ts, attempt, m->text, ack, est_timeout);
  if (result == MSG_SEND_FAILED) return false;

  p.ack = ack;
  p.attempt = attempt;
  p.timeout_at = millis() + est_timeout + 1000;
  return true;
}

void UITask::checkPending() {
  for (int i = 0; i < CD_PENDING_SENDS; i++) {
    Pending& p = _pending[i];
    if (p.seq == 0 || (long)(millis() - p.timeout_at) < 0) continue;

    Msg* m = findMsg(p.seq);
    if (m == NULL) { p.seq = 0; continue; }
    if (p.attempt + 1 < SEND_ATTEMPTS && sendDirect(i, p.attempt + 1)) continue;

    m->status = ST_FAILED;
    p.seq = 0;
    _dirty = true;
  }
}

void UITask::onACKRecv(uint32_t ack_crc) {
  for (int i = 0; i < CD_PENDING_SENDS; i++) {
    if (_pending[i].seq && _pending[i].ack == ack_crc) {
      Msg* m = findMsg(_pending[i].seq);
      if (m) m->status = ST_DELIVERED;
      _pending[i].seq = 0;
      _dirty = true;
    }
  }
}

/* --------------------------------------- receiving ------------------------------------- */

void UITask::onMessageRecv(mesh::Packet *pkt, const ContactInfo &from, uint8_t txt_type, uint32_t sender_timestamp, const char* text) {
  if (!(txt_type == TXT_TYPE_PLAIN || txt_type == TXT_TYPE_SIGNED_PLAIN)) return;   // not CLI traffic

  Msg* m = addMsg(CONV_DM, 0, from.id.pub_key, from.name, text, false);
  if (m == NULL) return;
  m->hops = pkt->isRouteFlood() ? pkt->path_len : 0xFF;
  m->snr4 = (int8_t)(pkt->getSNR() * 4);

  bool viewing = _screen == SCR_CHAT && _conv_kind == CONV_DM && memcmp(_conv_peer, from.id.pub_key, sizeof(_conv_peer)) == 0
                 && _display && _display->isOn();
  m->unread = !viewing;
  if (!viewing) {
    char name[32];
    sanitize(name, sizeof(name), from.name);
    char alert[54];
    snprintf(alert, sizeof(alert), "INCOMING // %s", name);
    showAlert(alert, 4000);
    _glitch_until = millis() + 350;
  }
  if (_display && !_display->isOn() && !hasConnection()) wake();
  _nodes_dirty = true;
  _dirty = true;
}

void UITask::onChannelMessageRecv(mesh::Packet *pkt, ChannelDetails& channel_details, const char* text) {
  int idx = -1;
  ChannelDetails cd;
  for (int i = 0; i < MAX_GROUP_CHANNELS; i++) {
    if (the_mesh.getChannel(i, cd) && cd.name[0] && memcmp(cd.channel.hash, channel_details.channel.hash, sizeof(cd.channel.hash)) == 0
        && memcmp(cd.channel.secret, channel_details.channel.secret, sizeof(cd.channel.secret)) == 0) {
      idx = i;
      break;
    }
  }
  if (idx < 0) return;

  // group texts arrive as "<sender>: <message>"
  char from[32] = "?";
  const char* body = text;
  const char* sep = strstr(text, ": ");
  if (sep && sep - text < (int)sizeof(from)) {
    memcpy(from, text, sep - text);
    from[sep - text] = 0;
    body = sep + 2;
  }

  Msg* m = addMsg(CONV_CHANNEL, idx, NULL, from, body, false);
  if (m == NULL) return;
  m->hops = pkt->isRouteFlood() ? pkt->path_len : 0xFF;
  m->snr4 = (int8_t)(pkt->getSNR() * 4);

  bool viewing = _screen == SCR_CHAT && _conv_kind == CONV_CHANNEL && _conv_chan == idx && _display && _display->isOn();
  m->unread = !viewing;
  if (!viewing) {
    char name[32];
    sanitize(name, sizeof(name), channel_details.name);
    char alert[54];
    snprintf(alert, sizeof(alert), "INCOMING // #%s // %s", name, m->from);
    showAlert(alert, 4000);
    _glitch_until = millis() + 350;
  }
  if (_display && !_display->isOn() && !hasConnection()) wake();
  _dirty = true;
}

void UITask::onQueueSizeChanged(int offline_queue_size) {
  _msgcount = offline_queue_size;
  _dirty = true;
}

void UITask::onDiscoveredContact(ContactInfo &contact, bool is_new, uint8_t path_len, const uint8_t* path) {
  _nodes_dirty = true;
  if (is_new) {
    char name[32];
    sanitize(name, sizeof(name), contact.name);
    char alert[54];
    snprintf(alert, sizeof(alert), "NEW NODE // [%s] %s", typeTag(contact.type), name);
    showAlert(alert, 3000);
  }
  _dirty = true;
}

void UITask::notify(UIEventType t) {
  _dirty = true;
}

void UITask::showAlert(const char* msg, int millis_dur) {
  strncpy(_alert, msg, sizeof(_alert) - 1);
  _alert[sizeof(_alert) - 1] = 0;
  _alert_until = millis() + millis_dur;
  _dirty = true;
}

/* -------------------------------------- navigation ------------------------------------- */

void UITask::openChannel(int list_pos) {
  if (list_pos < 0 || list_pos >= _num_chans) return;
  ChannelDetails cd;
  int idx = _chan_idx[list_pos];
  if (!the_mesh.getChannel(idx, cd)) return;

  _conv_kind = CONV_CHANNEL;
  _conv_chan = idx;
  sanitize(_conv_name, sizeof(_conv_name), cd.name);
  _screen = SCR_CHAT;
  _scroll = 0;
  markRead(CONV_CHANNEL, idx, NULL);
  _dirty = true;
}

void UITask::openNode(const uint8_t* pub) {
  int i;
  for (i = 0; i < _num_nodes; i++) {
    if (memcmp(_nodes[i].pub, pub, sizeof(_nodes[i].pub)) == 0) break;
  }
  if (i == _num_nodes) {
    showAlert("NOT IN CONTACTS", 2000);
    return;
  }
  const Node& n = _nodes[i];
  memcpy(_conv_peer, n.pub, sizeof(_conv_peer));
  strcpy(_conv_name, n.name);
  _scroll = 0;

  if (n.type == ADV_TYPE_CHAT) {
    _conv_kind = CONV_DM;
    _screen = SCR_CHAT;
    markRead(CONV_DM, 0, n.pub);
  } else {
    _screen = SCR_NODE;   // repeaters, rooms and sensors get an info page
  }
  _dirty = true;
}

void UITask::wake() {
  if (_display && !_display->isOn()) {
    _display->turnOn();
    _dirty = true;
  }
  _auto_off = millis() + _prefs.dim_secs * 1000UL;
}

void UITask::handleKey(int key) {
  bool was_off = _display && !_display->isOn();
  wake();
  if (was_off) return;   // the key that wakes the screen does nothing else

  if (_screen == SCR_BOOT) {
    _screen = SCR_LIST;
    _dirty = true;
    return;
  }
  if (key == TKEY_HOLD) {   // long-press the trackball: blank the screen
    if (_display) _display->turnOff();
    return;
  }

  switch (_screen) {
    case SCR_LIST: handleListKey(key); break;
    case SCR_CHAT: handleChatKey(key); break;
    case SCR_NODE:
      if (key == TKEY_BACKSPACE || key == TKEY_LEFT || key == TKEY_ESC || key == TKEY_CLICK || key == TKEY_ENTER) {
        _screen = SCR_LIST;
      } else if (key == 'r' || key == 'R') {
        ContactInfo* c = the_mesh.lookupContactByPubKey(_conv_peer, sizeof(_conv_peer));
        if (c) {
          the_mesh.resetPathTo(*c);
          showAlert("PATH RESET // NEXT TX FLOODS", 2500);
          _nodes_dirty = true;
        }
      }
      break;
    default: break;
  }
  _dirty = true;
}

void UITask::handleListKey(int key) {
  int count = (_tab == TAB_CHAN) ? _num_chans : (_tab == TAB_NODES) ? _num_nodes : (_tab == TAB_HEARD) ? num_heard : 0;
  int& sel = _sel[_tab];

  if (_tab == TAB_SYS) {   // the SYS tab is a command console
    if (key == TKEY_LEFT) { _tab = TAB_HEARD; return; }
    if (key == TKEY_RIGHT) { _tab = TAB_CHAN; return; }
    if (key == TKEY_ENTER || key == TKEY_CLICK) {
      if (_cmd_len > 0) {
        _cmd_buf[_cmd_len] = 0;
        consolePrint("> %s", _cmd_buf);
        runCommand(_cmd_buf);
        _cmd_len = 0;
        _cmd_buf[0] = 0;
      }
    } else if (key == TKEY_BACKSPACE) {
      if (_cmd_len > 0) _cmd_buf[--_cmd_len] = 0;
    } else if (key >= 32 && key < 127 && _cmd_len < (int)sizeof(_cmd_buf) - 1) {
      _cmd_buf[_cmd_len++] = key;
      _cmd_buf[_cmd_len] = 0;
    }
    return;
  }

  switch (key) {
    case TKEY_UP:    if (sel > 0) sel--; break;
    case TKEY_DOWN:  if (sel < count - 1) sel++; break;
    case TKEY_LEFT:  _tab = (Tab)((_tab + TAB_COUNT - 1) % TAB_COUNT); break;
    case TKEY_RIGHT: _tab = (Tab)((_tab + 1) % TAB_COUNT); break;
    case TKEY_ENTER:
    case TKEY_CLICK:
      if (count == 0) break;
      if (_tab == TAB_CHAN) {
        openChannel(sel);
      } else if (_tab == TAB_NODES) {
        openNode(_nodes[sel].pub);
      } else {
        openNode(heard[sel].pubkey_prefix);
      }
      break;
    default:
      // a letter jumps to the next entry starting with it
      if (isalnum(key) && count > 0 && _tab != TAB_HEARD) {
        for (int step = 1; step <= count; step++) {
          int i = (sel + step) % count;
          const char* name;
          ChannelDetails cd;
          if (_tab == TAB_CHAN) {
            the_mesh.getChannel(_chan_idx[i], cd);
            name = cd.name;
          } else {
            name = _nodes[i].name;
          }
          if (tolower(name[0]) == tolower(key)) { sel = i; break; }
        }
      }
      break;
  }
}

void UITask::handleChatKey(int key) {
  switch (key) {
    case TKEY_UP:    _scroll++; break;   // clamped when drawn
    case TKEY_DOWN:  if (_scroll > 0) _scroll--; break;
    case TKEY_LEFT:
    case TKEY_ESC:
      _screen = SCR_LIST;
      break;
    case TKEY_CLICK:
      if (_input_len == 0) { _screen = SCR_LIST; break; }
      sendInput();
      break;
    case TKEY_ENTER:
      sendInput();
      break;
    case TKEY_BACKSPACE:
      if (_input_len > 0) {
        _input_buf[--_input_len] = 0;
      } else {
        _screen = SCR_LIST;
      }
      break;
    default:
      if (key >= 32 && key < 127) {
        int max_len = CD_INPUT_MAX;
        if (_conv_kind == CONV_CHANNEL) max_len -= strlen(_node_prefs->node_name) + 2;   // room for "<name>: "
        if (_input_len < max_len) {
          _input_buf[_input_len++] = key;
          _input_buf[_input_len] = 0;
        }
      }
      break;
  }
  if (_screen == SCR_CHAT) markRead(_conv_kind, _conv_chan, _conv_peer);
}

/* --------------------------------------- console --------------------------------------- */

void UITask::consolePrint(const char* fmt, ...) {
  if (_console_len == CD_CONSOLE_LINES) {
    memmove(_console[0], _console[1], sizeof(_console[0]) * (CD_CONSOLE_LINES - 1));
    _console_len--;
  }
  va_list args;
  va_start(args, fmt);
  vsnprintf(_console[_console_len], sizeof(_console[0]), fmt, args);
  va_end(args);
  _console_len++;
}

void UITask::runCommand(const char* line) {
  char cmd[64];
  strncpy(cmd, line, sizeof(cmd) - 1);
  cmd[sizeof(cmd) - 1] = 0;
  char* arg = strchr(cmd, ' ');
  if (arg) { *arg++ = 0; while (*arg == ' ') arg++; } else { arg = cmd + strlen(cmd); }
  for (char* p = cmd; *p; p++) *p = tolower(*p);

  if (strcmp(cmd, "help") == 0 || strcmp(cmd, "?") == 0) {
    consolePrint("advert [flood]  ble on|off  gps on|off  name <x>");
    consolePrint("theme matrix|amber|gibson|redline  crt on|off");
    consolePrint("dim <secs>  tz <hours>  clear  reboot  off");
  } else if (strcmp(cmd, "advert") == 0) {
    bool flood = strcmp(arg, "flood") == 0;
    bool ok = flood ? the_mesh.advertFlood() : the_mesh.advert();
    consolePrint(ok ? (flood ? "ADVERT FLOODED" : "ADVERT SENT (ZERO HOP)") : "ADVERT FAILED");
  } else if (strcmp(cmd, "ble") == 0) {
    if (strcmp(arg, "on") == 0) enableBluetooth();
    else if (strcmp(arg, "off") == 0) disableBluetooth();
    consolePrint("BLE %s", isBluetoothEnabled() ? "ON" : "OFF");
  } else if (strcmp(cmd, "gps") == 0) {
    if (strcmp(arg, "on") == 0) setGPS(true);
    else if (strcmp(arg, "off") == 0) setGPS(false);
    consolePrint("GPS %s", getGPSState() ? "ON" : "OFF");
  } else if (strcmp(cmd, "name") == 0) {
    if (*arg) {
      strncpy(_node_prefs->node_name, arg, sizeof(_node_prefs->node_name) - 1);
      _node_prefs->node_name[sizeof(_node_prefs->node_name) - 1] = 0;
      the_mesh.savePrefs();
      consolePrint("HANDLE NOW %s // ADVERT TO ANNOUNCE", _node_prefs->node_name);
    } else {
      consolePrint("HANDLE %s", _node_prefs->node_name);
    }
  } else if (strcmp(cmd, "theme") == 0) {
    for (uint8_t i = 0; i < NUM_THEMES; i++) {
      if (strcmp(arg, THEMES[i].name) == 0) {
        _prefs.theme = i;
        savePrefs();
        applyTheme();
      }
    }
    consolePrint("THEME %s", THEMES[_prefs.theme].name);
  } else if (strcmp(cmd, "crt") == 0) {
    if (strcmp(arg, "on") == 0) _prefs.crt = 1;
    else if (strcmp(arg, "off") == 0) _prefs.crt = 0;
    savePrefs();
    consolePrint("CRT SCANLINES %s", _prefs.crt ? "ON" : "OFF");
  } else if (strcmp(cmd, "dim") == 0) {
    if (*arg) {
      int s = atoi(arg);
      _prefs.dim_secs = (s < 0) ? 0 : (s > 255) ? 255 : s;
      savePrefs();
    }
    if (_prefs.dim_secs) consolePrint("SCREEN BLANKS AFTER %us", _prefs.dim_secs);
    else consolePrint("SCREEN STAYS ON");
  } else if (strcmp(cmd, "tz") == 0) {
    if (*arg) {
      int tz = atoi(arg);
      if (tz >= -12 && tz <= 14) {
        _node_prefs->tz_offset = tz;
        the_mesh.savePrefs();
      }
    }
    consolePrint("TZ UTC%+d", _node_prefs->tz_offset);
  } else if (strcmp(cmd, "clear") == 0 || strcmp(cmd, "cls") == 0) {
    _console_len = 0;
  } else if (strcmp(cmd, "reboot") == 0) {
    shutdown(true);
  } else if (strcmp(cmd, "off") == 0) {
    shutdown(false);
  } else if (cmd[0]) {
    consolePrint("UNKNOWN COMMAND: %s // TRY help", cmd);
  }
  _dirty = true;
}

bool UITask::getGPSState() {
  if (_sensors == NULL) return false;
  for (int i = 0; i < _sensors->getNumSettings(); i++) {
    if (strcmp(_sensors->getSettingName(i), "gps") == 0) return strcmp(_sensors->getSettingValue(i), "1") == 0;
  }
  return false;
}

void UITask::setGPS(bool on) {
  if (_sensors == NULL) return;
  for (int i = 0; i < _sensors->getNumSettings(); i++) {
    if (strcmp(_sensors->getSettingName(i), "gps") == 0) {
      _sensors->setSettingValue("gps", on ? "1" : "0");
      _node_prefs->gps_enabled = on;
      the_mesh.savePrefs();
      return;
    }
  }
}

void UITask::shutdown(bool restart) {
  if (restart) {
    _board->reboot();
  } else {
    if (_display) {
      _display->clear();
      _display->endFrame();
      _display->turnOff();
    }
    _board->powerOff();
  }
}

/* ----------------------------------------- loop ---------------------------------------- */

void UITask::loop() {
  int ev = user_btn.check();
  if (ev == BUTTON_EVENT_CLICK) handleKey(TKEY_CLICK);
  else if (ev == BUTTON_EVENT_LONG_PRESS) handleKey(TKEY_HOLD);

  for (int n = 0; n < 8; n++) {   // bounded, so a fast trackball roll can't starve the mesh
    int key = _input.read();
    if (key == TKEY_NONE) break;
    handleKey(key);
  }

  checkPending();
  if (_nodes_dirty && (long)(millis() - _nodes_refresh_at) >= 0) {
    refreshNodes();
    _dirty = true;
  }

  if (_display == NULL) return;
  unsigned long now = millis();

  if (_display->isOn() && _prefs.dim_secs && _screen != SCR_BOOT && (long)(now - _auto_off) >= 0) {
    _display->turnOff();
  }
  if (!_display->isOn()) return;

  if (_screen == SCR_BOOT) {
    if (now - _boot_started > 4500) {
      _screen = SCR_LIST;
      _dirty = true;
    } else if ((long)(now - _next_render) >= 0) {
      render();
      _display->pushRows(0, CYBERDECK_H);
      _next_render = now + 40;
    }
    return;
  }

  bool glitching = (long)(_glitch_until - now) > 0;
  if (((_dirty || glitching) && now - _last_render >= 40) || (long)(now - _next_render) >= 0) {
    _dirty = false;
    _last_render = now;
    _next_render = now + 1000;   // clock, alert expiry
    _cursor_on = true;
    _next_blink = now + 500;
    render();
    _display->pushRows(0, CYBERDECK_H);
  } else if ((long)(now - _next_blink) >= 0 && _display->buffer() != NULL) {
    _cursor_on = !_cursor_on;
    _next_blink = now + 500;
    render();
    _display->pushRows(INPUT_Y - 2, ROW_H + 2);   // only the prompt line changes
  }
}

/* ---------------------------------------- drawing -------------------------------------- */

void UITask::text(int x, int y, uint16_t col, const char* s) {
  Adafruit_GFX& g = _display->gfx();
  g.setTextSize(1);
  g.setTextColor(col);
  g.setCursor(x, y);
  g.print(s);
}

void UITask::textf(int x, int y, uint16_t col, const char* fmt, ...) {
  char buf[COLS + 8];
  va_list args;
  va_start(args, fmt);
  vsnprintf(buf, sizeof(buf), fmt, args);
  va_end(args);
  text(x, y, col, buf);
}

int UITask::battPercent() const {
  int mv = getBattMilliVolts();
  int pct = (mv - 3300) * 100 / (4200 - 3300);
  return pct < 0 ? 0 : pct > 100 ? 100 : pct;
}

void UITask::fmtTime(char* dest, uint32_t epoch, bool with_secs) const {
  if (epoch < 1600000000UL) {   // clock not set yet
    strcpy(dest, with_secs ? "--:--:--" : "--:--");
    return;
  }
  epoch += (int32_t)_node_prefs->tz_offset * 3600;
  uint32_t secs = epoch % 86400;
  if (with_secs) sprintf(dest, "%02u:%02u:%02u", secs / 3600, (secs / 60) % 60, secs % 60);
  else sprintf(dest, "%02u:%02u", secs / 3600, (secs / 60) % 60);
}

void UITask::fmtAgo(char* dest, uint32_t epoch) const {
  uint32_t now = rtc_clock.getCurrentTime();
  if (epoch == 0 || epoch > now) { strcpy(dest, "--"); return; }
  uint32_t d = now - epoch;
  if (d < 60) sprintf(dest, "%us", d);
  else if (d < 3600) sprintf(dest, "%um", d / 60);
  else if (d < 86400) sprintf(dest, "%uh", d / 3600);
  else sprintf(dest, "%ud", d / 86400);
}

void UITask::render() {
  Adafruit_GFX& g = _display->gfx();
  g.fillScreen(c_bg);

  if (_screen == SCR_BOOT) {
    drawBoot();
  } else {
    drawHeader();
    switch (_screen) {
      case SCR_LIST:
        drawTabs();
        if (_tab == TAB_CHAN) drawChanList();
        else if (_tab == TAB_NODES) drawNodeList(false);
        else if (_tab == TAB_HEARD) drawNodeList(true);
        else drawSys();
        break;
      case SCR_CHAT: drawChat(); break;
      case SCR_NODE: drawNode(); break;
      default: break;
    }

    // alert banner, over whatever is underneath
    if (_alert[0] && (long)(_alert_until - millis()) > 0) {
      int w = (strlen(_alert) + 8) * CHAR_W;
      int x = (SCR_W - w) / 2;
      g.fillRect(x, 100, w, 22, c_bg);
      g.drawRect(x, 100, w, 22, c_alert);
      g.drawRect(x + 2, 102, w - 4, 18, c_alert);
      textf(x + 6, 107, c_alert, "<<< %s >>>", _alert);
    }
  }

  if (_prefs.crt) crtPass();
  if ((long)(_glitch_until - millis()) > 0) glitchPass();
}

void UITask::drawBoot() {
  char lines[16][COLS + 1];
  int n = 0;
  char tmp[COLS + 1];

  snprintf(lines[n++], COLS + 1, "MESHCORE//DECK BIOS %s", FIRMWARE_VERSION);
  snprintf(lines[n++], COLS + 1, "(C) 1995 NOBODY. ALL RIGHTS DISAVOWED.");
  lines[n++][0] = 0;

  #define BOOT_CHECK(label, ok) do { \
    snprintf(tmp, sizeof(tmp), "%s ", label); \
    int l = strlen(tmp); while (l < 40) tmp[l++] = '.'; tmp[l] = 0; \
    snprintf(lines[n++], COLS + 1, "%s %s", tmp, (ok) ? "[ OK ]" : "[FAIL]"); \
  } while (0)

  BOOT_CHECK("CPU ESP32-S3 240MHZ", true);
  char label[COLS + 1];
  snprintf(label, sizeof(label), "PSRAM %uK", (unsigned)(ESP.getPsramSize() / 1024));
  BOOT_CHECK(label, ESP.getPsramSize() > 0);
  snprintf(label, sizeof(label), "LORA SX1262 %.3fMHZ SF%u BW%.1f", _node_prefs->freq, _node_prefs->sf, _node_prefs->bw);
  BOOT_CHECK(label, true);
  BOOT_CHECK("KEYBOARD I2C 0x55", _input.hasKeyboard());
  snprintf(label, sizeof(label), "MSG BUFFER %d SLOTS", _max_msgs);
  BOOT_CHECK(label, _max_msgs > 0);
  lines[n++][0] = 0;

  const uint8_t* pk = the_mesh.self_id.pub_key;
  snprintf(lines[n++], COLS + 1, "HANDLE  %s", _node_prefs->node_name);
  snprintf(lines[n++], COLS + 1, "NODE ID %02X%02X%02X%02X%02X%02X  CONTACTS %d  CHANNELS %d",
           pk[0], pk[1], pk[2], pk[3], pk[4], pk[5], _num_nodes, _num_chans);
  if (isBluetoothEnabled()) snprintf(lines[n++], COLS + 1, "BLE PIN %06u", (unsigned)the_mesh.getBLEPin());
  else snprintf(lines[n++], COLS + 1, "BLE OFF");
  lines[n++][0] = 0;
  snprintf(lines[n++], COLS + 1, ">> JACKING IN");

  // type it out at ~250 chars/sec
  int budget = (millis() - _boot_started) / 4;
  int y = 8;
  for (int i = 0; i < n && budget > 0; i++, y += ROW_H + 2) {
    int len = strlen(lines[i]);
    int shown = (budget < len) ? budget : len;
    budget -= len + 6;   // a beat between lines
    memcpy(tmp, lines[i], shown);
    tmp[shown] = 0;
    uint16_t col = (i == 0) ? c_hi : c_fg;
    if (strstr(tmp, "[FAIL]")) col = c_alert;
    text(4, y, col, tmp);
    if (shown < len || i == n - 1) {
      bool blink = (millis() / 250) % 2;
      if (blink) _display->gfx().fillRect(4 + shown * CHAR_W, y, CHAR_W - 1, 8, c_fg);
      if (i == n - 1 && shown == len) {   // dots after JACKING IN
        int dots = ((millis() - _boot_started) / 300) % 4;
        for (int d = 0; d < dots; d++) text(4 + (len + d) * CHAR_W, y, c_fg, ".");
      }
      break;
    }
  }
}

void UITask::drawHeader() {
  Adafruit_GFX& g = _display->gfx();
  g.fillRect(0, 0, SCR_W, 11, c_fg);
  text(2, 2, c_bg, G_DARK G_MED G_LIGHT " MESHCORE//DECK");

  char clock[12];
  fmtTime(clock, rtc_clock.getCurrentTime());
  char right[48];
  int unread = totalUnread();
  if (unread) snprintf(right, sizeof(right), "MSG %d BAT%3d%% %s", unread, battPercent(), clock);
  else snprintf(right, sizeof(right), "BAT%3d%% %s", battPercent(), clock);
  int x = SCR_W - 2 - strlen(right) * CHAR_W;
  text(x, 2, c_bg, right);

  // radio link indicators, left of the battery
  int ix = x - 4;
  if (getGPSState()) { ix -= 4 * CHAR_W; text(ix, 2, c_bg, "GPS"); }
  if (isBluetoothEnabled()) {
    ix -= 5 * CHAR_W;
    text(ix, 2, c_bg, hasConnection() ? "BLE+" : "BLE");
  }
}

void UITask::drawTabs() {
  Adafruit_GFX& g = _display->gfx();
  int x = 2;
  for (int t = 0; t < TAB_COUNT; t++) {
    char label[16];
    int unread = 0;
    if (t == TAB_CHAN) {
      for (int i = 0; i < _num_chans; i++) unread += unreadFor(CONV_CHANNEL, _chan_idx[i], NULL);
    } else if (t == TAB_NODES) {
      for (int i = 0; i < _max_msgs; i++) if (_msgs[i].seq && _msgs[i].unread && _msgs[i].kind == CONV_DM) unread++;
    }
    snprintf(label, sizeof(label), unread ? " %s*" : " %s ", TAB_NAMES[t]);
    int w = strlen(label) * CHAR_W + CHAR_W;
    if (t == _tab) {
      g.fillRect(x, 13, w, 11, c_fg);
      text(x + 3, 15, c_bg, label);
    } else {
      g.drawRect(x, 13, w, 11, c_dim);
      text(x + 3, 15, unread ? c_hi : c_dim, label);
    }
    x += w + 3;
  }
  g.drawFastHLine(0, 24, SCR_W, c_dim);
}

void UITask::drawFooter(const char* hint) {
  Adafruit_GFX& g = _display->gfx();
  g.drawFastHLine(0, FOOT_Y - 3, SCR_W, c_dim);
  text(2, FOOT_Y, c_dim, hint);
}

static int listTop(int count, int sel, int rows) {
  int top = sel - rows / 2;
  if (top > count - rows) top = count - rows;
  return top < 0 ? 0 : top;
}

void UITask::drawChanList() {
  Adafruit_GFX& g = _display->gfx();
  int& sel = _sel[TAB_CHAN];
  if (sel >= _num_chans) sel = _num_chans ? _num_chans - 1 : 0;

  if (_num_chans == 0) {
    text(8, BODY_Y + 10, c_dim, "NO CHANNELS. ADD THEM FROM THE PHONE APP.");
  }
  int rows = BODY_ROWS - 1;
  int top = listTop(_num_chans, sel, rows);
  for (int r = 0; r < rows && top + r < _num_chans; r++) {
    int i = top + r, y = BODY_Y + 2 + r * ROW_H;
    int idx = _chan_idx[i];
    ChannelDetails cd;
    the_mesh.getChannel(idx, cd);
    char name[32];
    sanitize(name, sizeof(name), cd.name);

    // newest message in this channel
    Msg* last = NULL;
    for (uint32_t s = _next_seq - 1; s > 0 && s + _max_msgs >= _next_seq; s--) {
      Msg* m = findMsg(s);
      if (m && msgInConv(*m, CONV_CHANNEL, idx, NULL)) { last = m; break; }
    }
    int unread = unreadFor(CONV_CHANNEL, idx, NULL);
    bool is_sel = i == sel;
    uint16_t col = is_sel ? c_bg : c_fg;
    if (is_sel) g.fillRect(0, y - 1, SCR_W, ROW_H, c_fg);

    textf(2, y, col, "%s #%-18.18s", is_sel ? G_RIGHT : " ", name);
    if (unread) textf(26 * CHAR_W, y, is_sel ? c_bg : c_hi, "%2d NEW", unread);
    if (last) {
      char t[8];
      fmtTime(t, last->ts);
      textf(34 * CHAR_W, y, is_sel ? c_bg : c_dim, "%-12.12s %s", last->from, t);
    }
  }
  drawFooter(G_UP G_DOWN " SELECT  RET OPEN  " G_LEFT G_RIGHT " TAB  A-Z JUMP");
}

void UITask::drawNodeList(bool recent) {
  Adafruit_GFX& g = _display->gfx();
  int count;
  if (recent) {
    num_heard = the_mesh.getRecentlyHeard(heard, 16);
    count = 0;
    while (count < num_heard && heard[count].name[0]) count++;   // unused slots sort last
    num_heard = count;
  } else {
    count = _num_nodes;
  }
  int& sel = _sel[recent ? TAB_HEARD : TAB_NODES];
  if (sel >= count) sel = count ? count - 1 : 0;

  if (count == 0) {
    text(8, BODY_Y + 10, c_dim, recent ? "NOTHING HEARD YET. TRY: advert flood (SYS TAB)"
                                        : "NO CONTACTS YET. WAITING FOR ADVERTS...");
  }
  int rows = BODY_ROWS - 1;
  int top = listTop(count, sel, rows);
  for (int r = 0; r < rows && top + r < count; r++) {
    int i = top + r, y = BODY_Y + 2 + r * ROW_H;
    bool is_sel = i == sel;
    uint16_t col = is_sel ? c_bg : c_fg, dim = is_sel ? c_bg : c_dim;
    if (is_sel) g.fillRect(0, y - 1, SCR_W, ROW_H, c_fg);

    char name[32], ago[8], hops[8];
    const char* tag = "";
    uint8_t path_len;
    uint32_t when;
    int unread = 0;
    if (recent) {
      sanitize(name, sizeof(name), heard[i].name);
      path_len = heard[i].path_len;
      when = heard[i].recv_timestamp;
      for (int n = 0; n < _num_nodes; n++) {
        if (memcmp(_nodes[n].pub, heard[i].pubkey_prefix, sizeof(_nodes[n].pub)) == 0) { tag = typeTag(_nodes[n].type); break; }
      }
    } else {
      const Node& n = _nodes[i];
      strcpy(name, n.name);
      tag = typeTag(n.type);
      path_len = n.path_len;
      when = n.lastmod;
      unread = unreadFor(CONV_DM, 0, n.pub);
    }
    fmtAgo(ago, when);
    if (path_len == OUT_PATH_UNKNOWN) strcpy(hops, "FLOOD");
    else if (path_len == 0) strcpy(hops, "DIRECT");
    else snprintf(hops, sizeof(hops), "%dHOP", path_len);

    textf(2, y, col, "%s", is_sel ? G_RIGHT : " ");
    textf(2 * CHAR_W + 2, y, dim, "[%-4s]", tag);
    textf(9 * CHAR_W + 2, y, col, "%-22.22s", name);
    if (unread) textf(32 * CHAR_W, y, is_sel ? c_bg : c_hi, "%d NEW", unread);
    textf(39 * CHAR_W, y, dim, "%6s %5s", hops, ago);
  }
  drawFooter(G_UP G_DOWN " SELECT  RET OPEN  " G_LEFT G_RIGHT " TAB");
}

void UITask::drawSys() {
  Adafruit_GFX& g = _display->gfx();
  int y = BODY_Y + 2;
  const uint8_t* pk = the_mesh.self_id.pub_key;

  textf(2, y, c_dim, "HANDLE");
  textf(9 * CHAR_W, y, c_fg, "%-22.22s ID %02X%02X%02X%02X", _node_prefs->node_name, pk[0], pk[1], pk[2], pk[3]);
  y += ROW_H;
  textf(2, y, c_dim, "RADIO");
  textf(9 * CHAR_W, y, c_fg, "%.3fMHZ BW%.1f SF%u CR%u %ddBm", _node_prefs->freq, _node_prefs->bw, _node_prefs->sf,
        _node_prefs->cr, _node_prefs->tx_power_dbm);
  y += ROW_H;
  unsigned long up = millis() / 1000;
  textf(2, y, c_dim, "POWER");
  textf(9 * CHAR_W, y, c_fg, "%.2fV %d%%   UPTIME %luh%02lum", getBattMilliVolts() / 1000.0f, battPercent(), up / 3600, (up / 60) % 60);
  y += ROW_H;
  textf(2, y, c_dim, "BLE");
  if (isBluetoothEnabled()) textf(9 * CHAR_W, y, c_fg, "ON  PIN %06u  %s", (unsigned)the_mesh.getBLEPin(), hasConnection() ? "[LINKED]" : "[IDLE]");
  else textf(9 * CHAR_W, y, c_fg, "OFF");
  y += ROW_H;
  textf(2, y, c_dim, "GPS");
  if (getGPSState()) textf(9 * CHAR_W, y, c_fg, "ON  %.5f %.5f", _sensors->node_lat, _sensors->node_lon);
  else textf(9 * CHAR_W, y, c_fg, "OFF");
  y += ROW_H;
  textf(2, y, c_dim, "MESH");
  textf(9 * CHAR_W, y, c_fg, "CONTACTS %d  CHANNELS %d  APP QUEUE %d", _num_nodes, _num_chans, _msgcount);
  y += ROW_H;
  textf(2, y, c_dim, "SCOPE");
  textf(9 * CHAR_W, y, c_fg, "%s", _node_prefs->default_scope_name[0] ? _node_prefs->default_scope_name : "* (UNSCOPED)");
  y += ROW_H + 2;
  g.drawFastHLine(0, y - 2, SCR_W, c_dim);

  if (_console_len == 0) text(2, y + 1, c_dim, "TYPE help FOR COMMANDS");
  for (int i = 0; i < _console_len; i++) {
    text(2, y + 1 + i * ROW_H, _console[i][0] == '>' ? c_dim : c_fg, _console[i]);
  }
  drawInput(INPUT_Y, "root@deck:~#", _cmd_buf, _cmd_len);
  drawFooter("TYPE A COMMAND  RET RUN  " G_LEFT G_RIGHT " TAB");
}

void UITask::drawInput(int y, const char* prompt, const char* buf, int len) {
  Adafruit_GFX& g = _display->gfx();
  g.drawRect(0, y - 3, SCR_W, ROW_H + 4, c_dim);
  textf(3, y, c_hi, "%s", prompt);
  int px = 3 + (strlen(prompt) + 1) * CHAR_W;
  int room = (SCR_W - px - 4) / CHAR_W - 1;
  const char* shown = (len > room) ? buf + (len - room) : buf;   // keep the end of long input visible
  text(px, y, c_fg, shown);
  if (_cursor_on) {
    int cx = px + strlen(shown) * CHAR_W;
    g.fillRect(cx, y, CHAR_W - 1, 8, c_fg);
  }
}

void UITask::drawChat() {
  Adafruit_GFX& g = _display->gfx();

  // title strip
  int unread_elsewhere = totalUnread();
  if (_conv_kind == CONV_CHANNEL) textf(2, 14, c_hi, "CHANNEL #%s", _conv_name);
  else textf(2, 14, c_hi, "SECURE LINK // %s", _conv_name);
  if (unread_elsewhere) textf(SCR_W - 12 * CHAR_W, 14, c_alert, "%2d UNREAD " G_LEFT, unread_elsewhere);
  g.drawFastHLine(0, 24, SCR_W, c_dim);

  // lay out lines bottom-up: newest message last
  struct Line { const Msg* m; uint16_t start; uint8_t len; };
  static Line lines[BODY_ROWS + 200];
  const int max_lines = sizeof(lines) / sizeof(lines[0]);
  int n = 0;
  int want = BODY_ROWS + _scroll;
  if (want > max_lines) want = max_lines;

  static char full[COLS * 6];
  for (uint32_t s = _next_seq - 1; s > 0 && s + _max_msgs >= _next_seq && n < want; s--) {
    const Msg* m = findMsg(s);
    if (m == NULL || !msgInConv(*m, _conv_kind, _conv_chan, _conv_peer)) continue;

    // break this message into wrapped segments, then add them last-first
    char t[8];
    fmtTime(t, m->ts);
    int flen = snprintf(full, sizeof(full), "%s <%s> %s", t, m->from, m->text);
    if (flen >= (int)sizeof(full)) flen = sizeof(full) - 1;
    uint16_t seg_start[8];
    uint8_t seg_len[8];
    int segs = 0, pos = 0;
    while (pos < flen && segs < 8) {
      int width = (segs == 0) ? COLS : COLS - 3;   // continuation lines are indented
      int len = flen - pos;
      if (len > width) {
        len = width;
        for (int k = width; k > width / 2; k--) {
          if (full[pos + k] == ' ') { len = k; break; }
        }
      }
      seg_start[segs] = pos;
      seg_len[segs] = len;
      segs++;
      pos += len;
      while (full[pos] == ' ') pos++;
    }
    for (int k = segs - 1; k >= 0 && n < max_lines; k--) {
      lines[n].m = m;
      lines[n].start = seg_start[k];
      lines[n].len = seg_len[k];
      n++;
    }
  }
  if (_scroll > n - BODY_ROWS) _scroll = (n > BODY_ROWS) ? n - BODY_ROWS : 0;

  if (n == 0) {
    text(8, BODY_Y + 10, c_dim, _conv_kind == CONV_CHANNEL ? "NO TRAFFIC YET. SAY SOMETHING." : "LINK OPEN. NO MESSAGES YET.");
  }

  // draw: each line re-derives its text from the message, so only a pointer and offsets were stored
  for (int r = 0; r < BODY_ROWS; r++) {
    int li = _scroll + (BODY_ROWS - 1 - r);
    if (li >= n) continue;
    const Line& L = lines[li];
    const Msg* m = L.m;
    char t[8];
    fmtTime(t, m->ts);
    int flen = snprintf(full, sizeof(full), "%s <%s> %s", t, m->from, m->text);
    if (flen >= (int)sizeof(full)) flen = sizeof(full) - 1;

    int y = BODY_Y + 2 + r * ROW_H;
    int x = 2 + (L.start > 0 ? 3 * CHAR_W : 0);
    char seg[COLS + 1];
    int len = L.len < COLS ? L.len : COLS;
    memcpy(seg, &full[L.start], len);
    seg[len] = 0;

    uint16_t body_col = m->outgoing ? c_hi : c_fg;
    if (m->status == ST_FAILED) body_col = c_alert;
    if (L.start == 0) {
      // "[time] <from>" prefix in its own colours
      int tlen = strlen(t), plen = tlen + 1 + strlen(m->from) + 2;
      if (plen > len) plen = len;
      char pre[COLS + 1];
      memcpy(pre, seg, tlen);
      pre[tlen] = 0;
      text(x, y, c_dim, pre);
      memcpy(pre, seg + tlen, plen - tlen);
      pre[plen - tlen] = 0;
      text(x + tlen * CHAR_W, y, m->outgoing ? c_dim : c_hi, pre);
      text(x + plen * CHAR_W, y, body_col, seg + plen);
    } else {
      text(x, y, body_col, seg);
    }

    // delivery state at the end of a message's last line
    bool last_line = li == 0 || lines[li - 1].m != m;   // a message's final segment sits nearest the bottom
    if (m->outgoing && last_line) {
      const char* mark = (m->status == ST_SENDING) ? " ..." : (m->status == ST_DELIVERED) ? " " G_CHECK
                       : (m->status == ST_FAILED) ? " FAILED" : "";
      int mx = x + len * CHAR_W;
      if (mx + strlen(mark) * CHAR_W <= SCR_W) text(mx, y, m->status == ST_FAILED ? c_alert : c_dim, mark);
    } else if (!m->outgoing && last_line && m->hops != 0xFF) {
      // hops/SNR tag for received flood messages, right-aligned if it fits
      char tag[16];
      snprintf(tag, sizeof(tag), " %dh %+ddB", m->hops, m->snr4 / 4);
      int mx = x + len * CHAR_W;
      if (mx + (int)strlen(tag) * CHAR_W <= SCR_W - 2) text(SCR_W - 2 - strlen(tag) * CHAR_W, y, c_dim, tag);
    }
  }
  if (_scroll > 0) textf(SCR_W - 10 * CHAR_W, BODY_Y + 2, c_alert, G_DOWN " +%d", _scroll);

  char prompt[24];
  snprintf(prompt, sizeof(prompt), "%.10s>", _node_prefs->node_name);
  drawInput(INPUT_Y, prompt, _input_buf, _input_len);
  char hint[COLS + 1];
  int max_len = CD_INPUT_MAX - (_conv_kind == CONV_CHANNEL ? strlen(_node_prefs->node_name) + 2 : 0);
  snprintf(hint, sizeof(hint), "RET SEND  " G_UP G_DOWN " SCROLL  " G_LEFT " BACK  /help CMDS  %3d/%d", _input_len, max_len);
  drawFooter(hint);
}

void UITask::drawNode() {
  int i;
  for (i = 0; i < _num_nodes; i++) {
    if (memcmp(_nodes[i].pub, _conv_peer, sizeof(_conv_peer)) == 0) break;
  }
  if (i == _num_nodes) { _screen = SCR_LIST; return; }
  const Node& n = _nodes[i];

  int y = 14;
  textf(2, y, c_hi, "NODE DOSSIER // [%s] %s", typeTag(n.type), n.name);
  _display->gfx().drawFastHLine(0, 24, SCR_W, c_dim);
  y = BODY_Y + 4;

  ContactInfo* c = the_mesh.lookupContactByPubKey(n.pub, sizeof(n.pub));
  if (c) {
    char hex[65];
    for (int k = 0; k < 32; k++) sprintf(&hex[k * 2], "%02X", c->id.pub_key[k]);
    textf(2, y, c_dim, "PUBKEY");
    char half[33];
    memcpy(half, hex, 32); half[32] = 0;
    text(10 * CHAR_W, y, c_fg, half);
    y += ROW_H;
    text(10 * CHAR_W, y, c_fg, &hex[32]);
    y += ROW_H + 4;
  }
  char ago[8];
  fmtAgo(ago, n.lastmod);
  textf(2, y, c_dim, "HEARD");
  textf(10 * CHAR_W, y, c_fg, "%s AGO", ago);
  y += ROW_H;
  textf(2, y, c_dim, "ROUTE");
  if (n.path_len == OUT_PATH_UNKNOWN) textf(10 * CHAR_W, y, c_fg, "UNKNOWN // FLOODS");
  else if (n.path_len == 0) textf(10 * CHAR_W, y, c_fg, "DIRECT");
  else textf(10 * CHAR_W, y, c_fg, "%d HOPS VIA REPEATERS", n.path_len);
  y += ROW_H;
  textf(2, y, c_dim, "COORDS");
  if (n.lat || n.lon) textf(10 * CHAR_W, y, c_fg, "%.5f %.5f", n.lat / 1e6, n.lon / 1e6);
  else text(10 * CHAR_W, y, c_fg, "NOT SHARED");
  y += ROW_H + 8;

  if (n.type == ADV_TYPE_REPEATER) text(2, y, c_dim, "REPEATER. ADMIN AND LOGIN FROM THE PHONE APP.");
  else if (n.type == ADV_TYPE_ROOM) text(2, y, c_dim, "ROOM SERVER. JOIN AND POST FROM THE PHONE APP.");
  else if (n.type == ADV_TYPE_SENSOR) text(2, y, c_dim, "SENSOR. TELEMETRY FROM THE PHONE APP.");

  drawFooter("R RESET PATH   " G_LEFT " BACK");
}

void UITask::crtPass() {
  uint16_t* buf = _display->buffer();
  if (buf == NULL) return;
  // dim every other row to fake CRT scanlines
  for (int y = 1; y < CYBERDECK_H; y += 2) {
    uint16_t* row = buf + y * CYBERDECK_W;
    for (int x = 0; x < CYBERDECK_W; x++) row[x] = (row[x] >> 1) & 0x7BEF;
  }
}

void UITask::glitchPass() {
  uint16_t* buf = _display->buffer();
  if (buf == NULL) return;
  // tear a few horizontal bands sideways, like a bad VHS tracking line
  for (int k = 0; k < 5; k++) {
    int y = random(0, CYBERDECK_H - 12), h = random(2, 12), dx = random(-14, 15);
    for (int r = y; r < y + h; r++) {
      uint16_t* row = buf + r * CYBERDECK_W;
      if (dx > 0) memmove(row + dx, row, (CYBERDECK_W - dx) * 2);
      else if (dx < 0) memmove(row, row - dx, (CYBERDECK_W + dx) * 2);
      if (k == 0) for (int x = 0; x < CYBERDECK_W; x += 3) row[x] = c_alert;
    }
  }
}
