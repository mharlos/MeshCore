#pragma once

// MESHCORE//DECK - a standalone, keyboard-driven UI for the LilyGo T-Deck in the
// style of a 90s movie cyberdeck. It sits on top of the stock companion radio, so
// the phone app keeps working over BLE alongside it.
//
// Beyond plain messaging it adds: private/hashtag channel management, paging
// (an alarm on the other deck until acknowledged), keyword alerts and tones,
// persistent history, on-device radio settings, repeat and "mule"
// (store-and-forward) modes, room server login, repeater status and an RF log.

#include <MeshCore.h>
#include <helpers/ui/DisplayDriver.h>
#include <helpers/SensorManager.h>
#include <helpers/MultiSerialInterface.h>
#include <Arduino.h>

#include "../AbstractUITask.h"
#include "../NodePrefs.h"
#include <CyberDeckDisplay.h>
#include <TDeckInput.h>
#include <TDeckAudio.h>

#ifndef CD_MAX_MSGS
  #define CD_MAX_MSGS        256    // message history, across all conversations
#endif
#ifndef CD_PENDING_SENDS
  #define CD_PENDING_SENDS     8    // direct messages awaiting an ACK
#endif
#define CD_INPUT_MAX          MAX_TEXT_LEN
#define CD_CONSOLE_LINES      32    // console scrollback
#define CD_CONSOLE_ROWS        6    // console lines on screen
#define CD_RF_LOG             48
#define CD_MULE_SLOTS        256    // allocated; `mule size` sets how many are used
#define CD_REPEATERS          24
#define CD_NEIGHBOURS         16
#define CD_KEYWORDS            4
#define CD_ROOM_LOGINS         8
#define CD_ADMIN_LINES        40    // remote console scrollback
#define CD_TILE_CACHE         16    // decoded map tiles, 128KB each in PSRAM
#define CD_CRUMBS           4096    // breadcrumb trail points kept in RAM
#define CD_PAGE_PREFIX     "!PAGE"
#define CD_PAGE_ACK        "!PAGE-ACK"
#define CD_WHERE           "!WHERE"
#define CD_HOME            "!HOME"    // a page that means "come home" (pagers play a recall tone)   // pagers answer with their position
#define CD_REPORTS         16         // positions people sent in messages, for the map

class UITask : public AbstractUITask {
public:
  enum ConvKind : uint8_t { CONV_NONE = 0, CONV_CHANNEL, CONV_DM };

private:
  enum Screen : uint8_t { SCR_BOOT, SCR_LIST, SCR_CHAT, SCR_NODE, SCR_PAGE, SCR_GPS, SCR_MULE, SCR_ADMIN };
  enum Tab : uint8_t { TAB_CHAN = 0, TAB_NODES, TAB_HEARD, TAB_MAP, TAB_RF, TAB_SYS, TAB_COUNT };
  enum MapStyle : uint8_t { MS_COLOR = 0, MS_NIGHT, MS_PHOSPHOR, MS_COUNT };
  enum MsgStatus : uint8_t { ST_NONE = 0, ST_SENDING, ST_DELIVERED, ST_FAILED, ST_SENT };
  enum MsgFlags : uint8_t { MF_PAGE = 1, MF_KEYWORD = 2, MF_ROOM = 4, MF_ACKED = 8 };
  enum NodeMode : uint8_t { NM_VIEW = 0, NM_PASSWORD, NM_CONFIRM_DEL };
  enum ClockSrc : uint8_t { CLK_NONE = 0, CLK_MESH, CLK_SET };

  struct Msg {
    uint32_t seq;          // 0 = empty slot
    uint32_t ts;           // our clock
    uint8_t  kind;         // ConvKind
    uint8_t  chan_idx;
    uint8_t  peer[6];      // pub key prefix, for DMs and rooms
    uint8_t  outgoing;
    uint8_t  unread;
    uint8_t  status;       // MsgStatus, outgoing only
    uint8_t  hops;         // 0xFF = direct
    int8_t   snr4;
    uint8_t  flags;        // MsgFlags
    uint16_t rtt_ds;       // round trip to the ACK, tenths of a second
    char     from[32];
    char     text[CD_INPUT_MAX + 1];
  };

