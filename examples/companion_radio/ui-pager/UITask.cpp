#include "UITask.h"
#include <Arduino.h>
#include <helpers/TxtDataHelpers.h>
#include <helpers/AdvertDataHelpers.h>
#include <helpers/sensors/MicroNMEALocationProvider.h>
#include <target.h>
#include "../MyMesh.h"

extern MicroNMEALocationProvider nmea;

#ifndef USER_BTN_PRESSED
  #define USER_BTN_PRESSED LOW
#endif

#define DEBOUNCE_MS        40
#define CLICK_GAP_MS       550     // a pause this long ends a run of presses
#define HOLD_SOS_MS        3000
#define HOLD_OFF_MS        10000

#define PAGE_TONE_EVERY    2500
#define PAGE_TONE_FOR      (15 * 60 * 1000UL)   // then just the LED, until a press

#define SOS_RETRY_MS       45000   // until a repeater is heard passing it on
#define SOS_MAX_TRIES      6
#define SOS_UPDATE_MS      (5 * 60 * 1000UL)    // fresh position while it lasts
#define SOS_LASTS_MS       (30 * 60 * 1000UL)

#define ECHO_WAIT_MS       12000
#define FIX_WAIT_MS        (3 * 60 * 1000UL)
#define FIX_FRESH_MS       (10 * 60 * 1000UL)
#define GPS_SETTLE_MS      4000

#define ADVERT_FIRST_MS    20000
#define ADVERT_EVERY_MS    (12 * 3600 * 1000UL)
#define BATT_EVERY_MS      60000
#define LOW_BATT_MV        3500
#define LOW_BATT_REARM_MV  3800

#define CLOCK_FLOOR        1748736000UL   // 1 Jun 2025: anything earlier means nobody set us
#define CLOCK_AGREE_SECS   120

// tones (RTTTL)
static const char* T_PAGE    = "page:d=16,o=7,b=200:c,p,g6,p,c,p,g6,p,c,p,g6";
static const char* T_DM      = "dm:d=4,o=6,b=200:32e,32g,32b,16c7";
static const char* T_SENT    = "sent:d=16,o=6,b=200:c,e,g";
static const char* T_NOHEAR  = "nohear:d=8,o=5,b=160:g,c";
static const char* T_ARMED   = "armed:d=32,o=7,b=200:c,p,c,p,c";
static const char* T_SOS     = "sos:d=16,o=6,b=200:c7,g,c7,g,c7";
static const char* T_HELP    = "help:d=8,o=6,b=140:c,e,g,c7";
static const char* T_ERR     = "err:d=4,o=4,b=120:c";
static const char* T_ACK     = "ack:d=32,o=8,b=120:c";
static const char* T_QUIET   = "quiet:d=16,o=6,b=200:g,c";
static const char* T_LOUD    = "loud:d=16,o=6,b=200:c,g";
static const char* T_BATT[4] = {
  "b1:d=16,o=6,b=180:a",
  "b2:d=16,o=6,b=180:a,p,a",
  "b3:d=16,o=6,b=180:a,p,a,p,a",
  "b4:d=16,o=6,b=180:a,p,a,p,a,p,a",
};

static bool startsWithI(const char* s, const char* prefix) {
  while (*prefix) {
    if (tolower((unsigned char)*s) != tolower((unsigned char)*prefix)) return false;
    s++; prefix++;
  }
  return true;
}

static const char* stristr(const char* hay, const char* needle) {
  if (!*needle) return hay;
  for (; *hay; hay++) if (startsWithI(hay, needle)) return hay;
  return NULL;
}

static bool isPage(const char* body) {
  return strncmp(body, PG_PAGE_PREFIX, strlen(PG_PAGE_PREFIX)) == 0 && strncmp(body, PG_PAGE_ACK, strlen(PG_PAGE_ACK)) != 0;
}

