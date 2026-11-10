// Courier ("mule"), GPS power management and the courier screen for the cyberdeck UI.
//
// THE COURIER
// The deck keeps a rolling cache of recent messages worth carrying - every channel we're
// in except #hashtags (each can be overridden), DMs between our contacts, and our own
// posts - exactly as heard: still encrypted, sender's timestamp sealed inside. That cache
// fills whether or not courier mode is on, so switching it on before a trip means the
// last messages from here are already aboard.
//
// With courier mode on, a held message is handed to any repeater in range that isn't
// known to have it. "Known to have it" = we heard that repeater carrying it, or we gave
// it to them. Repeaters are found by listening (the last hop of every flood we hear)
// and, while we're carrying anything, by a short zero-hop "who's there?" probe - every
// minute on the move, every five when parked. Repeaters answer at most 4 of those per
// 2 minutes, by design. So driving out of the farm, nothing goes out until a repeater
// that never saw the farm traffic turns up - your brother's in town, or an isolated one
// on the way - and then it gets everything, oldest first, one packet every 2 seconds,
// within a budget of `mule rate` per 10 minutes.
//
// GPS POWER
// The T-Deck has no GPS enable pin, so turning the GPS "off" never cut its power. The
// receiver is a u-blox M10: a UBX-RXM-PMREQ puts it in software backup (microamps), and
// any byte on its serial line wakes it, usually with a fix again in seconds.
//   FOLLOW  awake, tracking. Still (within 100 m) for 30 minutes -> REST.
//   REST    asleep. Every 10 minutes -> CHECK.
//   CHECK   awake up to 90 s for a fix: moved 200 m+ -> FOLLOW, else back to REST.
// The map and GPS pages keep it in FOLLOW while they're on screen; `gps live on` always.

#include "UITask.h"
#include <math.h>
#include <target.h>
#include <helpers/AdvertDataHelpers.h>

#define SCR_W       CYBERDECK_W
#define CHAR_W      8
#define ROW_H       16
#define SMALL_W     6
#define TABS_Y      17
#define BODY_Y      35
#define FOOT_Y      232
#define COLS_MAX    41

#define MULE_TICK_MS        2000
#define MULE_NEAR_MOVING    (3 * 60 * 1000UL)    // a repeater counts as "here" this long after we hear it
#define MULE_NEAR_PARKED    (8 * 60 * 1000UL)
#define MULE_PROBE_MOVING   (60 * 1000UL)
#define MULE_PROBE_PARKED   (5 * 60 * 1000UL)
#define MULE_PROBE_NOGPS    (2 * 60 * 1000UL)

#define GPS_CHECK_EVERY     (10 * 60 * 1000UL)
#define GPS_CHECK_TIMEOUT   (90 * 1000UL)
#define GPS_STILL_MS        (30 * 60 * 1000UL)
#define GPS_STILL_M         100.0
#define GPS_MOVED_M         200.0

static double geoDistM(double lat1, double lon1, double lat2, double lon2) {
  double p1 = lat1 * M_PI / 180, p2 = lat2 * M_PI / 180;
  double dp = p2 - p1, dl = (lon2 - lon1) * M_PI / 180;
  double a = sin(dp / 2) * sin(dp / 2) + cos(p1) * cos(p2) * sin(dl / 2) * sin(dl / 2);
  return 6371000.0 * 2 * atan2(sqrt(a), sqrt(1 - a));
}

static bool ieq(const char* a, const char* b) {
  while (*a && *b) { if (tolower(*a++) != tolower(*b++)) return false; }
  return *a == *b;
}

static void chanName(char* dest, size_t sz, const char* name) {
  size_t j = 0;
  if (name[0] != '#' && j < sz - 1) dest[j++] = '#';
  for (size_t i = 0; name[i] && j < sz - 1; i++) {
    unsigned char c = name[i];
    dest[j++] = (c >= 32 && c < 127) ? c : '?';
  }
  dest[j] = 0;
}

static void addVia(uint8_t* via, uint8_t& nvia, uint8_t id) {
  for (int i = 0; i < nvia; i++) if (via[i] == id) return;
  if (nvia == 12) { memmove(via, via + 1, 11); nvia = 11; }
  via[nvia++] = id;
}