  struct Pending {
    uint32_t seq;          // the Msg being delivered, 0 = free
    uint32_t ack;
    uint32_t msg_ts;
    unsigned long timeout_at, first_sent;
    uint8_t  attempt;
  };

  struct Node {
    uint8_t  pub[6];
    char     name[32];
    uint8_t  type;
    uint8_t  path_len;
    uint32_t lastmod;
    int32_t  lat, lon;
  };

  struct Prefs {
    uint8_t magic;
    uint8_t theme;
    uint8_t crt;
    uint8_t dim_secs;      // 0 = never
    uint8_t ball;          // trackball sensitivity 1-5
    uint8_t beep;          // BEEP_* bits
    uint8_t volume;        // 0-10
    uint8_t kbl;           // keyboard backlight 0-255
    uint8_t mule;          // store-and-forward
    uint8_t history;       // keep messages across reboots
    uint8_t map_style;     // MapStyle
    uint8_t crumb_off;     // 1 = don't record breadcrumbs
    uint8_t map_nolabels;  // 1 = no node names on the map
    uint8_t reserved[3];
    uint64_t muted;        // channel slots that don't beep
    char keywords[CD_KEYWORDS][16];
    // --- added later: an older, shorter file loads with these all zero, so zero = default ---
    uint64_t mule_chan_set;   // channels whose carry setting was chosen by hand...
    uint64_t mule_chan_val;   // ...and what was chosen (otherwise: private yes, public/hashtag no)
    uint8_t  mule_dm_off;     // 1 = don't carry DMs between contacts
    uint8_t  mule_own_off;    // 1 = don't carry our own posts
    uint8_t  mule_size8;      // cache size in eights (0 = 64 messages)
    uint8_t  mule_rate;       // relays per 10 minutes (0 = 60)
    uint16_t mule_age_min;    // hold messages this long (0 = 6h)
    uint8_t  gps_always;      // 1 = never rest the GPS
    uint8_t  cpu_full;        // 1 = don't slow the CPU while the screen is off
    uint8_t  spare[6];
  };

  struct RfEntry {
    uint32_t at;           // millis
    uint8_t  type, route, hops, len;
    int8_t   snr4;
    int16_t  rssi;
  };

  enum MuleKind : uint8_t { MK_DM = 1, MK_CHAN, MK_OWN };

  struct MuleEntry {       // a message carried for delivery elsewhere
    uint8_t  hash[MAX_HASH_SIZE];
    uint32_t heard;        // millis, when first held
    uint32_t sent_ts;      // sender's clock, if we could read it
    uint8_t  kind;         // MuleKind
    uint8_t  chan;         // channel slot, 0xFF if a DM
    uint8_t  sends;        // times we've relayed it
    uint8_t  nvia;
    uint8_t  via[12];      // repeaters known to have it (first byte of their key)
    char     from[16];     // preview, when we could decrypt it
    char     text[40];
    uint8_t  len;
    uint8_t  raw[MAX_TRANS_UNIT + 1];
  };

  struct Repeater { uint8_t id; uint32_t last; int8_t snr4; };   // repeaters near us now

  enum GpsMode : uint8_t { GM_OFF = 0, GM_FOLLOW, GM_REST, GM_CHECK };

  struct Neighbour { uint8_t hash; uint32_t last; };

  struct Tile {            // a decoded 256x256 map tile, in PSRAM
    int8_t   z;
    uint8_t  state;        // 0 empty, 1 loaded, 2 not on the card
    uint8_t  style;
    int32_t  x, y;
    uint32_t used;         // LRU stamp
    uint16_t* px;
  };

  struct Crumb { int32_t lat, lon; uint32_t ts; };   // micro-degrees, our clock

  struct StatusInfo {      // a repeater's answer to a status request
    uint8_t  pub[6];
    uint32_t at;
    uint16_t batt_mv, queue;
    int16_t  noise, rssi, snr4;
    uint32_t recv, sent, air_secs, up_secs, flood_tx, direct_tx, flood_rx, direct_rx;
    bool     valid;
  };

