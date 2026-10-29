#pragma once

// Native-resolution display driver for the T-Deck's 320x240 ST7789, used by the
// cyberdeck UI. Unlike ST7789LCDDisplay (which scales a virtual 128x64 screen),
// this draws into a full framebuffer in PSRAM and pushes it to the panel, so the
// UI can use small text, box drawing and full-screen effects without flicker.

#include <helpers/ui/DisplayDriver.h>
#include <SPI.h>
#include <Adafruit_GFX.h>
#include <Adafruit_ST7789.h>

#define CYBERDECK_W  320
#define CYBERDECK_H  240

class CyberDeckDisplay : public DisplayDriver {
  SPIClass _spi;
  Adafruit_ST7789 _tft;
  GFXcanvas16* _canvas;
  bool _isOn;
  uint16_t _color;
  int _text_size;

public:
  CyberDeckDisplay();

  bool begin();

  // the surface to draw on: the framebuffer, or the panel itself if PSRAM allocation failed
  Adafruit_GFX& gfx() { return _canvas ? (Adafruit_GFX&)*_canvas : (Adafruit_GFX&)_tft; }
  uint16_t* buffer() { return _canvas ? _canvas->getBuffer() : NULL; }

  // push only rows [y, y+h) of the framebuffer to the panel
  void pushRows(int y, int h);

  bool isOn() override { return _isOn; }
  void turnOn() override;
  void turnOff() override;
  void clear() override;
  void startFrame(ColorVal bkg = UIColor::window_bkg) override;
  void setTextSize(int sz) override;
  void setColor(ColorVal c) override;
  void setCursor(int x, int y) override;
  void print(const char* str) override;
  void fillRect(int x, int y, int w, int h) override;
  void drawRect(int x, int y, int w, int h) override;
  void drawXbm(int x, int y, const uint8_t* bits, int w, int h) override;
  uint16_t getTextWidth(const char* str) override;
  void endFrame() override;
};