void UITask::begin(DisplayDriver* display, SensorManager* sensors, NodePrefs* node_prefs) {
  _sensors = sensors;
  _node_prefs = node_prefs;
  _started = millis();
  _next_advert = _started + ADVERT_FIRST_MS;
  _next_batt = _started + 5000;

#ifdef PIN_USER_BTN
  pinMode(PIN_USER_BTN, INPUT);
  // the press that switched us on may still be held: it mustn't count as a hold (SOS)
  _btn_down = _btn_swallow = digitalRead(PIN_USER_BTN) == USER_BTN_PRESSED;
  _btn_changed = millis();
#endif
#ifdef PIN_STATUS_LED
  pinMode(PIN_STATUS_LED, OUTPUT);
  digitalWrite(PIN_STATUS_LED, !LED_STATE_ON);
#endif
#ifdef PIN_BUZZER
  buzzer.begin();
  tone("hi:d=16,o=6,b=180:c,e,g,c7", true);
#endif
  _gps_on = false;
  gpsWant(_node_prefs->gps_enabled);   // GPS stays as the app left it; otherwise off until needed
}

/* ---------------- sound & light ---------------- */

void UITask::tone(const char* rtttl, bool force) {
#ifdef PIN_BUZZER
  // "quiet" only hushes the everyday chirps; alarms and button feedback always sound
  if (!force && _node_prefs->buzzer_quiet) return;
  buzzer.quiet(false);   // powers the buzzer driver; loop() powers it down when the tune ends
  buzzer.play(rtttl);
#endif
}

void UITask::notify(UIEventType t) {
  switch (t) {
    case UIEventType::contactMessage: tone(T_DM); break;
    case UIEventType::ack: tone(T_ACK); break;
    default: break;
  }
}

void UITask::ledTick() {
#ifdef PIN_STATUS_LED
  uint32_t now = millis();
  if ((long)(now - _next_led) < 0) return;
  // on-time, then off-time
  uint16_t on, off;
  if (_paging)                 { on = 100; off = 100; }
  else if (_sos)               { on = 60;  off = _led_state ? 140 : 1800; }
  else if (_unread)            { on = 200; off = 1800; }
  else if (_batt_mv && _batt_mv < LOW_BATT_MV) { on = 20; off = 1000; }
  else                         { on = 20;  off = 8000; }   // heartbeat: still alive
  _led_state = !_led_state;
  digitalWrite(PIN_STATUS_LED, _led_state ? LED_STATE_ON : !LED_STATE_ON);
  _next_led = now + (_led_state ? on : off);
#endif
}

/* ---------------- button ---------------- */

void UITask::buttonTick() {
#ifdef PIN_USER_BTN
  uint32_t now = millis();
  bool down = digitalRead(PIN_USER_BTN) == USER_BTN_PRESSED;
  if (down != _btn_down && (now - _btn_changed) > DEBOUNCE_MS) {
    _btn_down = down;
    _btn_changed = now;
    if (down) {
      _btn_pressed_at = now;
      _btn_armed = false;
      _btn_swallow = false;
      if (_paging) {          // any press answers a page
        ackPage();
        _btn_swallow = true;
        _clicks = 0;
      }
    } else {
      uint32_t held = now - _btn_pressed_at;
      if (_btn_swallow) {
        _btn_swallow = false;
      } else if (held >= HOLD_SOS_MS) {
        _clicks = 0;
        onHold();
      } else {
        _clicks++;
        _btn_released_at = now;
      }
    }
  }
  if (_btn_down && !_btn_swallow) {
    uint32_t held = now - _btn_pressed_at;
    if (held >= HOLD_OFF_MS) {
      _btn_swallow = true;
      shutdown();
    } else if (held >= HOLD_SOS_MS && !_btn_armed) {
      _btn_armed = true;
      tone(T_ARMED, true);   // let go now for SOS; keep holding to switch off
    }
  }
  if (!_btn_down && _clicks && (now - _btn_released_at) > CLICK_GAP_MS) {
    int n = _clicks;
    _clicks = 0;
    onClicks(n);
  }
#endif
}

