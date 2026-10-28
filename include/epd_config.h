#pragma once

#include <Arduino.h>
#include <GxEPD2.h>
#include <GxEPD2_7C.h>

#include "board_esp32_l_c73.h"

// Rotate the complete panel output 180 degrees, for an upside-down mounting.
// The paged renderer draws logical pages in reverse order so the page buffer
// stays aligned with the physical panel.
#ifndef EPD_ROTATE_180
#define EPD_ROTATE_180 1
#endif

namespace epd {

// GDEP073E01 full refresh measured at ~27 s on the ESP32-L(C73) kit, longer
// than GxEPD2's default 20 s busy timeout. The driver would otherwise report
// "Busy Timeout!" and send power-off while the panel is still refreshing.
//
// The refresh overrides only record how long the panel held BUSY. Host-side
// drawing and SPI transfer happen before that and are reported separately, so
// the demo log shows where the ~27 s goes.
class Gdep073e01LongTimeout : public GxEPD2_730c_GDEP073E01 {
 public:
  Gdep073e01LongTimeout(int16_t cs, int16_t dc, int16_t rst, int16_t busy)
      : GxEPD2_730c_GDEP073E01(cs, dc, rst, busy) {
    _busy_timeout = 45000000UL;  // 45 s
  }

  void refresh(bool partial_update_mode = false) override {
    const uint32_t start = millis();
    GxEPD2_730c_GDEP073E01::refresh(partial_update_mode);
    _last_refresh_ms = millis() - start;
  }

  void refresh(int16_t x, int16_t y, int16_t w, int16_t h) override {
    const uint32_t start = millis();
    GxEPD2_730c_GDEP073E01::refresh(x, y, w, h);
    _last_refresh_ms = millis() - start;
  }

  // Duration of the most recent panel refresh (the BUSY wait).
  uint32_t lastRefreshMs() const { return _last_refresh_ms; }

  // Read bytes from the controller over the bidirectional SDI line.
  // The DESPI-C73 exposes only SDI (ESP32 MOSI), so SCK is bit-banged and the
  // same pin is sampled as data input. CS is held low across command + read.
  // Returns true if the received data was not all 0x00/0xFF.
  bool readBytes(uint8_t cmd, uint8_t* data, uint32_t n) {
    _pSPIx->beginTransaction(_spi_settings);
    if (_dc >= 0) digitalWrite(_dc, LOW);
    if (_cs >= 0) digitalWrite(_cs, LOW);
    _pSPIx->transfer(cmd);
    if (_dc >= 0) digitalWrite(_dc, HIGH);
    _pSPIx->endTransaction();
    _pSPIx->end();

    pinMode(_sck, OUTPUT);
    digitalWrite(_sck, LOW);
    pinMode(_mosi, INPUT);

    bool plausible = false;
    for (uint32_t i = 0; i < n; i++) {
      uint8_t value = 0;
      for (uint8_t bit = 0; bit < 8; bit++) {
        digitalWrite(_sck, HIGH);
        delayMicroseconds(1);
        value = (value << 1) | (digitalRead(_mosi) ? 1 : 0);
        digitalWrite(_sck, LOW);
        delayMicroseconds(1);
      }
      data[i] = value;
      if (value != 0x00 && value != 0xFF) plausible = true;
    }

    if (_cs >= 0) digitalWrite(_cs, HIGH);
    _pSPIx->begin();
    return plausible;
  }

  // Split command/read variants for controllers that need CS released between
  // the read command and the data phase.
  void sendCommand(uint8_t cmd) { _writeCommand(cmd); }

  void readData(uint8_t* data, uint32_t n) {
    _pSPIx->end();
    pinMode(_sck, OUTPUT);
    digitalWrite(_sck, LOW);
    pinMode(_mosi, INPUT);
    if (_cs >= 0) digitalWrite(_cs, LOW);

    for (uint32_t i = 0; i < n; i++) {
      uint8_t value = 0;
      for (uint8_t bit = 0; bit < 8; bit++) {
        digitalWrite(_sck, HIGH);
        delayMicroseconds(1);
        value = (value << 1) | (digitalRead(_mosi) ? 1 : 0);
        digitalWrite(_sck, LOW);
        delayMicroseconds(1);
      }
      data[i] = value;
    }

    if (_cs >= 0) digitalWrite(_cs, HIGH);
    _pSPIx->begin();
  }

  // Streaming read API: keeps CS low across the whole OTP transfer so the
  // controller does not restart the read between chunks.
  void readBegin(uint8_t cmd) {
    _pSPIx->beginTransaction(_spi_settings);
    if (_dc >= 0) digitalWrite(_dc, LOW);
    if (_cs >= 0) digitalWrite(_cs, LOW);
    _pSPIx->transfer(cmd);
    if (_dc >= 0) digitalWrite(_dc, HIGH);
    _pSPIx->endTransaction();
    _pSPIx->end();
    pinMode(_sck, OUTPUT);
    digitalWrite(_sck, LOW);
    pinMode(_mosi, INPUT);
    // CS stays low for the whole read.
  }

