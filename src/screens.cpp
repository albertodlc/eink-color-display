#include "screens.h"

#include <Arduino.h>

namespace {

void drawCentered(epd::Display& display, const char* text, int16_t center_x,
                  int16_t y) {
  int16_t x1 = 0;
  int16_t y1 = 0;
  uint16_t w = 0;
  uint16_t h = 0;
  display.getTextBounds(text, 0, 0, &x1, &y1, &w, &h);
  display.setCursor(center_x - w / 2 - x1, y);
  display.print(text);
}

struct Swatch {
  uint16_t color;
  const char* label;
  uint16_t label_color;
};

}  // namespace

void drawSplash(epd::Display& display) {
  display.fillScreen(GxEPD_WHITE);

  // Header band.
  display.fillRect(0, 0, epd::kWidth, 120, GxEPD_BLACK);
  display.setTextSize(4);
  display.setTextColor(GxEPD_WHITE);
  display.setCursor(40, 30);
  display.print("E INK");
  display.setTextColor(GxEPD_YELLOW);
  display.print(" COLOR");

  display.setTextSize(2);
  display.setTextColor(GxEPD_WHITE);
  display.setCursor(40, 78);
  display.print("SPECTRA 6 / 7.3 inch");

  // Board and panel identification.
  display.setTextColor(GxEPD_BLACK);
  display.setCursor(40, 145);
  display.print("ESP32-L(C73) + GDEP073E01");

  display.setTextSize(1);
  display.setCursor(40, 180);
  display.print("800 x 480 px  |  6 native colors  |  SPI");

  // Native palette.
  const Swatch swatches[] = {
      {GxEPD_BLACK, "BLACK", GxEPD_WHITE},
      {GxEPD_WHITE, "WHITE", GxEPD_BLACK},
      {GxEPD_RED, "RED", GxEPD_WHITE},
      {GxEPD_YELLOW, "YELLOW", GxEPD_BLACK},
      {GxEPD_BLUE, "BLUE", GxEPD_WHITE},
      {GxEPD_GREEN, "GREEN", GxEPD_BLACK},
  };

  const int16_t sw = 100;
  const int16_t sh = 100;
  const int16_t gap = 20;
  const int16_t x0 = 50;
  const int16_t y0 = 230;

  for (uint8_t i = 0; i < 6; i++) {
    const int16_t x = x0 + i * (sw + gap);
    display.fillRect(x, y0, sw, sh, swatches[i].color);
    if (swatches[i].color == GxEPD_WHITE) {
      display.drawRect(x, y0, sw, sh, GxEPD_BLACK);
    }

    display.setTextSize(1);
    display.setTextColor(swatches[i].label_color);
    drawCentered(display, swatches[i].label, x + sw / 2, y0 + sh - 18);
  }

  // Footer.
  display.setTextSize(1);
  display.setTextColor(GxEPD_BLACK);
  display.setCursor(40, 440);
  display.print("Full refresh 15-22 s  |  display.hibernate() after update");
}

void drawColorBars(epd::Display& display) {
  const int16_t bars_h = epd::kHeight - 64;
  const int16_t bar_w = epd::kWidth / 6;

  const Swatch bars[] = {
      {GxEPD_BLACK, "BLACK", GxEPD_WHITE},
      {GxEPD_WHITE, "WHITE", GxEPD_BLACK},
      {GxEPD_RED, "RED", GxEPD_WHITE},
      {GxEPD_YELLOW, "YELLOW", GxEPD_BLACK},
      {GxEPD_BLUE, "BLUE", GxEPD_WHITE},
      {GxEPD_GREEN, "GREEN", GxEPD_BLACK},
  };

  display.fillScreen(GxEPD_WHITE);

  for (uint8_t i = 0; i < 6; i++) {
    const int16_t x = i * bar_w;
    const int16_t w = (i == 5) ? (epd::kWidth - 5 * bar_w) : bar_w;

    display.fillRect(x, 0, w, bars_h, bars[i].color);
    if (bars[i].color == GxEPD_WHITE) {
      display.drawRect(x, 0, w, bars_h, GxEPD_BLACK);
    }

    display.setTextSize(2);
    display.setTextColor(bars[i].label_color);
    drawCentered(display, bars[i].label, x + w / 2, bars_h - 40);
  }

  display.fillRect(0, epd::kHeight - 64, epd::kWidth, 64, GxEPD_BLACK);
  display.setTextSize(2);
  display.setTextColor(GxEPD_WHITE);
  drawCentered(display, "E INK SPECTRA 6  |  6 NATIVE COLORS",
               epd::kWidth / 2, epd::kHeight - 42);
}
