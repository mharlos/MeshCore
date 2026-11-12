#pragma once

// Pager UI for a screenless one-button tracker (SenseCAP T1000-E): a kid's farm pager.
//
// Still a full companion radio underneath, so the phone app (BLE) or a PC (USB) can set
// the name, channels and contacts. On top of that, the button and buzzer do this alone:
//
//   a page arrives                    alarm until the button is pressed; the press acks it.
//                                     On a channel, "!PAGE msg" rings every pager and
//                                     "!PAGE @[K1] @[D*] msg" only those named (* = prefix)
//   DM, or a channel post naming us   chirp, LED blinks until a press
//   1 press                           battery beeps (1-4), clears the blinking LED
//   2 presses                         "OK" to the farm channel
//   3 presses                         "Come get me" to the farm channel, with position
//   hold 3 s, let go                  SOS: pages every deck on the farm channel
//   4 presses                         cancel my SOS / quiet chirps on-off
//   hold 10 s                         power off
//   !WHERE (DM) / !WHERE [@[name]]    GPS on; replies with its position now and a fresh fix
//
// Named D<digit> (D1, D2): a dog collar. Presses only answer a page; nothing is ever sent.
//
// Everything it sends goes to the farm channel: the one named "farm", else the first
// channel that isn't Public. Without one it sounds an error and sends nothing.

#include <MeshCore.h>
#include <helpers/ui/DisplayDriver.h>
#include <helpers/SensorManager.h>
#include <stddef.h>

#ifdef PIN_BUZZER
  #include <helpers/ui/buzzer.h>
#endif

#include "../AbstractUITask.h"
#include "../NodePrefs.h"

#define PG_PAGE_PREFIX   "!PAGE"
#define PG_PAGE_ACK      "!PAGE-ACK"
#define PG_WHERE         "!WHERE"

class UITask : public AbstractUITask {
  SensorManager* _sensors = NULL;
  NodePrefs* _node_prefs = NULL;
#ifdef PIN_BUZZER
  genericBuzzer buzzer;
#endif

  // button
  bool _btn_down = false, _btn_swallow = false, _btn_armed = false;
  uint32_t _btn_changed = 0, _btn_pressed_at = 0, _btn_released_at = 0;
  uint8_t _clicks = 0;

  // an unanswered page
  bool _paging = false;
  uint32_t _page_started = 0, _page_next_tone = 0;
  bool _page_dm = false;
  uint8_t _page_peer[6];
  int _page_chan = -1;
  char _page_from[32];

  // unread DMs / mentions
  bool _unread = false;

  // my SOS
  bool _sos = false, _sos_has_pos = false;
  uint32_t _sos_started = 0, _sos_next_retry = 0, _sos_next_update = 0;
  uint8_t _sos_tries = 0;

  // was our last channel post heard repeated?
  bool _echo_wait = false, _echo_heard = false, _echo_is_sos = false;
  uint8_t _echo_hash[MAX_HASH_SIZE];
  uint32_t _echo_deadline = 0;
  bool _capture_echo = false;

  // "come get me" / !WHERE: send a fix when one arrives
  bool _want_fix = false;
  uint32_t _want_fix_until = 0;
  int _where_chan = -1;          // reply on a channel...
  uint8_t _where_peer[6];        // ...or by DM
  bool _where_dm = false;
  char _want_fix_label[24];

  // GPS, kept off unless something needs a position
  bool _gps_on = false;
  uint32_t _gps_on_since = 0;
  int32_t _fix_lat = 0, _fix_lon = 0;   // microdegrees
  uint32_t _fix_at = 0;
  bool _have_fix = false;

  // clock, from GPS, the app, or two nodes that agree
  uint32_t _clk_cand_ts = 0, _clk_cand_ms = 0;
  uint8_t _clk_cand_from[4];

  // housekeeping
  uint32_t _next_advert = 0, _next_batt = 0, _next_led = 0;
  bool _led_state = false, _low_batt_sent = false;
  uint16_t _batt_mv = 0;
  uint32_t _started = 0;

  void tone(const char* rtttl, bool force = false);
  void buttonTick();
  void onClicks(int n);
  void onHold();
  void ledTick();
  void gpsTick();
  void gpsWant(bool on);
  bool freshFix(uint32_t max_age_ms) const;
  void fmtPos(char* dest, size_t n, uint32_t max_age_ms) const;

  int farmChannel();
  bool sendChannel(int idx, const char* text, bool track_echo, bool is_sos = false);
  bool sendDM(const uint8_t* pub_prefix, const char* text);
  void sendToFarm(const char* text, bool is_sos = false);

  void startPage(bool dm, const uint8_t* peer, int chan, const char* from);
  void ackPage();
  void sendSOS();
  void cancelSOS();
  void replyWhere();
  void syncClockFrom(uint32_t their_ts, const uint8_t* from);
  bool clockSet() const;
  bool mentionsMe(const char* text) const;
  bool isDog() const;

public:
  UITask(mesh::MainBoard* board, MultiSerialInterface* serial) : AbstractUITask(board, serial) { }
  void begin(DisplayDriver* display, SensorManager* sensors, NodePrefs* node_prefs);

  // MyMesh::Listener
  void onMessageRecv(mesh::Packet *pkt, const ContactInfo &from, uint8_t txt_type, uint32_t sender_timestamp, const char* text) override;
  void onChannelMessageRecv(mesh::Packet *pkt, ChannelDetails& channel_details, const char* text) override;
  void onQueueSizeChanged(int offline_queue_size) override { }
  void onDiscoveredContact(ContactInfo &contact, bool is_new, uint8_t path_len, const uint8_t* path) override;
  void onFloodRecv(const mesh::Packet *pkt) override;
  void onFloodSent(const mesh::Packet *pkt) override;

  // from AbstractUITask
  void notify(UIEventType t = UIEventType::none) override;
  void loop() override;
  bool isBusy() override;

  void shutdown(bool restart = false);
};
