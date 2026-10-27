#pragma once

#include <GxEPD2.h>
#include <GxEPD2_7C.h>

#include "board_esp32_l_c73.h"

namespace epd {

// GDEP073E01 full refresh measured at ~27 s on the ESP32-L(C73) kit, longer
// than GxEPD2's default 20 s busy timeout. The driver would otherwise report
// "Busy Timeout!" and send power-off while the panel is still refreshing.
class Gdep073e01LongTimeout : public GxEPD2_730c_GDEP073E01 {
 public:
  Gdep073e01LongTimeout(int16_t cs, int16_t dc, int16_t rst, int16_t busy)
      : GxEPD2_730c_GDEP073E01(cs, dc, rst, busy) {
    _busy_timeout = 45000000UL;  // 45 s
  }
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