  uint8_t readByte() {
    uint8_t value = 0;
    for (uint8_t bit = 0; bit < 8; bit++) {
      digitalWrite(_sck, HIGH);
      delayMicroseconds(1);
      value = (value << 1) | (digitalRead(_mosi) ? 1 : 0);
      digitalWrite(_sck, LOW);
      delayMicroseconds(1);
    }
    return value;
  }

  uint8_t readByteFalling() {
    uint8_t value = 0;
    for (uint8_t bit = 0; bit < 8; bit++) {
      digitalWrite(_sck, HIGH);
      delayMicroseconds(1);
      digitalWrite(_sck, LOW);
      delayMicroseconds(1);
      value = (value << 1) | (digitalRead(_mosi) ? 1 : 0);
    }
    return value;
  }

  void readEnd() {
    if (_cs >= 0) digitalWrite(_cs, HIGH);
    _pSPIx->begin();
  }

  // Official Good Display partial-window sequence (CMD 0x83 + 0x10 + 0x50 +
  // 0x12). x/y are inclusive; w/h are the window size in pixels.
  // native_color is the controller 4 bpp code: 0 black, 1 white, 2 yellow,
  // 3 red, 5 blue, 6 green. The vendor sample sends a 0x00 prefix byte after
  // 0x10; set prefix=false for the ESPHome-style sequence.
  // CDI 0x3F + DRF 0x00 is the vendor sequence; CDI 0xFF + DRF 0x01 is the
  // DCVCOM variant used by some fast-partial experiments. Local test: both
  // took 26 s on this unit; see docs/refresh-speed.md.
  void partialWindowSolid(uint16_t x, uint16_t y, uint16_t w, uint16_t h,
                          uint8_t native_color, bool prefix = true,
                          uint8_t cdi = 0x3F, uint8_t drf = 0x00) {
    if (!_power_is_on) {
      _writeCommand(0x04);  // PON
      _waitWhileBusy("_PowerOn", 200);
      _power_is_on = true;
    }

    const uint16_t x_end = x + w - 1;
    const uint16_t y_end = y + h - 1;

    _writeCommand(0x83);  // PTLW partial window
    _writeData((x >> 8) & 0x03);
    _writeData(x & 0xFF);
    _writeData((x_end >> 8) & 0x03);
    _writeData(x_end & 0xFF);
    _writeData((y >> 8) & 0x03);
    _writeData(y & 0xFF);
    _writeData((y_end >> 8) & 0x03);
    _writeData(y_end & 0xFF);
    _writeData(0x01);  // PMODE = 1

    _writeCommand(0x10);  // DTM
    if (prefix) _writeData(0x00);

    const uint8_t packed = (native_color << 4) | (native_color & 0x0F);
    _startTransfer();
    for (uint16_t row = 0; row < h; row++) {
      for (uint16_t col = 0; col < w / 2; col++) {
        _transfer(packed);
      }
    }
    _endTransfer();

    _writeCommand(0x50);  // CDI
    _writeData(cdi);
    _writeCommand(0x12);  // DRF
    _writeData(drf);
    delay(20);  // ED2208 asserts BUSY low after DRF
    const uint32_t start = millis();
    _waitWhileBusy("_partial", 45000);
    _last_refresh_ms = millis() - start;
  }

  // "Tosone's Law" init variant for the SPD1657A controller used on this
  // panel: PSR (0x00) must be the first command after hardware reset, before
  // PWRR/BTST/PLL. The stock GxEPD2 sequence sends PSR later, which has been
  // reported to double DRF time (28-46 s instead of 14-15 s).
  // PSR.B = 0xE9 enables the internal LUT/fallback path.
  // NOTE: this simplified sequence hung at the BUSY timeout on the test unit.
  // The full published recipe is incomplete; see docs/otp-readback.md.
  void initTosone(bool use_lut = true) {
    if (_power_is_on) {
      _writeCommand(0x02);  // POF
      _writeData(0x00);
      _waitWhileBusy("_PowerOff", 150);
      _power_is_on = false;
    }
    _reset();
    _power_is_on = false;

    _writeCommand(0x00);  // PSR - must be first
    _writeData(0x5F);
    _writeData(use_lut ? 0xE9 : 0x69);

    _writeCommand(0x01);  // PWRR
    _writeData(0x3F);

    _writeCommand(0x03);  // POFS
    _writeData(0x00);
    _writeData(0x54);
    _writeData(0x00);
    _writeData(0x44);

    _writeCommand(0x05);  // BTST1
    _writeData(0x40);
    _writeData(0x1F);
    _writeData(0x1F);
    _writeData(0x2C);

    _writeCommand(0x06);  // BTST2
    _writeData(0x6F);
    _writeData(0x1F);
    _writeData(0x17);
    _writeData(0x49);

    _writeCommand(0x08);  // BTST3
    _writeData(0x6F);
    _writeData(0x1F);
    _writeData(0x1F);
    _writeData(0x22);

    _writeCommand(0x30);  // PLL (vendor canonical 0x08)
    _writeData(0x08);

    _writeCommand(0x50);  // CDI
    _writeData(0x3F);

    _writeCommand(0x60);  // TCON
    _writeData(0x02);
    _writeData(0x00);

    _writeCommand(0x61);  // TRES
    _writeData(0x03);
    _writeData(0x20);
    _writeData(0x01);
    _writeData(0xE0);

    _writeCommand(0x84);  // T_VDCS
    _writeData(0x01);

    _writeCommand(0xE3);  // PWS
    _writeData(0x2F);

    _writeCommand(0x04);  // PON
    _waitWhileBusy("_PowerOn", 200);
    _power_is_on = true;
    _init_display_done = true;
  }

