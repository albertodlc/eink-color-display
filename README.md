# eink-color-display

Firmware scaffold for the **Good Display ESP32-L(C73)** kit and the **GDEP073E01**
7.3" E Ink Spectra 6 panel (800x480, 6 native colors, SPI).

The demo initializes the panel, shows a splash screen and a six-color test card,
then puts the panel into deep sleep/hibernation. It is meant as a clean starting
point for a low-power color e-paper device.

## Hardware

| Item | Detail |
| --- | --- |
| Main board | ESP32-L, ESP32-WROOM-32D, 4 MB flash, USB-C + CH340 |
| Adapter | DESPI-C73, plugs into the ESP32-L `P5` header |
| Panel | GDEP073E01, 7.3", 800x480, E Ink Spectra 6 |
| Refresh | Full refresh 15-22 s |
| Logic | 3.3 V |

### Pinout (ESP32-L P5 / DESPI-C73 P2)

| Signal | GPIO |
| --- | --- |
| CS | 27 |
| DC | 14 |
| RST | 12 |
| BUSY | 13 |
| SCK | 18 |
| MOSI | 23 |
| 3V3 / GND | P5.1 / P5.2 |

`MISO` is not used by the panel. See [docs/hardware.md](docs/hardware.md) for the
full pin table, assembly steps, current measurement and references.

## Quick start (PlatformIO)

Install [PlatformIO Core](https://platformio.org/install/cli) or use the VS Code
extension.

1. Assemble the kit:
   - DESPI-C73 into the ESP32-L **P5** header.
   - Panel FPC into the adapter, gold contacts facing down; close the latch.
   - **Install the P3 jumper** (panel power).
2. Connect USB-C and install the CH340 driver if needed.
3. Build and flash:

```bash
pio run -t upload
pio device monitor
```

The monitor runs at **115200 baud**. The first refresh takes about **26-27 s** on
this kit; this is normal. The driver uses a 45 s busy timeout so it waits for the
panel instead of aborting at GxEPD2's default 20 s.

### Arduino IDE alternative

The official Good Display manual uses Arduino IDE:

1. Install the ESP32 board package.
2. Install **GxEPD2** (1.6.9 or newer) and **Adafruit GFX Library**.
3. Select board **WEMOS LOLIN32** (or ESP32 Dev Module), the CH340 COM port and
   upload speed **921600**.
4. Upload a sketch that uses the pin definitions from
   `include/board_esp32_l_c73.h`.

Do not use the `ESP32epdx` examples for this panel: its current tree has no
GDEP073E01 6-color driver. GxEPD2 includes `GxEPD2_730c_GDEP073E01`.

## What the demo does

1. Initializes the panel with `GxEPD2_730c_GDEP073E01`.
2. Draws a splash screen with the board/panel identification.
3. Draws six full-height color bars (black, white, red, yellow, blue, green).
4. Logs each refresh duration over serial.
5. Calls `display.hibernate()` so the panel and its supply are powered down.

The panel keeps the image without power.

## Project layout

```
platformio.ini                 PlatformIO / Arduino configuration
include/board_esp32_l_c73.h    Board pin definitions
include/epd_config.h           Panel type, dimensions and page buffer
include/screens.h              Screen drawing declarations
src/main.cpp                   Init, render loop, hibernation/deep sleep
src/screens.cpp                Splash and color-bar screens
tools/convert_image.py         PNG/JPG or Image2LCD -> 4 bpp converter
docs/hardware.md               Board, panel, pinout and assembly
docs/image-pipeline.md         Native bitmap format and image workflow
```

## Images

The panel uses 4 bpp packed data: 192,000 bytes for a full 800x480 frame.
GxEPD2 palette indices are `0 black, 1 white, 2 green, 3 blue, 4 red, 5 yellow`.

```bash
python3 -m pip install -r requirements.txt
python3 tools/convert_image.py foto.png -o src/foto.h --dither
```

Then:

```cpp
#include "foto.h"
display.drawNative(foto_800x480, nullptr, 0, 0, 800, 480, false, false, true);
```

See [docs/image-pipeline.md](docs/image-pipeline.md) for the Image2LCD settings,
the native format and memory notes.

## Low power

- Always call `display.hibernate()` after an update.
- For battery operation, enable timer deep sleep:

```ini
build_flags =
  -DCORE_DEBUG_LEVEL=0
  -DEPD_DEEP_SLEEP_SECONDS=3600
```

On wake, `setup()` runs again, reinitializes the panel and redraws the screen.
The image remains visible while the ESP32 sleeps.

- The panel refresh itself is the largest current draw (15-22 s). Update only
  when the content changes. Good Display recommends leaving at least **180 s**
  after a set of updates before the next set.
- Use a supply with enough current capacity; a weak battery can damage the panel.
- Multi-color e-paper does not operate below 0 °C.

## Troubleshooting

| Symptom | Check |
| --- | --- |
| Blank panel | **P3 jumper installed**, FPC orientation, latch closed, 3.3 V, BUSY wiring. |
| No serial port | USB-C **data** cable, CH340 driver, board selection. |
| Upload fails | Hold BOOT, tap RESET, release BOOT; check 921600 baud. |
| Wrong colors | Use GxEPD2 indices 0-5; call `drawNative(..., invert=false)`; regenerate the image. |
| Garbled image | FPC not fully inserted, unstable 5 V supply, SPI wires/pins. |
| Boot loop after wiring | Do not add an external pull-up to GPIO12 (panel RES); it is a boot strapping pin. |
| Refresh takes ~20 s | Normal for Spectra 6 full refresh. Partial refresh is not recommended for color. |

Do not connect or disconnect the panel FPC while the board is powered. A full
refresh flickers while it clears ghosting; this is normal. Always leave the panel
in sleep/hibernation after an update.

## Credits

- Good Display for the ESP32-L(C73) kit, schematics and GDEP073E01 documentation.
- Jean-Marc Zingg for [GxEPD2](https://github.com/ZinggJM/GxEPD2).
