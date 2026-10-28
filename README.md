# eink-color-display

Firmware scaffold for the **Good Display ESP32-L(C73)** kit and the **GDEP073E01**
7.3" E Ink Spectra 6 panel (800x480, 6 native colors, SPI).

The default mode is a **standalone air-traffic page**. The ESP32 joins Wi-Fi,
queries the public adsb.lol feed directly (no Tesserae server, no Python host),
draws a print-style poster of the nearest aircraft, then hibernates and
deep-sleeps for five minutes. On first boot it starts a local setup portal so
Wi-Fi credentials and the observing location are not hardcoded.

The firmware is self-contained: no Tesserae server, no Python host, no runtime
image service. See [docs/air-traffic.md](docs/air-traffic.md) for the data
source, setup portal, poster layout and limitations.

## Hardware

| Item | Detail |
| --- | --- |
| Main board | ESP32-L, ESP32-WROOM-32D, 4 MB flash, USB-C + CH340 |
| Adapter | DESPI-C73, plugs into the ESP32-L `P5` header |
| Panel | GDEP073E01, 7.3", 800x480, E Ink Spectra 6 |
| Refresh | Full refresh 15-22 s datasheet, ~27 s measured on this kit |
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

The complete screen is rotated 180° by default (`EPD_ROTATE_180=1`) for an
upside-down mounting. Build with `-DEPD_ROTATE_180=0` for the normal
orientation.

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

The monitor runs at **115200 baud**. The single refresh takes about **26-27 s** on
this kit; this is normal. The driver uses a 45 s busy timeout so it waits for the
panel instead of aborting at GxEPD2's default 20 s.

### First boot: air-traffic setup

The default firmware starts in air-traffic mode. On first boot the panel shows a
setup page and the board opens an open Wi-Fi AP named **`EInk-AirTraffic`**. The
AP starts after the setup page finishes drawing (~27 s), so wait for the page if
you do not see the network yet.

1. Join `EInk-AirTraffic`.
2. Open <http://192.168.4.1>.
3. Enter your Wi-Fi SSID/password, latitude, longitude and radius in kilometres.
4. Save. The board connects, fetches adsb.lol and refreshes the panel.

The update interval is 300 s by default, with the panel hibernated between
updates. Press **RESET** for an immediate update. To reopen the setup portal
without erasing Wi-Fi, tap **RESET** and hold **BOOT** for about two seconds.

The location is only sent to adsb.lol as part of the read-only feed request;
Wi-Fi credentials stay in NVS on the ESP32. See
[docs/air-traffic.md](docs/air-traffic.md) for details.

### Arduino IDE alternative

The official Good Display manual uses Arduino IDE:

1. Install the ESP32 board package.
2. Install **GxEPD2** (1.6.9 or newer), **Adafruit GFX Library**,
   **WiFiManager** and **ArduinoJson**.
3. Select board **WEMOS LOLIN32** (or ESP32 Dev Module), the CH340 COM port and
   upload speed **921600**.
4. Upload a sketch that uses the pin definitions from
   `include/board_esp32_l_c73.h`.

The air-traffic sources and the custom `partitions.csv` (3.5 MB application)
are set up for PlatformIO. The Arduino IDE alternative needs a matching 3.5 MB
app partition.

Do not use the `ESP32epdx` examples for this panel: its current tree has no
GDEP073E01 6-color driver. GxEPD2 includes `GxEPD2_730c_GDEP073E01`.

## Air-traffic mode (default)

The page is a print-style poster rather than a radar:

- A thin border, a `WITHIN <radius> KM - <N> AIRCRAFT IN THE AIR NOW` header
  and a `HH:MM UTC - Tue 27 Oct` timestamp from the feed.
- The nearest aircraft, drawn as side-view photographs: live JetPhotos or
  Planespotters thumbnails when available, otherwise bundled fallback photos
  for airliners, regional jets, business jets, turboprops, light singles and
  helicopters. One large aircraft when there is only one, two feature rows for
  two, a 2x2 grid for three or four, and a 3+2 grid for five.
- Each card puts the callsign and aircraft model on one line, then the
  altitude with climbing/descending/level and the route or distance/bearing on
  the next. The single-aircraft layout also shows a country flag, the
  registration/distance and a recent-route column. Ground aircraft are marked
  `on the ground`.
- Mode 6 draws a clean vector map centred on the configured location, with
  the aircraft plotted at their distance and bearing and oriented by their
  track. The map rectangle, not the radius, selects which aircraft are loaded
  (up to 40 markers). The map uses embedded Natural Earth coastlines, country
  and district borders, rivers, district names and city names, so it has no
  tile-service labels or raster artifacts. Aircraft identifiers use
  high-contrast black labels with a small country flag.