  // Good Display's "EPD_init_fast" sequence from docs/ESP32-GDEP073E01.zip.
  // GxEPD2 uses the stock (slower) sequence; this applies the vendor's
  // alternate PLL, booster and VCOM settings. Experimental: call after init()
  // and check both refresh time and image quality.
  void initFast() {
    if (_power_is_on) {
      _writeCommand(0x02);  // POF
      _writeData(0x00);
      _waitWhileBusy("_PowerOff", 150);
      _power_is_on = false;
    }
    _reset();
    _power_is_on = false;

    _writeCommand(0xAA);  // CMDH
    _writeData(0x49);
    _writeData(0x55);
    _writeData(0x20);
    _writeData(0x08);
    _writeData(0x09);
    _writeData(0x18);

    _writeCommand(0x01);  // PWRR
    _writeData(0x3F);
    _writeData(0x00);
    _writeData(0x32);
    _writeData(0x2A);
    _writeData(0x0E);
    _writeData(0x2A);

    _writeCommand(0x00);  // PSR
    _writeData(0x5F);
    _writeData(0x69);

    _writeCommand(0x03);  // POFS
    _writeData(0x00);
    _writeData(0x54);
    _writeData(0x00);
    _writeData(0x44);

    _writeCommand(0x05);  // BTST1
    _writeData(0x40);
    _writeData(0x1F);
    _writeData(0x1F);
    _writeData(0x2C);

    _writeCommand(0x06);  // BTST2
    _writeData(0x6F);
    _writeData(0x1F);
    _writeData(0x16);
    _writeData(0x25);

    _writeCommand(0x08);  // BTST3
    _writeData(0x6F);
    _writeData(0x1F);
    _writeData(0x1F);
    _writeData(0x22);

    _writeCommand(0x13);  // IPC
    _writeData(0x00);
    _writeData(0x04);

    _writeCommand(0x30);  // PLL
    _writeData(0x02);

    _writeCommand(0x41);  // TSE
    _writeData(0x00);

    _writeCommand(0x50);  // CDI
    _writeData(0x3F);

    _writeCommand(0x60);  // TCON
    _writeData(0x02);
    _writeData(0x00);

    _writeCommand(0x61);  // TRES
    _writeData(0x03);
    _writeData(0x20);
    _writeData(0x01);
    _writeData(0xE0);

    _writeCommand(0x82);  // VDCS
    _writeData(0x1E);

    _writeCommand(0x84);  // T_VDCS
    _writeData(0x01);

    _writeCommand(0x86);  // AGID
    _writeData(0x00);

    _writeCommand(0xE3);  // PWS
    _writeData(0x2F);

    _writeCommand(0xE0);  // CCSET
    _writeData(0x00);

    _writeCommand(0xE6);  // TSSET
    _writeData(0x00);

    _writeCommand(0x04);  // PON
    _waitWhileBusy("_PowerOn", 200);
    _power_is_on = true;
    _init_display_done = true;
  }

 private:
  uint32_t _last_refresh_ms = 0;
};

// Good Display GDEP073E01: 7.3", 800x480, E Ink Spectra 6.
using Panel = Gdep073e01LongTimeout;

static constexpr uint16_t kWidth  = Panel::WIDTH;
static constexpr uint16_t kHeight = Panel::HEIGHT;

// 4 bpp page buffer = 400 bytes x 160 rows = 64,000 bytes.
// 480 / 160 = 3 pages, which keeps RAM comfortable on the ESP32.
static constexpr uint16_t kPageHeight = 160;

using Display = GxEPD2_7C<Panel, kPageHeight>;

}  // namespace epd
