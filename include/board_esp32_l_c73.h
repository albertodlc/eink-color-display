#pragma once

// Good Display ESP32-L (C73 kit)
// Main board: ESP32-WROOM-32D, 4 MB flash, CH340 USB-serial, 5 V USB-C.
// Adapter:    DESPI-C73, plugged into the ESP32-L "P5" (EPD DRIVE) header.
//
// Pin mapping derived from Good Display schematics:
//   ESP32-L_SCH.pdf  -> U5 ESP32 module and P5 connector
//   DESPI-C73-SCH.pdf -> P2 host connector (3V3, GND, SDI, SCLK, CS, D/C, RES, BUSY)
//
// ESP32-L P5 / DESPI-C73 P2:
//   pin 1  3.3V_S2
//   pin 2  GND
//   pin 3  SPI_MOSI  -> GPIO23
//   pin 4  SPI_SCK   -> GPIO18
//   pin 5  CS        -> GPIO27
//   pin 6  D/C       -> GPIO14
//   pin 7  RES       -> GPIO12
//   pin 8  BUSY      -> GPIO13

#define BOARD_NAME "ESP32-L(C73)"
#define BOARD_MCU  "ESP32-WROOM-32D"

#define EPD_CS_PIN   27
#define EPD_DC_PIN   14
#define EPD_RST_PIN  12
#define EPD_BUSY_PIN 13

// ESP32 VSPI defaults (also used by the Arduino ESP32 core).
#define EPD_SCK_PIN  18
#define EPD_MOSI_PIN 23
#define EPD_MISO_PIN 19  // not connected to the e-paper panel