void UITask::onClicks(int n) {
  char msg[96], pos[40];
  if (n == 1) {
    _unread = false;
    _batt_mv = _board->getBattMilliVolts();
    int pct = (_batt_mv - 3300) * 100 / (4150 - 3300);
    int beeps = pct > 75 ? 4 : pct > 50 ? 3 : pct > 25 ? 2 : 1;
    tone(T_BATT[beeps - 1], true);
  } else if (n == 2) {
    fmtPos(pos, sizeof(pos), FIX_FRESH_MS);
    snprintf(msg, sizeof(msg), "OK - all good%s", pos);
    sendToFarm(msg);
  } else if (n == 3) {
    fmtPos(pos, sizeof(pos), FIX_FRESH_MS);
    snprintf(msg, sizeof(msg), "Come get me please%s", pos[0] ? pos : " (finding GPS...)");
    sendToFarm(msg);
    if (!freshFix(60000)) {   // follow up with a fresh position
      _want_fix = true;
      _want_fix_until = millis() + FIX_WAIT_MS;
      _where_dm = false;
      _where_chan = farmChannel();
      strcpy(_want_fix_label, "Here I am");
      gpsWant(true);
    }
  } else {
    if (_sos) {
      cancelSOS();
    } else {
      _node_prefs->buzzer_quiet = !_node_prefs->buzzer_quiet;
      tone(_node_prefs->buzzer_quiet ? T_QUIET : T_LOUD, true);
      the_mesh.savePrefs();
    }
  }
}

void UITask::onHold() {
  sendSOS();
}

/* ---------------- GPS ---------------- */

void UITask::gpsWant(bool on) {
  if (on == _gps_on) return;
  if (!on && (_node_prefs->gps_enabled || _sos || _want_fix)) return;   // still needed
  _gps_on = on;
  if (on) _gps_on_since = millis();
  _sensors->setSettingValue("gps", on ? "1" : "0");
}

bool UITask::freshFix(uint32_t max_age_ms) const {
  return _have_fix && (millis() - _fix_at) < max_age_ms;
}

void UITask::fmtPos(char* dest, size_t n, uint32_t max_age_ms) const {
  dest[0] = 0;
  if (freshFix(max_age_ms)) {
    snprintf(dest, n, " @ %.5f,%.5f", _fix_lat / 1e6, _fix_lon / 1e6);
  } else if (_have_fix) {
    snprintf(dest, n, " @ ~%.5f,%.5f (%lu min old)", _fix_lat / 1e6, _fix_lon / 1e6, (unsigned long)((millis() - _fix_at) / 60000));
  }
}

void UITask::gpsTick() {
  if (!_gps_on) return;
  uint32_t now = millis();
  if (now - _gps_on_since > GPS_SETTLE_MS && nmea.isValid() && nmea.fixAgeMs() < 2000) {
    _fix_lat = nmea.getLatitude();
    _fix_lon = nmea.getLongitude();
    _fix_at = now;
    _have_fix = true;
  }

  // somebody's waiting for a position
  if (_want_fix && freshFix(5000)) {
    _want_fix = false;
    char msg[96];
    snprintf(msg, sizeof(msg), "%s @ %.5f,%.5f", _want_fix_label, _fix_lat / 1e6, _fix_lon / 1e6);
    if (_where_dm) sendDM(_where_peer, msg);
    else if (_where_chan >= 0) sendChannel(_where_chan, msg, false);
  } else if (_want_fix && (long)(now - _want_fix_until) > 0) {
    _want_fix = false;   // no sky; give up quietly
  }

  if (_sos && !_sos_has_pos && freshFix(5000)) {
    _sos_has_pos = true;
    char msg[96];
    snprintf(msg, sizeof(msg), "SOS position @ %.5f,%.5f", _fix_lat / 1e6, _fix_lon / 1e6);
    sendToFarm(msg);
    _sos_next_update = now + SOS_UPDATE_MS;
  }

  if (!_want_fix && !_sos) gpsWant(false);
}

/* ---------------- sending ---------------- */

int UITask::farmChannel() {
  ChannelDetails cd;
  int first = -1;
  for (int i = 0; i < MAX_GROUP_CHANNELS; i++) {
    if (!the_mesh.getChannel(i, cd) || !cd.name[0]) continue;
    const char* nm = cd.name[0] == '#' ? cd.name + 1 : cd.name;
    if (strcasecmp(nm, "farm") == 0) return i;
    if (first < 0 && strcasecmp(nm, "public") != 0) first = i;
  }
  return first;
}

bool UITask::sendChannel(int idx, const char* text, bool track_echo, bool is_sos) {
  ChannelDetails cd;
  if (idx < 0 || !the_mesh.getChannel(idx, cd) || !cd.name[0]) return false;
  uint32_t ts = the_mesh.getRTCClock()->getCurrentTimeUnique();
  _capture_echo = track_echo;
  bool ok = the_mesh.sendGroupMessage(ts, cd.channel, _node_prefs->node_name, text, strlen(text));
  _capture_echo = false;
  if (ok && track_echo) {
    _echo_wait = true;
    _echo_heard = false;
    _echo_is_sos = is_sos;
    _echo_deadline = millis() + ECHO_WAIT_MS;
  }
  return ok;
}

