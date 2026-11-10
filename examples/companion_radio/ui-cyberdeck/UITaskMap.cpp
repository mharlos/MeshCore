// MAP tab and GPS page for the cyberdeck UI.
//
// Tiles are standard slippy-map PNGs on the SD card at /maps/{z}/{x}/{y}.png (256px;
// 512px "retina" tiles are halved). Decoded tiles live in a small PSRAM cache so
// panning over the same ground is instant; zooming past the deepest level on the card
// enlarges its tiles. On top go the breadcrumb trail, every contact that has shared a
// position, and us.

#include "UITask.h"
#include <SD.h>
#include <PNGdec.h>
#include <JPEGDEC.h>
#include <math.h>
#include <new>
#include <target.h>
#include <helpers/AdvertDataHelpers.h>

#define SCR_W       CYBERDECK_W
#define CHAR_W      8
#define ROW_H       16
#define SMALL_W     6
#define TABS_Y      17
#define BODY_Y      35
#define FOOT_Y      232
#define MAP_Y       BODY_Y
#define MAP_H       (FOOT_Y - 2 - MAP_Y)

#define TILE           256
#define MAX_OVERZOOM   2
#define TILE_FILE_MAX  65536
#define CRUMB_DIR      "/deck"
#define CRUMB_FILE     "/deck/crumbs.bin"
#define CRUMB_MIN_M    15.0     // a new crumb every this many metres moved
#define CRUMB_GAP_SECS 600      // longer than this between crumbs: don't join them

#define TKEY_CLICK   0x85
#define MAP_PAN_IDLE_MS  5000

#ifdef CD_SERIAL_BRIDGE
  #define MAP_TRACE(...)  Serial.printf(__VA_ARGS__)
#else
  #define MAP_TRACE(...)
#endif

static bool s_hide_crumbs = false;
static const char* STYLE_NAMES[] = { "COLOR", "NIGHT", "PHOSPHOR" };

/* ------------------------------------- projection -------------------------------------- */

static double worldSize(int z) { return TILE * (double)(1UL << z); }
static double lonToX(double lon, int z) { return (lon + 180.0) / 360.0 * worldSize(z); }
static double latToY(double lat, int z) {
  double r = lat * M_PI / 180.0;
  return (1.0 - log(tan(r) + 1.0 / cos(r)) / M_PI) / 2.0 * worldSize(z);
}
static double xToLon(double x, int z) { return x / worldSize(z) * 360.0 - 180.0; }
static double yToLat(double y, int z) {
  double n = M_PI - 2.0 * M_PI * y / worldSize(z);
  return 180.0 / M_PI * atan(sinh(n));
}

static double distM(double lat1, double lon1, double lat2, double lon2) {
  double p1 = lat1 * M_PI / 180, p2 = lat2 * M_PI / 180;
  double dp = p2 - p1, dl = (lon2 - lon1) * M_PI / 180;
  double a = sin(dp / 2) * sin(dp / 2) + cos(p1) * cos(p2) * sin(dl / 2) * sin(dl / 2);
  return 6371000.0 * 2 * atan2(sqrt(a), sqrt(1 - a));
}

static double bearingDeg(double lat1, double lon1, double lat2, double lon2) {
  double p1 = lat1 * M_PI / 180, p2 = lat2 * M_PI / 180, dl = (lon2 - lon1) * M_PI / 180;
  double y = sin(dl) * cos(p2), x = cos(p1) * sin(p2) - sin(p1) * cos(p2) * cos(dl);
  double b = atan2(y, x) * 180 / M_PI;
  return b < 0 ? b + 360 : b;
}

static const char* compass(double deg) {
  static const char* pts[] = { "N", "NE", "E", "SE", "S", "SW", "W", "NW" };
  return pts[(int)((deg + 22.5) / 45.0) % 8];
}

static void fmtDist(char* dest, size_t sz, double m) {
  if (m < 1000) snprintf(dest, sz, "%dM", (int)m);
  else if (m < 10000) snprintf(dest, sz, "%.2fKM", m / 1000);
  else snprintf(dest, sz, "%dKM", (int)(m / 1000));
}

static const char* nodeTag(uint8_t type) {
  switch (type) {
    case ADV_TYPE_CHAT:     return "USR";
    case ADV_TYPE_REPEATER: return "RPT";
    case ADV_TYPE_ROOM:     return "RMS";
    case ADV_TYPE_SENSOR:   return "SNS";
    default:                return "???";
  }
}

/* ---------------------------------------- tiles ---------------------------------------- */

struct DecodeCtx {
  PNG* png;
  uint16_t* dst;
  int scale;           // 1 for 256px tiles, 2 for 512px
  uint8_t style;
  uint16_t fg;
  uint16_t line[2 * TILE];
};