  CyberDeckDisplay* _display;
  SensorManager* _sensors;
  NodePrefs* _node_prefs;
  TDeckInput _input;
  TDeckAudio _audio;
  Prefs _prefs;

  Screen _screen, _page_return;
  Tab _tab;
  int _sel[TAB_COUNT];
  int _scroll;              // chat scrollback, in lines from the bottom

  ConvKind _conv_kind;      // the open chat
  uint8_t _conv_chan;
  uint8_t _conv_peer[6];
  char _conv_name[32];

  NodeMode _node_mode;
  char _input_buf[CD_INPUT_MAX + 1];
  int  _input_len;
  char _cmd_buf[96];
  int  _cmd_len;
  char _console[CD_CONSOLE_LINES][48];
  int  _console_len, _console_scroll;
  char _last_cmd[96];

  Msg* _msgs;
  int _max_msgs;
  uint32_t _next_seq;
  Pending _pending[CD_PENDING_SENDS];
  bool _hist_ok;
  bool _sd_ok;
  bool sdBegin();

  // map
  Tile _tiles[CD_TILE_CACHE];
  uint32_t _tile_clock;
  uint8_t* _png_buf;        // one tile's file
  void* _png;               // PNGdec decoder, kept off the stack
  void* _jpeg;              // JPEGDEC decoder
  int _map_zmin, _map_zmax; // zoom levels on the card, -1 if none
  int _map_z;
  double _map_lat, _map_lon;   // view centre
  bool _map_follow, _map_pan, _map_inited;
  unsigned long _map_pan_at;   // last pan activity: pan mode lets go after a few idle seconds
  int _map_node_sel;        // into _nodes, -1 = none
  // N on the map: a list of nodes with a position, nearest (or fewest hops) first
  bool _map_pick;
  uint8_t _map_pick_by;     // 0 = distance, 1 = hops
  int _map_pick_sel, _map_pick_top;
  int mapPickList(int* out, double& ref_lat, double& ref_lon, bool& have_ref);
  bool mapPickKey(int key);
  void drawMapPick();
  double _home_lat, _home_lon;
  Crumb* _crumbs;
  int _crumb_head, _crumb_count;
  unsigned long _crumb_check;
  bool _gps_fix_seen;

  void mapInit();
  Tile* getTile(int z, int32_t x, int32_t y);
  bool loadTile(Tile& t);
  void clearTiles();
  bool myPosition(double& lat, double& lon);
  void recordCrumb();
  void loadCrumbs();
  void clearCrumbs();
  bool handleMapKey(int key);   // true if the map used the key
  void drawMap();
  void drawGps();

  Node* _nodes;
  int _num_nodes;
  bool _nodes_dirty;

  // positions found in messages ("@ 45.1,-122.9", SOS pages, !WHERE replies): pagers
  // keep theirs out of public adverts, so this is how they get on the map
  struct Report { uint8_t pub[6]; int32_t lat, lon; uint32_t ts; };
  Report _reports[CD_REPORTS];
  void notePosition(const uint8_t* pub, const char* text);
  bool contactByName(const char* name, uint8_t* pub_out) const;
  int addressPage(const char* arg, char* out, size_t n);   // returns how many names it addressed
  bool chanPosterName(const char* word, char* proper);
  unsigned long _nodes_refresh_at;

  int _chan_idx[MAX_GROUP_CHANNELS];   // valid channel slots, in order
  int _num_chans;

  RfEntry* _rf;
  int _rf_head, _rf_count;
  uint32_t _rf_total;

  MuleEntry* _mule;
  Neighbour _neigh[CD_NEIGHBOURS];
  Repeater _rpt[CD_REPEATERS];
  unsigned long _mule_next_tick, _mule_next_probe;
  uint32_t _mule_relayed, _mule_probe_tag;
  unsigned long _mule_tokens_at;
  int _mule_tokens;
  int _mule_last_own;       // entry just stored for our own post, for its preview
  int _mule_scroll;

