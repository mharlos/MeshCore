#include "UITask.h"
#include <SPIFFS.h>
#include <SD.h>
#include <dirent.h>
#include <math.h>
#include <stddef.h>
#include <stdarg.h>
#include <esp_system.h>
#include <helpers/TxtDataHelpers.h>
#include <helpers/AdvertDataHelpers.h>
#include <Utils.h>
#include <target.h>
#include "font_spleen8x16.h"

#ifndef CD_DEFAULT_DIM_SECS
  #define CD_DEFAULT_DIM_SECS  90
#endif
#define CD_PREFS_FILE   "/cdeck"
#define CD_PREFS_MAGIC  0xD2          // bump when Prefs changes shape
#define CD_HIST_FILE    "/cdhist"
#define CD_HIST_MAGIC   0x43444832    // "CDH2"

#ifdef RTC_TIME_MIN
  #define CLOCK_FLOOR   RTC_TIME_MIN  // what the clock reads after a cold boot
#else
  #define CLOCK_FLOOR   1772323200UL
#endif

// Two fonts: Spleen 8x16 for everything you read, the built-in 6x8 for the status
// bar and key hints.
#define SCR_W       CYBERDECK_W
#define SCR_H       CYBERDECK_H
#define CHAR_W      8
#define ROW_H       16
#define COLS        (SCR_W / CHAR_W)    // 40
#define SMALL_W     6
#define TABS_Y      17
#define BODY_Y      35
#define BODY_ROWS   11
#define INPUT_Y     214
#define FOOT_Y      232

#define TKEY_CLICK   0x85
#define TKEY_HOLD    0x86

#define SEND_ATTEMPTS  3

#define BEEP_DM      1
#define BEEP_CHAN    2
#define BEEP_KEYS    4

#define BOOT_MILLIS        5200
#define PAGE_TONE_EVERY    2500
#define PAGE_TONE_FOR      (10 * 60 * 1000UL)   // stop the alarm (not the screen) after this
#define MULE_NEIGH_STALE   (30 * 60 * 1000UL)
#define CLOCK_AGREE_SECS   600

#define RGB(r, g, b)  (uint16_t)((((r) & 0xF8) << 8) | (((g) & 0xFC) << 3) | ((b) >> 3))

// CP437 glyphs
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

struct RadioPreset { const char* name; float freq, bw; uint8_t sf, cr; };
// from docs/radio_presets.md, docs/faq.md and docs/cli_commands.md
static const RadioPreset PRESETS[] = {
  { "usa",       910.525f, 62.5f, 7, 5 },
  { "eu-narrow", 869.618f, 62.5f, 8, 5 },
  { "au-narrow", 916.575f, 62.5f, 7, 7 },
  { "nz-narrow", 917.375f, 62.5f, 7, 5 },
  { "pt-868",    869.618f, 62.5f, 7, 6 },
};
#define NUM_PRESETS  (sizeof(PRESETS) / sizeof(PRESETS[0]))

static const char* TAB_NAMES[] = { "CHAN", "NODES", "HEARD", "MAP", "RF", "SYS" };

static AdvertPath heard[16];
static int num_heard = 0;

// copy text for the CP437 fonts: ASCII as-is, each UTF-8 sequence as one small block
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
    case ADV_TYPE_ROOM:     return "RMS";
    case ADV_TYPE_SENSOR:   return "SNS";
    default:                return "???";
  }
}

static const char* payloadName(uint8_t t) {
  static const char* names[] = { "REQ", "RESP", "TXT", "ACK", "ADVERT", "GRPTXT", "GRPDAT", "ANON",
                                 "PATH", "TRACE", "MULTI", "CTRL", "T0C", "T0D", "T0E", "RAW" };
  return names[t & 0x0F];
}

// "#name" for channels; hashtag channels already carry their '#'
static void chanLabel(char* dest, size_t sz, const char* name) {
  char clean[32];
  sanitize(clean, sizeof(clean), name);
  snprintf(dest, sz, clean[0] == '#' ? "%s" : "#%s", clean);
}

static bool strieq(const char* a, const char* b) {
  while (*a && *b) { if (tolower(*a++) != tolower(*b++)) return false; }
  return *a == *b;
}

static bool stristr(const char* hay, const char* needle) {
  size_t n = strlen(needle);
  if (n == 0) return false;
  for (; *hay; hay++) {
    size_t i = 0;
    while (i < n && hay[i] && tolower(hay[i]) == tolower(needle[i])) i++;
    if (i == n) return true;
  }
  return false;
}

static void toHex(char* dest, const uint8_t* src, int len) {
  for (int i = 0; i < len; i++) sprintf(&dest[i * 2], "%02x", src[i]);
}

// days since 1970-01-01 <-> civil date (Howard Hinnant's algorithms)
static int32_t daysFromCivil(int y, unsigned m, unsigned d) {
  y -= m <= 2;
  int era = (y >= 0 ? y : y - 399) / 400;
  unsigned yoe = (unsigned)(y - era * 400);
  unsigned doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
  unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
  return era * 146097 + (int32_t)doe - 719468;
}

static void civilFromDays(int32_t z, int& y, unsigned& m, unsigned& d) {
  z += 719468;
  int era = (z >= 0 ? z : z - 146096) / 146097;
  unsigned doe = (unsigned)(z - era * 146097);
  unsigned yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
  y = (int)yoe + era * 400;
  unsigned doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
  unsigned mp = (5 * doy + 2) / 153;
  d = doy - (153 * mp + 2) / 5 + 1;
  m = mp < 10 ? mp + 3 : mp - 9;
  y += (m <= 2);
}

UITask::UITask(mesh::MainBoard* board, MultiSerialInterface* serial) : AbstractUITask(board, serial) {
  _display = NULL;
  _sensors = NULL;
  _node_prefs = NULL;
  _msgs = NULL;
  _max_msgs = 0;
  _nodes = NULL;
  _rf = NULL;
  _mule = NULL;
  _num_nodes = _num_chans = 0;
  _next_seq = 1;
  _hist_ok = false;
  _sd_ok = false;
  memset(_pending, 0, sizeof(_pending));
  memset(_sel, 0, sizeof(_sel));
  memset(_neigh, 0, sizeof(_neigh));
  memset(_logged_in, 0, sizeof(_logged_in));
  memset(_login_pending, 0, sizeof(_login_pending));
  memset(&_status, 0, sizeof(_status));
  _num_logged_in = 0;
  _login_timeout = _status_timeout = 0;
  _rf_head = _rf_count = 0;
  _rf_total = 0;
  _mule_next_tick = _mule_next_probe = _mule_tokens_at = 0;
  _mule_tokens = -1;
  _mule_relayed = _mule_probe_tag = 0;
  _mule_last_own = -1;
  _mule_scroll = 0;
  memset(_rpt, 0, sizeof(_rpt));
  _gps_mode = GM_OFF;
  _gps_next = _gps_since = _gps_anchor_at = 0;
  _gps_anchor_lat = _gps_anchor_lon = 0;
  _gps_asleep = _cpu_low = false;
  _batt_n = 0;
  _batt_next = 0;
  _chat_sel = -1;
  _chat_sel_seq = 0;
  _num_admins = 0;
  _acon_len = _acon_scroll = _acmd_len = 0;
  _acmd[0] = 0;
  _aq_sel = 0;
  _aq_confirm = false;
  _admin_wait_until = 0;
  _screen = _page_return = SCR_BOOT;
  _tab = TAB_CHAN;
  _scroll = 0;
  _conv_kind = CONV_NONE;
  _conv_chan = 0;
  memset(_conv_peer, 0, sizeof(_conv_peer));
  _conv_name[0] = 0;
  _node_mode = NM_VIEW;
  _input_len = _cmd_len = _console_len = _console_scroll = 0;
  _input_buf[0] = _cmd_buf[0] = _last_cmd[0] = 0;
  _alert[0] = 0;
  _msgcount = 0;
  _dirty = true;
  _cursor_on = true;
  _nodes_dirty = true;
  _clock_src = CLK_NONE;
  _clk_cand_ts = _clk_cand_ms = _clk_last_rtc = _clk_last_ms = 0;
  memset(_clk_cand_from, 0, sizeof(_clk_cand_from));
  _page_seq = 0;
  _page_next_tone = _page_started = 0;
  memset(_tiles, 0, sizeof(_tiles));
  _tile_clock = 0;
  _png_buf = NULL;
  _png = NULL;
  _jpeg = NULL;
  _map_zmin = _map_zmax = -1;
  _map_z = 12;
  _map_lat = _map_lon = _home_lat = _home_lon = 0;
  _map_follow = true;
  _map_pan = _map_inited = false;
  _map_pan_at = 0;
  _map_node_sel = -1;
  _crumbs = NULL;
  _crumb_head = _crumb_count = 0;
  _crumb_check = 0;
  _gps_fix_seen = false;
  _kbl_flash = false;
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
  _rf = (RfEntry*) ps_calloc(CD_RF_LOG, sizeof(RfEntry));
  _mule = (MuleEntry*) ps_calloc(CD_MULE_SLOTS, sizeof(MuleEntry));

  user_btn.begin();
  _input.begin();
  sdBegin();
  loadCrumbs();
  loadPrefs();
  _input.setSensitivity(_prefs.ball);
  _audio.begin();
  _audio.setVolume(_prefs.volume);
  if (_prefs.kbl) _input.setBacklight(_prefs.kbl);
  applyTheme();
  if (_prefs.history) loadHistory();
  refreshChannels();
  refreshNodes();

  // a warm reboot restores the time from RTC memory; a cold boot starts at the floor
  _clk_last_rtc = rtc_clock.getCurrentTime();
  _clk_last_ms = millis();
  if (_clk_last_rtc > CLOCK_FLOOR + 86400) _clock_src = CLK_SET;

  _boot_started = millis();
  _auto_off = millis() + _prefs.dim_secs * 1000UL;
  _screen = SCR_BOOT;
  _dirty = true;
  if (_display) _display->turnOn();
  beep(Tones::BOOT);
}

/* --------------------------------------- SD card --------------------------------------- */
// The card shares the display/LoRa bus (SCK 40, MISO 38, MOSI 41); only its select differs.

#ifndef PIN_SD_CS
  #define PIN_SD_CS  39
#endif
static SPIClass sd_spi(HSPI);

bool UITask::sdBegin() {
  pinMode(PIN_SD_CS, OUTPUT);
  digitalWrite(PIN_SD_CS, HIGH);
  pinMode(PIN_TFT_CS, OUTPUT);
  digitalWrite(PIN_TFT_CS, HIGH);   // keep the others off the bus while the card wakes
  pinMode(P_LORA_NSS, OUTPUT);
  digitalWrite(P_LORA_NSS, HIGH);
  sd_spi.begin(P_LORA_SCLK, P_LORA_MISO, P_LORA_MOSI);
  _sd_ok = SD.begin(PIN_SD_CS, sd_spi, 20000000) && SD.cardType() != CARD_NONE;   // 40MHz is no faster over USB
  return _sd_ok;
}

#ifdef CD_SERIAL_BRIDGE
// Summarise a directory tree: files per folder, sizes, a few names. Folders with many
// sub-folders (map tiles: z/x/y) only have a couple of them opened, to keep it short.
static void sdWalk(const char* path, int depth, int& lines) {
  if (lines > 400) return;
  File dir = SD.open(path);
  if (!dir || !dir.isDirectory()) return;

  int files = 0, dirs = 0;
  uint64_t bytes = 0;
  char samples[3][40];
  int nsamp = 0;
  char subs[3][96];
  int nsub = 0;
  char last_sub[96] = "";
  for (File f = dir.openNextFile(); f; f = dir.openNextFile()) {
    const char* name = f.name();
    const char* base = strrchr(name, '/') ? strrchr(name, '/') + 1 : name;
    if (f.isDirectory()) {
      dirs++;
      char sub[96];
      snprintf(sub, sizeof(sub), "%s%s%s", path, strcmp(path, "/") == 0 ? "" : "/", base);
      if (nsub < 2) strcpy(subs[nsub++], sub);
      else strcpy(last_sub, sub);
    } else {
      files++;
      bytes += f.size();
      if (nsamp < 3) snprintf(samples[nsamp++], sizeof(samples[0]), "%s(%u)", base, (unsigned)f.size());
    }
    f.close();
    if (files + dirs > 20000) break;
  }
  dir.close();

  Serial.printf("%*s%s  dirs=%d files=%d size=%lluKB", depth * 2, "", path, dirs, files, (unsigned long long)(bytes / 1024));
  for (int i = 0; i < nsamp; i++) Serial.printf(" %s", samples[i]);
  Serial.println();
  lines++;

  if (depth >= 6) return;
  for (int i = 0; i < nsub; i++) sdWalk(subs[i], depth + 1, lines);
  if (dirs > 2 && last_sub[0]) {
    if (dirs > 3) { Serial.printf("%*s... %d more dirs ...\n", (depth + 1) * 2, "", dirs - 3); lines++; }
    sdWalk(last_sub, depth + 1, lines);
  }
}

// zlib-compatible CRC32, so the PC can check each file with zlib.crc32()
static uint32_t crc32Update(uint32_t crc, const uint8_t* p, size_t n) {
  static uint32_t table[256];
  static bool ready = false;
  if (!ready) {
    for (uint32_t i = 0; i < 256; i++) {
      uint32_t c = i;
      for (int k = 0; k < 8; k++) c = (c & 1) ? 0xEDB88320 ^ (c >> 1) : c >> 1;
      table[i] = c;
    }
    ready = true;
  }
  crc = ~crc;
  while (n--) crc = table[(crc ^ *p++) & 0xFF] ^ (crc >> 8);
  return ~crc;
}

static String bridgeLine() {
  Serial.setTimeout(2000);
  String s = Serial.readStringUntil('\n');
  s.trim();
  return s;
}

static void bridgeWrite(const uint8_t* p, size_t n) {
  while (n > 0) {
    size_t w = Serial.write(p, n);
    p += w;
    n -= w;
    if (w == 0) delay(1);
  }
}

// "#LS path" then "D name" / "F name" lines, then "#END count". Reads the directory
// entries directly: File::openNextFile() opens every file, ~25ms each on a big folder.
static void sdList(const String& path) {
  String vfs = String("/sd") + path;   // where the SD library mounts the card
  DIR* dir = opendir(vfs.c_str());
  if (dir == NULL) { Serial.printf("\n#ERR %s\n", path.c_str()); return; }
  Serial.printf("\n#LS %s\n", path.c_str());
  int n = 0;
  for (struct dirent* e = readdir(dir); e; e = readdir(dir)) {
    if (e->d_name[0] == '.') continue;
    Serial.printf("%c %s\n", e->d_type == DT_DIR ? 'D' : 'F', e->d_name);
    if ((++n & 63) == 0) yield();
  }
  closedir(dir);
  Serial.printf("#END %d\n", n);
}

// every file directly in a folder: "#F name size\n" + bytes + "\n#C crc32hex\n" ... "#END count"
static void sdSendDir(const String& path) {
  File dir = SD.open(path);
  if (!dir || !dir.isDirectory()) { Serial.printf("\n#ERR %s\n", path.c_str()); return; }
  static uint8_t buf[4096];
  Serial.printf("\n#DIR %s\n", path.c_str());
  int n = 0;
  for (File f = dir.openNextFile(); f; f = dir.openNextFile()) {
    if (f.isDirectory()) { f.close(); continue; }
    const char* base = strrchr(f.name(), '/') ? strrchr(f.name(), '/') + 1 : f.name();
    uint32_t size = f.size(), left = size, crc = 0;
    Serial.printf("#F %s %u\n", base, (unsigned)size);
    while (left > 0) {
      int got = f.read(buf, left < sizeof(buf) ? left : sizeof(buf));
      if (got <= 0) { memset(buf, 0, sizeof(buf)); got = left < sizeof(buf) ? left : sizeof(buf); }   // keep the framing; the CRC will flag it
      crc = crc32Update(crc, buf, got);
      bridgeWrite(buf, got);
      left -= got;
    }
    f.close();
    Serial.printf("\n#C %08x\n", (unsigned)crc);
    n++;
    yield();
  }
  dir.close();
  Serial.printf("#END %d\n", n);
}
#endif

/* ---------------------------------------- prefs ---------------------------------------- */