/* -------------------------------------- listening -------------------------------------- */

void UITask::onFloodRecv(const mesh::Packet *pkt) {
  // only repeaters forward, so the last hop of any flood is a repeater in range
  uint8_t count = pkt->getPathHashCount();
  int hop = count > 0 ? pkt->path[(count - 1) * pkt->getPathHashSize()] : -1;
  if (hop >= 0) muleRepeaterSeen((uint8_t)hop, pkt->_snr);

  if (_mule == NULL) return;
  uint8_t t = pkt->getPayloadType();
  if (t != PAYLOAD_TYPE_TXT_MSG && t != PAYLOAD_TYPE_GRP_TXT) return;

  uint8_t hash[MAX_HASH_SIZE];
  pkt->calculatePacketHash(hash);
  MuleEntry* e = muleFind(hash);
  if (e) {   // already aboard: whoever passed it to us has it
    if (hop >= 0) addVia(e->via, e->nvia, (uint8_t)hop);
    return;
  }
  muleConsider(pkt, false, hop);
}

void UITask::onFloodSent(const mesh::Packet *pkt) {
  if (_mule == NULL || _prefs.mule_own_off) return;
  uint8_t t = pkt->getPayloadType();
  if (t == PAYLOAD_TYPE_TXT_MSG || t == PAYLOAD_TYPE_GRP_TXT) muleConsider(pkt, true, -1);
}

void UITask::onControlDataRecv(const mesh::Packet *pkt) {
  // an answer to our "who's there?": [0x90|type][snr][tag x4][key prefix]
  if (pkt->payload_len < 7) return;
  if ((pkt->payload[0] & 0xF0) != 0x90 || (pkt->payload[0] & 0x0F) != ADV_TYPE_REPEATER) return;
  uint32_t tag;
  memcpy(&tag, &pkt->payload[2], 4);
  if (tag != _mule_probe_tag) return;
  muleRepeaterSeen(pkt->payload[6], (int8_t)pkt->_snr);
}

void UITask::muleNeighbour(uint8_t hash) {
  muleRepeaterSeen(hash, 0);
}

void UITask::muleRepeaterSeen(uint8_t id, int8_t snr4) {
  int slot = 0;
  for (int i = 0; i < CD_REPEATERS; i++) {
    if (_rpt[i].last && _rpt[i].id == id) { slot = i; break; }
    if (_rpt[i].last < _rpt[slot].last) slot = i;
  }
  _rpt[slot].id = id;
  _rpt[slot].last = millis();
  _rpt[slot].snr4 = snr4;
}

static bool movingNow(LocationProvider* lp) {
#ifdef NMEA_KEEP_RECENT
  if (lp == NULL) return false;
  MicroNMEALocationProvider* gp = static_cast<MicroNMEALocationProvider*>(lp);
  return gp->fixAgeMs() < 5000 && gp->nmeaState().getSpeed() / 1000.0 * 1.852 > 5.0;
#else
  return false;
#endif
}

int UITask::muleRepeatersNear() const {
  unsigned long window = movingNow(_sensors ? _sensors->getLocationProvider() : NULL) ? MULE_NEAR_MOVING : MULE_NEAR_PARKED;
  int n = 0;
  for (int i = 0; i < CD_REPEATERS; i++) if (_rpt[i].last && millis() - _rpt[i].last < window) n++;
  return n;
}

/* ---------------------------------------- holding --------------------------------------- */

bool UITask::muleCarriesChan(int idx) const {
  if (idx < 0 || idx >= 64) return false;
  if ((_prefs.mule_chan_set >> idx) & 1) return (_prefs.mule_chan_val >> idx) & 1;
  ChannelDetails cd;
  if (!the_mesh.getChannel(idx, cd) || !cd.name[0]) return false;
  return cd.name[0] != '#';   // everything we're in except #hashtags, unless told otherwise
}

