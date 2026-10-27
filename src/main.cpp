#include <Arduino.h>

#if EPD_DEEP_SLEEP_SECONDS > 0
#include <esp_sleep.h>
#endif

#include "epd_config.h"
#include "screens.h"

epd::Display display(epd::Panel(EPD_CS_PIN, EPD_DC_PIN, EPD_RST_PIN, EPD_BUSY_PIN));

static void renderScreen(void (*draw)(epd::Display&), const char* name) {
  Serial.printf("[epd] rendering %s...\n", name);
  Serial.flush();

  const uint32_t start = millis();
  display.firstPage();
  do {
    draw(display);
  } while (display.nextPage());

  Serial.printf("[epd] %s refreshed in %lu ms\n", name,
                (unsigned long)(millis() - start));
  Serial.flush();
}

#if EPD_DEEP_SLEEP_SECONDS > 0
static void enterDeepSleep() {
  Serial.printf("[sys] deep sleep for %d s\n", EPD_DEEP_SLEEP_SECONDS);
  Serial.flush();
  esp_sleep_enable_timer_wakeup((uint64_t)EPD_DEEP_SLEEP_SECONDS * 1000000ULL);
  esp_deep_sleep_start();
}
#endif

void setup() {
  Serial.begin(115200);
  delay(300);

  Serial.println();
  Serial.println("==========================================");
  Serial.println(" E Ink Spectra 6 bring-up");
  Serial.printf("  board : %s (%s)\n", BOARD_NAME, BOARD_MCU);
  Serial.printf("  panel : GDEP073E01 %ux%u\n", epd::kWidth, epd::kHeight);
  Serial.printf("  pages : %u x %u px\n", (unsigned)epd::kPageHeight,
                (unsigned)((epd::kHeight + epd::kPageHeight - 1) / epd::kPageHeight));
  Serial.println("==========================================");

  // 10 MHz: the panel supports up to 20 MHz, but the ESP32-L wiring and the
  // official Good Display sample use 10 MHz, which is more reliable here.
  display.init(115200, true, 10, false, SPI,
               SPISettings(10000000, MSBFIRST, SPI_MODE0));
  display.setRotation(0);
  display.setFullWindow();
  display.setTextWrap(false);

  renderScreen(drawSplash, "splash");
  delay(2000);
  renderScreen(drawColorBars, "color bars");

  Serial.println("[epd] hibernating panel");
  display.hibernate();

#if EPD_DEEP_SLEEP_SECONDS > 0
  enterDeepSleep();
#endif

  Serial.println("[epd] done");
}

void loop() {
  // The panel keeps the last image without power.
  // For battery operation enable EPD_DEEP_SLEEP_SECONDS and let the timer wake
  // the ESP32 to redraw the screen.
  delay(1000);
}