  // GPS power: rest when still, follow when moving
  GpsMode _gps_mode;
  unsigned long _gps_next, _gps_since, _gps_anchor_at;
  double _gps_anchor_lat, _gps_anchor_lon;
  bool _gps_asleep, _cpu_low;
  uint16_t _batt_hist[24];  // mV every 5 minutes, for a drain estimate
  int _batt_n;
  unsigned long _batt_next;

  // remote admin console for a repeater or room we're logged into as admin
  uint8_t _admins[CD_ROOM_LOGINS][6];   // where our login has admin rights
  int _num_admins;
  char _acon[CD_ADMIN_LINES][54];
  int _acon_len, _acon_scroll;
  char _acmd[96];
  int _acmd_len;
  int _aq_sel;              // quick-menu pick while the prompt is empty, -1 = none
  bool _aq_confirm;         // a risky quick command waiting for a second press
  unsigned long _admin_wait_until;

  // chat: a message picked with the trackball, to @mention its sender
  int _chat_sel;
  uint32_t _chat_sel_seq;

  uint8_t _logged_in[CD_ROOM_LOGINS][6];
  int _num_logged_in;
  uint8_t _login_pending[6];
  unsigned long _login_timeout;
  StatusInfo _status;
  unsigned long _status_timeout;

  ClockSrc _clock_src;
  uint32_t _clk_cand_ts, _clk_cand_ms, _clk_last_rtc, _clk_last_ms;
  uint8_t  _clk_cand_from[4];

  uint32_t _page_seq;       // the page on screen
  unsigned long _page_next_tone, _page_started;
  bool _kbl_flash;

  unsigned long _boot_started;
  unsigned long _next_render, _last_render, _next_blink, _auto_off, _glitch_until, _alert_until;
  bool _dirty, _cursor_on;
  char _alert[54];
  int _msgcount;

  // colours for the current theme
  uint16_t c_fg, c_dim, c_hi, c_alert, c_bg;

  void applyTheme();
  void loadPrefs();
  void savePrefs();

  // history
  void loadHistory();
  void saveMsg(const Msg* m);
  void wipeHistory();

  void refreshNodes();
  void refreshChannels();
  Msg* addMsg(ConvKind kind, uint8_t chan, const uint8_t* peer, const char* from, const char* text, bool outgoing);
  Msg* findMsg(uint32_t seq);
  void stampSent(Msg* m, uint32_t sender_ts, bool mark_late);
  int  unreadFor(ConvKind kind, uint8_t chan, const uint8_t* peer) const;
  int  totalUnread() const;
  void markRead(ConvKind kind, uint8_t chan, const uint8_t* peer);
  bool msgInConv(const Msg& m, ConvKind kind, uint8_t chan, const uint8_t* peer) const;
  const Node* findNode(const uint8_t* pub) const;

  void openChannel(int list_pos);
  void openNode(const uint8_t* pub);
  void openChat(const uint8_t* pub);
  void sendInput();
  bool sendText(const char* text, uint8_t flags = 0);   // to the open conversation
  bool sendDirect(int slot, uint8_t attempt);
  void checkPending();

  // features
  int  addChannel(const char* name, const uint8_t* secret);
  void showChannelKey(int idx);
  bool parseKey(const char* s, uint8_t* secret);
  bool isMuted(int chan) const { return chan < 64 && (_prefs.muted >> chan) & 1; }
  bool matchesKeyword(const char* text) const;
  void startPage(Msg* m);
  void ackPage();
  void syncClockFrom(uint32_t their_ts, const uint8_t* from);
  void watchClock();
  void setClock(uint32_t epoch, ClockSrc src);
  bool clockValid() const { return _clock_src != CLK_NONE; }
  void muleStore(const mesh::Packet* pkt, MuleKind kind, uint8_t chan, int via);
  void muleConsider(const mesh::Packet* pkt, bool own, int via);
  void muleNeighbour(uint8_t hash);
  void muleRepeaterSeen(uint8_t id, int8_t snr4);
  int  muleRepeatersNear() const;
  void muleTick();
  bool muleDue(MuleEntry& e);
  bool muleRelay(MuleEntry& e);
  int  muleFlush();
  int  muleCount(bool due_only = false);
  bool muleCarriesChan(int idx) const;
  MuleEntry* muleFind(const uint8_t* hash);
  int  muleSlots() const { int n = (_prefs.mule_size8 ? _prefs.mule_size8 : 8) * 8; return n > CD_MULE_SLOTS ? CD_MULE_SLOTS : n; }
  uint32_t muleMaxAgeMs() const { return (_prefs.mule_age_min ? _prefs.mule_age_min : 360) * 60000UL; }
  int  muleRate() const { return _prefs.mule_rate ? _prefs.mule_rate : 60; }
  void muleStatus();
  void drawMule();