- The feed supplies the callsign, type, altitude, vertical rate and position.
- The header count is the total within the radius; the poster shows as many
  aircraft as the selected display mode allows (four by default).

The layout is inspired by the
[Overhead](https://github.com/dmellok/tesserae-widget-overhead) widget, but it
uses no Tesserae server, airline logos or AI paintings.
Routes are optional: they come from adsbdb for the displayed aircraft and
are cached in RTC memory for two hours; when no route is known the line falls
back to distance/bearing. See [docs/air-traffic.md](docs/air-traffic.md) for the
data source, portal, layout and limitations.

### Display modes (layout testing)

`AIR_TRAFFIC_MODE` selects the initial/fallback poster layout at compile time:

| Mode | Layout |
| --- | --- |
| `0` | Live feed, up to 4 aircraft |
| `1` | Built-in demo feed, `AIR_TRAFFIC_DEMO_COUNT` aircraft |
| `2` | One live aircraft, with flag, registration/distance and recent routes |
| `3` | Two live aircraft |
| `4` | Three live aircraft |
| `5` | Five live aircraft (3 on top, 2 centred below) |
| `6` | Vector map centred on the configured location and radius |

By default the display starts at the initial mode (`0`, auto/2×2), then cycles
through `2, 6, 3, 4, 5`; after the last view it returns to the initial mode and
repeats. Override the sequence with:

```bash
PLATFORMIO_BUILD_FLAGS="-DAIR_TRAFFIC_CYCLE_SEQUENCE=2,6,3,4,5" \
  pio run -t upload
```

The start view is `AIR_TRAFFIC_CYCLE_INITIAL_MODE` (defaults to
`AIR_TRAFFIC_MODE`). Include demo mode with `1` in the sequence. Disable cycling with
`-DAIR_TRAFFIC_CYCLE_MODES=0`; `AIR_TRAFFIC_MODE` then selects one fixed view:

```bash
PLATFORMIO_BUILD_FLAGS="-DAIR_TRAFFIC_CYCLE_MODES=0 -DAIR_TRAFFIC_MODE=2" \
  pio run -t upload
PLATFORMIO_BUILD_FLAGS="-DAIR_TRAFFIC_CYCLE_MODES=0 -DAIR_TRAFFIC_MODE=6" \
  pio run -t upload
```

The demo feed renders the sample aircraft from the Overhead reference print
without using adsb.lol. It may briefly join Wi-Fi to fetch card photos. A fixed
demo build (`-DAIR_TRAFFIC_CYCLE_MODES=0 -DAIR_TRAFFIC_MODE=1`) does not
deep-sleep, so press RESET to redraw; when demo is included in the cycling
sequence it deep-sleeps like the other views. Live modes fetch the feed and
deep-sleep as usual. Mode 6 renders the map on-device from embedded Natural
Earth data, so it makes no map HTTP request; if the map buffer cannot be
allocated it shows `MAP DATA UNAVAILABLE`. Flash without flags to use the
default cycling sequence. The legacy `-DAIR_TRAFFIC_DEMO=1` flag is still
accepted as an alias for mode 1; the sample aircraft are in
`fill_demo_data()` in `src/air_traffic.cpp`.

## Refresh timing

Each wake performs one full refresh, so boot-to-content is one panel cycle
(~27 s). The air-traffic mode logs:

```text
[air] render total 27425 ms (panel 26022 ms)
```

The draw part is Adafruit GFX rendering; each paged pass only draws the 160-row
band that belongs to its page. The transfer part covers page-buffer clearing,
the 192 KB SPI transfer and the power-off command. The panel part is the
electrophoretic waveform and is fixed by the panel; it is not affected by SPI
speed, CPU speed or image content. It is temperature compensated, so a cold
panel is slower (the 15-22 s datasheet figure is at about 25 °C).

The vendor sample also has an `EPD_init_fast()` sequence; on this unit it was
**slower** (30.5 s) than the stock GxEPD2 init, so it is off by default. See
`docs/hardware.md` for the experiment and the `-DEPD_FAST_INIT=1` flag.

For the full speed survey — partial-refresh claims (2.1 s claimed, 26 s measured
here), the Tosone 14-15 s claim, `DRF`/`CDI` details and hardware alternatives —
see [docs/refresh-speed.md](docs/refresh-speed.md).

## Project layout

```
platformio.ini                 PlatformIO / Arduino configuration
partitions.csv                 3.5 MB application partition layout
assets/aircraft/*.jpg          Bundled fallback aircraft photos
assets/aircraft/ATTRIBUTION.md Photo sources and licences
include/board_esp32_l_c73.h    Board pin definitions
include/epd_config.h           Panel type, dimensions and page buffer
include/air_traffic.h          Air-traffic data structures and airTrafficRun()
include/air_traffic_config.h   Portal, radius, update and layout constants
src/main.cpp                   Panel init, OTP test modes, air-traffic entry point
src/air_traffic.cpp            Wi-Fi portal, adsb.lol fetch, poster renderer
src/fallback_photos.h          RGB332 fallback photos (PROGMEM)
src/flags.h                    Generated country flags for registrations
src/vector_map.h               Generated Natural Earth vector map data
tools/make_fallback_photos.py  Fallback photo cropper/RGB332 packer (Pillow)
tools/make_flags.py            Flag downloader/quantiser/packer (Pillow)
tools/make_vector_map.py       Natural Earth vector map packer
tools/dump_otp.py              Serial OTP dump capture (pyserial)
docs/hardware.md               Board, panel, pinout and assembly
docs/air-traffic.md            Standalone air-traffic mode and poster layout
docs/fallback-photos-preview.png  Generated fallback photo preview
docs/otp-readback.md           SPD1657A controller/OTP readback research
docs/refresh-speed.md          Refresh-speed research and measured timings
docs/SPD1657A-1.0.pdf          SPD1657A controller datasheet (Advance Info)
```

## Controller research

The panel is sold as **ED2208-GCA** but the silicon reports a Solomon Systech
**SPD1657A** controller (`REV 0x70` returns `01 01 04`). The 44-page
[SPD1657A Rev 1.0 datasheet](docs/SPD1657A-1.0.pdf) is now in `docs/`. There is
no public controller firmware: the 96 KB OTP holds the waveform table, not
executable code, and is factory-programmed (OTP program mode needs VPP 7.5 V).
The shared SDI line is bidirectional, so the ESP32 can read `REV`, `FLG` and
`ROTP`; see [docs/otp-readback.md](docs/otp-readback.md) for the commands, the
PSR `LUT_EN` detail, the GitHub/repo sweep and the verified 2 KB prefix.

## Low power

- Always call `display.hibernate()` after an update.
- Air-traffic mode hibernates the panel and deep-sleeps for
  `AIR_TRAFFIC_UPDATE_S` (300 s by default). Press **RESET** for an immediate
  update.
- On wake, `setup()` runs again, reinitializes the panel and redraws the page.
  The last image remains visible while the ESP32 sleeps.
- The panel refresh itself is the largest current draw (26-27 s measured). Update
  only when the content changes. Good Display recommends leaving at least **180 s**
  after a set of updates before the next set.
- Use a supply with enough current capacity; a weak battery can damage the panel.
- Multi-color e-paper does not operate below 0 °C.

## Troubleshooting

| Symptom | Check |
| --- | --- |
| Blank panel | **P3 jumper installed**, FPC orientation, latch closed, 3.3 V, BUSY wiring. |
| No serial port | USB-C **data** cable, CH340 driver, board selection. |
| Upload fails | Hold BOOT, tap RESET, release BOOT; check 921600 baud. |
| Garbled image | FPC not fully inserted, unstable 5 V supply, SPI wires/pins. |
| Boot loop after wiring | Do not add an external pull-up to GPIO12 (panel RES); it is a boot strapping pin. |
| Refresh takes ~27 s | Normal for Spectra 6 full refresh. Partial refresh has no speed benefit on this controller and is not recommended for color. |
| `EInk-AirTraffic` AP not visible | Wait for the ~27 s setup-page refresh; the AP starts afterwards. |
| Air-traffic portal does not load | Join the AP first, then open `http://192.168.4.1`; some phones keep mobile data as the default route. |
| `HTTP -1` on the panel | No internet, DNS/TLS failure or adsb.lol unavailable. Check Wi-Fi and press RESET. |
| `NO AIRCRAFT OVERHEAD` | Wrong latitude/longitude, a too-small radius, or quiet airspace. Reconfigure with a larger radius. |

Do not connect or disconnect the panel FPC while the board is powered. A full
refresh flickers while it clears ghosting; this is normal. Always leave the panel
in sleep/hibernation after an update.

## Credits

- Good Display for the ESP32-L(C73) kit, schematics and GDEP073E01 documentation.
- Jean-Marc Zingg for [GxEPD2](https://github.com/ZinggJM/GxEPD2).
- [adsb.lol](https://adsb.lol) for the public ADS-B feed and
  [adsbdb](https://www.adsbdb.com/) for optional route lookups.
- [tzapu/WiFiManager](https://github.com/tzapu/WiFiManager) for the setup
  portal and [bblanchon/ArduinoJson](https://github.com/bblanchon/ArduinoJson)
  for JSON parsing.
- [dmellok/tesserae-widget-overhead](https://github.com/dmellok/tesserae-widget-overhead)
  for the visual idea. Overhead is AGPL-3.0; this firmware is an independent
  implementation and does not include its code.