void UITask::muleConsider(const mesh::Packet* pkt, bool own, int via) {
  if (pkt->getPayloadType() == PAYLOAD_TYPE_GRP_TXT) {
    int idx = -1;
    ChannelDetails cd;
    for (int i = 0; i < MAX_GROUP_CHANNELS; i++) {
      if (the_mesh.getChannel(i, cd) && cd.name[0] && cd.channel.hash[0] == pkt->payload[0]) { idx = i; break; }
    }
    if (own) muleStore(pkt, MK_OWN, idx < 0 ? 0xFF : idx, via);
    else if (idx >= 0 && muleCarriesChan(idx)) muleStore(pkt, MK_CHAN, idx, via);
    return;
  }
  // a direct message: [dest hash][src hash][mac][ciphertext]
  if (own) { muleStore(pkt, MK_OWN, 0xFF, via); return; }
  if (_prefs.mule_dm_off || pkt->payload[0] == the_mesh.self_id.pub_key[0]) return;
  for (int i = 0; i < _num_nodes; i++) {
    if (_nodes[i].type == ADV_TYPE_CHAT && _nodes[i].pub[0] == pkt->payload[0]) {
      muleStore(pkt, MK_DM, 0xFF, via);
      return;
    }
  }
}

UITask::MuleEntry* UITask::muleFind(const uint8_t* hash) {
  if (_mule == NULL) return NULL;
  for (int i = 0; i < muleSlots(); i++) {
    if (_mule[i].len && memcmp(_mule[i].hash, hash, MAX_HASH_SIZE) == 0) return &_mule[i];
  }
  return NULL;
}

void UITask::muleStore(const mesh::Packet* pkt, MuleKind kind, uint8_t chan, int via) {
  if (_mule == NULL) return;
  uint8_t hash[MAX_HASH_SIZE];
  pkt->calculatePacketHash(hash);
  if (muleFind(hash)) return;

  int slots = muleSlots(), pick = -1;
  for (int i = 0; i < slots; i++) {
    if (_mule[i].len == 0 || millis() - _mule[i].heard > muleMaxAgeMs()) { pick = i; break; }
    if (pick < 0 || _mule[i].heard < _mule[pick].heard) pick = i;   // full: the oldest makes room
  }
  MuleEntry& e = _mule[pick];
  memset(&e, 0, sizeof(e));
  memcpy(e.hash, hash, sizeof(hash));
  e.len = pkt->writeTo(e.raw);
  e.heard = millis();
  e.kind = kind;
  e.chan = chan;
  if (via >= 0) addVia(e.via, e.nvia, (uint8_t)via);
  if (kind == MK_OWN) {
    _mule_last_own = pick;
    // our own post is in every repeater in range the moment it goes out
    unsigned long window = MULE_NEAR_PARKED;
    for (int i = 0; i < CD_REPEATERS; i++) {
      if (_rpt[i].last && millis() - _rpt[i].last < window) addVia(e.via, e.nvia, _rpt[i].id);
    }
  }
}

/* --------------------------------------- delivering ------------------------------------- */

bool UITask::muleDue(MuleEntry& e) {
  if (e.len == 0) return false;
  if (millis() - e.heard > muleMaxAgeMs()) { e.len = 0; return false; }
  unsigned long window = movingNow(_sensors ? _sensors->getLocationProvider() : NULL) ? MULE_NEAR_MOVING : MULE_NEAR_PARKED;
  for (int i = 0; i < CD_REPEATERS; i++) {
    if (!_rpt[i].last || millis() - _rpt[i].last >= window) continue;
    bool has = false;
    for (int v = 0; v < e.nvia && !has; v++) has = e.via[v] == _rpt[i].id;
    if (!has) return true;   // a repeater here that hasn't got it
  }
  return false;
}

bool UITask::muleRelay(MuleEntry& e) {
  if (!the_mesh.uiReflood(e.raw, e.len, random(50, 600))) return false;
  e.sends++;
  _mule_relayed++;
  // everyone in range just got it
  unsigned long window = movingNow(_sensors ? _sensors->getLocationProvider() : NULL) ? MULE_NEAR_MOVING : MULE_NEAR_PARKED;
  for (int i = 0; i < CD_REPEATERS; i++) {
    if (_rpt[i].last && millis() - _rpt[i].last < window) addVia(e.via, e.nvia, _rpt[i].id);
  }
  return true;
}