bool UITask::sendDM(const uint8_t* pub_prefix, const char* text) {
  ContactInfo* c = the_mesh.lookupContactByPubKey(pub_prefix, 6);
  if (c == NULL) return false;
  uint32_t ack = 0, est = 0;
  uint32_t ts = the_mesh.getRTCClock()->getCurrentTimeUnique();
  return the_mesh.sendMessage(*c, ts, 0, text, ack, est) != MSG_SEND_FAILED;
}

void UITask::sendToFarm(const char* text, bool is_sos) {
  int idx = farmChannel();
  if (idx < 0 || !sendChannel(idx, text, true, is_sos)) {
    tone(T_ERR, true);   // no farm channel set up, or the radio queue is full
  }
}

void UITask::onFloodSent(const mesh::Packet *pkt) {
  if (_capture_echo) {
    ((mesh::Packet*)pkt)->calculatePacketHash(_echo_hash);
    _capture_echo = false;
  }
}

void UITask::onFloodRecv(const mesh::Packet *pkt) {
  if (!_echo_wait || _echo_heard) return;
  uint8_t h[MAX_HASH_SIZE];
  ((mesh::Packet*)pkt)->calculatePacketHash(h);
  if (memcmp(h, _echo_hash, MAX_HASH_SIZE) == 0) {
    _echo_heard = true;   // a repeater passed it on
    _echo_wait = false;
    if (_echo_is_sos) _sos_next_retry = 0;
    tone(T_SENT, true);
  }
}

/* ---------------- paging ---------------- */

void UITask::startPage(bool dm, const uint8_t* peer, int chan, const char* from) {
  _paging = true;
  _page_dm = dm;
  if (peer) memcpy(_page_peer, peer, 6);
  _page_chan = chan;
  strncpy(_page_from, from, sizeof(_page_from) - 1);
  _page_from[sizeof(_page_from) - 1] = 0;
  _page_started = millis();
  _page_next_tone = _page_started;
}

void UITask::ackPage() {
  _paging = false;
  _unread = false;
  char ack[64];
  bool ok;
  if (_page_dm) {
    snprintf(ack, sizeof(ack), "%s %s", PG_PAGE_ACK, _node_prefs->node_name);
    ok = sendDM(_page_peer, ack);
  } else {
    snprintf(ack, sizeof(ack), "%s %s", PG_PAGE_ACK, _page_from);
    ok = sendChannel(_page_chan, ack, false);
  }
  tone(ok ? T_SENT : T_ERR, true);
}

void UITask::sendSOS() {
  char msg[96], pos[40];
  fmtPos(pos, sizeof(pos), FIX_FRESH_MS);
  if (freshFix(FIX_FRESH_MS)) {
    snprintf(msg, sizeof(msg), "%s SOS %.5f,%.5f %s needs help", PG_PAGE_PREFIX, _fix_lat / 1e6, _fix_lon / 1e6, _node_prefs->node_name);
  } else if (_have_fix) {
    snprintf(msg, sizeof(msg), "%s SOS ~%.5f,%.5f %s needs help", PG_PAGE_PREFIX, _fix_lat / 1e6, _fix_lon / 1e6, _node_prefs->node_name);
  } else {
    snprintf(msg, sizeof(msg), "%s SOS %s needs help", PG_PAGE_PREFIX, _node_prefs->node_name);
  }
  if (farmChannel() < 0) { tone(T_ERR, true); return; }
  _sos = true;
  _sos_started = millis();
  _sos_tries = 1;
  _sos_has_pos = freshFix(60000);
  _sos_next_retry = millis() + SOS_RETRY_MS;
  _sos_next_update = millis() + SOS_UPDATE_MS;
  gpsWant(true);
  tone(T_SOS, true);
  sendToFarm(msg, true);
}

void UITask::cancelSOS() {
  _sos = false;
  _sos_next_retry = 0;
  sendToFarm("SOS cancelled - I'm OK");
  gpsWant(false);
}