void UITask::loadPrefs() {
  memset(&_prefs, 0, sizeof(_prefs));
  _prefs.magic = CD_PREFS_MAGIC;
  _prefs.theme = 0;
  _prefs.crt = 0;
  _prefs.dim_secs = CD_DEFAULT_DIM_SECS;
  _prefs.ball = 3;
  _prefs.beep = BEEP_DM | BEEP_CHAN;
  _prefs.volume = 6;
  _prefs.kbl = 0;
  _prefs.mule = 0;
  _prefs.history = 1;

  File f = SPIFFS.open(CD_PREFS_FILE, "r");
  if (f) {
    Prefs p;
    memset(&p, 0, sizeof(p));
    // fields only get added at the end, so an older (shorter) file is fine: the rest stays 0 = default
    size_t got = f.read((uint8_t*)&p, sizeof(p));
    if (got >= offsetof(Prefs, mule_chan_set) && p.magic == CD_PREFS_MAGIC) {
      _prefs = p;
      if (_prefs.theme >= NUM_THEMES) _prefs.theme = 0;
      if (_prefs.ball < 1 || _prefs.ball > 5) _prefs.ball = 3;
      if (_prefs.volume > 10) _prefs.volume = 6;
      for (int i = 0; i < CD_KEYWORDS; i++) _prefs.keywords[i][sizeof(_prefs.keywords[i]) - 1] = 0;
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

void UITask::beep(const Tone* t) {
  _audio.play(t);
}

/* --------------------------------------- history --------------------------------------- */
// A fixed-size file of message slots: slot (seq % max) holds message seq, just like RAM.

struct HistHeader { uint32_t magic; uint16_t msg_size; uint16_t slots; };

void UITask::loadHistory() {
  _hist_ok = false;
  if (_max_msgs == 0) return;
  HistHeader want = { CD_HIST_MAGIC, (uint16_t)sizeof(Msg), (uint16_t)_max_msgs };

  File f = SPIFFS.open(CD_HIST_FILE, "r");
  if (f) {
    HistHeader h;
    bool ok = f.read((uint8_t*)&h, sizeof(h)) == sizeof(h) && memcmp(&h, &want, sizeof(h)) == 0;
    if (ok) {
      uint32_t max_seq = 0;
      for (int i = 0; i < _max_msgs; i++) {
        Msg m;
        if (f.read((uint8_t*)&m, sizeof(m)) != sizeof(m)) break;
        if (m.seq == 0 || m.kind < CONV_CHANNEL || m.kind > CONV_DM || (int)(m.seq % _max_msgs) != i) continue;
        m.from[sizeof(m.from) - 1] = 0;
        m.text[sizeof(m.text) - 1] = 0;
        if (m.status == ST_SENDING) m.status = ST_FAILED;   // the retries died with the reboot
        _msgs[i] = m;
        if (m.seq > max_seq) max_seq = m.seq;
      }
      _next_seq = max_seq + 1;
      f.close();
      _hist_ok = true;
      return;
    }
    f.close();
  }
  wipeHistory();
}

void UITask::wipeHistory() {
  HistHeader h = { CD_HIST_MAGIC, (uint16_t)sizeof(Msg), (uint16_t)_max_msgs };
  File f = SPIFFS.open(CD_HIST_FILE, "w");
  if (!f) { _hist_ok = false; return; }
  f.write((const uint8_t*)&h, sizeof(h));
  static uint8_t zeros[512];
  size_t left = (size_t)_max_msgs * sizeof(Msg);
  while (left > 0) {
    size_t n = left < sizeof(zeros) ? left : sizeof(zeros);
    f.write(zeros, n);
    left -= n;
  }
  f.close();
  _hist_ok = true;
}

void UITask::saveMsg(const Msg* m) {
  if (!_prefs.history || !_hist_ok || m == NULL || _max_msgs == 0) return;
  File f = SPIFFS.open(CD_HIST_FILE, "r+");
  if (!f) return;
  f.seek(sizeof(HistHeader) + (size_t)(m - _msgs) * sizeof(Msg));
  f.write((const uint8_t*)m, sizeof(Msg));
  f.close();
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

const UITask::Node* UITask::findNode(const uint8_t* pub) const {
  for (int i = 0; i < _num_nodes; i++) {
    if (memcmp(_nodes[i].pub, pub, 6) == 0) return &_nodes[i];
  }
  return NULL;
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

// Show when it was written, not when it reached us - a carried message can be hours old.
// rtt_ds is free on incoming messages: it holds how many minutes late it arrived.
void UITask::stampSent(Msg* m, uint32_t sender_ts, bool mark_late) {
  if (m == NULL || !clockValid() || sender_ts == 0) return;
  uint32_t now = rtc_clock.getCurrentTime();
  if (sender_ts > now + 300 || sender_ts + 7 * 86400UL < now) return;   // their clock is off: keep ours
  m->ts = sender_ts;
  if (mark_late && now > sender_ts + 300) {
    uint32_t late = (now - sender_ts) / 60;
    m->rtt_ds = late > 65535 ? 65535 : late;
  }
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
  File f;
  for (int i = 0; i < _max_msgs; i++) {
    if (_msgs[i].unread && msgInConv(_msgs[i], kind, chan, peer)) {
      _msgs[i].unread = 0;
      if (_prefs.history && _hist_ok) {
        if (!f) f = SPIFFS.open(CD_HIST_FILE, "r+");
        if (f) {
          f.seek(sizeof(HistHeader) + (size_t)i * sizeof(Msg));
          f.write((const uint8_t*)&_msgs[i], sizeof(Msg));
        }
      }
    }
  }
  if (f) f.close();
}

/* --------------------------------------- sending --------------------------------------- */

bool UITask::sendText(const char* text, uint8_t flags) {
  int len = strlen(text);
  if (len == 0) return false;

  if (_conv_kind == CONV_CHANNEL) {
    ChannelDetails cd;
    if (!the_mesh.getChannel(_conv_chan, cd) || !cd.name[0]) {
      showAlert("CHANNEL GONE", 2000);
      return false;
    }
    uint32_t ts = the_mesh.getRTCClock()->getCurrentTimeUnique();
    Msg* m = addMsg(CONV_CHANNEL, _conv_chan, NULL, _node_prefs->node_name, text, true);
    bool ok = the_mesh.sendGroupMessage(ts, cd.channel, _node_prefs->node_name, text, len);
    if (_mule_last_own >= 0 && _mule) {   // the courier took a copy as it went out
      MuleEntry& o = _mule[_mule_last_own];
      strncpy(o.from, _node_prefs->node_name, sizeof(o.from) - 1);
      strncpy(o.text, text, sizeof(o.text) - 1);
      o.sent_ts = ts;
      _mule_last_own = -1;
    }
    if (m) {
      m->status = ok ? ST_SENT : ST_FAILED;
      m->flags = flags;
      saveMsg(m);
    }
    if (!ok) showAlert("TX FAILED: QUEUE FULL", 2500);
    _dirty = true;
    return ok;
  }
  if (_conv_kind == CONV_DM) {
    Msg* m = addMsg(CONV_DM, 0, _conv_peer, _node_prefs->node_name, text, true);
    if (m == NULL) return false;
    m->flags = flags;

    int slot = -1;
    for (int i = 0; i < CD_PENDING_SENDS; i++) {
      if (_pending[i].seq == 0) { slot = i; break; }
    }
    bool ok = false;
    if (slot < 0) {
      m->status = ST_FAILED;
      showAlert("TOO MANY UNACKED MSGS", 2500);
    } else {
      _pending[slot].seq = m->seq;
      _pending[slot].msg_ts = the_mesh.getRTCClock()->getCurrentTimeUnique();
      _pending[slot].first_sent = millis();
      m->status = ST_SENDING;
      ok = sendDirect(slot, 0);
      if (_mule_last_own >= 0 && _mule) {   // it flooded, and the courier took a copy
        MuleEntry& o = _mule[_mule_last_own];
        snprintf(o.from, sizeof(o.from), ">%.14s", _conv_name);
        strncpy(o.text, text, sizeof(o.text) - 1);
        o.sent_ts = _pending[slot].msg_ts;
        _mule_last_own = -1;
      }
      if (!ok) {
        m->status = ST_FAILED;
        _pending[slot].seq = 0;
        showAlert("TX FAILED", 2500);
      }
    }
    saveMsg(m);
    _dirty = true;
    return ok;
  }
  return false;
}

void UITask::sendInput() {
  if (_input_len == 0) return;
  _input_buf[_input_len] = 0;

  if (_input_buf[0] == '/') {   // commands work from any chat too
    _console_len = 0;
    _console_scroll = 0;
    runCommand(&_input_buf[1]);
    if (_screen == SCR_CHAT) showAlert(_console_len > 0 ? _console[0] : "OK", 3000);
  } else {
    sendText(_input_buf);
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
    saveMsg(m);
    beep(Tones::FAILED);
    _dirty = true;
  }

  if (_login_pending[0] && (long)(millis() - _login_timeout) >= 0) {
    memset(_login_pending, 0, sizeof(_login_pending));
    showAlert("LOGIN TIMED OUT", 2500);
  }
  if (_status_timeout && (long)(millis() - _status_timeout) >= 0) {
    _status_timeout = 0;
    if (!_status.valid) showAlert("NO STATUS REPLY", 2500);
  }
}

void UITask::onACKRecv(uint32_t ack_crc) {
  for (int i = 0; i < CD_PENDING_SENDS; i++) {
    if (_pending[i].seq && _pending[i].ack == ack_crc) {
      Msg* m = findMsg(_pending[i].seq);
      if (m) {
        m->status = ST_DELIVERED;
        unsigned long rtt = (millis() - _pending[i].first_sent) / 100;
        m->rtt_ds = rtt > 65535 ? 65535 : rtt;
        saveMsg(m);
      }
      _pending[i].seq = 0;
      if (_prefs.beep & BEEP_DM) beep(Tones::DELIVERED);
      _dirty = true;
    }
  }
}

/* --------------------------------------- receiving ------------------------------------- */

bool UITask::matchesKeyword(const char* text) const {
  for (int i = 0; i < CD_KEYWORDS; i++) {
    if (_prefs.keywords[i][0] && stristr(text, _prefs.keywords[i])) return true;
  }
  char at[34];
  snprintf(at, sizeof(at), "@%s", _node_prefs->node_name);   // a mention always counts
  if (stristr(text, at)) return true;
  snprintf(at, sizeof(at), "@[%s]", _node_prefs->node_name);  // ...in the phone app's form too
  return stristr(text, at);
}

void UITask::onMessageRecv(mesh::Packet *pkt, const ContactInfo &from, uint8_t txt_type, uint32_t sender_timestamp, const char* text) {
  if (txt_type == TXT_TYPE_CLI_DATA) {   // a remote node answering our admin console
    if (isAdminOf(from.id.pub_key)) {
      if (memcmp(_conv_peer, from.id.pub_key, 6) != 0) {   // a reply from a node we've since left
        memcpy(_conv_peer, from.id.pub_key, 6);
        sanitize(_conv_name, sizeof(_conv_name), from.name);
      }
      adminReply(text);
    }
    return;
  }
  if (!(txt_type == TXT_TYPE_PLAIN || txt_type == TXT_TYPE_SIGNED_PLAIN)) return;   // other CLI traffic

  // a room post is signed by its author; say who, not just which room
  char author[32];
  bool room = txt_type == TXT_TYPE_SIGNED_PLAIN;
  if (room) {
    const uint8_t* pfx = the_mesh.uiLastSignedPrefix();
    ContactInfo* a = the_mesh.lookupContactByPubKey(pfx, 4);
    if (a) sanitize(author, sizeof(author), a->name);
    else snprintf(author, sizeof(author), "%02X%02X%02X%02X", pfx[0], pfx[1], pfx[2], pfx[3]);
  } else {
    syncClockFrom(sender_timestamp, from.id.pub_key);
  }

  Msg* m = addMsg(CONV_DM, 0, from.id.pub_key, room ? author : from.name, text, false);
  if (m == NULL) return;
  m->hops = pkt->isRouteFlood() ? pkt->path_len : 0xFF;
  m->snr4 = (int8_t)(pkt->getSNR() * 4);
  if (room) m->flags |= MF_ROOM;
  stampSent(m, sender_timestamp, !room);   // room servers replay history on purpose: not "late"

  char name[32];
  sanitize(name, sizeof(name), from.name);
  bool is_page = !room && strncmp(text, CD_PAGE_PREFIX, strlen(CD_PAGE_PREFIX)) == 0 && strncmp(text, CD_PAGE_ACK, strlen(CD_PAGE_ACK)) != 0;
  bool is_page_ack = !room && strncmp(text, CD_PAGE_ACK, strlen(CD_PAGE_ACK)) == 0;

  bool viewing = _screen == SCR_CHAT && _conv_kind == CONV_DM && memcmp(_conv_peer, from.id.pub_key, sizeof(_conv_peer)) == 0
                 && _display && _display->isOn();
  m->unread = !viewing;

  if (is_page) {
    m->flags |= MF_PAGE;
    saveMsg(m);
    startPage(m);
    return;
  }
  if (is_page_ack) {
    // mark our most recent page to them as acknowledged
    for (uint32_t s = _next_seq - 1; s > 0 && s + _max_msgs >= _next_seq; s--) {
      Msg* p = findMsg(s);
      if (p && p->outgoing && (p->flags & MF_PAGE) && p->kind == CONV_DM && memcmp(p->peer, from.id.pub_key, 6) == 0) {
        p->flags |= MF_ACKED;
        saveMsg(p);
        break;
      }
    }
    char alert[54];
    snprintf(alert, sizeof(alert), "PAGE ACK // %s", name);
    showAlert(alert, 6000);
    beep(Tones::DELIVERED);
  } else if (!viewing) {
    char alert[54];
    snprintf(alert, sizeof(alert), room ? "ROOM // %s // %s" : "INCOMING // %s", name, author);
    showAlert(alert, 4000);
    _glitch_until = millis() + 350;
  }
  if (!is_page_ack && (_prefs.beep & BEEP_DM)) beep(Tones::DM);
  saveMsg(m);
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
  stampSent(m, the_mesh.uiLastChannelTimestamp(), true);

  // if the courier picked this up, give its cache entry something readable
  uint8_t hash[MAX_HASH_SIZE];
  pkt->calculatePacketHash(hash);
  MuleEntry* held = muleFind(hash);
  if (held) {
    strncpy(held->from, m->from, sizeof(held->from) - 1);
    strncpy(held->text, m->text, sizeof(held->text) - 1);
    held->sent_ts = the_mesh.uiLastChannelTimestamp();
  }

  char chan[34];
  chanLabel(chan, sizeof(chan), channel_details.name);

  bool viewing = _screen == SCR_CHAT && _conv_kind == CONV_CHANNEL && _conv_chan == idx && _display && _display->isOn();
  m->unread = !viewing;

  bool is_page = strncmp(body, CD_PAGE_PREFIX, strlen(CD_PAGE_PREFIX)) == 0 && strncmp(body, CD_PAGE_ACK, strlen(CD_PAGE_ACK)) != 0;
  bool is_page_ack = strncmp(body, CD_PAGE_ACK, strlen(CD_PAGE_ACK)) == 0;
  if (is_page) {
    m->flags |= MF_PAGE;
    saveMsg(m);
    startPage(m);
    return;
  }

  if (is_page_ack && stristr(body + strlen(CD_PAGE_ACK), _node_prefs->node_name)) {
    char alert[54];
    snprintf(alert, sizeof(alert), "PAGE ACK // %s", m->from);
    showAlert(alert, 6000);
    beep(Tones::DELIVERED);
  } else if (matchesKeyword(body)) {
    m->flags |= MF_KEYWORD;
    char alert[54];
    snprintf(alert, sizeof(alert), "KEYWORD // %s // %s", chan, m->from);
    showAlert(alert, 5000);
    _glitch_until = millis() + 350;
    beep(Tones::KEYWORD);
    if (_display && !_display->isOn()) wake();
  } else {
    if (!viewing) {
      char alert[54];
      snprintf(alert, sizeof(alert), "INCOMING // %s // %s", chan, m->from);
      showAlert(alert, 4000);
      _glitch_until = millis() + 350;
    }
    if ((_prefs.beep & BEEP_CHAN) && !isMuted(idx)) beep(Tones::CHANNEL);
    if (_display && !_display->isOn() && !hasConnection() && !isMuted(idx)) wake();
  }
  saveMsg(m);
  _dirty = true;
}

void UITask::onQueueSizeChanged(int offline_queue_size) {
  _msgcount = offline_queue_size;
  _dirty = true;
}

void UITask::onDiscoveredContact(ContactInfo &contact, bool is_new, uint8_t path_len, const uint8_t* path) {
  _nodes_dirty = true;
  syncClockFrom(contact.last_advert_timestamp, contact.id.pub_key);
  if (is_new) {
    char name[32];
    sanitize(name, sizeof(name), contact.name);
    char alert[54];
    snprintf(alert, sizeof(alert), "NEW NODE // [%s] %s", typeTag(contact.type), name);
    showAlert(alert, 3000);
  }
  _dirty = true;
}

void UITask::onLoginResult(const ContactInfo &contact, bool ok, uint8_t permissions) {
  if (!_login_pending[0] || memcmp(_login_pending, contact.id.pub_key, 6) != 0) return;
  memset(_login_pending, 0, sizeof(_login_pending));
  if (!ok) {
    showAlert("ACCESS DENIED", 3000);
    beep(Tones::FAILED);
    return;
  }
  if (!isLoggedIn(contact.id.pub_key)) {
    int slot = _num_logged_in < CD_ROOM_LOGINS ? _num_logged_in++ : CD_ROOM_LOGINS - 1;
    memcpy(_logged_in[slot], contact.id.pub_key, 6);
  }
  showAlert((permissions & 0x01) ? "ACCESS GRANTED // ADMIN" : "ACCESS GRANTED", 3000);
  if (permissions & 0x01) {   // admin: remember it, and open the console on a repeater
    if (!isAdminOf(contact.id.pub_key)) {
      int slot = _num_admins < CD_ROOM_LOGINS ? _num_admins++ : CD_ROOM_LOGINS - 1;
      memcpy(_admins[slot], contact.id.pub_key, 6);
    }
    if (contact.type == ADV_TYPE_REPEATER) {
      memcpy(_conv_peer, contact.id.pub_key, 6);
      sanitize(_conv_name, sizeof(_conv_name), contact.name);
      _acon_len = 0;
      openAdmin();
      return;
    }
  }
  beep(Tones::DELIVERED);
  if (contact.type == ADV_TYPE_ROOM) openChat(contact.id.pub_key);
  _dirty = true;
}

void UITask::onStatusRecv(const ContactInfo &contact, const uint8_t *data, uint8_t len) {
  if (len < 44) return;
  StatusInfo& s = _status;
  memcpy(s.pub, contact.id.pub_key, 6);
  s.at = millis();
  memcpy(&s.batt_mv, &data[0], 2);
  memcpy(&s.queue, &data[2], 2);
  memcpy(&s.noise, &data[4], 2);
  memcpy(&s.rssi, &data[6], 2);
  memcpy(&s.recv, &data[8], 4);
  memcpy(&s.sent, &data[12], 4);
  memcpy(&s.air_secs, &data[16], 4);
  memcpy(&s.up_secs, &data[20], 4);
  memcpy(&s.flood_tx, &data[24], 4);
  memcpy(&s.direct_tx, &data[28], 4);
  memcpy(&s.flood_rx, &data[32], 4);
  memcpy(&s.direct_rx, &data[36], 4);
  memcpy(&s.snr4, &data[42], 2);
  s.valid = true;
  _status_timeout = 0;
  showAlert("STATUS RECEIVED", 1500);
  _dirty = true;
}

void UITask::onRawRx(float snr, float rssi, const uint8_t raw[], int len) {
  if (_rf == NULL || len < 2) return;
  RfEntry& e = _rf[_rf_head];
  _rf_head = (_rf_head + 1) % CD_RF_LOG;
  if (_rf_count < CD_RF_LOG) _rf_count++;
  _rf_total++;

  uint8_t h = raw[0];
  e.at = millis();
  e.route = h & PH_ROUTE_MASK;
  e.type = (h >> PH_TYPE_SHIFT) & PH_TYPE_MASK;
  int i = 1;
  if (e.route == ROUTE_TYPE_TRANSPORT_FLOOD || e.route == ROUTE_TYPE_TRANSPORT_DIRECT) i += 4;
  e.hops = (i < len) ? (raw[i] & 63) : 0;
  e.snr4 = (int8_t)(snr * 4);
  e.rssi = (int16_t)rssi;
  e.len = len > 255 ? 255 : len;
  if (_screen == SCR_LIST && _tab == TAB_RF) _dirty = true;
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

/* ---------------------------------------- clock ---------------------------------------- */

void UITask::setClock(uint32_t epoch, ClockSrc src) {
  rtc_clock.setCurrentTime(epoch);
  _clock_src = src;
  _clk_last_rtc = epoch;
  _clk_last_ms = millis();
  _dirty = true;
}

// With no time source, adopt the mesh's time once two different nodes agree on it.
void UITask::syncClockFrom(uint32_t their_ts, const uint8_t* from) {
  if (_clock_src != CLK_NONE || their_ts < CLOCK_FLOOR + 86400) return;
  uint32_t now_ms = millis();
  if (_clk_cand_ts && memcmp(_clk_cand_from, from, 4) != 0) {
    uint32_t expect = _clk_cand_ts + (now_ms - _clk_cand_ms) / 1000;
    int32_t diff = (int32_t)(their_ts - expect);
    if (diff > -CLOCK_AGREE_SECS && diff < CLOCK_AGREE_SECS) {
      setClock(their_ts > expect ? their_ts : expect, CLK_MESH);
      showAlert("CLOCK SYNCED FROM MESH", 3000);
      return;
    }
  }
  _clk_cand_ts = their_ts;
  _clk_cand_ms = now_ms;
  memcpy(_clk_cand_from, from, 4);
}

// notice when the phone app or GPS sets the clock behind our back
void UITask::watchClock() {
  uint32_t now = rtc_clock.getCurrentTime();
  uint32_t expect = _clk_last_rtc + (millis() - _clk_last_ms) / 1000;
  if ((int32_t)(now - expect) > 30 && now > CLOCK_FLOOR + 86400) {
    _clock_src = CLK_SET;
    _dirty = true;
  }
  _clk_last_rtc = now;
  _clk_last_ms = millis();
}

bool UITask::isLoggedIn(const uint8_t* pub) const {
  for (int i = 0; i < _num_logged_in; i++) if (memcmp(_logged_in[i], pub, 6) == 0) return true;
  return false;
}

/* ---------------------------------------- paging --------------------------------------- */

void UITask::startPage(Msg* m) {
  if (_screen != SCR_PAGE) _page_return = (_screen == SCR_BOOT) ? SCR_LIST : _screen;
  _page_seq = m->seq;
  _screen = SCR_PAGE;
  _page_next_tone = _page_started = millis();
  if (_display && !_display->isOn()) _display->turnOn();
  _auto_off = millis() + 0x7FFFFFFF;
  _dirty = true;
}

void UITask::ackPage() {
  Msg* m = findMsg(_page_seq);
  _audio.stop();
  _input.setBacklight(_prefs.kbl);
  _screen = _page_return;
  _page_seq = 0;
  _auto_off = millis() + _prefs.dim_secs * 1000UL;
  _dirty = true;
  if (m == NULL) return;

  m->flags |= MF_ACKED;
  m->unread = 0;
  saveMsg(m);

  // tell whoever paged us, in the same place they paged us from
  ConvKind kind = _conv_kind;
  uint8_t chan = _conv_chan, peer[6];
  memcpy(peer, _conv_peer, 6);
  char saved_name[32];
  strcpy(saved_name, _conv_name);

  char ack[64];
  if (m->kind == CONV_DM) {
    _conv_kind = CONV_DM;
    memcpy(_conv_peer, m->peer, 6);
    snprintf(ack, sizeof(ack), "%s %s", CD_PAGE_ACK, _node_prefs->node_name);
  } else {
    _conv_kind = CONV_CHANNEL;
    _conv_chan = m->chan_idx;
    snprintf(ack, sizeof(ack), "%s %s", CD_PAGE_ACK, m->from);
  }
  bool ok = sendText(ack);
  _conv_kind = kind;
  _conv_chan = chan;
  memcpy(_conv_peer, peer, 6);
  strcpy(_conv_name, saved_name);
  showAlert(ok ? "PAGE ACKNOWLEDGED" : "ACK NOT SENT", 2500);
}

/* --------------------------------------- channels -------------------------------------- */

int UITask::addChannel(const char* name, const uint8_t* secret) {
  ChannelDetails cd;
  int free_slot = -1;
  for (int i = 0; i < MAX_GROUP_CHANNELS; i++) {
    if (!the_mesh.getChannel(i, cd)) continue;
    if (cd.name[0] && memcmp(cd.channel.secret, secret, 16) == 0) return i;   // already joined
    if (!cd.name[0] && free_slot < 0) free_slot = i;
  }
  if (free_slot < 0) return -1;

  memset(&cd, 0, sizeof(cd));
  strncpy(cd.name, name, sizeof(cd.name) - 1);
  memcpy(cd.channel.secret, secret, 16);
  if (!the_mesh.uiSetChannel(free_slot, cd)) return -1;

  // anything left over from a channel that used this slot before isn't ours to show
  for (int i = 0; i < _max_msgs; i++) {
    if (_msgs[i].seq && _msgs[i].kind == CONV_CHANNEL && _msgs[i].chan_idx == free_slot) {
      _msgs[i].seq = 0;
      saveMsg(&_msgs[i]);
    }
  }
  if (free_slot < 64) _prefs.muted &= ~(1ULL << free_slot);
  refreshChannels();
  return free_slot;
}

bool UITask::parseKey(const char* s, uint8_t* secret) {
  char hex[33];
  int n = 0;
  for (; *s && n < 33; s++) {
    if (isxdigit(*s)) hex[n++] = *s;
    else if (*s != ' ' && *s != '-' && *s != ':') return false;
  }
  if (n != 32) return false;
  for (int i = 0; i < 16; i++) {
    char b[3] = { hex[i * 2], hex[i * 2 + 1], 0 };
    secret[i] = (uint8_t)strtoul(b, NULL, 16);
  }
  return true;
}

void UITask::showChannelKey(int idx) {
  ChannelDetails cd;
  if (!the_mesh.getChannel(idx, cd) || !cd.name[0]) { consolePrint("NO SUCH CHANNEL"); return; }
  char hex[33], label[34];
  toHex(hex, cd.channel.secret, 16);
  chanLabel(label, sizeof(label), cd.name);
  consolePrint("%s", hex);
  consolePrint("KEY FOR %s", label);
}

// a channel by name ("farm", "#farm"), or the open/selected one when name is empty
static int findChannelByName(const char* name) {
  ChannelDetails cd;
  const char* want = (name[0] == '#') ? name + 1 : name;
  for (int i = 0; i < MAX_GROUP_CHANNELS; i++) {
    if (!the_mesh.getChannel(i, cd) || !cd.name[0]) continue;
    const char* have = (cd.name[0] == '#') ? cd.name + 1 : cd.name;
    if (strieq(have, want)) return i;
  }
  return -1;
}

/* -------------------------------------- navigation ------------------------------------- */

void UITask::openChannel(int list_pos) {
  if (list_pos < 0 || list_pos >= _num_chans) return;
  ChannelDetails cd;
  int idx = _chan_idx[list_pos];
  if (!the_mesh.getChannel(idx, cd)) return;

  _conv_kind = CONV_CHANNEL;
  _conv_chan = idx;
  chanLabel(_conv_name, sizeof(_conv_name), cd.name);
  _screen = SCR_CHAT;
  _scroll = 0;
  markRead(CONV_CHANNEL, idx, NULL);
  _dirty = true;
}

void UITask::openChat(const uint8_t* pub) {
  const Node* n = findNode(pub);
  memcpy(_conv_peer, pub, sizeof(_conv_peer));
  if (n) strcpy(_conv_name, n->name);
  _conv_kind = CONV_DM;
  _screen = SCR_CHAT;
  _scroll = 0;
  markRead(CONV_DM, 0, pub);
  _dirty = true;
}

void UITask::openNode(const uint8_t* pub) {
  const Node* n = findNode(pub);
  if (n == NULL) {
    showAlert("NOT IN CONTACTS", 2000);
    return;
  }
  memcpy(_conv_peer, n->pub, sizeof(_conv_peer));
  strcpy(_conv_name, n->name);
  _scroll = 0;
  _node_mode = NM_VIEW;

  if (n->type == ADV_TYPE_CHAT) {
    openChat(n->pub);
  } else if (n->type == ADV_TYPE_ROOM && isLoggedIn(n->pub)) {
    openChat(n->pub);
  } else {
    _screen = SCR_NODE;   // repeaters, sensors and rooms not yet joined get an info page
  }
  _dirty = true;
}

void UITask::wake() {
  if (_cpu_low) {   // full speed before the first frame, not a second later
    setCpuFrequencyMhz(240);
    _cpu_low = false;
  }
  if (_display && !_display->isOn()) {
    _display->turnOn();
    _dirty = true;
  }
  if (_screen != SCR_PAGE) _auto_off = millis() + _prefs.dim_secs * 1000UL;
}

void UITask::handleKey(int key) {
  bool was_off = _display && !_display->isOn();
  wake();
  if (was_off) return;   // the key that wakes the screen does nothing else

  if ((_prefs.beep & BEEP_KEYS) && key < TKEY_UP) beep(Tones::CLICK);

  if (_screen == SCR_BOOT) {
    _screen = SCR_LIST;
    _glitch_until = millis() + 250;
    _dirty = true;
    return;
  }
  if (_screen == SCR_PAGE) {   // any key or a click acknowledges; a drifting trackball doesn't
    if (key < TKEY_UP || key == TKEY_CLICK || key == TKEY_HOLD) ackPage();
    return;
  }
  if (key == TKEY_HOLD) {   // long-press the trackball: blank the screen
    if (_display) _display->turnOff();
    return;
  }

  switch (_screen) {
    case SCR_LIST: handleListKey(key); break;
    case SCR_CHAT: handleChatKey(key); break;
    case SCR_NODE: handleNodeKey(key); break;
    case SCR_ADMIN: handleAdminKey(key); break;
    case SCR_GPS:
      if (key == TKEY_ESC || key == TKEY_BACKSPACE || key == TKEY_LEFT || key == TKEY_CLICK || key == TKEY_ENTER || key == 'g' || key == 'G') {
        _screen = SCR_LIST;
      }
      break;
    case SCR_MULE:
      if (key == TKEY_UP) _mule_scroll--;
      else if (key == TKEY_DOWN) _mule_scroll++;
      else if (key == 'o' || key == 'O') {
        _prefs.mule = !_prefs.mule;
        savePrefs();
        showAlert(_prefs.mule ? "COURIER ON" : "COURIER OFF // STILL COLLECTING", 2000);
      } else if (key == 'f' || key == 'F') {
        char a[32];
        snprintf(a, sizeof(a), "SENDING %d HELD", muleFlush());
        showAlert(a, 2000);
      } else if (key == 'x' || key == 'X') {
        if (_mule) memset(_mule, 0, sizeof(MuleEntry) * CD_MULE_SLOTS);
        showAlert("COURIER EMPTIED", 1500);
      } else if (key == TKEY_ESC || key == TKEY_BACKSPACE || key == TKEY_LEFT || key == TKEY_CLICK || key == TKEY_ENTER) {
        _screen = SCR_LIST;
      }
      break;
    default: break;
  }
  _dirty = true;
}

void UITask::handleNodeKey(int key) {
  ContactInfo* c = the_mesh.lookupContactByPubKey(_conv_peer, sizeof(_conv_peer));
  const Node* n = findNode(_conv_peer);

  if (_node_mode == NM_PASSWORD) {
    if (key == TKEY_ENTER || key == TKEY_CLICK) {
      _input_buf[_input_len] = 0;
      uint32_t est = 0;
      int r = the_mesh.uiLogin(_conv_peer, _input_buf, est);
      if (r == MSG_SEND_FAILED) {
        showAlert("LOGIN NOT SENT", 2500);
      } else {
        memcpy(_login_pending, _conv_peer, 6);
        _login_timeout = millis() + est + 4000;
        showAlert("AUTHENTICATING...", (int)(est + 4000));
      }
      _node_mode = NM_VIEW;
      _input_len = 0;
      _input_buf[0] = 0;
    } else if (key == TKEY_ESC || key == TKEY_LEFT) {
      _node_mode = NM_VIEW;
      _input_len = 0;
      _input_buf[0] = 0;
    } else if (key == TKEY_BACKSPACE) {
      if (_input_len > 0) _input_buf[--_input_len] = 0;
    } else if (key >= 32 && key < 127 && _input_len < 15) {
      _input_buf[_input_len++] = key;
      _input_buf[_input_len] = 0;
    }
    return;
  }

  if (_node_mode == NM_CONFIRM_DEL) {
    _node_mode = NM_VIEW;
    if (key == 'd' || key == 'D') {
      if (the_mesh.uiRemoveContact(_conv_peer)) {
        showAlert("CONTACT DELETED", 2000);
        refreshNodes();
        _screen = SCR_LIST;
      } else {
        showAlert("DELETE FAILED", 2000);
      }
    }
    return;
  }

  switch (key) {
    case TKEY_BACKSPACE: case TKEY_LEFT: case TKEY_ESC:
      _screen = SCR_LIST;
      break;
    case TKEY_ENTER: case TKEY_CLICK:
      if (n && (n->type == ADV_TYPE_CHAT || (n->type == ADV_TYPE_ROOM && isLoggedIn(n->pub)))) openChat(n->pub);
      else if (n && n->type == ADV_TYPE_ROOM) { _node_mode = NM_PASSWORD; _input_len = 0; _input_buf[0] = 0; }
      else _screen = SCR_LIST;
      break;
    case 'r': case 'R':
      if (c) {
        the_mesh.resetPathTo(*c);
        showAlert("PATH RESET // NEXT TX FLOODS", 2500);
        _nodes_dirty = true;
      }
      break;
    case 's': case 'S':
      showAlert(the_mesh.uiShareContact(_conv_peer) ? "CONTACT SHARED (ZERO HOP)" : "SHARE FAILED", 2500);
      break;
    case 'd': case 'D':
      _node_mode = NM_CONFIRM_DEL;
      break;
    case 'l': case 'L':
      if (n && (n->type == ADV_TYPE_ROOM || n->type == ADV_TYPE_REPEATER)) {
        _node_mode = NM_PASSWORD;
        _input_len = 0;
        _input_buf[0] = 0;
      }
      break;
    case 'a': case 'A':
      if (n && isAdminOf(n->pub)) openAdmin();
      else showAlert("LOG IN AS ADMIN FIRST (L)", 2000);
      break;
    case 't': case 'T':
      if (n && n->type == ADV_TYPE_REPEATER) {
        uint32_t est = 0;
        if (the_mesh.uiRequestStatus(_conv_peer, est) == MSG_SEND_FAILED) {
          showAlert("REQUEST NOT SENT", 2000);
        } else {
          _status.valid = false;
          memcpy(_status.pub, _conv_peer, 6);
          _status_timeout = millis() + est + 4000;
          showAlert("REQUESTING STATUS...", (int)(est + 4000));
        }
      }
      break;
    default: break;
  }
}

void UITask::handleListKey(int key) {
  int count = (_tab == TAB_CHAN) ? _num_chans : (_tab == TAB_NODES) ? _num_nodes : (_tab == TAB_HEARD) ? num_heard : 0;
  int& sel = _sel[_tab];

  if (_tab != TAB_MAP) _map_pan = false;   // never come back to a map that's still panning
  if (_tab == TAB_MAP && handleMapKey(key)) return;
  if (_tab == TAB_RF && (key == 'm' || key == 'M')) { _screen = SCR_MULE; _mule_scroll = 0; return; }

  if (_tab == TAB_SYS) {   // the SYS tab is a command console
    if (key == TKEY_LEFT) { _tab = TAB_RF; return; }
    if (key == TKEY_RIGHT) { _tab = TAB_CHAN; return; }
    if (key == TKEY_UP) { if (_console_scroll < _console_len - CD_CONSOLE_ROWS) _console_scroll++; return; }
    if (key == TKEY_DOWN) { if (_console_scroll > 0) _console_scroll--; return; }
    if (key == TKEY_ENTER || key == TKEY_CLICK) {
      if (_cmd_len == 0 && key == TKEY_CLICK && _last_cmd[0]) {   // click on an empty prompt repeats
        strcpy(_cmd_buf, _last_cmd);
        _cmd_len = strlen(_cmd_buf);
      }
      if (_cmd_len > 0) {
        _cmd_buf[_cmd_len] = 0;
        strcpy(_last_cmd, _cmd_buf);
        consolePrint("> %s", _cmd_buf);
        _console_scroll = 0;
        runCommand(_cmd_buf);
        _cmd_len = 0;
        _cmd_buf[0] = 0;
      }
    } else if (key == TKEY_BACKSPACE) {
      if (_cmd_len > 0) _cmd_buf[--_cmd_len] = 0;
    } else if (key == TKEY_ESC) {
      _cmd_len = 0;
      _cmd_buf[0] = 0;
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
      } else if (_tab == TAB_HEARD) {
        openNode(heard[sel].pubkey_prefix);
      }
      break;
    case '/':   // jump to the console from anywhere
      _tab = TAB_SYS;
      break;
    default:
      // a letter jumps to the next entry starting with it
      if (isalnum(key) && count > 0 && (_tab == TAB_CHAN || _tab == TAB_NODES)) {
        for (int step = 1; step <= count; step++) {
          int i = (sel + step) % count;
          const char* name;
          ChannelDetails cd;
          if (_tab == TAB_CHAN) {
            the_mesh.getChannel(_chan_idx[i], cd);
            name = (cd.name[0] == '#') ? cd.name + 1 : cd.name;
          } else {
            name = _nodes[i].name;
          }
          if (tolower(name[0]) == tolower(key)) { sel = i; break; }
        }
      }
      break;
  }
}

// "@[name] " for the picked message's sender, as the phone app writes mentions
static bool insertMention(char* buf, int& len, int max_len, const char* name) {
  char m[40];
  snprintf(m, sizeof(m), "%s@[%s] ", (len > 0 && buf[len - 1] != ' ') ? " " : "", name);
  int n = strlen(m);
  if (len + n > max_len) return false;
  memcpy(buf + len, m, n + 1);
  len += n;
  return true;
}

void UITask::handleChatKey(int key) {
  int max_len = CD_INPUT_MAX;
  if (_conv_kind == CONV_CHANNEL) max_len -= strlen(_node_prefs->node_name) + 2;   // room for "<name>: "

  // the trackball picks a message (newest first); click/enter @mentions whoever wrote it
  auto mention = [&]() {
    Msg* m = findMsg(_chat_sel_seq);
    if (m == NULL) return;
    if (m->outgoing) showAlert("THAT ONE'S YOURS", 1200);
    else if (!insertMention(_input_buf, _input_len, max_len, m->from)) showAlert("NO ROOM FOR THE NAME", 1500);
    _chat_sel = -1;
  };

  switch (key) {
    case TKEY_UP:    _chat_sel++; _chat_sel_seq = 0; break;   // clamped, and pinned to a message, when drawn
    case TKEY_DOWN:  if (_chat_sel >= 0) _chat_sel--; _chat_sel_seq = 0; break;
    case TKEY_LEFT:
    case TKEY_ESC:
      if (_chat_sel >= 0) _chat_sel = -1;
      else _screen = SCR_LIST;
      break;
    case TKEY_CLICK:
      if (_chat_sel >= 0) mention();
      else if (_input_len == 0) _screen = SCR_LIST;
      else sendInput();
      break;
    case TKEY_ENTER:
      if (_chat_sel >= 0 && _input_len == 0) mention();
      else { _chat_sel = -1; sendInput(); }
      break;
    case TKEY_BACKSPACE:
      if (_input_len > 0) {
        _input_buf[--_input_len] = 0;
      } else if (_chat_sel >= 0) {
        _chat_sel = -1;
      } else {
        _screen = SCR_LIST;
      }
      break;
    default:
      if (key >= 32 && key < 127) {
        if (_input_len < max_len) {
          _input_buf[_input_len++] = key;
          _input_buf[_input_len] = 0;
        }
      }
      break;
  }
  if (_screen == SCR_CHAT) markRead(_conv_kind, _conv_chan, _conv_peer);
  else _chat_sel = -1;
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

static bool argOn(const char* a)  { return strcmp(a, "on") == 0 || strcmp(a, "1") == 0; }
static bool argOff(const char* a) { return strcmp(a, "off") == 0 || strcmp(a, "0") == 0; }

void UITask::runCommand(const char* line) {
  char cmd[96];
  strncpy(cmd, line, sizeof(cmd) - 1);
  cmd[sizeof(cmd) - 1] = 0;
  char* arg = strchr(cmd, ' ');
  if (arg) { *arg++ = 0; while (*arg == ' ') arg++; } else { arg = cmd + strlen(cmd); }
  for (char* p = cmd; *p; p++) *p = tolower(*p);
  bool in_chat = _screen == SCR_CHAT;
  char argl[64];   // lower-cased copy, for keywords and names
  strncpy(argl, arg, sizeof(argl) - 1);
  argl[sizeof(argl) - 1] = 0;
  for (char* p = argl; *p; p++) *p = tolower(*p);

  /* ---- help ---- */
  if (strcmp(cmd, "help") == 0 || strcmp(cmd, "?") == 0) {
    if (strcmp(argl, "chan") == 0) {
      consolePrint("join #name     PUBLIC HASHTAG CHANNEL");
      consolePrint("newchan name   PRIVATE, NEW RANDOM KEY");
      consolePrint("addchan name <32 hex> JOIN PRIVATE");
      consolePrint("key [name]     SHOW KEY TO SHARE");
      consolePrint("delchan name   mute/unmute [name]");
      consolePrint("chans          LIST ALL CHANNELS");
    } else if (strcmp(argl, "radio") == 0) {
      consolePrint("radio [MHz bw sf cr]  preset [name]");
      consolePrint("tx <dBm>       repeat on|off");
      consolePrint("mule on|off  COURIER // help mule");
      consolePrint("scope <region>|off  e.g. scope pdx");
      consolePrint("advert [flood]");
    } else if (strcmp(argl, "mule") == 0) {
      consolePrint("mule          THE COURIER SCREEN");
      consolePrint("mule on|off   DELIVER (IT ALWAYS COLLECTS)");
      consolePrint("mule status  flush  clear");
      consolePrint("mule chans    WHICH CHANNELS RIDE ALONG");
      consolePrint("mule chan <name> on|off|auto");
      consolePrint("mule dm|own on|off  DMS / YOUR POSTS");
      consolePrint("mule size <n> age <min> rate <n/10min>");
    } else if (strcmp(argl, "alert") == 0) {
      consolePrint("/page [txt]  ALARM ON THEIR DECK");
      consolePrint("/sos         PAGE WITH YOUR POSITION");
      consolePrint("alert <word>  alert -<word>  alert");
      consolePrint("beep on|off|dm|keys  vol 0-10");
      consolePrint("kbl 0-255    KEYBOARD BACKLIGHT");
    } else if (strcmp(argl, "net") == 0) {
      consolePrint("time HH:MM[:SS]  date YYYY-MM-DD");
      consolePrint("clock  tz <h>  name <handle>");
      consolePrint("ble on|off  gps on|off  pos");
      consolePrint("pin <6 digits>|random  BLE PAIRING PIN");
      consolePrint("power  gps live on|off  BATTERY");
      consolePrint("crumbs on|off|clear  sd");
      consolePrint("MAP: " G_UP G_DOWN " ZOOM CLICK PAN C ME N/P NODE");
      consolePrint("     G GPS  S STYLE  B TRAIL  L LABELS");
      consolePrint("/loc  SEND YOUR POSITION");
      consolePrint("NODE PAGE: L LOGIN T STATUS S SHARE");
      consolePrint("           R RESET PATH  D DELETE");
    } else if (strcmp(argl, "ui") == 0) {
      consolePrint("theme matrix|amber|gibson|redline");
      consolePrint("ball 1-5  crt on|off  dim <secs>");
      consolePrint("history on|off|wipe  clear");
      consolePrint("reboot  off");
    } else {
      consolePrint("help chan   CHANNELS & PRIVATE KEYS");
      consolePrint("help alert  PAGING, BEEPS, KEYWORDS");
      consolePrint("help radio  RF, REPEAT, MULE");
      consolePrint("help net    CLOCK, GPS, NODES");
      consolePrint("help ui     LOOK & FEEL");
      consolePrint("help mule   CARRY MESSAGES BETWEEN AREAS");
      consolePrint(G_UP G_DOWN " SCROLLS. CLICK REPEATS LAST CMD");
    }

  /* ---- channels ---- */
  } else if (strcmp(cmd, "join") == 0) {
    if (!argl[0]) { consolePrint("USAGE: join #name"); }
    else {
      char name[32];
      snprintf(name, sizeof(name), argl[0] == '#' ? "%s" : "#%s", argl);
      uint8_t hash[32];
      mesh::Utils::sha256(hash, sizeof(hash), (const uint8_t*)name, strlen(name));
      int idx = addChannel(name, hash);
      if (idx < 0) consolePrint("NO FREE CHANNEL SLOTS");
      else consolePrint("JOINED %s", name);
    }
  } else if (strcmp(cmd, "newchan") == 0) {
    if (!arg[0]) { consolePrint("USAGE: newchan <name>"); }
    else {
      uint8_t secret[16];
      esp_fill_random(secret, sizeof(secret));
      int idx = addChannel(arg, secret);
      if (idx < 0) { consolePrint("NO FREE CHANNEL SLOTS"); }
      else {
        consolePrint("PRIVATE CHANNEL %s CREATED", arg);
        consolePrint("GIVE THIS KEY TO YOUR GROUP:");
        showChannelKey(idx);
      }
    }
  } else if (strcmp(cmd, "addchan") == 0) {
    char* key = strrchr(arg, ' ');
    uint8_t secret[16];
    if (key == NULL) { consolePrint("USAGE: addchan <name> <32 hex key>"); }
    else {
      *key++ = 0;
      if (!parseKey(key, secret)) consolePrint("KEY MUST BE 32 HEX DIGITS");
      else {
        int idx = addChannel(arg, secret);
        if (idx < 0) consolePrint("NO FREE CHANNEL SLOTS");
        else consolePrint("JOINED PRIVATE CHANNEL %s", arg);
      }
    }
  } else if (strcmp(cmd, "key") == 0) {
    int idx = argl[0] ? findChannelByName(argl) : (in_chat && _conv_kind == CONV_CHANNEL) ? _conv_chan
              : (_num_chans ? _chan_idx[_sel[TAB_CHAN] < _num_chans ? _sel[TAB_CHAN] : 0] : -1);
    if (idx < 0) consolePrint("NO SUCH CHANNEL");
    else showChannelKey(idx);
  } else if (strcmp(cmd, "delchan") == 0) {
    int idx = argl[0] ? findChannelByName(argl) : (in_chat && _conv_kind == CONV_CHANNEL) ? _conv_chan : -1;
    if (idx < 0) consolePrint("USAGE: delchan <name>");
    else {
      ChannelDetails cd;
      memset(&cd, 0, sizeof(cd));
      the_mesh.uiSetChannel(idx, cd);
      for (int i = 0; i < _max_msgs; i++) {
        if (_msgs[i].seq && _msgs[i].kind == CONV_CHANNEL && _msgs[i].chan_idx == idx) {
          _msgs[i].seq = 0;
          saveMsg(&_msgs[i]);
        }
      }
      refreshChannels();
      if (in_chat && _conv_kind == CONV_CHANNEL && _conv_chan == idx) _screen = SCR_LIST;
      consolePrint("CHANNEL DELETED");
    }
  } else if (strcmp(cmd, "mute") == 0 || strcmp(cmd, "unmute") == 0) {
    int idx = argl[0] ? findChannelByName(argl) : (in_chat && _conv_kind == CONV_CHANNEL) ? _conv_chan : -1;
    if (idx < 0 || idx >= 64) consolePrint("USAGE: %s <channel>", cmd);
    else {
      if (cmd[0] == 'm') _prefs.muted |= (1ULL << idx); else _prefs.muted &= ~(1ULL << idx);
      savePrefs();
      consolePrint(cmd[0] == 'm' ? "MUTED (KEYWORDS STILL ALERT)" : "UNMUTED");
    }
  } else if (strcmp(cmd, "chans") == 0) {
    ChannelDetails cd;
    for (int i = 0; i < _num_chans; i++) {
      the_mesh.getChannel(_chan_idx[i], cd);
      char label[34];
      chanLabel(label, sizeof(label), cd.name);
      consolePrint("%2d %-24s%s", _chan_idx[i], label, isMuted(_chan_idx[i]) ? " MUTED" : "");
    }
    if (_num_chans == 0) consolePrint("NO CHANNELS");

  /* ---- paging & alerts ---- */
  } else if (strcmp(cmd, "page") == 0 || strcmp(cmd, "sos") == 0) {
    if (!in_chat) { consolePrint("OPEN A CHAT, THEN TYPE /%s", cmd); }
    else {
      char msg[CD_INPUT_MAX + 1];
      if (cmd[0] == 's') {
        LocationProvider* lp = _sensors ? _sensors->getLocationProvider() : NULL;
        if (lp && lp->isValid()) snprintf(msg, sizeof(msg), "%s SOS %.5f,%.5f %s", CD_PAGE_PREFIX, lp->getLatitude() / 1e6, lp->getLongitude() / 1e6, arg);
        else if (_node_prefs->node_lat || _node_prefs->node_lon) snprintf(msg, sizeof(msg), "%s SOS ~%.5f,%.5f %s", CD_PAGE_PREFIX, _node_prefs->node_lat, _node_prefs->node_lon, arg);
        else snprintf(msg, sizeof(msg), "%s SOS %s", CD_PAGE_PREFIX, arg);
      } else {
        snprintf(msg, sizeof(msg), "%s %s", CD_PAGE_PREFIX, arg);
      }
      consolePrint(sendText(msg, MF_PAGE) ? "PAGE SENT" : "PAGE FAILED");
    }
  } else if (strcmp(cmd, "loc") == 0) {
    LocationProvider* lp = _sensors ? _sensors->getLocationProvider() : NULL;
    if (!in_chat) consolePrint("OPEN A CHAT, THEN TYPE /loc");
    else if (lp == NULL || !lp->isValid()) consolePrint("NO GPS FIX");
    else {
      char msg[64];
      snprintf(msg, sizeof(msg), "LOC %.5f,%.5f %s", lp->getLatitude() / 1e6, lp->getLongitude() / 1e6, arg);
      consolePrint(sendText(msg) ? "POSITION SENT" : "SEND FAILED");
    }
  } else if (strcmp(cmd, "info") == 0) {
    if (in_chat && _conv_kind == CONV_DM) { _screen = SCR_NODE; _node_mode = NM_VIEW; consolePrint("DOSSIER"); }
    else consolePrint("OPEN A DIRECT CHAT FIRST");
  } else if (strcmp(cmd, "alert") == 0) {
    if (argl[0] == '-') {
      for (int i = 0; i < CD_KEYWORDS; i++) if (strieq(_prefs.keywords[i], argl + 1)) _prefs.keywords[i][0] = 0;
      savePrefs();
      consolePrint("REMOVED %s", argl + 1);
    } else if (argl[0]) {
      int slot = -1;
      for (int i = 0; i < CD_KEYWORDS; i++) if (!_prefs.keywords[i][0]) { slot = i; break; }
      if (slot < 0) consolePrint("4 KEYWORDS MAX. alert -<word> FIRST");
      else {
        strncpy(_prefs.keywords[slot], argl, sizeof(_prefs.keywords[slot]) - 1);
        savePrefs();
        consolePrint("ALERTING ON \"%s\"", _prefs.keywords[slot]);
      }
    } else {
      char list[48] = "";
      for (int i = 0; i < CD_KEYWORDS; i++) {
        if (_prefs.keywords[i][0]) { strcat(list, _prefs.keywords[i]); strcat(list, " "); }
      }
      consolePrint("KEYWORDS: %s@%s", list, _node_prefs->node_name);
    }
  } else if (strcmp(cmd, "beep") == 0) {
    if (argOn(argl)) _prefs.beep = BEEP_DM | BEEP_CHAN;
    else if (argOff(argl)) _prefs.beep = 0;
    else if (strcmp(argl, "dm") == 0) _prefs.beep = BEEP_DM;
    else if (strcmp(argl, "keys") == 0) _prefs.beep ^= BEEP_KEYS;
    savePrefs();
    consolePrint("BEEP DM:%s CHAN:%s KEYS:%s", (_prefs.beep & BEEP_DM) ? "ON" : "OFF",
                 (_prefs.beep & BEEP_CHAN) ? "ON" : "OFF", (_prefs.beep & BEEP_KEYS) ? "ON" : "OFF");
  } else if (strcmp(cmd, "vol") == 0) {
    if (argl[0]) {
      int v = atoi(argl);
      _prefs.volume = v < 0 ? 0 : v > 10 ? 10 : v;
      _audio.setVolume(_prefs.volume);
      savePrefs();
      beep(Tones::DM);
    }
    consolePrint("VOLUME %u/10%s", _prefs.volume, _audio.ready() ? "" : " (NO AUDIO)");
  } else if (strcmp(cmd, "kbl") == 0) {
    if (argl[0]) {
      int v = atoi(argl);
      _prefs.kbl = v < 0 ? 0 : v > 255 ? 255 : v;
      _input.setBacklight(_prefs.kbl);
      savePrefs();
    }
    consolePrint("KEYBOARD BACKLIGHT %u", _prefs.kbl);

  /* ---- radio ---- */
  } else if (strcmp(cmd, "radio") == 0) {
    float f, bw;
    int sf, cr;
    if (argl[0]) {
      if (sscanf(argl, "%f %f %d %d", &f, &bw, &sf, &cr) == 4) {
        char was[48];
        snprintf(was, sizeof(was), "WAS radio %.3f %g %u %u", _node_prefs->freq, _node_prefs->bw, _node_prefs->sf, _node_prefs->cr);
        if (the_mesh.uiSetRadio(f, bw, sf, cr)) consolePrint("%s", was);
        else consolePrint("OUT OF RANGE");
      } else {
        consolePrint("USAGE: radio <MHz> <bw> <sf> <cr>");
      }
    }
    consolePrint("RF %.3f BW%g SF%u CR%u %ddBm", _node_prefs->freq, _node_prefs->bw, _node_prefs->sf, _node_prefs->cr,
                 _node_prefs->tx_power_dbm);
  } else if (strcmp(cmd, "preset") == 0) {
    bool found = false;
    for (uint8_t i = 0; i < NUM_PRESETS && argl[0]; i++) {
      if (strcmp(argl, PRESETS[i].name) == 0) {
        found = the_mesh.uiSetRadio(PRESETS[i].freq, PRESETS[i].bw, PRESETS[i].sf, PRESETS[i].cr);
        consolePrint(found ? "PRESET %s APPLIED" : "PRESET %s FAILED", PRESETS[i].name);
        consolePrint("RF %.3f BW%g SF%u CR%u", PRESETS[i].freq, PRESETS[i].bw, PRESETS[i].sf, PRESETS[i].cr);
        found = true;
      }
    }
    if (!found) {
      for (uint8_t i = 0; i < NUM_PRESETS; i++) {
        consolePrint("%-10s %.3f BW%g SF%u CR%u", PRESETS[i].name, PRESETS[i].freq, PRESETS[i].bw, PRESETS[i].sf, PRESETS[i].cr);
      }
    }
  } else if (strcmp(cmd, "tx") == 0) {
    if (argl[0] && !the_mesh.uiSetTxPower(atoi(argl))) consolePrint("RANGE -9 TO %d", MAX_LORA_TX_POWER);
    consolePrint("TX POWER %ddBm", _node_prefs->tx_power_dbm);
  } else if (strcmp(cmd, "repeat") == 0) {
    if (argOn(argl)) the_mesh.uiSetRepeat(true);
    else if (argOff(argl)) the_mesh.uiSetRepeat(false);
    consolePrint("REPEAT %s", _node_prefs->isRepeatEn() ? "ON // FORWARDING PACKETS" : "OFF");
    if (_node_prefs->isRepeatEn() && !the_mesh.uiRepeatFreqAllowed()) {
      consolePrint("WARN: NOT AN OFF-GRID FREQ. EXTRA");
      consolePrint("      FLOOD TRAFFIC FOR EVERYONE.");
    }
  } else if (strcmp(cmd, "mule") == 0 || strcmp(cmd, "courier") == 0) {
    char sub[16] = "";
    sscanf(argl, "%15s", sub);
    const char* rest = argl + strlen(sub);
    while (*rest == ' ') rest++;
    bool show = true;

    if (!sub[0] || strcmp(sub, "show") == 0 || strcmp(sub, "list") == 0) {   // the courier screen
      _screen = SCR_MULE;
      _mule_scroll = 0;
      consolePrint("COURIER SCREEN");
      return;
    }
    if (argOn(sub)) { _prefs.mule = 1; _mule_next_probe = 0; savePrefs(); }
    else if (argOff(sub)) { _prefs.mule = 0; savePrefs(); }
    else if (strcmp(sub, "flush") == 0) { consolePrint("SENDING %d NOW", muleFlush()); show = false; }
    else if (strcmp(sub, "clear") == 0) { if (_mule) memset(_mule, 0, sizeof(MuleEntry) * CD_MULE_SLOTS); consolePrint("EMPTIED"); }
    else if (strcmp(sub, "status") == 0) { }
    else if (strcmp(sub, "dm") == 0 || strcmp(sub, "own") == 0) {
      uint8_t& off = (sub[0] == 'd') ? _prefs.mule_dm_off : _prefs.mule_own_off;
      if (argOn(rest)) off = 0; else if (argOff(rest)) off = 1;
      savePrefs();
      consolePrint(sub[0] == 'd' ? "CARRY DMS BETWEEN CONTACTS: %s" : "CARRY YOUR OWN POSTS: %s", off ? "NO" : "YES");
      show = false;
    } else if (strcmp(sub, "age") == 0 || strcmp(sub, "size") == 0 || strcmp(sub, "rate") == 0) {
      if (*rest) {
        int v = atoi(rest);
        if (sub[0] == 'a' && v >= 10 && v <= 1440) _prefs.mule_age_min = v;                         // minutes
        else if (sub[0] == 's' && v >= 8 && v <= CD_MULE_SLOTS) _prefs.mule_size8 = (v + 7) / 8;    // messages
        else if (sub[0] == 'r' && v >= 1 && v <= 200) { _prefs.mule_rate = v; _mule_tokens = -1; }   // per 10 min
        else consolePrint(sub[0] == 'a' ? "AGE: 10-1440 MINUTES" : sub[0] == 's' ? "SIZE: 8-256 MESSAGES" : "RATE: 1-200 PER 10 MIN");
        savePrefs();
      }
    } else if (strcmp(sub, "chans") == 0 || strcmp(sub, "chan") == 0) {
      if (sub[4] == 0 && *rest) {   // mule chan <name> on|off|auto
        char name[32] = "", val[8] = "";
        sscanf(rest, "%31s %7s", name, val);
        int idx = findChannelByName(name);
        if (idx < 0 || idx >= 64) consolePrint("NO SUCH CHANNEL");
        else {
          uint64_t bit = 1ULL << idx;
          if (strcmp(val, "auto") == 0) _prefs.mule_chan_set &= ~bit;
          else if (argOn(val)) { _prefs.mule_chan_set |= bit; _prefs.mule_chan_val |= bit; }
          else if (argOff(val)) { _prefs.mule_chan_set |= bit; _prefs.mule_chan_val &= ~bit; }
          savePrefs();
        }
      }
      ChannelDetails cd;
      for (int i = 0; i < _num_chans; i++) {
        the_mesh.getChannel(_chan_idx[i], cd);
        char label[34];
        chanLabel(label, sizeof(label), cd.name);
        consolePrint("%-20s %s%s", label, muleCarriesChan(_chan_idx[i]) ? "CARRY" : "-",
                     (_prefs.mule_chan_set >> _chan_idx[i]) & 1 ? "" : " (AUTO)");
      }
      show = false;
    } else if (strcmp(sub, "help") == 0) {
      runCommand("help mule");
      return;
    }
    if (show) muleStatus();
  } else if (strcmp(cmd, "scope") == 0) {
    if (argOff(argl) || strcmp(argl, "*") == 0 || strcmp(argl, "none") == 0) {
      the_mesh.uiSetDefaultScope("");
    } else if (argl[0]) {
      bool ok = true;
      for (const char* p = argl[0] == '#' ? argl + 1 : argl; *p; p++) {
        if (!isalnum(*p) && *p != '-' && *p != '_') ok = false;
      }
      if (ok) the_mesh.uiSetDefaultScope(argl);
      else consolePrint("REGION NAMES: a-z 0-9 - _");
    }
    const char* sc = the_mesh.uiDefaultScope();
    if (sc[0]) {
      consolePrint("SCOPE %s // FLOODS TAGGED %s", sc, sc);
      consolePrint("ONLY REPEATERS CARRYING REGION");
      consolePrint("%s WILL FORWARD. scope off TO UNDO", sc);
    } else {
      consolePrint("SCOPE OFF // ALL REPEATERS FORWARD");
    }
  } else if (strcmp(cmd, "advert") == 0) {
    bool flood = strcmp(argl, "flood") == 0;
    bool ok = flood ? the_mesh.advertFlood() : the_mesh.advert();
    consolePrint(ok ? (flood ? "ADVERT FLOODED" : "ADVERT SENT (ZERO HOP)") : "ADVERT FAILED");

  /* ---- net & clock ---- */
  } else if (strcmp(cmd, "time") == 0 || strcmp(cmd, "date") == 0) {
    int32_t tz = (int32_t)_node_prefs->tz_offset * 3600;
    uint32_t local = rtc_clock.getCurrentTime() + tz;
    int32_t days = local / 86400;
    uint32_t secs = local % 86400;
    bool ok = false;
    if (cmd[0] == 't' && argl[0]) {
      int h, m, s = 0;
      if (sscanf(argl, "%d:%d:%d", &h, &m, &s) >= 2 && h >= 0 && h < 24 && m >= 0 && m < 60 && s >= 0 && s < 60) {
        secs = h * 3600 + m * 60 + s;
        ok = true;
      }
    } else if (cmd[0] == 'd' && argl[0]) {
      int y, mo, d;
      if (sscanf(argl, "%d-%d-%d", &y, &mo, &d) == 3 && y >= 2024 && mo >= 1 && mo <= 12 && d >= 1 && d <= 31) {
        days = daysFromCivil(y, mo, d);
        ok = true;
      }
    }
    if (ok) {
      uint32_t epoch = (uint32_t)days * 86400 + secs - tz;
      if (epoch < rtc_clock.getCurrentTime()) consolePrint("WARN: CLOCK WENT BACKWARDS");
      setClock(epoch, CLK_SET);
    } else if (argl[0]) {
      consolePrint(cmd[0] == 't' ? "USAGE: time HH:MM[:SS]" : "USAGE: date YYYY-MM-DD");
    }
    runCommand("clock");
    return;
  } else if (strcmp(cmd, "clock") == 0) {
    uint32_t local = rtc_clock.getCurrentTime() + (int32_t)_node_prefs->tz_offset * 3600;
    int y; unsigned mo, d;
    civilFromDays(local / 86400, y, mo, d);
    uint32_t s = local % 86400;
    static const char* SRC[] = { "NOT SET", "FROM MESH", "SET" };
    consolePrint("%04d-%02u-%02u %02u:%02u:%02u UTC%+d", y, mo, d, s / 3600, (s / 60) % 60, s % 60, _node_prefs->tz_offset);
    consolePrint("CLOCK %s", SRC[_clock_src]);
  } else if (strcmp(cmd, "tz") == 0) {
    if (argl[0]) {
      int tz = atoi(argl);
      if (tz >= -12 && tz <= 14) {
        _node_prefs->tz_offset = tz;
        the_mesh.savePrefs();
      }
    }
    consolePrint("TZ UTC%+d", _node_prefs->tz_offset);
  } else if (strcmp(cmd, "pos") == 0) {
    LocationProvider* lp = _sensors ? _sensors->getLocationProvider() : NULL;
    if (lp == NULL) consolePrint("NO GPS HARDWARE");
    else if (!getGPSState()) consolePrint("GPS OFF // gps on");
    else if (!lp->isValid()) consolePrint("NO FIX // %ld SATS", lp->satellitesCount());
    else consolePrint("%.5f,%.5f %ld SATS", lp->getLatitude() / 1e6, lp->getLongitude() / 1e6, lp->satellitesCount());
  } else if (strcmp(cmd, "crumbs") == 0) {
    if (argOn(argl)) { _prefs.crumb_off = 0; savePrefs(); }
    else if (argOff(argl)) { _prefs.crumb_off = 1; savePrefs(); }
    else if (strcmp(argl, "clear") == 0) { clearCrumbs(); consolePrint("TRAIL CLEARED"); }
    consolePrint("TRAIL %s // %d POINTS%s", _prefs.crumb_off ? "PAUSED" : "RECORDING", _crumb_count, _sd_ok ? " (SAVED TO SD)" : "");
  } else if (strcmp(cmd, "sd") == 0) {
    if (!_sd_ok) sdBegin();
    if (!_sd_ok) consolePrint("NO SD CARD");
    else {
      if (!_map_inited) mapInit();
      if (_map_zmax >= 0) consolePrint("SD %.1fGB // MAP ZOOM %d-%d", SD.cardSize() / 1073741824.0, _map_zmin, _map_zmax);
      else consolePrint("SD %.1fGB // NO /maps FOLDER", SD.cardSize() / 1073741824.0);
    }
  } else if (strcmp(cmd, "pin") == 0) {
    // stored like the app's "set device PIN": 0 = a fresh random PIN every boot
    bool changed = false;
    if (strcmp(argl, "random") == 0) {
      _node_prefs->ble_pin = 0;
      changed = true;
    } else if (argl[0]) {
      long p = atol(argl);
      if (strlen(argl) == 6 && p >= 100000 && p <= 999999) {
        _node_prefs->ble_pin = (uint32_t)p;
        changed = true;
      } else {
        consolePrint("PIN MUST BE 6 DIGITS, 100000-999999");
      }
    }
    if (changed) the_mesh.savePrefs();
    consolePrint("BLE PIN NOW %06u", (unsigned)the_mesh.getBLEPin());
    if (_node_prefs->ble_pin) consolePrint("FIXED PIN %06u", (unsigned)_node_prefs->ble_pin);
    else consolePrint("RANDOM PIN EACH BOOT");
    if (changed) consolePrint("TAKES EFFECT AFTER reboot");
  } else if (strcmp(cmd, "ble") == 0) {
    if (argOn(argl)) enableBluetooth();
    else if (argOff(argl)) disableBluetooth();
    consolePrint("BLE %s", isBluetoothEnabled() ? "ON" : "OFF");
  } else if (strcmp(cmd, "gps") == 0) {
    if (argOn(argl)) setGPS(true);
    else if (argOff(argl)) setGPS(false);
    else if (strncmp(argl, "live", 4) == 0) {
      if (argOn(argl + 4 + (argl[4] == ' '))) _prefs.gps_always = 1;
      else if (argOff(argl + 4 + (argl[4] == ' '))) _prefs.gps_always = 0;
      savePrefs();
    }
    static const char* MODES[] = { "OFF (POWERED DOWN)", "FOLLOW (AWAKE)", "REST (ASLEEP)", "CHECKING" };
    consolePrint("GPS %s // %s", getGPSState() ? "ON" : "OFF", MODES[_gps_mode]);
    if (_prefs.gps_always) consolePrint("LIVE: NEVER RESTS (gps live off)");
    else consolePrint("RESTS AFTER 30MIN STILL, CHECKS /10MIN");
  } else if (strcmp(cmd, "power") == 0) {
    if (strcmp(argl, "cpu full") == 0) { _prefs.cpu_full = 1; savePrefs(); }
    else if (strcmp(argl, "cpu save") == 0) { _prefs.cpu_full = 0; savePrefs(); }
    int mv = getBattMilliVolts();
    consolePrint("BATT %.2fV %d%%", mv / 1000.0f, battPercent());
    if (_batt_n >= 3) {   // samples are 5 minutes apart
      float hours = (_batt_n - 1) * 5 / 60.0f;
      consolePrint("DRAIN %.0fMV/H OVER %.1fH", (_batt_hist[0] - _batt_hist[_batt_n - 1]) / hours, hours);
    } else {
      consolePrint("DRAIN: ASK AGAIN IN %d MIN", (3 - _batt_n) * 5);
    }
    static const char* MODES[] = { "OFF", "FOLLOW", "REST", "CHECK" };
    consolePrint("GPS %s%s  CPU %s", MODES[_gps_mode], _prefs.gps_always ? " (LIVE)" : "",
                 _prefs.cpu_full ? "FULL" : "80MHZ WHEN DARK");
    consolePrint("SCREEN OFF AFTER %us  courier %s", _prefs.dim_secs, _prefs.mule ? "ON" : "OFF");
    static const char* WHY[] = { "?", "POWER ON", "EXT PIN", "SOFTWARE", "PANIC", "INT WDT", "TASK WDT", "WDT",
                                 "DEEP SLEEP", "BROWNOUT", "SDIO", "USB", "JTAG" };
    int why = (int)esp_reset_reason();
    consolePrint("LAST RESTART: %s  UP %lus", why < 13 ? WHY[why] : "?", millis() / 1000);
  } else if (strcmp(cmd, "name") == 0) {
    if (*arg) {
      strncpy(_node_prefs->node_name, arg, sizeof(_node_prefs->node_name) - 1);
      _node_prefs->node_name[sizeof(_node_prefs->node_name) - 1] = 0;
      the_mesh.savePrefs();
      consolePrint("HANDLE NOW %s", _node_prefs->node_name);
      consolePrint("RUN advert flood TO ANNOUNCE IT");
    } else {
      consolePrint("HANDLE %s", _node_prefs->node_name);
    }

  /* ---- ui ---- */
  } else if (strcmp(cmd, "theme") == 0) {
    for (uint8_t i = 0; i < NUM_THEMES; i++) {
      if (strcmp(argl, THEMES[i].name) == 0) {
        _prefs.theme = i;
        savePrefs();
        applyTheme();
      }
    }
    consolePrint("THEME %s", THEMES[_prefs.theme].name);
  } else if (strcmp(cmd, "ball") == 0) {
    if (argl[0]) {
      int b = atoi(argl);
      if (b >= 1 && b <= 5) {
        _prefs.ball = b;
        _input.setSensitivity(b);
        savePrefs();
      }
    }
    consolePrint("TRACKBALL SENSITIVITY %u/5", _prefs.ball);
  } else if (strcmp(cmd, "crt") == 0) {
    if (argOn(argl)) _prefs.crt = 1;
    else if (argOff(argl)) _prefs.crt = 0;
    savePrefs();
    consolePrint("CRT SCANLINES %s", _prefs.crt ? "ON" : "OFF");
  } else if (strcmp(cmd, "dim") == 0) {
    if (argl[0]) {
      int s = atoi(argl);
      _prefs.dim_secs = (s < 0) ? 0 : (s > 255) ? 255 : s;
      savePrefs();
    }
    if (_prefs.dim_secs) consolePrint("SCREEN BLANKS AFTER %us", _prefs.dim_secs);
    else consolePrint("SCREEN STAYS ON");
  } else if (strcmp(cmd, "history") == 0) {
    if (argOn(argl)) { _prefs.history = 1; savePrefs(); if (!_hist_ok) wipeHistory(); }
    else if (argOff(argl)) { _prefs.history = 0; savePrefs(); }
    else if (strcmp(argl, "wipe") == 0) {
      memset(_msgs, 0, sizeof(Msg) * _max_msgs);
      memset(_pending, 0, sizeof(_pending));
      _next_seq = 1;
      wipeHistory();
      consolePrint("HISTORY WIPED");
    }
    int n = 0;
    for (int i = 0; i < _max_msgs; i++) if (_msgs[i].seq) n++;
    consolePrint("HISTORY %s // %d/%d MSGS", _prefs.history ? "SAVED" : "RAM ONLY", n, _max_msgs);
  } else if (strcmp(cmd, "clear") == 0 || strcmp(cmd, "cls") == 0) {
    _console_len = 0;
    _console_scroll = 0;
  } else if (strcmp(cmd, "reboot") == 0) {
    shutdown(true);
  } else if (strcmp(cmd, "off") == 0) {
    shutdown(false);
  } else if (cmd[0]) {
    consolePrint("UNKNOWN: %s // TRY help", cmd);
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

#ifdef CD_SERIAL_BRIDGE
// Drive the deck from a PC over USB serial, for testing without touching it.
// 0x01 dumps the framebuffer, 0x02-0x05 roll the trackball up/down/left/right,
// 0x06 clicks it, 0x07 holds it; anything else is typed as if on the keyboard.
void UITask::serialBridge() {
  for (int n = 0; n < 64 && Serial.available(); n++) {
    int c = Serial.read();
    switch (c) {
      case 0x01: {
        uint16_t* buf = _display ? _display->buffer() : NULL;
        if (buf == NULL) { Serial.println("#SHOT none"); break; }
        if (!_display->isOn()) { _display->turnOn(); }
        render();
        Serial.printf("\n#SHOT %d %d\n", CYBERDECK_W, CYBERDECK_H);
        const uint8_t* p = (const uint8_t*) buf;
        size_t left = CYBERDECK_W * CYBERDECK_H * 2;
        while (left > 0) {
          size_t chunk = left < 4096 ? left : 4096;
          size_t w = Serial.write(p, chunk);
          p += w;
          left -= w;
          if (w == 0) delay(1);
        }
        Serial.println("\n#END");
        break;
      }
      case 0x02: handleKey(TKEY_UP); break;
      case 0x03: handleKey(TKEY_DOWN); break;
      case 0x04: handleKey(TKEY_LEFT); break;
      case 0x05: handleKey(TKEY_RIGHT); break;
      case 0x06: handleKey(TKEY_CLICK); break;
      case 0x07: handleKey(TKEY_HOLD); break;
      case 0x11:     // straight to the SYS console, so typed test input can never become a message
        if (_display && !_display->isOn()) _display->turnOn();
        _auto_off = millis() + _prefs.dim_secs * 1000UL;
        if (_screen == SCR_PAGE) ackPage();
        _screen = SCR_LIST;
        _tab = TAB_SYS;
        _node_mode = NM_VIEW;
        _cmd_len = 0;
        _cmd_buf[0] = 0;
        _dirty = true;
        break;
      case 0x14:     // list one folder: 0x14 <path>\n
      case 0x13: {   // send every file in one folder: 0x13 <path>\n
        String path = bridgeLine();
        if (!_sd_ok) sdBegin();
        if (!_sd_ok) { Serial.println("\n#ERR no card"); break; }
        if (c == 0x14) sdList(path); else sdSendDir(path);
        break;
      }
      case 0x12: {   // walk the SD card
        if (!_sd_ok) sdBegin();
        if (!_sd_ok) { Serial.println("\n#SD none"); break; }
        Serial.printf("\n#SD type=%d size=%lluMB used=%lluMB\n", SD.cardType(), SD.cardSize() >> 20, SD.usedBytes() >> 20);
        int lines = 0;
        sdWalk("/", 0, lines);
        Serial.println("#END");
        break;
      }
      case 0x10: {   // pretend a page just arrived, to see the pager without a second deck
        static const uint8_t nobody[6] = { 0 };
        Msg* m = addMsg(CONV_DM, 0, nobody, "TEST", CD_PAGE_PREFIX " gate left open at north paddock", false);
        if (m) { m->flags |= MF_PAGE; m->unread = 1; startPage(m); }
        break;
      }
      case '\n': break;
      default: if (c > 0) handleKey(c); break;
    }
  }
}
#endif

void UITask::loop() {
  int ev = user_btn.check();
  if (ev == BUTTON_EVENT_CLICK) handleKey(TKEY_CLICK);
  else if (ev == BUTTON_EVENT_LONG_PRESS) handleKey(TKEY_HOLD);

  for (int n = 0; n < 8; n++) {   // bounded, so a fast trackball roll can't starve the mesh
    int key = _input.read();
    if (key == TKEY_NONE) break;
    handleKey(key);
  }
#ifdef CD_SERIAL_BRIDGE
  serialBridge();
#endif

  checkPending();
  if (_nodes_dirty && (long)(millis() - _nodes_refresh_at) >= 0) {
    refreshNodes();
    _dirty = true;
  }

  if (_display == NULL) return;
  unsigned long now = millis();

  // a page keeps sounding (and the screen on) until someone acknowledges it
  if (_screen == SCR_PAGE) {
    Msg* pm = findMsg(_page_seq);
    if (pm == NULL) { _screen = _page_return; _dirty = true; }
    else if ((long)(now - _page_next_tone) >= 0) {
      _page_next_tone = now + PAGE_TONE_EVERY;
      if (now - _page_started < PAGE_TONE_FOR) beep(Tones::PAGE);
      _kbl_flash = !_kbl_flash;
      _input.setBacklight(_kbl_flash ? 255 : 0);
    }
    if (!_display->isOn()) _display->turnOn();
  }

  if ((now - _clk_last_ms) >= 1000) watchClock();
  if ((long)(now - _mule_next_tick) >= 0) {
    _mule_next_tick = now + 2000;
    muleTick();
  }
  if ((long)(now - _crumb_check) >= 0) {   // breadcrumbs, and keep the map and GPS page live
    _crumb_check = now + 1000;
    gpsTick();
    powerTick();
    recordCrumb();
    if ((_screen == SCR_LIST && _tab == TAB_MAP) || _screen == SCR_GPS) _dirty = true;
  }

  if (_display->isOn() && _prefs.dim_secs && _screen != SCR_BOOT && _screen != SCR_PAGE && (long)(now - _auto_off) >= 0) {
    _display->turnOff();
  }
  if (!_display->isOn()) return;

  if (_screen == SCR_BOOT) {
    if (now - _boot_started > BOOT_MILLIS) {
      _screen = SCR_LIST;
      _glitch_until = now + 250;
      _dirty = true;
    } else if ((long)(now - _next_render) >= 0) {
      render();
      _display->pushRows(0, CYBERDECK_H);
      _next_render = now + 33;
    }
    return;
  }

  bool glitching = (long)(_glitch_until - now) > 0;
  unsigned long frame_every = (_screen == SCR_PAGE) ? 250 : 1000;
  if (((_dirty || glitching) && now - _last_render >= 40) || (long)(now - _next_render) >= 0) {
    _dirty = false;
    _last_render = now;
    _next_render = now + frame_every;   // clock, alert expiry, page flashing
    _cursor_on = true;
    _next_blink = now + 500;
    render();
    _display->pushRows(0, CYBERDECK_H);
  } else if ((long)(now - _next_blink) >= 0 && _display->buffer() != NULL && _screen != SCR_PAGE) {
    _cursor_on = !_cursor_on;
    _next_blink = now + 500;
    render();
    _display->pushRows(INPUT_Y - 2, ROW_H + 4);   // only the prompt line changes
  }
}

/* ---------------------------------------- drawing -------------------------------------- */

void UITask::text(int x, int y, uint16_t col, const char* s) {
  uint16_t* buf = _display->buffer();
  Adafruit_GFX& g = _display->gfx();
  for (; *s && x <= SCR_W - CHAR_W; s++, x += CHAR_W) {
    if (x < 0) continue;
    const uint8_t* glyph = &SPLEEN_8X16_CP437[(uint8_t)*s * 16];
    for (int r = 0; r < 16; r++) {
      int py = y + r;
      uint8_t bits = glyph[r];
      if (bits == 0 || py < 0 || py >= CYBERDECK_H) continue;
      if (buf) {
        uint16_t* px = buf + py * CYBERDECK_W + x;
        for (int c = 0; c < 8; c++) if (bits & (0x80 >> c)) px[c] = col;
      } else {
        for (int c = 0; c < 8; c++) if (bits & (0x80 >> c)) g.drawPixel(x + c, py, col);
      }
    }
  }
}

// the 8x16 font blown up, each pixel a block with a dark line under it, like a CRT
void UITask::bigText(int x, int y, int scale, uint16_t col, const char* s) {
  Adafruit_GFX& g = _display->gfx();
  int bh = scale > 2 ? scale - 1 : scale;
  for (; *s; s++, x += CHAR_W * scale) {
    const uint8_t* glyph = &SPLEEN_8X16_CP437[(uint8_t)*s * 16];
    for (int r = 0; r < 16; r++) {
      uint8_t bits = glyph[r];
      for (int c = 0; c < 8; c++) {
        if (bits & (0x80 >> c)) g.fillRect(x + c * scale, y + r * scale, scale, bh, col);
      }
    }
  }
}

void UITask::textf(int x, int y, uint16_t col, const char* fmt, ...) {
  char buf[COLS + 8];
  va_list args;
  va_start(args, fmt);
  vsnprintf(buf, sizeof(buf), fmt, args);
  va_end(args);
  text(x, y, col, buf);
}

void UITask::stext(int x, int y, uint16_t col, const char* s) {
  Adafruit_GFX& g = _display->gfx();
  g.setTextSize(1);
  g.setTextColor(col);
  g.setCursor(x, y);
  g.print(s);
}

int UITask::battPercent() const {
  int mv = getBattMilliVolts();
  int pct = (mv - 3300) * 100 / (4200 - 3300);
  return pct < 0 ? 0 : pct > 100 ? 100 : pct;
}

void UITask::fmtTime(char* dest, uint32_t epoch, bool with_secs) const {
  if (epoch < 1600000000UL) {
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
  } else if (_screen == SCR_PAGE) {
    drawPage();
  } else {
    drawHeader();
    switch (_screen) {
      case SCR_LIST:
        drawTabs();
        if (_tab == TAB_CHAN) drawChanList();
        else if (_tab == TAB_NODES) drawNodeList(false);
        else if (_tab == TAB_HEARD) drawNodeList(true);
        else if (_tab == TAB_MAP) drawMap();
        else if (_tab == TAB_RF) drawRf();
        else drawSys();
        break;
      case SCR_CHAT: drawChat(); break;
      case SCR_NODE: drawNode(); break;
      case SCR_GPS: drawGps(); break;
      case SCR_MULE: drawMule(); break;
      case SCR_ADMIN: drawAdmin(); break;
      default: break;
    }

    // alert banner, over whatever is underneath
    if (_alert[0] && (long)(_alert_until - millis()) > 0) {
      char a[COLS + 1];
      snprintf(a, sizeof(a), "<< %.32s >>", _alert);
      int w = strlen(a) * CHAR_W;
      int x = (SCR_W - w) / 2;
      g.fillRect(x - 6, 94, w + 12, 30, c_bg);
      g.drawRect(x - 6, 94, w + 12, 30, c_alert);
      g.drawRect(x - 4, 96, w + 8, 26, c_alert);
      text(x, 101, c_alert, a);
    }
  }

  if (_prefs.crt) crtPass();
  if ((long)(_glitch_until - millis()) > 0) glitchPass();
}

// cheap, repeatable noise for the boot effects
static uint32_t hash32(uint32_t x) {
  x ^= x >> 16; x *= 0x7feb352d; x ^= x >> 15; x *= 0x846ca68b; x ^= x >> 16;
  return x;
}

void UITask::drawBoot() {
  Adafruit_GFX& g = _display->gfx();
  uint32_t t = millis() - _boot_started;

  // 1. digital rain, thinning out as the logo resolves
  if (t < 1700) {
    int rows = SCR_H / ROW_H + 1;
    for (int c = 0; c < COLS; c++) {
      uint32_t h = hash32(c * 7919 + 17);
      if (t > 900 && (h % 800) < t - 900) continue;   // columns drop out
      int speed = 6 + (h % 9);                          // rows per second
      int head = (int)((t * speed) / 1000 + (h >> 8) % rows) % (rows + 10);
      for (int k = 0; k < 9; k++) {
        int r = head - k;
        if (r < 0 || r >= rows) continue;
        char ch[2] = { (char)(33 + hash32(c * 131 + r * 17 + (t / 90) * (k == 0)) % 90), 0 };
        uint16_t col = (k == 0) ? c_hi : (k < 4) ? c_fg : c_dim;
        text(c * CHAR_W, r * ROW_H, col, ch);
      }
    }
  }

  // 2. the logo, wiped in from the top with a bright scan line
  if (t > 600) {
    const char* logo = "MESHCORE";
    const int lx = (SCR_W - 8 * CHAR_W * 3) / 2, ly = 8, lh = 102;   // logo block: 3x, 2x, then the build line
    g.fillRect(0, ly - 4, SCR_W, lh + 4, c_bg);
    bigText(lx, ly, 3, c_fg, logo);
    int dx = (SCR_W - 6 * CHAR_W * 2) / 2;
    bigText(dx, ly + 50, 2, c_hi, "//DECK");

    const char* build = "FLOCK AND FURROW BUILD";
    int bw = (strlen(build) + 6) * CHAR_W;   // with "-= " and " =-" either side
    int bx = (SCR_W - bw) / 2, by = ly + 85;
    text(bx, by, c_dim, "-=");
    text(bx + 3 * CHAR_W, by, c_fg, build);
    text(bx + bw - 2 * CHAR_W, by, c_dim, "=-");

    int reveal = (int)((t - 600) * 90 / 1000) - 4;   // px of logo shown
    if (reveal < lh) {
      g.fillRect(0, ly + reveal, SCR_W, lh - reveal, c_bg);
      if (reveal >= 0) g.drawFastHLine(0, ly + reveal, SCR_W, c_hi);
    }
  }

  // 3. power-on self test
  if (t > 1700) {
    char label[48];
    struct { const char* l; bool ok; } checks[6];
    static char l0[40], l1[40], l2[40], l3[40], l4[40], l5[40];
    snprintf(l0, sizeof(l0), "RF  %.3f SF%u BW%g", _node_prefs->freq, _node_prefs->sf, _node_prefs->bw);
    snprintf(l1, sizeof(l1), "MEM PSRAM %uK", (unsigned)(ESP.getPsramSize() / 1024));
    snprintf(l2, sizeof(l2), "KBD I2C 0x55");
    snprintf(l3, sizeof(l3), "SND I2S SPEAKER VOL %u", _prefs.volume);
    int n = 0;
    for (int i = 0; i < _max_msgs; i++) if (_msgs[i].seq) n++;
    snprintf(l4, sizeof(l4), "LOG %d MSGS %s", n, _prefs.history ? "RESTORED" : "(RAM)");
    snprintf(l5, sizeof(l5), "NET %d NODES %d CHANNELS", _num_nodes, _num_chans);
    checks[0] = { l0, true };
    checks[1] = { l1, ESP.getPsramSize() > 0 };
    checks[2] = { l2, _input.hasKeyboard() };
    checks[3] = { l3, _audio.ready() };
    checks[4] = { l4, _max_msgs > 0 };
    checks[5] = { l5, true };

    int shown = (int)(t - 1700) / 260;
    for (int i = 0; i < 6 && i <= shown; i++) {
      int y = 115 + i * ROW_H;
      bool done = i < shown;
      snprintf(label, sizeof(label), "%-30.30s", checks[i].l);
      for (char* p = label; *p; p++) if (*p == ' ' && p > label + (int)strlen(checks[i].l)) *p = '.';
      text(4, y, c_fg, label);
      if (done) text(4 + 32 * CHAR_W, y, checks[i].ok ? c_hi : c_alert, checks[i].ok ? "[ OK ]" : "[FAIL]");
      else if ((t / 120) % 2) text(4 + 32 * CHAR_W, y, c_dim, "[ .. ]");
    }

    // progress bar
    int pct = (int)((t - 1700) * 100 / (BOOT_MILLIS - 2300));
    if (pct > 100) pct = 100;
    g.drawRect(4, 212, SCR_W - 8, 10, c_dim);
    int w = (SCR_W - 12) * pct / 100;
    for (int x = 0; x < w; x += 4) g.fillRect(6 + x, 214, 3, 6, c_fg);

    const uint8_t* pk = the_mesh.self_id.pub_key;
    char foot[64];
    if (isBluetoothEnabled()) snprintf(foot, sizeof(foot), "%s  ID %02X%02X%02X%02X  BLE PIN %06u", _node_prefs->node_name, pk[0], pk[1], pk[2], pk[3], (unsigned)the_mesh.getBLEPin());
    else snprintf(foot, sizeof(foot), "%s  ID %02X%02X%02X%02X  BLE OFF", _node_prefs->node_name, pk[0], pk[1], pk[2], pk[3]);
    stext(4, 228, c_dim, foot);
  }

  // 4. the handshake
  if (t > BOOT_MILLIS - 600) {
    const char* msg = " ACCESS GRANTED ";
    int w = strlen(msg) * CHAR_W * 2;
    int x = (SCR_W - w) / 2, y = 128;
    bool on = ((t / 100) % 2) == 0;
    g.fillRect(x - 6, y - 6, w + 12, 32 + 12, on ? c_fg : c_bg);
    g.drawRect(x - 6, y - 6, w + 12, 32 + 12, c_hi);
    bigText(x, y, 2, on ? c_bg : c_fg, msg);
  }
}

void UITask::drawHeader() {
  Adafruit_GFX& g = _display->gfx();
  g.fillRect(0, 0, SCR_W, 16, c_fg);
  text(2, 0, c_bg, G_DARK "MESHCORE//DECK");

  char clock[12];
  fmtTime(clock, rtc_clock.getCurrentTime());
  char right[64];
  int pos = 0;
  int unread = totalUnread();
  if (unread) pos += snprintf(right + pos, sizeof(right) - pos, "MSG%d ", unread);
  if (_node_prefs->isRepeatEn()) pos += snprintf(right + pos, sizeof(right) - pos, "RPT ");
  if (_prefs.mule) pos += snprintf(right + pos, sizeof(right) - pos, "MULE ");
  if (isBluetoothEnabled()) pos += snprintf(right + pos, sizeof(right) - pos, hasConnection() ? "BLE+ " : "BLE ");
  if (getGPSState()) pos += snprintf(right + pos, sizeof(right) - pos, "GPS ");
  snprintf(right + pos, sizeof(right) - pos, "%d%% %s%s", battPercent(), clock, clockValid() ? "" : "?");
  stext(SCR_W - 3 - strlen(right) * SMALL_W, 4, c_bg, right);
}

void UITask::drawTabs() {
  Adafruit_GFX& g = _display->gfx();
  int x = 0;
  for (int t = 0; t < TAB_COUNT; t++) {
    char label[16];
    int unread = 0;
    if (t == TAB_CHAN) {
      for (int i = 0; i < _max_msgs; i++) if (_msgs[i].seq && _msgs[i].unread && _msgs[i].kind == CONV_CHANNEL) unread++;
    } else if (t == TAB_NODES) {
      for (int i = 0; i < _max_msgs; i++) if (_msgs[i].seq && _msgs[i].unread && _msgs[i].kind == CONV_DM) unread++;
    }
    snprintf(label, sizeof(label), unread ? " %s*" : " %s ", TAB_NAMES[t]);
    int w = strlen(label) * CHAR_W + CHAR_W / 2;
    if (t == _tab) {
      g.fillRect(x, TABS_Y - 1, w, ROW_H + 1, c_fg);
      text(x, TABS_Y, c_bg, label);
    } else {
      g.drawRect(x, TABS_Y - 1, w, ROW_H + 1, c_dim);
      text(x, TABS_Y, unread ? c_hi : c_dim, label);
    }
    x += w + 3;
  }
  g.drawFastHLine(0, TABS_Y + ROW_H + 1, SCR_W, c_dim);
}

void UITask::drawFooter(const char* hint) {
  Adafruit_GFX& g = _display->gfx();
  g.drawFastHLine(0, FOOT_Y - 2, SCR_W, c_dim);
  stext(2, FOOT_Y, c_dim, hint);
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
    text(4, BODY_Y + ROW_H, c_dim, "NO CHANNELS YET.");
    text(4, BODY_Y + 2 * ROW_H, c_dim, "/ THEN: join #name  OR  newchan");
  }
  int top = listTop(_num_chans, sel, BODY_ROWS);
  for (int r = 0; r < BODY_ROWS && top + r < _num_chans; r++) {
    int i = top + r, y = BODY_Y + r * ROW_H;
    int idx = _chan_idx[i];
    ChannelDetails cd;
    the_mesh.getChannel(idx, cd);
    char name[34];
    chanLabel(name, sizeof(name), cd.name);

    // newest message in this channel
    Msg* last = NULL;
    for (uint32_t s = _next_seq - 1; s > 0 && s + _max_msgs >= _next_seq; s--) {
      Msg* m = findMsg(s);
      if (m && msgInConv(*m, CONV_CHANNEL, idx, NULL)) { last = m; break; }
    }
    int unread = unreadFor(CONV_CHANNEL, idx, NULL);
    bool is_sel = i == sel;
    if (is_sel) g.fillRect(0, y, SCR_W, ROW_H, c_fg);

    textf(0, y, is_sel ? c_bg : c_fg, "%s%-19.19s", is_sel ? G_RIGHT : " ", name);
    if (isMuted(idx)) text(20 * CHAR_W, y, is_sel ? c_bg : c_dim, "\x0E");   // a muted note
    if (unread) textf(22 * CHAR_W, y, is_sel ? c_bg : c_hi, "%2d NEW", unread);
    if (last) {
      char t[8];
      fmtTime(t, last->ts);
      text(35 * CHAR_W, y, is_sel ? c_bg : c_dim, t);
    }
  }
  drawFooter(G_UP G_DOWN " SELECT  RET OPEN  " G_LEFT G_RIGHT " TAB  / CMD  A-Z JUMP");
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
    text(4, BODY_Y + ROW_H, c_dim, recent ? "NOTHING HEARD YET." : "NO CONTACTS YET.");
    text(4, BODY_Y + 2 * ROW_H, c_dim, recent ? "TRY advert flood ON THE SYS TAB." : "WAITING FOR ADVERTS...");
  }
  int top = listTop(count, sel, BODY_ROWS);
  for (int r = 0; r < BODY_ROWS && top + r < count; r++) {
    int i = top + r, y = BODY_Y + r * ROW_H;
    bool is_sel = i == sel;
    uint16_t col = is_sel ? c_bg : c_fg, dim = is_sel ? c_bg : c_dim;
    if (is_sel) g.fillRect(0, y, SCR_W, ROW_H, c_fg);

    char name[32], ago[8], hops[8];
    const char* tag = "";
    uint8_t path_len;
    uint32_t when;
    int unread = 0;
    bool in = false;
    if (recent) {
      sanitize(name, sizeof(name), heard[i].name);
      path_len = heard[i].path_len;
      when = heard[i].recv_timestamp;
      const Node* n = findNode(heard[i].pubkey_prefix);
      if (n) tag = typeTag(n->type);
    } else {
      const Node& n = _nodes[i];
      strcpy(name, n.name);
      tag = typeTag(n.type);
      path_len = n.path_len;
      when = n.lastmod;
      unread = unreadFor(CONV_DM, 0, n.pub);
      in = isLoggedIn(n.pub);
    }
    fmtAgo(ago, when);
    if (path_len == OUT_PATH_UNKNOWN) strcpy(hops, "FLOOD");
    else if ((path_len & 63) == 0) strcpy(hops, "DIRECT");
    else snprintf(hops, sizeof(hops), "%dHOP", path_len & 63);

    text(0, y, col, is_sel ? G_RIGHT : " ");
    textf(1 * CHAR_W, y, dim, "%-3s", tag);
    textf(5 * CHAR_W, y, col, "%-17.17s", name);
    if (unread) text(23 * CHAR_W, y, is_sel ? c_bg : c_hi, "*");
    else if (in) text(23 * CHAR_W, y, is_sel ? c_bg : c_hi, "\x0F");
    textf(25 * CHAR_W, y, dim, "%6s", hops);
    textf(35 * CHAR_W, y, dim, "%4s", ago);
  }
  drawFooter(G_UP G_DOWN " SELECT  RET OPEN  " G_LEFT G_RIGHT " TAB  / CMD");
}

void UITask::drawRf() {
  int y = BODY_Y;
  textf(0, y, c_fg, "NF%4d RSSI%4d SNR%5.1f", radio_driver.getNoiseFloor(), (int)radio_driver.getLastRSSI(), radio_driver.getLastSNR());
  y += ROW_H;
  textf(0, y, c_dim, "RX %-6u TX %-6u ERR %-4u", (unsigned)radio_driver.getPacketsRecv(), (unsigned)radio_driver.getPacketsSent(),
        (unsigned)radio_driver.getPacketsRecvErrors());
  if (_prefs.mule || _node_prefs->isRepeatEn()) {
    char m[24];
    snprintf(m, sizeof(m), "%s%s", _node_prefs->isRepeatEn() ? "RPT " : "", _prefs.mule ? "MULE" : "");
    text(SCR_W - strlen(m) * CHAR_W, y, c_hi, m);
  }
  y += ROW_H + 2;
  _display->gfx().drawFastHLine(0, y - 2, SCR_W, c_dim);
  stext(0, y, c_dim, " AGE  TYPE    ROUTE  HOPS   SNR   RSSI  LEN");
  y += 10;

  if (_rf_count == 0) text(4, y + 4, c_dim, "LISTENING...");
  static const char* ROUTES[] = { "T-FLD", "FLOOD", "DIRCT", "T-DIR" };
  for (int k = 0; k < 8 && k < _rf_count; k++) {
    const RfEntry& e = _rf[(_rf_head - 1 - k + CD_RF_LOG) % CD_RF_LOG];
    uint32_t age = (millis() - e.at) / 1000;
    char a[8];
    if (age < 100) snprintf(a, sizeof(a), "%2us", (unsigned)age);
    else if (age < 6000) snprintf(a, sizeof(a), "%2um", (unsigned)(age / 60));
    else strcpy(a, "old");
    uint16_t col = (e.type == PAYLOAD_TYPE_TXT_MSG || e.type == PAYLOAD_TYPE_GRP_TXT) ? c_hi : c_fg;
    textf(0, y, k == 0 ? c_hi : col, "%4s %-6s %-5s %3u %5.1f %4d %3u", a, payloadName(e.type), ROUTES[e.route & 3], e.hops,
          e.snr4 / 4.0f, e.rssi, e.len);
    y += ROW_H;
  }
  char foot[48];
  snprintf(foot, sizeof(foot), "%u PACKETS HEARD   " G_LEFT G_RIGHT " TAB", (unsigned)_rf_total);
  drawFooter(foot);
}

void UITask::drawSys() {
  Adafruit_GFX& g = _display->gfx();
  int y = BODY_Y;
  const uint8_t* pk = the_mesh.self_id.pub_key;

  text(0, y, c_dim, "ID");
  textf(5 * CHAR_W, y, c_fg, "%-22.22s %02X%02X%02X%02X", _node_prefs->node_name, pk[0], pk[1], pk[2], pk[3]);
  y += ROW_H;
  text(0, y, c_dim, "RF");
  textf(5 * CHAR_W, y, c_fg, "%.3f BW%g SF%u CR%u %ddB", _node_prefs->freq, _node_prefs->bw, _node_prefs->sf,
        _node_prefs->cr, _node_prefs->tx_power_dbm);
  y += ROW_H;
  unsigned long up = millis() / 1000;
  text(0, y, c_dim, "PWR");
  char ble[16];
  if (isBluetoothEnabled()) snprintf(ble, sizeof(ble), "BLE %06u", (unsigned)the_mesh.getBLEPin());
  else strcpy(ble, "BLE OFF");
  textf(5 * CHAR_W, y, c_fg, "%.2fV %d%% UP%luh%02lum %s", getBattMilliVolts() / 1000.0f, battPercent(), up / 3600, (up / 60) % 60, ble);
  y += ROW_H;
  text(0, y, c_dim, "MODE");
  const char* sc = the_mesh.uiDefaultScope();
  textf(5 * CHAR_W, y, c_fg, "RPT:%s MULE:%s CLK:%s SC:%.6s", _node_prefs->isRepeatEn() ? "ON" : "OFF", _prefs.mule ? "ON" : "OFF",
        _clock_src == CLK_NONE ? "?" : _clock_src == CLK_MESH ? "MESH" : "OK", sc[0] ? sc : "*");
  y += ROW_H + 3;
  g.drawFastHLine(0, y - 2, SCR_W, c_dim);

  if (_console_len == 0) text(0, y, c_dim, "TYPE help FOR COMMANDS");
  int first = _console_len - CD_CONSOLE_ROWS - _console_scroll;
  if (first < 0) first = 0;
  for (int r = 0; r < CD_CONSOLE_ROWS && first + r < _console_len; r++) {
    const char* line = _console[first + r];
    text(0, y + r * ROW_H, line[0] == '>' ? c_dim : c_hi, line);
  }
  if (_console_scroll > 0) {
    char sc[12];
    snprintf(sc, sizeof(sc), G_DOWN " +%d", _console_scroll);
    stext(SCR_W - 3 - strlen(sc) * SMALL_W, y + 1, c_alert, sc);
  }
  drawInput(INPUT_Y, "root@deck:~#", _cmd_buf, _cmd_len);
  drawFooter("help FOR CMDS  RET RUN  " G_UP G_DOWN " SCROLL  " G_LEFT G_RIGHT " TAB");
}

void UITask::drawInput(int y, const char* prompt, const char* buf, int len, bool masked) {
  Adafruit_GFX& g = _display->gfx();
  g.drawRect(0, y - 2, SCR_W, ROW_H + 2, c_dim);
  text(2, y - 1, c_hi, prompt);
  int px = 2 + (strlen(prompt) + 1) * CHAR_W;
  int room = (SCR_W - px - 2) / CHAR_W - 1;
  const char* shown = (len > room) ? buf + (len - room) : buf;   // keep the end of long input visible
  if (masked) {
    char stars[CD_INPUT_MAX + 1];
    int n = strlen(shown);
    memset(stars, '*', n);
    stars[n] = 0;
    text(px, y - 1, c_fg, stars);
  } else {
    text(px, y - 1, c_fg, shown);
  }
  if (_cursor_on) g.fillRect(px + strlen(shown) * CHAR_W, y, CHAR_W - 1, ROW_H - 3, c_fg);
}

void UITask::drawChat() {
  Adafruit_GFX& g = _display->gfx();

  // title strip, where the tabs usually are
  const Node* peer = (_conv_kind == CONV_DM) ? findNode(_conv_peer) : NULL;
  bool room = peer && peer->type == ADV_TYPE_ROOM;
  if (_conv_kind == CONV_CHANNEL) textf(0, TABS_Y, c_hi, "%.24s%s", _conv_name, isMuted(_conv_chan) ? " \x0E" : "");
  else textf(0, TABS_Y, c_hi, room ? "ROOM // %.24s" : "LINK // %.24s", _conv_name);
  int unread_elsewhere = totalUnread();
  if (unread_elsewhere) {
    char u[16];
    snprintf(u, sizeof(u), "%d UNREAD " G_LEFT, unread_elsewhere);
    stext(SCR_W - 3 - strlen(u) * SMALL_W, TABS_Y + 4, c_alert, u);
  }
  g.drawFastHLine(0, TABS_Y + ROW_H + 1, SCR_W, c_dim);

  // lay out lines bottom-up: newest message last
  struct Line { const Msg* m; uint16_t start; uint8_t len; };
  static Line lines[BODY_ROWS + 200];
  const int max_lines = sizeof(lines) / sizeof(lines[0]);
  int n = 0;
  int want = (_chat_sel >= 0) ? max_lines : BODY_ROWS + _scroll;   // a picked message may be far back
  if (want > max_lines) want = max_lines;
  int msgs = 0, sel_first = -1, sel_last = -1;

  static char full[COLS * 8];
  for (uint32_t s = _next_seq - 1; s > 0 && s + _max_msgs >= _next_seq && n < want; s--) {
    const Msg* m = findMsg(s);
    if (m == NULL || !msgInConv(*m, _conv_kind, _conv_chan, _conv_peer)) continue;

    // break this message into wrapped segments, then add them last-first
    char t[8];
    fmtTime(t, m->ts);
    int flen = snprintf(full, sizeof(full), "%s %s: %s", t, m->from, m->text);
    if (flen >= (int)sizeof(full)) flen = sizeof(full) - 1;
    uint16_t seg_start[8];
    uint8_t seg_len[8];
    int segs = 0, pos = 0;
    while (pos < flen && segs < 8) {
      int width = (segs == 0) ? COLS : COLS - 2;   // continuation lines are indented
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
  // the picked message (counted newest first): find its lines and keep them on screen
  if (_chat_sel >= 0) {
    int ord = -1;
    const Msg* prev = NULL;
    if (_chat_sel_seq) {   // stay on the same message even if new ones arrive below it
      for (int li = 0; li < n; li++) {
        if (lines[li].m != prev) { prev = lines[li].m; ord++; if (prev->seq == _chat_sel_seq) { _chat_sel = ord; break; } }
      }
      ord = -1;
      prev = NULL;
    }
    for (int li = 0; li < n; li++) {
      if (lines[li].m != prev) { prev = lines[li].m; ord++; msgs = ord + 1; }
      if (ord == _chat_sel) { if (sel_first < 0) sel_first = li; sel_last = li; }
    }
    if (sel_first < 0 && msgs > 0) {   // ran off the oldest end: stay on the oldest
      _chat_sel = msgs - 1;
      prev = NULL; ord = -1;
      for (int li = 0; li < n; li++) {
        if (lines[li].m != prev) { prev = lines[li].m; ord++; }
        if (ord == _chat_sel) { if (sel_first < 0) sel_first = li; sel_last = li; }
      }
    }
    if (sel_first >= 0) {
      _chat_sel_seq = lines[sel_first].m->seq;
      if (sel_last >= _scroll + BODY_ROWS) _scroll = sel_last - BODY_ROWS + 1;
      if (sel_first < _scroll) _scroll = sel_first;
    } else {
      _chat_sel = -1;
    }
  } else {
    _scroll = 0;
  }
  if (_scroll > n - BODY_ROWS) _scroll = (n > BODY_ROWS) ? n - BODY_ROWS : 0;

  if (n == 0) {
    text(4, BODY_Y + ROW_H, c_dim, _conv_kind == CONV_CHANNEL ? "NO TRAFFIC YET. SAY SOMETHING." : "LINK OPEN. NO MESSAGES YET.");
    text(4, BODY_Y + 2 * ROW_H, c_dim, "/page TO RAISE THE ALARM.");
  }

  // each line re-derives its text from the message, so only a pointer and offsets were stored
  for (int r = 0; r < BODY_ROWS; r++) {
    int li = _scroll + (BODY_ROWS - 1 - r);
    if (li >= n) continue;
    const Line& L = lines[li];
    const Msg* m = L.m;
    if (li >= sel_first && li <= sel_last && sel_first >= 0) {   // the picked message gets a bar and a box
      int yy = BODY_Y + r * ROW_H;
      _display->gfx().fillRect(0, yy, 3, ROW_H, c_hi);
      if (li == sel_last) _display->gfx().drawFastHLine(0, yy, SCR_W, c_dim);
      if (li == sel_first) _display->gfx().drawFastHLine(0, yy + ROW_H - 1, SCR_W, c_dim);
    }
    char t[8];
    fmtTime(t, m->ts);
    int flen = snprintf(full, sizeof(full), "%s %s: %s", t, m->from, m->text);
    if (flen >= (int)sizeof(full)) flen = sizeof(full) - 1;

    int y = BODY_Y + r * ROW_H;
    int x = (L.start > 0) ? 2 * CHAR_W : 0;
    char seg[COLS + 1];
    int len = L.len < COLS ? L.len : COLS;
    memcpy(seg, &full[L.start], len);
    seg[len] = 0;

    uint16_t body_col = m->outgoing ? c_hi : c_fg;
    if (m->status == ST_FAILED || (m->flags & MF_PAGE)) body_col = c_alert;
    else if (m->flags & MF_KEYWORD) body_col = c_hi;
    if (m->flags & MF_PAGE) _display->gfx().fillRect(0, y, 2, ROW_H, c_alert);
    if (L.start == 0) {
      // "time from:" prefix in its own colours
      int tlen = strlen(t), plen = tlen + 1 + strlen(m->from) + 1;
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

    // a message's final segment sits nearest the bottom: delivery state or route goes after it
    bool last_line = li == 0 || lines[li - 1].m != m;
    int end_x = x + len * CHAR_W;
    if (m->outgoing && last_line) {
      char mark[16] = "";
      if (m->status == ST_SENDING) strcpy(mark, " ...");
      else if (m->status == ST_FAILED) strcpy(mark, " FAIL");
      else if (m->status == ST_DELIVERED && m->rtt_ds) snprintf(mark, sizeof(mark), " " G_CHECK "%u.%us", m->rtt_ds / 10, m->rtt_ds % 10);
      else if (m->status == ST_DELIVERED) strcpy(mark, " " G_CHECK);
      if ((m->flags & MF_PAGE) && (m->flags & MF_ACKED)) strcat(mark, " ACK'D");
      if (end_x + (int)strlen(mark) * CHAR_W <= SCR_W) text(end_x, y, m->status == ST_FAILED ? c_alert : c_dim, mark);
      else if (mark[0]) stext(SCR_W - 2 - strlen(mark) * SMALL_W, y + 5, c_dim, mark);
    } else if (!m->outgoing && last_line && (m->hops != 0xFF || m->rtt_ds)) {
      // route, and how late it arrived if it was carried in
      char tag[28] = "", late[12] = "";
      if (m->rtt_ds >= 1440) snprintf(late, sizeof(late), "+%ud", m->rtt_ds / 1440);
      else if (m->rtt_ds >= 60) snprintf(late, sizeof(late), "+%uh%02u", m->rtt_ds / 60, m->rtt_ds % 60);
      else if (m->rtt_ds) snprintf(late, sizeof(late), "+%um", m->rtt_ds);
      if (m->hops != 0xFF) snprintf(tag, sizeof(tag), "%s%s%dh %+ddB", late, late[0] ? " " : "", m->hops & 63, m->snr4 / 4);
      else strcpy(tag, late);
      int tx = SCR_W - 2 - strlen(tag) * SMALL_W;
      if (end_x + SMALL_W <= tx) stext(tx, y + 5, m->rtt_ds ? c_alert : c_dim, tag);
      else if (late[0]) stext(SCR_W - 2 - strlen(late) * SMALL_W, y + 5, c_alert, late);
    }
  }
  if (_scroll > 0) {
    char sc[12];
    snprintf(sc, sizeof(sc), G_DOWN " +%d", _scroll);
    stext(SCR_W - 3 - strlen(sc) * SMALL_W, BODY_Y + 1, c_alert, sc);
  }

  char prompt[16];
  snprintf(prompt, sizeof(prompt), "%.8s>", _node_prefs->node_name);
  drawInput(INPUT_Y, prompt, _input_buf, _input_len);
  char hint[64];
  int max_len = CD_INPUT_MAX - (_conv_kind == CONV_CHANNEL ? strlen(_node_prefs->node_name) + 2 : 0);
  if (sel_first >= 0) snprintf(hint, sizeof(hint), "CLICK/RET @MENTION THEM  " G_UP G_DOWN " PICK  ESC CANCEL  %3d/%d", _input_len, max_len);
  else snprintf(hint, sizeof(hint), "RET SEND " G_UP G_DOWN " PICK MSG " G_LEFT " BACK /page /sos %3d/%d", _input_len, max_len);
  drawFooter(hint);
}

void UITask::drawNode() {
  const Node* np = findNode(_conv_peer);
  if (np == NULL) { _screen = SCR_LIST; return; }
  const Node& n = *np;
  Adafruit_GFX& g = _display->gfx();

  textf(0, TABS_Y, c_hi, "DOSSIER // [%s] %.22s", typeTag(n.type), n.name);
  g.drawFastHLine(0, TABS_Y + ROW_H + 1, SCR_W, c_dim);
  int y = BODY_Y;

  ContactInfo* c = the_mesh.lookupContactByPubKey(n.pub, sizeof(n.pub));
  if (c) {
    char hex[65];
    for (int k = 0; k < 32; k++) sprintf(&hex[k * 2], "%02X", c->id.pub_key[k]);
    text(0, y, c_dim, "KEY");
    char half[33];
    memcpy(half, hex, 32); half[32] = 0;
    text(7 * CHAR_W, y, c_fg, half);
    y += ROW_H;
    text(7 * CHAR_W, y, c_fg, &hex[32]);
    y += ROW_H + 2;
  }
  char ago[8];
  fmtAgo(ago, n.lastmod);
  text(0, y, c_dim, "HEARD");
  textf(7 * CHAR_W, y, c_fg, "%s AGO", ago);
  y += ROW_H;
  text(0, y, c_dim, "ROUTE");
  if (n.path_len == OUT_PATH_UNKNOWN) text(7 * CHAR_W, y, c_fg, "UNKNOWN // FLOODS");
  else if ((n.path_len & 63) == 0) text(7 * CHAR_W, y, c_fg, "DIRECT");
  else textf(7 * CHAR_W, y, c_fg, "%d HOPS VIA REPEATERS", n.path_len & 63);
  y += ROW_H;
  text(0, y, c_dim, "COORD");
  if (n.lat || n.lon) textf(7 * CHAR_W, y, c_fg, "%.5f %.5f", n.lat / 1e6, n.lon / 1e6);
  else text(7 * CHAR_W, y, c_fg, "NOT SHARED");
  y += ROW_H + 4;

  // a repeater's own report, if we've asked for one
  if (_status.valid && memcmp(_status.pub, n.pub, 6) == 0) {
    const StatusInfo& s = _status;
    textf(0, y, c_hi, "BATT %.2fV UP %lud%02luh Q%u", s.batt_mv / 1000.0f, (unsigned long)(s.up_secs / 86400),
          (unsigned long)((s.up_secs / 3600) % 24), s.queue);
    y += ROW_H;
    textf(0, y, c_hi, "NF%d RSSI%d SNR%.1f AIR%lum", s.noise, s.rssi, s.snr4 / 4.0f, (unsigned long)(s.air_secs / 60));
    y += ROW_H;
    textf(0, y, c_hi, "RX %lu  TX %lu", (unsigned long)s.recv, (unsigned long)s.sent);
    y += ROW_H;
  } else if (n.type == ADV_TYPE_ROOM) {
    text(0, y, c_dim, isLoggedIn(n.pub) ? "JOINED. RET TO OPEN THE ROOM." : "RET OR L TO LOG IN (BLANK = GUEST)");
  } else if (n.type == ADV_TYPE_REPEATER) {
    text(0, y, c_dim, "T FOR LIVE STATUS. L TO LOG IN.");
  }

  if (_node_mode == NM_PASSWORD) {
    drawInput(INPUT_Y, "PASSWORD:", _input_buf, _input_len, true);
    drawFooter("RET LOGIN   ESC CANCEL");
  } else if (_node_mode == NM_CONFIRM_DEL) {
    text(0, INPUT_Y - 1, c_alert, "PRESS D AGAIN TO DELETE. ANY KEY: NO");
    drawFooter("THIS FORGETS THE CONTACT AND ITS PATH");
  } else if (n.type == ADV_TYPE_CHAT) {
    drawFooter("RET CHAT  S SHARE  R RESET PATH  D DEL  " G_LEFT " BACK");
  } else if (n.type == ADV_TYPE_REPEATER) {
    drawFooter(isAdminOf(n.pub) ? "A ADMIN CONSOLE  T STATUS  S SHARE  R PATH  D DEL"
                                : "T STATUS  L LOGIN  S SHARE  R PATH  D DEL  " G_LEFT);
  } else {
    drawFooter("RET/L LOGIN  S SHARE  R PATH  D DEL  " G_LEFT " BACK");
  }
}

void UITask::drawPage() {
  Adafruit_GFX& g = _display->gfx();
  const Msg* m = findMsg(_page_seq);
  bool flash = ((millis() / 400) % 2) == 0;
  uint16_t bg = flash ? c_alert : c_bg, fg = flash ? c_bg : c_alert;

  g.fillScreen(bg);
  for (int k = 0; k < 4; k++) g.drawRect(k * 3, k * 3, SCR_W - k * 6, SCR_H - k * 6, fg);
  bigText((SCR_W - 4 * CHAR_W * 4) / 2, 16, 4, fg, "PAGE");
  if (m == NULL) return;

  char from[48], when[12];
  fmtTime(when, m->ts);
  if (m->kind == CONV_CHANNEL) {
    ChannelDetails cd;
    char label[34] = "?";
    if (the_mesh.getChannel(m->chan_idx, cd)) chanLabel(label, sizeof(label), cd.name);
    snprintf(from, sizeof(from), "%s IN %s", m->from, label);
  } else {
    snprintf(from, sizeof(from), "FROM %s", m->from);
  }
  int w = strlen(from) * CHAR_W;
  text((SCR_W - w) / 2, 92, fg, from);

  // the message, minus the !PAGE marker, wrapped over a few lines
  const char* body = m->text + strlen(CD_PAGE_PREFIX);
  while (*body == ' ') body++;
  if (!*body) body = "(NO MESSAGE)";
  int y = 120;
  const int cols = 36;
  while (*body && y < 190) {
    int len = strlen(body);
    if (len > cols) {
      len = cols;
      for (int k = cols; k > cols / 2; k--) if (body[k] == ' ') { len = k; break; }
    }
    char line[cols + 1];
    memcpy(line, body, len);
    line[len] = 0;
    text((SCR_W - len * CHAR_W) / 2, y, fg, line);
    body += len;
    while (*body == ' ') body++;
    y += ROW_H;
  }
  char foot[40];
  snprintf(foot, sizeof(foot), "%s  //  ANY KEY TO ACKNOWLEDGE", when);
  stext((SCR_W - strlen(foot) * SMALL_W) / 2, SCR_H - 22, fg, foot);
}

void UITask::crtPass() {
  uint16_t* buf = _display->buffer();
  if (buf == NULL) return;
  // knock a quarter off every other row: a hint of CRT scanlines that keeps text legible
  for (int y = 1; y < CYBERDECK_H; y += 2) {
    uint16_t* row = buf + y * CYBERDECK_W;
    for (int x = 0; x < CYBERDECK_W; x++) row[x] -= (row[x] >> 2) & 0x39E7;
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