void UITask::muleTick() {
  if (_mule == NULL) return;
  unsigned long now = millis();

  // budget: muleRate() relays per 10 minutes
  int rate = muleRate();
  unsigned long period = 600000UL / rate;
  if (_mule_tokens < 0) { _mule_tokens = rate; _mule_tokens_at = now; }
  while (_mule_tokens < rate && now - _mule_tokens_at >= period) { _mule_tokens++; _mule_tokens_at += period; }
  if (_mule_tokens >= rate) _mule_tokens_at = now;

  if (!_prefs.mule) return;

  // anything aboard? then look for repeaters, gently
  int held = muleCount();
  if (held > 0 && (long)(now - _mule_next_probe) >= 0) {
    LocationProvider* lp = _sensors ? _sensors->getLocationProvider() : NULL;
    double lat, lon;
    unsigned long every = !myPosition(lat, lon) ? MULE_PROBE_NOGPS : movingNow(lp) ? MULE_PROBE_MOVING : MULE_PROBE_PARKED;
    uint32_t tag;
    if (the_mesh.uiDiscoverRepeaters(tag)) _mule_probe_tag = tag;
    _mule_next_probe = now + every;
  }

  if (_mule_tokens <= 0) return;
  int pick = -1;
  for (int i = 0; i < muleSlots(); i++) {
    if (muleDue(_mule[i]) && (pick < 0 || _mule[i].heard < _mule[pick].heard)) pick = i;   // oldest first
  }
  if (pick >= 0 && muleRelay(_mule[pick])) {
    _mule_tokens--;
    if (_screen == SCR_MULE) _dirty = true;
  }
}

int UITask::muleFlush() {
  if (_mule == NULL) return 0;
  int n = 0;
  for (int i = 0; i < muleSlots() && n < 64; i++) {
    MuleEntry& e = _mule[i];
    if (!e.len || millis() - e.heard > muleMaxAgeMs()) continue;
    if (the_mesh.uiReflood(e.raw, e.len, 500 + n * 2000 + random(0, 500))) {
      e.sends++;
      _mule_relayed++;
      n++;
    }
  }
  return n;
}

int UITask::muleCount(bool due_only) {
  if (_mule == NULL) return 0;
  int n = 0;
  for (int i = 0; i < muleSlots(); i++) {
    MuleEntry& e = _mule[i];
    if (!e.len || millis() - e.heard > muleMaxAgeMs()) continue;
    if (!due_only || muleDue(e)) n++;
  }
  return n;
}

void UITask::muleStatus() {
  int ch = 0, dm = 0, own = 0, n = 0;
  for (int i = 0; _mule && i < muleSlots(); i++) {
    const MuleEntry& e = _mule[i];
    if (!e.len || millis() - e.heard > muleMaxAgeMs()) continue;
    n++;
    if (e.kind == MK_DM) dm++; else if (e.kind == MK_CHAN) ch++; else own++;
  }
  consolePrint("COURIER %s // %d/%d HELD", _prefs.mule ? "ON" : "OFF", n, muleSlots());
  consolePrint("CHAN %d  DM %d  YOURS %d  DUE NOW %d", ch, dm, own, muleCount(true));
  unsigned age = _prefs.mule_age_min ? _prefs.mule_age_min : 360;
  consolePrint("SENT %u  MAX %d/10MIN  HOLD %uH%02uM", (unsigned)_mule_relayed, muleRate(), age / 60, age % 60);
  consolePrint("%d REPEATERS IN RANGE", muleRepeatersNear());
}

/* ------------------------------------------ GPS ----------------------------------------- */