void UITask::replyWhere() {
  char msg[96], pos[40];
  fmtPos(pos, sizeof(pos), FIX_FRESH_MS);
  uint16_t mv = _board->getBattMilliVolts();
  int pct = constrain((mv - 3300) * 100 / (4150 - 3300), 0, 100);
  snprintf(msg, sizeof(msg), "%s, batt %d%%", pos[0] ? pos + 1 : "no GPS fix yet", pct);
  if (_where_dm) sendDM(_where_peer, msg);
  else sendChannel(_where_chan, msg, false);
  if (!freshFix(60000)) {   // and a fresh one when it comes
    _want_fix = true;
    _want_fix_until = millis() + FIX_WAIT_MS;
    strcpy(_want_fix_label, "Now");
    gpsWant(true);
  }
}

/* ---------------- receiving ---------------- */

bool UITask::mentionsMe(const char* text) const {
  const char* me = _node_prefs->node_name;
  return me[0] && stristr(text, me) != NULL;
}

void UITask::onMessageRecv(mesh::Packet *pkt, const ContactInfo &from, uint8_t txt_type, uint32_t sender_timestamp, const char* text) {
  if (!(txt_type == TXT_TYPE_PLAIN || txt_type == TXT_TYPE_SIGNED_PLAIN)) return;
  if (from.type != ADV_TYPE_CHAT) {   // room servers replay old posts; just chirp
    tone(T_DM);
    return;
  }
  syncClockFrom(sender_timestamp, from.id.pub_key);

  if (isPage(text)) {
    startPage(true, from.id.pub_key, -1, from.name);
  } else if (startsWithI(text, PG_PAGE_ACK)) {
    if (_sos) { _sos = false; tone(T_HELP, true); gpsWant(false); }
    else tone(T_ACK);
  } else if (startsWithI(text, PG_WHERE)) {
    _where_dm = true;
    memcpy(_where_peer, from.id.pub_key, 6);
    replyWhere();
  } else {
    _unread = true;
    tone(T_DM);
  }
}

void UITask::onChannelMessageRecv(mesh::Packet *pkt, ChannelDetails& channel_details, const char* text) {
  int idx = -1;
  ChannelDetails cd;
  for (int i = 0; i < MAX_GROUP_CHANNELS; i++) {
    if (the_mesh.getChannel(i, cd) && cd.name[0] && memcmp(cd.channel.secret, channel_details.channel.secret, sizeof(cd.channel.secret)) == 0) {
      idx = i;
      break;
    }
  }
  const char* nm = channel_details.name[0] == '#' ? channel_details.name + 1 : channel_details.name;
  bool is_public = strcasecmp(nm, "public") == 0;

  // group texts arrive as "<sender>: <message>"
  char from[32] = "?";
  const char* body = text;
  const char* sep = strstr(text, ": ");
  if (sep && sep - text < (int)sizeof(from)) {
    memcpy(from, text, sep - text);
    from[sep - text] = 0;
    body = sep + 2;
  }
  if (!is_public) syncClockFrom(the_mesh.uiLastChannelTimestamp(), (const uint8_t*)from);

  if (is_public) {
    // strangers' channel: only a direct mention gets a chirp, never an alarm
    if (mentionsMe(body)) { _unread = true; tone(T_DM); }
    return;
  }
  if (isPage(body)) {
    startPage(false, NULL, idx, from);
  } else if (startsWithI(body, PG_PAGE_ACK)) {
    if (_sos && mentionsMe(body + strlen(PG_PAGE_ACK))) { _sos = false; tone(T_HELP, true); gpsWant(false); }
  } else if (startsWithI(body, PG_WHERE)) {
    const char* who = body + strlen(PG_WHERE);
    while (*who == ' ') who++;
    if (!*who || mentionsMe(who)) {
      _where_dm = false;
      _where_chan = idx;
      replyWhere();
    }
  } else if (mentionsMe(body)) {
    _unread = true;
    tone(T_DM);
  }
}

void UITask::onDiscoveredContact(ContactInfo &contact, bool is_new, uint8_t path_len, const uint8_t* path) {
  syncClockFrom(contact.last_advert_timestamp, contact.id.pub_key);
}

/* ---------------- clock ---------------- */

bool UITask::clockSet() const {
  return rtc_clock.getCurrentTime() > CLOCK_FLOOR;
}

