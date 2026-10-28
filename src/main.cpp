#include <Arduino.h>

#include "air_traffic.h"
#include "epd_config.h"

// Experimental: apply Good Display's "EPD_init_fast" register sequence after
// the normal GxEPD2 init. Measure refresh time and image quality before keeping.
#ifndef EPD_FAST_INIT
#define EPD_FAST_INIT 0
#endif

// Experimental: "Tosone's Law" init, PSR first after reset, with PSR.B=0xE9.
// Reported to halve DRF time on the SPD1657A (14-15 s vs 28-46 s).
#ifndef EPD_TOSONE_INIT
#define EPD_TOSONE_INIT 0
#endif

// Read-only probe: read REV/FLG/version over the bidirectional SDI line.
#ifndef EPD_READ_TEST
#define EPD_READ_TEST 0
#endif

// Dump the 96,000-byte OTP (waveform) over serial.
#ifndef EPD_DUMP_OTP
#define EPD_DUMP_OTP 0
#endif

epd::Display display(epd::Panel(EPD_CS_PIN, EPD_DC_PIN, EPD_RST_PIN, EPD_BUSY_PIN));

#if EPD_READ_TEST
static void readRegisters() {
  uint8_t rev[3] = {0};
  uint8_t flg = 0;
  uint8_t ver = 0;

  display.epd2.enableRead(EPD_SCK_PIN, EPD_MOSI_PIN);

  bool ok = display.epd2.readBytes(0x70, rev, 3);
  Serial.printf("[otp] REV: %02X %02X %02X (%s)\n", rev[0], rev[1], rev[2],
                ok ? "data" : "no data");
  ok = display.epd2.readBytes(0x71, &flg, 1);
  Serial.printf("[otp] FLG: %02X (%s)\n", flg, ok ? "data" : "no data");
  ok = display.epd2.readBytes(0xCD, &ver, 1);
  Serial.printf("[otp] VER: %02X (%s)\n", ver, ok ? "data" : "no data");

  uint8_t otp[33] = {0};
  ok = display.epd2.readBytes(0x92, otp, sizeof(otp));
  Serial.printf("[otp] ROTP continuous (%s):", ok ? "data" : "no data");
  for (uint8_t i = 1; i < sizeof(otp); i++) {
    Serial.printf(" %02X", otp[i]);
  }
  Serial.println();

  // Split command/read: release CS after 0x92, optionally wait, then clock.
  static const uint16_t kDelays[] = {0, 1, 10, 100};
  for (uint8_t d = 0; d < sizeof(kDelays) / sizeof(kDelays[0]); d++) {
    uint8_t sample[17] = {0};
    display.epd2.sendCommand(0x92);
    const int busy_before = digitalRead(EPD_BUSY_PIN);
    delay(kDelays[d]);
    const int busy_after = digitalRead(EPD_BUSY_PIN);
    display.epd2.readData(sample, sizeof(sample));
    Serial.printf("[otp] ROTP split delay=%ums busy=%d/%d:", kDelays[d],
                  busy_before, busy_after);
    for (uint8_t i = 0; i < sizeof(sample); i++) {
      Serial.printf(" %02X", sample[i]);
    }
    Serial.println();
    display.epd2.sendCommand(0x11);  // DSP: stop data transmission
    delay(1);
  }

  uint8_t sram[16] = {0};
  display.epd2.sendCommand(0x93);  // RSRAM
  delay(1);
  display.epd2.readData(sram, sizeof(sram));
  Serial.print("[otp] RSRAM split:");
  for (uint8_t i = 0; i < sizeof(sram); i++) {
    Serial.printf(" %02X", sram[i]);
  }
  Serial.println();
  Serial.flush();
}
#endif

#if EPD_DUMP_OTP
static void dumpOtp() {
  constexpr uint32_t kSize = 96000;
  uint8_t* otp = static_cast<uint8_t*>(malloc(kSize + 1));
  if (!otp) {
    Serial.printf("[otp] malloc failed, free heap=%u\n", ESP.getFreeHeap());
    return;
  }

  Serial.printf("[otp] reading 96000 bytes via ROTP 0x92 (free heap=%u)\n",
                ESP.getFreeHeap());
  Serial.flush();

  display.epd2.enableRead(EPD_SCK_PIN, EPD_MOSI_PIN);
  display.epd2.readBegin(0x92);  // ROTP; keeps CS low, releases MOSI
  delay(1);

  // Hardware SPI read: attach MISO to the shared SDI pin and leave MOSI
  // unattached, so the panel can drive the same line.
  SPI.begin(EPD_SCK_PIN, EPD_MOSI_PIN, -1, -1);
  SPI.beginTransaction(SPISettings(10000000, MSBFIRST, SPI_MODE0));
  for (uint32_t i = 0; i <= kSize; i++) {
    otp[i] = SPI.transfer(0x00);
  }
  SPI.endTransaction();
  SPI.end();
  digitalWrite(EPD_CS_PIN, HIGH);

  // Restore the normal display SPI pins for hibernate().
  SPI.begin(EPD_SCK_PIN, -1, EPD_MOSI_PIN, -1);
  display.epd2.sendCommand(0x11);  // DSP: stop data transmission
  delay(1);

  Serial.println("[otp] BEGIN 96000");
  Serial.flush();
  Serial.write(otp + 1, kSize);
  Serial.flush();
  Serial.println();
  Serial.println("[otp] END");
  Serial.flush();

  free(otp);
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
  Serial.printf("  init  : %s\n",
                EPD_TOSONE_INIT
                    ? "Tosone (PSR first)"
                    : (EPD_FAST_INIT ? "vendor fast" : "GxEPD2 stock"));
  Serial.printf("  rotate: %s\n", EPD_ROTATE_180 ? "180 deg" : "0 deg");
  Serial.println("  image : air traffic");
  Serial.println("==========================================");

  // 10 MHz: the panel supports up to 20 MHz, but the ESP32-L wiring and the
  // official Good Display sample use 10 MHz, which is more reliable here.
  display.init(115200, true, 10, false, SPI,
               SPISettings(10000000, MSBFIRST, SPI_MODE0));
#if EPD_TOSONE_INIT
  Serial.println("[epd] applying Tosone init (PSR first)");
  display.epd2.initTosone(false);
#elif EPD_FAST_INIT
  Serial.println("[epd] applying vendor fast init");
  display.epd2.initFast();
#endif
  display.setRotation(EPD_ROTATE_180 ? 2 : 0);
  display.setFullWindow();
  display.setTextWrap(false);

#if EPD_DUMP_OTP
  dumpOtp();
  Serial.println("[otp] dump done, hibernating");
  display.hibernate();
  return;
#endif

#if EPD_READ_TEST
  readRegisters();
  Serial.println("[epd] read test done, hibernating");
  display.hibernate();
  return;
#endif

  airTrafficRun(display);
}

void loop() {
  // airTrafficRun() hibernates the panel and, in live mode, deep-sleeps after
  // each update. The panel keeps the last image without power.
  delay(1000);
}