void UITask::gpsSleep(bool sleep) {
#ifdef NMEA_KEEP_RECENT
  LocationProvider* lp = _sensors ? _sensors->getLocationProvider() : NULL;
  if (lp == NULL) return;
  Stream* s = static_cast<MicroNMEALocationProvider*>(lp)->serialPort();
  if (sleep) {
    // UBX-RXM-PMREQ: backup mode, no time limit, wake on UART RX
    uint8_t m[24] = { 0xB5, 0x62, 0x02, 0x41, 0x10, 0x00,
                      0x00, 0x00, 0x00, 0x00,     // version, reserved
                      0x00, 0x00, 0x00, 0x00,     // duration 0 = until woken
                      0x02, 0x00, 0x00, 0x00,     // flags: backup
                      0x08, 0x00, 0x00, 0x00,     // wakeupSources: uartrx
                      0, 0 };
    uint8_t a = 0, b = 0;
    for (int i = 2; i < 22; i++) { a += m[i]; b += a; }
    m[22] = a; m[23] = b;
    s->write(m, sizeof(m));
    s->flush();
  } else {
    // any traffic on its RX line wakes it; the first bytes are thrown away
    for (int i = 0; i < 16; i++) s->write((uint8_t)0xFF);
    s->flush();
  }
  _gps_asleep = sleep;
#endif
}

void UITask::gpsTick() {
#ifdef NMEA_KEEP_RECENT
  LocationProvider* lp = _sensors ? _sensors->getLocationProvider() : NULL;
  if (lp == NULL) return;
  unsigned long now = millis();

  if (!getGPSState()) {   // switched off: actually power it down this time
    if (_gps_mode != GM_OFF) { gpsSleep(true); _gps_mode = GM_OFF; }
    return;
  }
  MicroNMEALocationProvider* gp = static_cast<MicroNMEALocationProvider*>(lp);
  bool want_live = _prefs.gps_always || _screen == SCR_GPS || (_screen == SCR_LIST && _tab == TAB_MAP && _display && _display->isOn());
  double lat = 0, lon = 0;
  bool fresh = gp->fixAgeMs() < 2000 && myPosition(lat, lon);

  switch (_gps_mode) {
    case GM_OFF:
      gpsSleep(false);
      _gps_mode = GM_FOLLOW;
      _gps_anchor_at = now;
      break;
    case GM_FOLLOW:
      if (fresh && ((_gps_anchor_lat == 0 && _gps_anchor_lon == 0) || geoDistM(lat, lon, _gps_anchor_lat, _gps_anchor_lon) > GPS_STILL_M)) {
        _gps_anchor_lat = lat; _gps_anchor_lon = lon;
        _gps_anchor_at = now;
      }
      if (!want_live && now - _gps_anchor_at > GPS_STILL_MS) {   // still (or no sky) for half an hour
        gpsSleep(true);
        _gps_mode = GM_REST;
        _gps_next = now + GPS_CHECK_EVERY;
      }
      break;
    case GM_REST:
      if (want_live) { gpsSleep(false); _gps_mode = GM_FOLLOW; _gps_anchor_at = now; }
      else if ((long)(now - _gps_next) >= 0) { gpsSleep(false); _gps_mode = GM_CHECK; _gps_since = now; }
      break;
    case GM_CHECK:
      if (want_live) { _gps_mode = GM_FOLLOW; _gps_anchor_at = now; break; }
      if (fresh && now - _gps_since > 3000) {
        bool moved = (_gps_anchor_lat == 0 && _gps_anchor_lon == 0) || geoDistM(lat, lon, _gps_anchor_lat, _gps_anchor_lon) > GPS_MOVED_M;
        if (moved) {
          _gps_mode = GM_FOLLOW;
          _gps_anchor_lat = lat; _gps_anchor_lon = lon;
          _gps_anchor_at = now;
        } else {
          gpsSleep(true);
          _gps_mode = GM_REST;
          _gps_next = now + GPS_CHECK_EVERY;
        }
      } else if (now - _gps_since > GPS_CHECK_TIMEOUT) {   // no fix in time: try again later
        gpsSleep(true);
        _gps_mode = GM_REST;
        _gps_next = now + GPS_CHECK_EVERY;
      }
      break;
  }
#endif
}