static inline uint16_t styled(uint16_t c, uint8_t style, uint16_t fg) {
  if (style == 0) return c;
  int r = (c >> 11) << 3, g = ((c >> 5) & 0x3F) << 2, b = (c & 0x1F) << 3;
  if (style == 1) {   // NIGHT: same map, much less glare
    r = r * 9 / 20; g = g * 9 / 20; b = b * 9 / 20;
  } else {            // PHOSPHOR: roads glow, open land is dim, woods/water/text go dark
    int y = (r * 77 + g * 150 + b * 29) >> 8;
    int f = (y - 225) * 255 / 30;   // street maps: roads ~255, land ~240, woods/water ~200
    if (f < 0) f = 0; else if (f > 255) f = 255;
    f = f * f / 255;
    int fr = (fg >> 11) << 3, fgg = ((fg >> 5) & 0x3F) << 2, fb = (fg & 0x1F) << 3;
    r = fr * f / 255; g = fgg * f / 255; b = fb * f / 255;
  }
  return (uint16_t)(((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3));
}

static int pngDraw(PNGDRAW* d) {
  DecodeCtx* ctx = (DecodeCtx*) d->pUser;
  if (d->y % ctx->scale) return 1;
  int row = d->y / ctx->scale;
  if (row >= TILE) return 1;
  ctx->png->getLineAsRGB565(d, ctx->line, PNG_RGB565_LITTLE_ENDIAN, 0xffffffff);
  uint16_t* out = ctx->dst + row * TILE;
  for (int x = 0; x < TILE; x++) out[x] = styled(ctx->line[x * ctx->scale], ctx->style, ctx->fg);
  return 1;
}

static int jpegDraw(JPEGDRAW* d) {
  DecodeCtx* ctx = (DecodeCtx*) d->pUser;
  int w = d->iWidthUsed < TILE - d->x ? d->iWidthUsed : TILE - d->x;
  for (int r = 0; r < d->iHeight; r++) {
    int y = d->y + r;
    if (y >= TILE) break;
    uint16_t* out = ctx->dst + y * TILE + d->x;
    const uint16_t* in = d->pPixels + r * d->iWidth;
    for (int c = 0; c < w; c++) out[c] = styled(in[c], ctx->style, ctx->fg);
  }
  return 1;
}

void UITask::clearTiles() {
  for (int i = 0; i < CD_TILE_CACHE; i++) _tiles[i].state = 0;
}

void UITask::mapInit() {
  _map_inited = true;
  for (int i = 0; i < CD_TILE_CACHE; i++) {
    _tiles[i].px = (uint16_t*) ps_malloc(TILE * TILE * 2);
    _tiles[i].state = 0;
  }
  _png_buf = (uint8_t*) ps_malloc(TILE_FILE_MAX);
  void* mem = ps_malloc(sizeof(PNG));
  _png = mem ? new (mem) PNG() : NULL;
  mem = ps_malloc(sizeof(JPEGDEC));
  _jpeg = mem ? new (mem) JPEGDEC() : NULL;

  // which zoom levels are on the card, and roughly where
  _map_zmin = _map_zmax = -1;
  if (_sd_ok) {
    File root = SD.open("/maps");
    if (root && root.isDirectory()) {
      for (File f = root.openNextFile(); f; f = root.openNextFile()) {
        const char* base = strrchr(f.name(), '/') ? strrchr(f.name(), '/') + 1 : f.name();
        if (f.isDirectory() && isdigit(base[0])) {
          int z = atoi(base);
          if (z >= 0 && z <= 20) {
            if (_map_zmin < 0 || z < _map_zmin) _map_zmin = z;
            if (z > _map_zmax) _map_zmax = z;
          }
        }
        f.close();
      }
      root.close();
    }
  }
  if (_map_zmin >= 0) {   // the middle of the coverage at the widest zoom
    char path[24];
    snprintf(path, sizeof(path), "/maps/%d", _map_zmin);
    long xmin = -1, xmax = -1, ymin = -1, ymax = -1;
    File zdir = SD.open(path);
    char first_x[48] = "";
    for (File f = zdir.openNextFile(); f; f = zdir.openNextFile()) {
      const char* base = strrchr(f.name(), '/') ? strrchr(f.name(), '/') + 1 : f.name();
      long x = atol(base);
      if (xmin < 0 || x < xmin) { xmin = x; snprintf(first_x, sizeof(first_x), "%s/%s", path, base); }
      if (x > xmax) xmax = x;
      f.close();
    }
    zdir.close();
    File xdir = SD.open(first_x);
    for (File f = xdir.openNextFile(); f; f = xdir.openNextFile()) {
      const char* base = strrchr(f.name(), '/') ? strrchr(f.name(), '/') + 1 : f.name();
      long y = atol(base);
      if (ymin < 0 || y < ymin) ymin = y;
      if (y > ymax) ymax = y;
      f.close();
    }
    xdir.close();
    if (xmin >= 0 && ymin >= 0) {
      _home_lon = xToLon((xmin + xmax + 1) * TILE / 2.0, _map_zmin);
      _home_lat = yToLat((ymin + ymax + 1) * TILE / 2.0, _map_zmin);
    }
  }

  _map_z = (_map_zmax >= 0) ? _map_zmax : 12;
  double lat, lon;
  if (myPosition(lat, lon)) {
    _map_lat = lat; _map_lon = lon;
  } else if (_crumb_count > 0) {
    const Crumb& c = _crumbs[(_crumb_head + CD_CRUMBS - 1) % CD_CRUMBS];
    _map_lat = c.lat / 1e6; _map_lon = c.lon / 1e6;
  } else {
    _map_lat = _home_lat; _map_lon = _home_lon;
    if (_map_zmin >= 0) _map_z = _map_zmin + (_map_zmax - _map_zmin) / 2;
  }
  _map_follow = true;
}

UITask::Tile* UITask::getTile(int z, int32_t x, int32_t y) {
  int32_t n = 1L << z;
  if (y < 0 || y >= n) return NULL;
  x = ((x % n) + n) % n;

  int slot = -1;
  for (int i = 0; i < CD_TILE_CACHE; i++) {
    Tile& t = _tiles[i];
    if (t.state && t.z == z && t.x == x && t.y == y) {
      t.used = ++_tile_clock;
      return &t;
    }
    if (t.px && (slot < 0 || (_tiles[slot].state && (t.state == 0 || t.used < _tiles[slot].used)))) slot = i;
  }
  if (slot < 0) return NULL;   // no PSRAM
  Tile& t = _tiles[slot];
  t.z = z; t.x = x; t.y = y;
  t.used = ++_tile_clock;
  t.style = _prefs.map_style;
  t.state = 2;                 // claim the slot before loading, so a parent lookup can't take it
  t.state = loadTile(t) ? 1 : 2;
  return &t;
}

bool UITask::loadTile(Tile& t) {
  if (_map_zmax < 0 || _png == NULL || _png_buf == NULL) return false;

  if (t.z > _map_zmax) {   // past the deepest level: blow up a piece of its tile
    int d = t.z - _map_zmax;
    Tile* p = getTile(_map_zmax, t.x >> d, t.y >> d);
    if (p == NULL || p->state != 1 || p == &t) return false;
    int span = TILE >> d;
    int ox = (t.x & ((1 << d) - 1)) * span, oy = (t.y & ((1 << d) - 1)) * span;
    for (int r = 0; r < TILE; r++) {
      const uint16_t* src = p->px + (oy + (r >> d)) * TILE + ox;
      uint16_t* dst = t.px + r * TILE;
      for (int c = 0; c < TILE; c++) dst[c] = src[c >> d];
    }
    return true;
  }
  if (t.z < _map_zmin) return false;

  char path[48];
  snprintf(path, sizeof(path), "/maps/%d/%ld/%ld.png", t.z, (long)t.x, (long)t.y);
  File f = SD.open(path);
  if (!f) { MAP_TRACE("tile %s: open failed\n", path); return false; }
  size_t size = f.size();
  if (size == 0 || size > TILE_FILE_MAX) { f.close(); MAP_TRACE("tile %s: size %u\n", path, (unsigned)size); return false; }
  size_t got = f.read(_png_buf, size);
  f.close();
  if (got != size) { MAP_TRACE("tile %s: read %u of %u\n", path, (unsigned)got, (unsigned)size); return false; }

  static DecodeCtx ctx;
  ctx.dst = t.px;
  ctx.style = t.style;
  ctx.fg = c_fg;

  // trust the bytes, not the extension: plenty of tile packs are JPEGs named .png
  if (_png_buf[0] == 0xFF && _png_buf[1] == 0xD8) {
    JPEGDEC* jpeg = (JPEGDEC*) _jpeg;
    if (jpeg == NULL || !jpeg->openRAM(_png_buf, size, jpegDraw)) {
      MAP_TRACE("tile %s: jpeg open err %d\n", path, jpeg ? jpeg->getLastError() : -1);
      return false;
    }
    jpeg->setPixelType(RGB565_LITTLE_ENDIAN);
    jpeg->setUserPointer(&ctx);
    if (jpeg->getWidth() < TILE || jpeg->getHeight() < TILE) memset(t.px, 0, TILE * TILE * 2);
    int ok = jpeg->decode(0, 0, jpeg->getWidth() >= 2 * TILE ? JPEG_SCALE_HALF : 0);
    if (!ok) MAP_TRACE("tile %s: jpeg decode err %d\n", path, jpeg->getLastError());
    jpeg->close();
    return ok;
  }

  PNG* png = (PNG*) _png;
  int orc = png->openRAM(_png_buf, size, pngDraw);
  if (orc != PNG_SUCCESS) {
    MAP_TRACE("tile %s: not PNG or JPEG (err %d, starts %02X %02X %02X %02X)\n", path, png->getLastError(),
              _png_buf[0], _png_buf[1], _png_buf[2], _png_buf[3]);
    return false;
  }
  ctx.png = png;
  ctx.scale = png->getWidth() >= 2 * TILE ? 2 : 1;
  if (png->getWidth() < TILE) memset(t.px, 0, TILE * TILE * 2);
  int rc = png->decode(&ctx, 0);
  if (rc != PNG_SUCCESS) MAP_TRACE("tile %s: png decode err %d\n", path, rc);
  png->close();
  return rc == PNG_SUCCESS;
}

/* ------------------------------------ GPS & crumbs ------------------------------------- */

bool UITask::myPosition(double& lat, double& lon) {
  LocationProvider* lp = _sensors ? _sensors->getLocationProvider() : NULL;
  if (lp == NULL || !lp->isValid()) return false;
  lat = lp->getLatitude() / 1e6;
  lon = lp->getLongitude() / 1e6;
  return lat != 0 || lon != 0;
}

void UITask::loadCrumbs() {
  _crumbs = (Crumb*) ps_calloc(CD_CRUMBS, sizeof(Crumb));
  _crumb_head = _crumb_count = 0;
  if (_crumbs == NULL || !_sd_ok) return;
  File f = SD.open(CRUMB_FILE);
  if (!f) return;
  size_t n = f.size() / sizeof(Crumb);
  size_t start = n > CD_CRUMBS ? n - CD_CRUMBS : 0;
  f.seek(start * sizeof(Crumb));
  while (_crumb_count < CD_CRUMBS && f.read((uint8_t*)&_crumbs[_crumb_count], sizeof(Crumb)) == sizeof(Crumb)) _crumb_count++;
  f.close();
  _crumb_head = _crumb_count % CD_CRUMBS;
}

void UITask::clearCrumbs() {
  _crumb_head = _crumb_count = 0;
  if (_sd_ok) SD.remove(CRUMB_FILE);
}

void UITask::recordCrumb() {
  double lat, lon;
  if (!myPosition(lat, lon)) return;
  if (!_gps_fix_seen) {   // first fix: put the map where we are
    _gps_fix_seen = true;
    if (_map_inited && _map_follow) { _map_lat = lat; _map_lon = lon; }
  }
  if (_prefs.crumb_off || _crumbs == NULL) return;
  if (_crumb_count > 0) {
    const Crumb& last = _crumbs[(_crumb_head + CD_CRUMBS - 1) % CD_CRUMBS];
    if (distM(last.lat / 1e6, last.lon / 1e6, lat, lon) < CRUMB_MIN_M) return;
  }
  Crumb c = { (int32_t)lround(lat * 1e6), (int32_t)lround(lon * 1e6), rtc_clock.getCurrentTime() };
  _crumbs[_crumb_head] = c;
  _crumb_head = (_crumb_head + 1) % CD_CRUMBS;
  if (_crumb_count < CD_CRUMBS) _crumb_count++;

  if (_sd_ok) {
    if (!SD.exists(CRUMB_DIR)) SD.mkdir(CRUMB_DIR);
    File f = SD.open(CRUMB_FILE, FILE_APPEND);
    if (f) { f.write((const uint8_t*)&c, sizeof(c)); f.close(); }
  }
}

/* ----------------------------------------- keys ---------------------------------------- */

bool UITask::handleMapKey(int key) {
  if (!_map_inited) mapInit();
  int zlo = _map_zmin >= 0 ? _map_zmin : 2;
  int zhi = _map_zmax >= 0 ? _map_zmax + MAX_OVERZOOM : 18;

  // pan mode lets go by itself, so left/right are never stuck moving the map
  if (_map_pan && millis() - _map_pan_at > MAP_PAN_IDLE_MS) _map_pan = false;

  if (_map_pan && (key == TKEY_UP || key == TKEY_DOWN || key == TKEY_LEFT || key == TKEY_RIGHT)) {
    _map_pan_at = millis();
    double cx = lonToX(_map_lon, _map_z), cy = latToY(_map_lat, _map_z);
    const int step = 48;
    if (key == TKEY_UP) cy -= step;
    if (key == TKEY_DOWN) cy += step;
    if (key == TKEY_LEFT) cx -= step;
    if (key == TKEY_RIGHT) cx += step;
    _map_lon = xToLon(cx, _map_z);
    _map_lat = yToLat(cy, _map_z);
    _map_follow = false;
    return true;
  }

  switch (key) {
    case TKEY_UP: case '+': case '=':
      if (_map_z < zhi) _map_z++;
      return true;
    case TKEY_DOWN: case '-': case '_':
      if (_map_z > zlo) _map_z--;
      return true;
    case TKEY_CLICK: case TKEY_ENTER:
      _map_pan = !_map_pan;
      _map_pan_at = millis();
      return true;
    case TKEY_ESC:
      if (_map_pan) _map_pan = false;
      else _map_node_sel = -1;
      return true;
    case 'c': case 'C': {
      double lat, lon;
      _map_node_sel = -1;
      if (myPosition(lat, lon)) {
        _map_lat = lat; _map_lon = lon;
        _map_follow = true;
      } else if (_crumb_count > 0) {
        const Crumb& c = _crumbs[(_crumb_head + CD_CRUMBS - 1) % CD_CRUMBS];
        _map_lat = c.lat / 1e6; _map_lon = c.lon / 1e6;
        _map_follow = true;
        showAlert("NO FIX // SHOWING LAST CRUMB", 2000);
      } else {
        showAlert("NO GPS FIX YET", 2000);
      }
      return true;
    }
    case 'n': case 'N': case 'p': case 'P': {
      int dir = (key == 'n' || key == 'N') ? 1 : -1;
      for (int step = 1; step <= _num_nodes; step++) {
        int i = ((_map_node_sel < 0 ? (dir > 0 ? -1 : 0) : _map_node_sel) + dir * step + _num_nodes * 2) % _num_nodes;
        if (_nodes[i].lat || _nodes[i].lon) {
          _map_node_sel = i;
          _map_lat = _nodes[i].lat / 1e6;
          _map_lon = _nodes[i].lon / 1e6;
          _map_follow = false;
          return true;
        }
      }
      showAlert("NO NODES HAVE SHARED A POSITION", 2500);
      return true;
    }
    case 'g': case 'G':
      _screen = SCR_GPS;
      return true;
    case 's': case 'S':
      _prefs.map_style = (_prefs.map_style + 1) % MS_COUNT;
      savePrefs();
      clearTiles();
      showAlert(STYLE_NAMES[_prefs.map_style], 1000);
      return true;
    case 'b': case 'B':
      s_hide_crumbs = !s_hide_crumbs;
      showAlert(s_hide_crumbs ? "TRAIL HIDDEN" : "TRAIL SHOWN", 1000);
      return true;
    case 'l': case 'L':
      _prefs.map_nolabels = !_prefs.map_nolabels;
      savePrefs();
      return true;
    default:
      return false;   // left/right still change tabs
  }
}

/* ---------------------------------------- drawing -------------------------------------- */

void UITask::drawMap() {
  if (!_map_inited) mapInit();
  if (_map_pan && millis() - _map_pan_at > MAP_PAN_IDLE_MS) _map_pan = false;
  Adafruit_GFX& g = _display->gfx();
  uint16_t* buf = _display->buffer();

  double me_lat = 0, me_lon = 0;
  bool have_fix = myPosition(me_lat, me_lon);
  if (_map_follow && have_fix) { _map_lat = me_lat; _map_lon = me_lon; }

  const int z = _map_z;
  double cx = lonToX(_map_lon, z), cy = latToY(_map_lat, z);
  long left = (long)floor(cx - SCR_W / 2.0), top = (long)floor(cy - MAP_H / 2.0);

  // tiles
  int missing = 0, total = 0;
  if (buf) {
    long tx0 = (long)floor(left / (double)TILE), tx1 = (long)floor((left + SCR_W - 1) / (double)TILE);
    long ty0 = (long)floor(top / (double)TILE), ty1 = (long)floor((top + MAP_H - 1) / (double)TILE);
    for (long ty = ty0; ty <= ty1; ty++) {
      for (long tx = tx0; tx <= tx1; tx++) {
        total++;
        Tile* t = getTile(z, tx, ty);
        int sx = (int)(tx * TILE - left), sy = MAP_Y + (int)(ty * TILE - top);
        int x0 = sx < 0 ? 0 : sx, x1 = sx + TILE > SCR_W ? SCR_W : sx + TILE;
        int y0 = sy < MAP_Y ? MAP_Y : sy, y1 = sy + TILE > MAP_Y + MAP_H ? MAP_Y + MAP_H : sy + TILE;
        if (x1 <= x0 || y1 <= y0) continue;
        if (t && t->state == 1) {
          for (int y = y0; y < y1; y++) {
            memcpy(buf + y * SCR_W + x0, t->px + (y - sy) * TILE + (x0 - sx), (x1 - x0) * 2);
          }
        } else {
          missing++;
          for (int y = y0; y < y1; y++) {
            uint16_t* row = buf + y * SCR_W;
            for (int x = x0; x < x1; x++) row[x] = (((x + y) & 15) == 0) ? c_dim : 0;
          }
        }
      }
    }
  }

  auto project = [&](double lat, double lon, int& x, int& y) {
    x = (int)lround(lonToX(lon, z) - left);
    y = MAP_Y + (int)lround(latToY(lat, z) - top);
  };
  auto inMap = [&](int x, int y, int margin) {
    return x >= -margin && x < SCR_W + margin && y >= MAP_Y - margin && y < MAP_Y + MAP_H + margin;
  };

  // breadcrumbs, oldest first
  if (!s_hide_crumbs && _crumbs) {
    int px = 0, py = 0;
    uint32_t pts = 0;
    bool have_prev = false;
    for (int i = 0; i < _crumb_count; i++) {
      const Crumb& c = _crumbs[(_crumb_head - _crumb_count + i + CD_CRUMBS) % CD_CRUMBS];
      int x, y;
      project(c.lat / 1e6, c.lon / 1e6, x, y);
      if (have_prev && c.ts - pts < CRUMB_GAP_SECS && (inMap(x, y, 400) || inMap(px, py, 400))) {
        g.drawLine(px, py, x, y, c_hi);
        g.drawLine(px + 1, py, x + 1, y, c_hi);
      }
      if (z >= 15 && inMap(x, y, 2)) g.fillRect(x - 1, y - 1, 3, 3, c_alert);
      px = x; py = y; pts = c.ts;
      have_prev = true;
    }
  }

  // contacts with a position
  for (int i = 0; i < _num_nodes; i++) {
    const Node& n = _nodes[i];
    if (!n.lat && !n.lon) continue;
    int x, y;
    project(n.lat / 1e6, n.lon / 1e6, x, y);
    if (!inMap(x, y, 4)) continue;
    uint16_t col = (n.type == ADV_TYPE_CHAT) ? c_hi : (n.type == ADV_TYPE_REPEATER) ? c_fg : c_alert;
    switch (n.type) {
      case ADV_TYPE_CHAT:
        g.fillCircle(x, y, 5, 0); g.fillCircle(x, y, 4, col); break;
      case ADV_TYPE_REPEATER:
        g.fillTriangle(x, y - 7, x - 6, y + 4, x + 6, y + 4, 0);
        g.fillTriangle(x, y - 5, x - 4, y + 3, x + 4, y + 3, col); break;
      case ADV_TYPE_ROOM:
        g.fillRect(x - 5, y - 5, 11, 11, 0); g.fillRect(x - 4, y - 4, 9, 9, col); break;
      default:
        g.fillTriangle(x, y - 6, x - 6, y, x, y + 6, 0); g.fillTriangle(x, y - 6, x + 6, y, x, y + 6, 0);
        g.fillTriangle(x, y - 4, x - 4, y, x, y + 4, col); g.fillTriangle(x, y - 4, x + 4, y, x, y + 4, col); break;
    }
    if (i == _map_node_sel && (millis() / 400) % 2) g.drawCircle(x, y, 10, c_alert);
    if ((!_prefs.map_nolabels && z >= 11) || i == _map_node_sel) {
      char label[12];
      snprintf(label, sizeof(label), "%.10s", n.name);
      int w = strlen(label) * SMALL_W + 2;
      int lx = x + 8, ly = y - 4;
      if (lx + w > SCR_W) lx = x - 8 - w;
      g.fillRect(lx, ly - 1, w, 10, 0);
      stext(lx + 1, ly, col, label);
    }
  }

  // us
  LocationProvider* lp = _sensors ? _sensors->getLocationProvider() : NULL;
  if (have_fix) {
    int x, y;
    project(me_lat, me_lon, x, y);
    if (inMap(x, y, 8)) {
#ifdef NMEA_KEEP_RECENT
      const MicroNMEA& nm = static_cast<MicroNMEALocationProvider*>(lp)->nmeaState();
      double kmh = nm.getSpeed() / 1000.0 * 1.852;
      if (kmh > 1.5) {
        double a = nm.getCourse() / 1000.0 * M_PI / 180;
        g.drawLine(x, y, x + (int)(18 * sin(a)), y - (int)(18 * cos(a)), c_alert);
      }
#endif
      g.drawCircle(x, y, 7, 0);
      g.drawCircle(x, y, 6, c_alert);
      g.drawCircle(x, y, 5, c_alert);
      g.fillCircle(x, y, 2, c_alert);
    }
  } else if (_crumb_count > 0) {   // where we last were
    const Crumb& c = _crumbs[(_crumb_head + CD_CRUMBS - 1) % CD_CRUMBS];
    int x, y;
    project(c.lat / 1e6, c.lon / 1e6, x, y);
    if (inMap(x, y, 6)) { g.drawRect(x - 4, y - 4, 9, 9, c_alert); g.drawRect(x - 5, y - 5, 11, 11, 0); }
  }

  // read-out, top left
  {
    char l1[32], l2[32], l3[32];
    snprintf(l1, sizeof(l1), "Z%d %s%s", z, STYLE_NAMES[_prefs.map_style], _map_pan ? " PAN" : _map_follow ? " FOLLOW" : "");
    snprintf(l2, sizeof(l2), "%.5f %.5f", have_fix ? me_lat : _map_lat, have_fix ? me_lon : _map_lon);
    if (lp == NULL) strcpy(l3, "NO GPS");
    else if (!getGPSState()) strcpy(l3, "GPS OFF // gps on");
    else if (!have_fix) snprintf(l3, sizeof(l3), "NO FIX // %ld SATS", lp->satellitesCount());
    else {
#ifdef NMEA_KEEP_RECENT
      const MicroNMEA& nm = static_cast<MicroNMEALocationProvider*>(lp)->nmeaState();
      snprintf(l3, sizeof(l3), "%u SAT HDOP%.1f %dKMH", nm.getNumSatellites(), nm.getHDOP() / 10.0, (int)(nm.getSpeed() / 1000.0 * 1.852));
#else
      snprintf(l3, sizeof(l3), "%ld SATS", lp->satellitesCount());
#endif
    }
    int w = 0;
    w = max(w, (int)strlen(l1)); w = max(w, (int)strlen(l2)); w = max(w, (int)strlen(l3));
    g.fillRect(0, MAP_Y, w * SMALL_W + 5, 30, 0);
    g.drawRect(0, MAP_Y, w * SMALL_W + 5, 30, c_dim);
    stext(3, MAP_Y + 2, _map_pan ? c_alert : c_hi, l1);
    stext(3, MAP_Y + 11, c_fg, l2);
    stext(3, MAP_Y + 20, have_fix ? c_fg : c_alert, l3);
  }

  // scale bar, bottom right
  {
    double mpp = 156543.03392 * cos(_map_lat * M_PI / 180) / (double)(1UL << z);
    static const double steps[] = { 5, 10, 20, 50, 100, 200, 500, 1000, 2000, 5000, 10000, 20000, 50000, 100000 };
    double len = steps[0];
    for (double s : steps) if (s / mpp <= 90) len = s;
    int px = (int)(len / mpp);
    char lab[12];
    fmtDist(lab, sizeof(lab), len);
    int x1 = SCR_W - 6, x0 = x1 - px, y = MAP_Y + MAP_H - 6;
    g.fillRect(x0 - 3, y - 12, px + 7, 16, 0);
    g.drawFastHLine(x0, y, px, c_hi);
    g.drawFastVLine(x0, y - 3, 4, c_hi);
    g.drawFastVLine(x1, y - 3, 4, c_hi);
    stext(x1 - strlen(lab) * SMALL_W, y - 11, c_hi, lab);
  }

  // the selected node, bottom left
  if (_map_node_sel >= 0 && _map_node_sel < _num_nodes) {
    const Node& n = _nodes[_map_node_sel];
    char line[48], ago[8], dist[12] = "";
    fmtAgo(ago, n.lastmod);
    double ref_lat = have_fix ? me_lat : (_crumb_count ? _crumbs[(_crumb_head + CD_CRUMBS - 1) % CD_CRUMBS].lat / 1e6 : 0);
    double ref_lon = have_fix ? me_lon : (_crumb_count ? _crumbs[(_crumb_head + CD_CRUMBS - 1) % CD_CRUMBS].lon / 1e6 : 0);
    if (have_fix || _crumb_count) {
      double d = distM(ref_lat, ref_lon, n.lat / 1e6, n.lon / 1e6);
      char ds[10];
      fmtDist(ds, sizeof(ds), d);
      snprintf(dist, sizeof(dist), "%s %s", ds, compass(bearingDeg(ref_lat, ref_lon, n.lat / 1e6, n.lon / 1e6)));
    }
    snprintf(line, sizeof(line), "[%s] %.12s %s %s", nodeTag(n.type), n.name, dist, ago);
    g.fillRect(0, MAP_Y + MAP_H - 18, strlen(line) * SMALL_W + 6, 12, 0);
    stext(3, MAP_Y + MAP_H - 16, c_hi, line);
  }

  if (_map_zmax < 0) {
    const char* msg = _sd_ok ? "NO /maps ON THE SD CARD" : "NO SD CARD // NODES ONLY";
    int w = strlen(msg) * CHAR_W;
    g.fillRect((SCR_W - w) / 2 - 4, MAP_Y + MAP_H / 2 - 10, w + 8, 20, 0);
    text((SCR_W - w) / 2, MAP_Y + MAP_H / 2 - 8, c_alert, msg);
  } else if (total && missing == total) {
    const char* msg = "NO TILES HERE AT THIS ZOOM";
    int w = strlen(msg) * CHAR_W;
    g.fillRect((SCR_W - w) / 2 - 4, MAP_Y + MAP_H / 2 + 14, w + 8, 20, 0);
    text((SCR_W - w) / 2, MAP_Y + MAP_H / 2 + 16, c_dim, msg);
  }

  // the chrome goes back over anything that strayed outside the map
  g.fillRect(0, 0, SCR_W, MAP_Y, c_bg);
  g.fillRect(0, MAP_Y + MAP_H, SCR_W, CYBERDECK_H - MAP_Y - MAP_H, c_bg);
  drawHeader();
  drawTabs();
  drawFooter(_map_pan ? "PAN: BALL MOVES MAP  CLICK OR WAIT 5s TO STOP"
                      : "\x18\x19 ZOOM  CLICK PAN  C ME  N NODE  G GPS  S STYLE");
}

void UITask::drawGps() {
  Adafruit_GFX& g = _display->gfx();
  LocationProvider* lp = _sensors ? _sensors->getLocationProvider() : NULL;
  text(0, TABS_Y, c_hi, "GPS // RECEIVER");
  g.drawFastHLine(0, TABS_Y + ROW_H + 1, SCR_W, c_dim);
  int y = BODY_Y;

  if (lp == NULL) {
    text(0, y, c_alert, "NO GPS ON THIS DECK");
    drawFooter("G / ESC BACK");
    return;
  }

#ifdef NMEA_KEEP_RECENT
  MicroNMEALocationProvider* gp = static_cast<MicroNMEALocationProvider*>(lp);
  const MicroNMEA& nm = gp->nmeaState();
  bool fix = nm.isValid();
  textf(0, y, fix ? c_hi : c_alert, "FIX  %-4s NAV %c  SATS %-2u HDOP %.1f", fix ? "YES" : "NO", nm.getNavSystem() ? nm.getNavSystem() : '-',
        nm.getNumSatellites(), nm.getHDOP() / 10.0);
  y += ROW_H;

  double lat = nm.getLatitude() / 1e6, lon = nm.getLongitude() / 1e6;
  auto dms = [](char* d, size_t sz, double v, char pos, char neg) {
    char h = v >= 0 ? pos : neg;
    v = fabs(v);
    int deg = (int)v;
    double mins = (v - deg) * 60;
    int m = (int)mins;
    snprintf(d, sz, "%d\xF8%02d'%04.1f\"%c", deg, m, (mins - m) * 60, h);
  };
  char a[24];
  dms(a, sizeof(a), lat, 'N', 'S');
  textf(0, y, c_fg, "LAT %11.6f  %s", lat, a);
  y += ROW_H;
  dms(a, sizeof(a), lon, 'E', 'W');
  textf(0, y, c_fg, "LON %11.6f  %s", lon, a);
  y += ROW_H;
  long alt_mm = 0;
  if (nm.getAltitude(alt_mm)) textf(0, y, c_fg, "ALT %.1fM  %dFT", alt_mm / 1000.0, (int)(alt_mm / 304.8));
  else text(0, y, c_dim, "ALT --");
  y += ROW_H;
  double kmh = nm.getSpeed() / 1000.0 * 1.852, crs = nm.getCourse() / 1000.0;
  textf(0, y, c_fg, "SPD %.1fKMH %.1fMPH  CRS %d %s", kmh, kmh / 1.609344, (int)crs, compass(crs));
  y += ROW_H;
  if (nm.getYear() > 2000) textf(0, y, c_fg, "UTC %04u-%02u-%02u %02u:%02u:%02u", nm.getYear(), nm.getMonth(), nm.getDay(),
                                 nm.getHour(), nm.getMinute(), nm.getSecond());
  else text(0, y, c_dim, "UTC --");
  y += ROW_H + 2;

  char diag[96];
  lp->formatDiagnostics(diag, sizeof(diag));
  stext(0, y, c_dim, diag);
  y += 10;
  g.drawFastHLine(0, y - 1, SCR_W, c_dim);
  y += 1;
  for (int i = 0; i < 8 && y < FOOT_Y - 10; i++, y += 9) {
    const char* s = gp->recentSentence(i);
    if (s == NULL) { if (i == 0) stext(0, y, c_dim, "WAITING FOR NMEA..."); break; }
    char line[54];
    snprintf(line, sizeof(line), "%.53s", s);
    stext(0, y, i == 0 ? c_hi : c_fg, line);
  }
  char foot[48];
  snprintf(foot, sizeof(foot), "G / ESC BACK   %lu NMEA SENTENCES", (unsigned long)gp->sentencesSeen());
  drawFooter(foot);
#else
  textf(0, y, c_fg, "FIX %s  %ld SATS", lp->isValid() ? "YES" : "NO", lp->satellitesCount());
  drawFooter("G / ESC BACK");
#endif
}