  bool isAdminOf(const uint8_t* pub) const;
  void openAdmin();
  void adminPrint(const char* fmt, ...);
  void adminSend(const char* cmd);
  void adminReply(const char* text);
  void handleAdminKey(int key);
  void drawAdmin();

  void gpsTick();
  void gpsSleep(bool sleep);
  void powerTick();
  bool isLoggedIn(const uint8_t* pub) const;
  void beep(const Tone* t);

  void runCommand(const char* cmd);
  void consolePrint(const char* fmt, ...);
  void showAlert(const char* text, int millis);

  void handleKey(int key);
  void handleListKey(int key);
  void handleChatKey(int key);
  void handleNodeKey(int key);
  void wake();
#ifdef CD_SERIAL_BRIDGE
  void serialBridge();
#endif

  // drawing
  void render();
  void drawBoot();
  void drawHeader();
  void drawTabs();
  void drawFooter(const char* hint);
  void drawChanList();
  void drawNodeList(bool recent);
  void drawRf();
  void drawSys();
  void drawChat();
  void drawNode();
  void drawPage();
  void drawInput(int y, const char* prompt, const char* buf, int len, bool masked = false);
  void text(int x, int y, uint16_t col, const char* s);      // 8x16
  void textf(int x, int y, uint16_t col, const char* fmt, ...);
  void bigText(int x, int y, int scale, uint16_t col, const char* s);
  void stext(int x, int y, uint16_t col, const char* s);     // 6x8
  void crtPass();
  void glitchPass();

  int  battPercent() const;
  void fmtTime(char* dest, uint32_t epoch, bool with_secs = false) const;
  void fmtAgo(char* dest, uint32_t epoch) const;

public:
  UITask(mesh::MainBoard* board, MultiSerialInterface* serial);

  void begin(DisplayDriver* display, SensorManager* sensors, NodePrefs* node_prefs);
  int  getMsgCount() const { return _msgcount; }
  bool hasDisplay() const { return _display != NULL; }
  bool getGPSState();
  void setGPS(bool on);
  void shutdown(bool restart = false);

  // MyMesh::Listener
  void onMessageRecv(mesh::Packet *pkt, const ContactInfo &from, uint8_t txt_type, uint32_t sender_timestamp, const char* text) override;
  void onChannelMessageRecv(mesh::Packet *pkt, ChannelDetails& channel_details, const char* text) override;
  void onQueueSizeChanged(int offline_queue_size) override;
  void onDiscoveredContact(ContactInfo &contact, bool is_new, uint8_t path_len, const uint8_t* path) override;
  void onACKRecv(uint32_t ack_crc) override;
  void onLoginResult(const ContactInfo &contact, bool ok, uint8_t permissions) override;
  void onStatusRecv(const ContactInfo &contact, const uint8_t *data, uint8_t len) override;
  void onFloodRecv(const mesh::Packet *pkt) override;
  void onRawRx(float snr, float rssi, const uint8_t raw[], int len) override;
  void onFloodSent(const mesh::Packet *pkt) override;
  void onControlDataRecv(const mesh::Packet *pkt) override;

  // AbstractUITask
  void notify(UIEventType t = UIEventType::none) override;
  void loop() override;
};