// with no app and no GPS, take the time once two different nodes agree on it
void UITask::syncClockFrom(uint32_t their_ts, const uint8_t* from) {
  if (clockSet() || their_ts < CLOCK_FLOOR) return;
  uint32_t now_ms = millis();
  if (_clk_cand_ts && memcmp(_clk_cand_from, from, 4) != 0) {
    uint32_t expect = _clk_cand_ts + (now_ms - _clk_cand_ms) / 1000;
    int32_t diff = (int32_t)(their_ts - expect);
    if (diff > -CLOCK_AGREE_SECS && diff < CLOCK_AGREE_SECS) {
      rtc_clock.setCurrentTime(their_ts > expect ? their_ts : expect);
      return;
    }
  }
  _clk_cand_ts = their_ts;
  _clk_cand_ms = now_ms;
  memcpy(_clk_cand_from, from, 4);
}

/* ---------------- loop ---------------- */

void UITask::shutdown(bool restart) {
#ifdef PIN_STATUS_LED
  digitalWrite(PIN_STATUS_LED, !LED_STATE_ON);
#endif
#ifdef PIN_BUZZER
  buzzer.quiet(false);
  buzzer.shutdown();
  uint32_t t = millis();
  while (buzzer.isPlaying() && millis() - t < 2500) buzzer.loop();
#endif
  if (restart) _board->reboot();
  else _board->powerOff();
}

void UITask::loop() {
  uint32_t now = millis();
  buttonTick();
  gpsTick();

  if (_paging && (now - _page_started) < PAGE_TONE_FOR && (long)(now - _page_next_tone) >= 0) {
    tone(T_PAGE, true);
    _page_next_tone = now + PAGE_TONE_EVERY;
  }

  if (_echo_wait && (long)(now - _echo_deadline) > 0) {
    _echo_wait = false;
    tone(T_NOHEAR, true);   // no repeater heard it go by
  }

  if (_sos) {
    if ((now - _sos_started) > SOS_LASTS_MS) {
      _sos = false;
      gpsWant(false);
    } else if (_sos_next_retry && (long)(now - _sos_next_retry) > 0 && !_echo_wait) {
      if (_sos_tries < SOS_MAX_TRIES) {
        _sos_tries++;
        char msg[96];
        if (_have_fix) snprintf(msg, sizeof(msg), "%s SOS %s%.5f,%.5f %s needs help", PG_PAGE_PREFIX, freshFix(FIX_FRESH_MS) ? "" : "~",
                                _fix_lat / 1e6, _fix_lon / 1e6, _node_prefs->node_name);
        else snprintf(msg, sizeof(msg), "%s SOS %s needs help", PG_PAGE_PREFIX, _node_prefs->node_name);
        sendToFarm(msg, true);
        _sos_next_retry = now + SOS_RETRY_MS;
      } else {
        _sos_next_retry = 0;
      }
    } else if (_sos_has_pos && (long)(now - _sos_next_update) > 0) {
      _sos_next_update = now + SOS_UPDATE_MS;
      if (freshFix(60000)) {
        char msg[96];
        snprintf(msg, sizeof(msg), "SOS position @ %.5f,%.5f", _fix_lat / 1e6, _fix_lon / 1e6);
        sendToFarm(msg);
      }
    }
  }

  if ((long)(now - _next_advert) > 0) {
    _next_advert = now + ADVERT_EVERY_MS;
    the_mesh.advertFlood();   // so the decks know we're here (no position: adverts are public)
  }

  if ((long)(now - _next_batt) > 0) {
    _next_batt = now + BATT_EVERY_MS;
    _batt_mv = _board->getBattMilliVolts();
    if (_batt_mv > LOW_BATT_REARM_MV || _board->isExternalPowered()) {
      _low_batt_sent = false;
    } else if (_batt_mv < LOW_BATT_MV && !_low_batt_sent && _batt_mv > 2500) {
      _low_batt_sent = true;
      tone(T_NOHEAR, true);
      int idx = farmChannel();
      if (idx >= 0) sendChannel(idx, "battery low - please charge me", false);
    }
  }

  ledTick();
#ifdef PIN_BUZZER
  if (buzzer.isPlaying()) buzzer.loop();
  else if (!buzzer.isQuiet()) buzzer.quiet(true);   // driver off between tunes
#endif
}
