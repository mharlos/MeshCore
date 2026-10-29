#pragma once

// MESHCORE//DECK - a standalone, keyboard-driven UI for the LilyGo T-Deck in the
// style of a 90s movie cyberdeck. It sits on top of the stock companion radio, so
// the phone app keeps working over BLE alongside it; nothing about how the node
// talks to the mesh changes.

#include <MeshCore.h>
#include <helpers/ui/DisplayDriver.h>
#include <helpers/SensorManager.h>
#include <helpers/MultiSerialInterface.h>
#include <Arduino.h>

#include "../AbstractUITask.h"
#include "../NodePrefs.h"
#include <CyberDeckDisplay.h>
#include <TDeckInput.h>

#ifndef CD_MAX_MSGS
  #define CD_MAX_MSGS        256    // message history kept in PSRAM, across all conversations
#endif
#ifndef CD_PENDING_SENDS
  #define CD_PENDING_SENDS     8    // direct messages awaiting an ACK
#endif
#define CD_INPUT_MAX          MAX_TEXT_LEN
#define CD_CONSOLE_LINES      9

class UITask : public AbstractUITask {
public:
  enum ConvKind : uint8_t { CONV_NONE = 0, CONV_CHANNEL, CONV_DM };

private:
  enum Screen : uint8_t { SCR_BOOT, SCR_LIST, SCR_CHAT, SCR_NODE };
  enum Tab : uint8_t { TAB_CHAN = 0, TAB_NODES, TAB_HEARD, TAB_SYS, TAB_COUNT };
  enum MsgStatus : uint8_t { ST_NONE = 0, ST_SENDING, ST_DELIVERED, ST_FAILED, ST_SENT };

  struct Msg {
    uint32_t seq;          // 0 = empty slot
    uint32_t ts;           // our clock
    uint8_t  kind;         // ConvKind
    uint8_t  chan_idx;
    uint8_t  peer[6];      // pub key prefix, for DMs
    uint8_t  outgoing;
    uint8_t  unread;
    uint8_t  status;       // MsgStatus, outgoing only
    uint8_t  hops;         // 0xFF = direct
    int8_t   snr4;
    char     from[32];
    char     text[CD_INPUT_MAX + 1];
  };

  struct Pending {
    uint32_t seq;          // the Msg being delivered, 0 = free
    uint32_t ack;
    uint32_t msg_ts;
    unsigned long timeout_at;
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
  };

  CyberDeckDisplay* _display;
  SensorManager* _sensors;
  NodePrefs* _node_prefs;
  TDeckInput _input;
  Prefs _prefs;

  Screen _screen;
  Tab _tab;
  int _sel[TAB_COUNT];
  int _scroll;              // chat scrollback, in lines from the bottom

  ConvKind _conv_kind;      // the open chat
  uint8_t _conv_chan;
  uint8_t _conv_peer[6];
  char _conv_name[32];

  char _input_buf[CD_INPUT_MAX + 1];
  int  _input_len;
  char _cmd_buf[64];
  int  _cmd_len;
  char _console[CD_CONSOLE_LINES][54];
  int  _console_len;

  Msg* _msgs;
  int _max_msgs;
  uint32_t _next_seq;
  Pending _pending[CD_PENDING_SENDS];

  Node* _nodes;
  int _num_nodes;
  bool _nodes_dirty;
  unsigned long _nodes_refresh_at;

  int _chan_idx[MAX_GROUP_CHANNELS];   // valid channel slots, in order
  int _num_chans;

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

  void refreshNodes();
  void refreshChannels();
  Msg* addMsg(ConvKind kind, uint8_t chan, const uint8_t* peer, const char* from, const char* text, bool outgoing);
  Msg* findMsg(uint32_t seq);
  int  unreadFor(ConvKind kind, uint8_t chan, const uint8_t* peer) const;
  int  totalUnread() const;
  void markRead(ConvKind kind, uint8_t chan, const uint8_t* peer);
  bool msgInConv(const Msg& m, ConvKind kind, uint8_t chan, const uint8_t* peer) const;

  void openChannel(int list_pos);
  void openNode(const uint8_t* pub);
  void sendInput();
  bool sendDirect(int slot, uint8_t attempt);
  void checkPending();

  void runCommand(const char* cmd);
  void consolePrint(const char* fmt, ...);
  void showAlert(const char* text, int millis);

  void handleKey(int key);
  void handleListKey(int key);
  void handleChatKey(int key);
  void wake();

  // drawing
  void render();
  void drawBoot();
  void drawHeader();
  void drawTabs();
  void drawFooter(const char* hint);
  void drawChanList();
  void drawNodeList(bool recent);
  void drawSys();
  void drawChat();
  void drawNode();
  void drawInput(int y, const char* prompt, const char* buf, int len);
  void text(int x, int y, uint16_t col, const char* s);
  void textf(int x, int y, uint16_t col, const char* fmt, ...);
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

  // AbstractUITask
  void notify(UIEventType t = UIEventType::none) override;
  void loop() override;
};