void UITask::powerTick() {
  unsigned long now = millis();
  // the CPU idles at a third of the speed while nobody's looking
  bool want_low = !_prefs.cpu_full && _display && !_display->isOn() && _screen != SCR_PAGE;
  if (want_low != _cpu_low) {
    setCpuFrequencyMhz(want_low ? 80 : 240);
    _cpu_low = want_low;
  }
  // battery every 5 minutes, for the drain estimate on `power`
  if ((long)(now - _batt_next) >= 0) {
    _batt_next = now + 5 * 60 * 1000UL;
    if (_batt_n == 24) { memmove(_batt_hist, _batt_hist + 1, sizeof(_batt_hist[0]) * 23); _batt_n = 23; }
    _batt_hist[_batt_n++] = getBattMilliVolts();
  }
}

/* -------------------------------------- the screen -------------------------------------- */

void UITask::drawMule() {
  Adafruit_GFX& g = _display->gfx();
  int held = muleCount(), due = muleCount(true);
  textf(0, TABS_Y, _prefs.mule ? c_hi : c_alert, "COURIER // %s", _prefs.mule ? "ON" : "OFF - HOLDING ONLY");
  g.drawFastHLine(0, TABS_Y + ROW_H + 1, SCR_W, c_dim);

  char status[64];
  snprintf(status, sizeof(status), "HELD %d/%d  DUE %d  SENT %u  REPEATERS HERE %d", held, muleSlots(), due,
           (unsigned)_mule_relayed, muleRepeatersNear());
  stext(0, BODY_Y, c_fg, status);

  // newest first
  int order[CD_MULE_SLOTS];
  int n = 0;
  for (int i = 0; _mule && i < muleSlots(); i++) {
    if (_mule[i].len && millis() - _mule[i].heard <= muleMaxAgeMs()) order[n++] = i;
  }
  for (int a = 1; a < n; a++) {
    int v = order[a], b = a - 1;
    while (b >= 0 && _mule[order[b]].heard < _mule[v].heard) { order[b + 1] = order[b]; b--; }
    order[b + 1] = v;
  }
  const int rows = 11;
  if (_mule_scroll > n - rows) _mule_scroll = n > rows ? n - rows : 0;
  if (_mule_scroll < 0) _mule_scroll = 0;
  if (n == 0) {
    text(4, BODY_Y + 2 * ROW_H, c_dim, "NOTHING ABOARD YET.");
    text(4, BODY_Y + 3 * ROW_H, c_dim, "MESSAGES HEARD HERE ARE COLLECTED");
    text(4, BODY_Y + 4 * ROW_H, c_dim, "AUTOMATICALLY. mule on TO DELIVER.");
  }
  int y = BODY_Y + 11;
  for (int r = 0; r < rows && _mule_scroll + r < n; r++, y += ROW_H) {
    MuleEntry& e = _mule[order[_mule_scroll + r]];
    char where[16] = "DM", when[8], line[COLS_MAX];
    ChannelDetails cd;
    if (e.chan != 0xFF && the_mesh.getChannel(e.chan, cd)) chanName(where, sizeof(where), cd.name);
    else if (e.kind != MK_OWN) {
      mesh::Packet p;
      if (p.readFrom(e.raw, e.len)) snprintf(where, sizeof(where), "DM>%02X", p.payload[0]);
    }
    if (e.sent_ts) fmtTime(when, e.sent_ts);
    else { unsigned m = (millis() - e.heard) / 60000; snprintf(when, sizeof(when), "-%um", m); }
    if (e.text[0]) snprintf(line, sizeof(line), "%-8.8s %s %s: %s", where, when, e.from, e.text);
    else snprintf(line, sizeof(line), "%-8.8s %s %s", where, when, e.kind == MK_DM ? "(sealed)" : "");
    bool due_now = _prefs.mule && muleDue(e);
    text(0, y, due_now ? c_hi : (e.kind == MK_DM ? c_dim : c_fg), line);
    char tag[8];
    snprintf(tag, sizeof(tag), "%s%u", e.kind == MK_OWN ? "*" : "", e.sends);
    g.fillRect(SCR_W - strlen(tag) * SMALL_W - 3, y, strlen(tag) * SMALL_W + 3, ROW_H, c_bg);
    stext(SCR_W - strlen(tag) * SMALL_W - 1, y + 5, e.sends ? c_hi : c_dim, tag);
  }
  drawFooter("\x18\x19 SCROLL  O ON/OFF  F SEND ALL NOW  X EMPTY  ESC BACK");
}
