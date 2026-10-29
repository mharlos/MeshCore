#ifdef CYBERDECK_UI

#include "CyberDeckDisplay.h"

#ifndef PIN_TFT_MISO
  #define PIN_TFT_MISO -1
#endif

#ifndef DISPLAY_ROTATION
  #define DISPLAY_ROTATION 3
#endif

CyberDeckDisplay::CyberDeckDisplay() : DisplayDriver(CYBERDECK_W, CYBERDECK_H),
    _spi(HSPI), _tft(&_spi, PIN_TFT_CS, PIN_TFT_DC, PIN_TFT_RST), _canvas(NULL) {
  _isOn = false;
  _color = 0xFFFF;
  _text_size = 1;
}

bool CyberDeckDisplay::begin() {
  if (_isOn) return true;

  if (PIN_TFT_LEDA_CTL != -1) {
    pinMode(PIN_TFT_LEDA_CTL, OUTPUT);
    digitalWrite(PIN_TFT_LEDA_CTL, HIGH);
  }

  _spi.begin(PIN_TFT_SCL, PIN_TFT_MISO, PIN_TFT_SDA, PIN_TFT_CS);
  _tft.init(CYBERDECK_H, CYBERDECK_W);   // panel is portrait natively
  _tft.setRotation(DISPLAY_ROTATION);
  _tft.setSPISpeed(40e6);
  _tft.fillScreen(ST77XX_BLACK);

  if (_canvas == NULL) {
    _canvas = new GFXcanvas16(CYBERDECK_W, CYBERDECK_H);   // 150KB, malloc places it in PSRAM
    if (_canvas->getBuffer() == NULL) {   // no PSRAM: fall back to drawing on the panel directly
      delete _canvas;
      _canvas = NULL;
    }
  }
  gfx().cp437(true);
  gfx().setTextWrap(false);

  _isOn = true;
  return true;
}

void CyberDeckDisplay::turnOn() {
  if (!_isOn) {
    if (PIN_TFT_LEDA_CTL != -1) digitalWrite(PIN_TFT_LEDA_CTL, HIGH);
    _isOn = true;
  }
}

void CyberDeckDisplay::turnOff() {
  if (_isOn) {
    if (PIN_TFT_LEDA_CTL != -1) digitalWrite(PIN_TFT_LEDA_CTL, LOW);
    _isOn = false;
  }
}

void CyberDeckDisplay::pushRows(int y, int h) {
  if (_canvas == NULL || !_isOn) return;
  if (y < 0) { h += y; y = 0; }
  if (y + h > CYBERDECK_H) h = CYBERDECK_H - y;
  if (h <= 0) return;

  _tft.startWrite();
  _tft.setAddrWindow(0, y, CYBERDECK_W, h);
  _tft.writePixels(_canvas->getBuffer() + y * CYBERDECK_W, (uint32_t)CYBERDECK_W * h, true, false);
  _tft.endWrite();
}

void CyberDeckDisplay::clear() {
  gfx().fillScreen(0);
}

void CyberDeckDisplay::startFrame(ColorVal bkg) {
  gfx().fillScreen(0);   // the cyberdeck is always dark, whatever the caller asks for
  _color = 0xFFFF;
  gfx().setTextColor(_color);
  setTextSize(1);
}

void CyberDeckDisplay::setTextSize(int sz) {
  _text_size = sz;
  gfx().setTextSize(sz);
}

void CyberDeckDisplay::setColor(ColorVal c) {
  // DisplayDriver callers outside the cyberdeck UI pass theme colours meant for a
  // white background; render all of them as phosphor green on black
  _color = (c == 0) ? 0 : 0x37EC;
  gfx().setTextColor(_color);
}

void CyberDeckDisplay::setCursor(int x, int y) {
  gfx().setCursor(x, y);
}

void CyberDeckDisplay::print(const char* str) {
  gfx().print(str);
}

void CyberDeckDisplay::fillRect(int x, int y, int w, int h) {
  gfx().fillRect(x, y, w, h, _color);
}

void CyberDeckDisplay::drawRect(int x, int y, int w, int h) {
  gfx().drawRect(x, y, w, h, _color);
}

void CyberDeckDisplay::drawXbm(int x, int y, const uint8_t* bits, int w, int h) {
  gfx().drawXBitmap(x, y, bits, w, h, _color);
}

uint16_t CyberDeckDisplay::getTextWidth(const char* str) {
  return strlen(str) * 6 * _text_size;   // built-in font is fixed 6px per glyph
}

void CyberDeckDisplay::endFrame() {
  pushRows(0, CYBERDECK_H);
}

#endif
