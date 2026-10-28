# Standalone air traffic

The firmware is a self-contained air-traffic display. It does not need the
Tesserae server, a Python host, API keys or AI paintings. The ESP32 queries the
public [adsb.lol](https://adsb.lol) feed directly and draws a print-style poster
of the nearest aircraft on the 800x480 Spectra 6 panel.

This is a deliberately smaller version of the
[Overhead](https://github.com/dmellok/tesserae-widget-overhead) widget: the
poster composition, callsigns, aircraft types, altitude trend and route or
distance/bearing are kept; the painted skies, airline marks and UFOs are not.
The aircraft use live JetPhotos/Planespotters thumbnails when available and
bundled side-view fallback photos otherwise.

## Boot sequence

1. Load Wi-Fi credentials and location from NVS (`Preferences`).
2. On first boot (or when BOOT is held after reset), show the setup page and
   start an open Wi-Fi AP named `EInk-AirTraffic`.
3. Join the configured Wi-Fi network.
4. Request the adsb.lol point feed for the configured latitude, longitude and
   radius.
5. Parse the JSON, keep the nearest `AIR_TRAFFIC_MAX_AIRCRAFT` aircraft and
   show the nearest `AIR_TRAFFIC_ACTIVE_CARDS` selected by `AIR_TRAFFIC_MODE`.
6. Optionally resolve routes from adsbdb for the displayed aircraft
   (RTC-cached for two hours); missing routes fall back to distance/bearing.
   Map mode skips this and the photo lookup.
7. Draw the poster or map, hibernate the panel, then deep-sleep for
   `AIR_TRAFFIC_UPDATE_S` seconds.

Press **RESET** at any time for an immediate update.

## First boot

The setup AP starts after the first panel refresh completes (about 27 s). If
the AP is not visible yet, wait for the setup page to finish drawing.

1. Join the open Wi-Fi network **`EInk-AirTraffic`**.
2. Open **<http://192.168.4.1>**.
3. Enter:
   - your Wi-Fi SSID and password,
   - latitude (decimal degrees, south negative),
   - longitude (decimal degrees, west negative),
   - radius in kilometres (1-200; the default is 100 km).
4. Save. The board connects, fetches the feed and refreshes the panel.

The Wi-Fi password and location are stored in NVS on the ESP32. They are not
compiled into the firmware. The location is sent to adsb.lol as part of the
read-only feed request; Wi-Fi credentials never leave the board.

## Reconfiguring

Tap **RESET**, then hold **BOOT** for about two seconds while the panel
initializes. The setup page and portal reopen without erasing the saved Wi-Fi
network, so only the location fields need to be changed.

If the portal times out (10 minutes), the board draws an error page, sleeps for
five minutes and tries again.

## Data source

```text
https://api.adsb.lol/v2/lat/{lat}/lon/{lon}/dist/{nm}
```

- No API key.
- `radius_km` is converted to nautical miles with `ceil(km / 1.852)` because
  the feed uses nautical miles.
- The response is a point query: `ac` is the aircraft array, `now` is the
  server time in epoch **milliseconds**, and each aircraft can include `dst`
  (distance in nm) and `dir` (bearing in degrees) relative to the requested
  point.
- The firmware uses `dst`/`dir` when present and falls back to a haversine
  calculation from `lat`/`lon`.
- `alt_baro` can be a number of feet or the string `ground`.
- `baro_rate` is the vertical rate in feet per minute; the poster uses it for
  `climbing`, `descending` or `level`.
- The public feed has rate limits (currently about 500 requests/day per IP).
  A 300 s update interval uses 288 requests/day; leave headroom for retries and
  do not lower the interval below the panel vendor's 180 s recommendation.

Only the fields used by the page are materialised:

```text
now, ac[].hex, flight, t, alt_baro, baro_rate, lat, lon, dst, dir, track
```

The reader parses one aircraft object at a time and keeps only the nearest
`AIR_TRAFFIC_MAX_AIRCRAFT`, so the heap cost stays small even for a large radius
on an ESP32 without PSRAM.

### Optional routes (adsbdb)

```text
https://api.adsbdb.com/v0/callsign/{callsign}
```

- Only the aircraft displayed by the selected mode are looked up (at most
  five), and only when the callsign contains a digit (a plausible flight
  number). Map mode skips route lookups and photos entirely.
- The response's `origin.municipality` and `destination.municipality` become
  `Dublin to Milan`; empty names fall back to the IATA codes.
- A 3.5 s timeout per request. Failures or unknown callsigns leave the route
  empty and the card shows distance/bearing instead.
- One request per displayed aircraft (up to five per wake). A 12-entry cache
  in RTC memory keeps successful and failed lookups for two hours; it survives
  deep sleep but not a power cycle. This avoids hammering adsbdb for the same
  flight every five minutes.
- adsbdb is a second public service; if it is down the poster still renders
  from the ADS-B data.

## The poster

The page imitates a framed print:

```text
+--------------------------------------------------------------+
| WITHIN 100 KM - 4 AIRCRAFT IN THE AIR NOW  22:12 UTC - Tue... |
|--------------------------------------------------------------|
|                                                              |
|        [side-view airliner]        [side-view airliner]       |
|        VOZ858  Boeing 737-800       QFA693  Boeing 737-800     |
|     7,200 ft descending - 12 km   6,000 ft climbing - 18 km    |
|                                                              |
|                         ...                                   |
+--------------------------------------------------------------+
```

- The header shows the radius, the number of aircraft within it, and the feed
  time/date (UTC). The date comes from the feed's `now` field, so no RTC or NTP
  is needed.
- `AIR_TRAFFIC_MODE` selects the poster composition. One aircraft: one large
  feature card with the airliner above two text columns. The left column has
  callsign/type, altitude, route or distance, and a country flag plus the
  registration; the right column lists the aircraft's latest routes. Two: two
  feature rows with the airliner on the left and the text on the right. Three
  or four: a 2x2 grid; an odd last card is centred. Five: three cards on top
  and two centred below. Six: the real-map view described above.
- Each card puts the callsign and full aircraft type on one line, then the
  altitude with `climbing`/`descending`/`level` (from `baro_rate`) and route or
  distance/bearing on the next line.
- Ground aircraft show `on the ground`.
- ICAO type codes are mapped to readable names for common Airbus, Boeing,
  Embraer, ATR and Bombardier types; unknown codes fall back to the ICAO code.
- Live photos come from JetPhotos/Planespotters; when unavailable, a bundled
  side-view photo for the aircraft family is used. Sources and licences are in
  [`assets/aircraft/ATTRIBUTION.md`](../assets/aircraft/ATTRIBUTION.md).
- The header count is the total within the radius; the poster shows the number
  of aircraft allowed by the selected mode.

The details line uses the route when adsbdb knows it (`Dublin to Milan`) and
`12 km / 210 deg` otherwise. Routes are often missing for positioning, ferry
and ground flights; the ADS-B distance/bearing is always available, so the card
never has a blank line.

In the single-aircraft layout, the flag comes from the registration prefix
(`EC-` Spain, `PH-` Netherlands, `G-` United Kingdom, and so on) using the
generated `src/flags.h` table. The right column lists up to four recent flights
from JetAPI's FlightRadar24 data, for example `28 Oct Paris > Amsterdam`. That
list is cached in RTC memory for 30 minutes; when the lookup fails the column
shows `NO RECENT FLIGHTS`. Regenerate the flags after editing the prefix table
with `python3 tools/make_flags.py`.

When a live thumbnail is unavailable, the firmware uses a bundled side-view
photo for the aircraft family: narrowbody (A320/B737/B757), widebody (A330),
regional jet (E190), turboprop (ATR 72/Dash 8), business jet (Citation CJ3),
light single (Cessna 172) and helicopter (AW139). Sources and licences are in
[`assets/aircraft/ATTRIBUTION.md`](../assets/aircraft/ATTRIBUTION.md). Live
thumbnails are decoded at 320x128, expanded from RGB565 to 8-bit channels with
bit replication, and mapped to the six inks. The bundled fallbacks are stored as
smooth 480x192 RGB332 data and Floyd-Steinberg dithered at the final card
resolution.

### Character set

The panel uses the ASCII 5x7 Adafruit font. Natural Earth names in
`src/vector_map.h` are transliterated when the header is generated, and UTF-8
route/history names from adsbdb and JetAPI are transliterated at runtime:
`Nápoli -> Napoli`, `Logroño -> Logrono`, `Álava -> Alava`,
`San Sebastián -> San Sebastian`, `Zürich -> Zurich`. Non-Latin characters are
omitted rather than sent to the font as multi-byte sequences, which would draw
as garbage.

## Configuration

Compile-time defaults live in `include/air_traffic_config.h`:

| Constant | Default | Meaning |
| --- | --- | --- |
| `AIR_TRAFFIC_AP_NAME` | `EInk-AirTraffic` | Setup AP name |
| `AIR_TRAFFIC_PORTAL_TIMEOUT_S` | 600 | Portal timeout |
| `AIR_TRAFFIC_RADIUS_KM` | 100 | Default radius on first boot |
| `AIR_TRAFFIC_UPDATE_S` | 300 | Deep-sleep interval |
| `AIR_TRAFFIC_SLEEP` | 1 | Set to 0 to disable deep sleep |
| `AIR_TRAFFIC_MAX_AIRCRAFT` | 10 | Nearest aircraft kept while parsing |
| `AIR_TRAFFIC_PAGE_AIRCRAFT` | 5 | Maximum cards any mode can draw |
| `AIR_TRAFFIC_MODE` | 0 | 0 auto, 1 demo, 2 one, 3 two, 4 three, 5 five, 6 map |
| `AIR_TRAFFIC_ACTIVE_CARDS` | mode-dependent | Cards drawn by the selected mode |
| `AIR_TRAFFIC_DEMO` | derived | 1 only when `AIR_TRAFFIC_MODE=1` |
| `AIR_TRAFFIC_DEMO_COUNT` | 4 | Demo aircraft: 1-5 |

The radius is also editable in the portal. Latitude, longitude and radius are
validated before they are saved.

### Display modes

`AIR_TRAFFIC_MODE` selects the poster composition at compile time:

| Mode | Aircraft | Layout |
| --- | --- | --- |
| `0` | live, up to 4 | 2x2 grid, or fewer |
| `1` | demo, `AIR_TRAFFIC_DEMO_COUNT` | matches the demo count |
| `2` | 1 | large feature card with flag, registration/distance and recent routes |
| `3` | 2 | two feature rows |
| `4` | 3 | 2x2 grid with the last card centred |
| `5` | 5 | three on top, two centred below |
| `6` | live, up to 10 | vector map with aircraft markers |

```bash
PLATFORMIO_BUILD_FLAGS="-DAIR_TRAFFIC_MODE=2" pio run -t upload
PLATFORMIO_BUILD_FLAGS="-DAIR_TRAFFIC_MODE=5" pio run -t upload
PLATFORMIO_BUILD_FLAGS="-DAIR_TRAFFIC_MODE=6" pio run -t upload
```

Mode `0` is the default. `AIR_TRAFFIC_ACTIVE_CARDS` is derived from the mode;
the poster arrays are sized for five cards, and a larger demo count is clamped
with a build warning. `AIR_TRAFFIC_DEMO` is no longer set directly: it is 1 only
in mode 1, and the legacy `-DAIR_TRAFFIC_DEMO=1` flag is accepted as an alias
for mode 1. Flash without a flag to return to the live auto layout.

#### Demo feed

The demo feed renders the sample aircraft from the Overhead reference print. It
does not read the saved location or call adsb.lol/adsbdb. It briefly joins the
saved Wi-Fi network so the cards can use live JetAPI/Planespotters photos; when
that fails, the bundled fallback photos are used:

```bash
PLATFORMIO_BUILD_FLAGS="-DAIR_TRAFFIC_MODE=1" pio run -t upload
```

The sample data is in `fill_demo_data()` in `src/air_traffic.cpp`:

| Callsign | Type | Altitude | Route |
| --- | --- | --- | --- |
| VOZ858 | Boeing 737-800 | 7,200 ft descending | Sydney to Melbourne |
| QFA693 | Boeing 737-800 | 6,000 ft climbing | Melbourne to Adelaide |
| VOZ875 | Boeing 737-800 | 15,450 ft climbing | Melbourne to Sydney |
| JST521 | Airbus A320 | 2,975 ft descending | Sydney to Melbourne |
| QFA476 | Boeing 737-800 | 21,750 ft climbing | Perth to Melbourne |
| QFA478 | Boeing 737-800 | 10,900 ft climbing | Melbourne to Sydney |

The demo timestamp is `2026-10-22 18:28 UTC` (Thursday), matching the reference.
The panel hibernates after the render but the ESP32 does not deep-sleep; press
RESET to redraw.

Use `AIR_TRAFFIC_DEMO_COUNT=1` to `5` to exercise the adaptive layouts:

```bash
PLATFORMIO_BUILD_FLAGS="-DAIR_TRAFFIC_MODE=1 -DAIR_TRAFFIC_DEMO_COUNT=5" \
  pio run -t upload
```

The demo build still contains the Wi-Fi/TLS code, so its flash size is
unchanged.

#### Map mode

Mode 6 renders a clean vector map centred on the configured latitude/longitude
from the Natural Earth 1:50m dataset embedded in the firmware. It makes no map
HTTP request. The map covers 10% more than the selected radius and has the same
5:3 aspect as the panel. The firmware draws:

- white land, blue water, black coastlines and country borders, green district
  boundaries, dotted blue rivers and black district names;
- city names in a lower-contrast green, with district and city labels
  collision-filtered;
- each aircraft at its ADS-B distance and bearing, rotated by its ground track;
- aircraft callsigns on black labels with white text and a small country flag;
- the home marker, north and `(c) Natural Earth`;
- the usual `WITHIN ... KM - N AIRCRAFT` header.

There is no range circle or other legend over the map. The vector image is
rasterised once per wake into a 54 KB two-bit buffer at 600x360, scaled to the
800x480 panel, which takes about 6.7 s on the ESP32; the panel refresh still
dominates the wake. If the buffer cannot be allocated, the panel shows
`MAP DATA UNAVAILABLE`.

The source is Natural Earth: 1:50m ocean polygons, country borders and rivers,
plus 1:10m first-order district boundaries, district labels and populated
places (public domain). District boundaries are included for Europe; the rest
of the world still has country borders, rivers and city labels. It is
intentionally sparse: no roads, no raster tiles and no tile-provider labels.
Regenerate `src/vector_map.h` with `python3 tools/make_vector_map.py`.

## Partition and dependencies

Wi-Fi, TLS, WiFiManager and ArduinoJson push the application above the default
1.3 MB partition. `platformio.ini` uses `huge_app.csv` (3 MB application on the
4 MB flash):

```ini
board_build.partitions = huge_app.csv
```

Measured build: about **1.45 MB flash (46%)** and **113 KB static RAM (35%)**.

### PlatformIO / ESP32 Arduino 3.x note

The framework `WiFi` library includes `Network.h` from the separate core
`Networking` library, but PlatformIO does not add that include path to the
`WiFi` library automatically. The project therefore depends on `Networking`
and adds its include directory explicitly:

```ini
lib_deps =
  ...
  Networking

build_flags =
  -DCORE_DEBUG_LEVEL=0
  -I$PROJECT_PACKAGES_DIR/framework-arduinoespressif32/libraries/Network/src
```

Do not point `lib_extra_dirs` at the `Network` folder itself: PlatformIO then
treats its `src` subfolder as a separate library and the WiFi build still fails.

## Memory and timing

Representative serial log (location/radius replaced with a generic example):

```text
[air] Wi-Fi connect
[air] location 51.50740, -0.12780 radius 100 km
[air] GET https://api.adsb.lol/v2/lat/51.50740/lon/-0.12780/dist/28
[air] payload 12544 bytes
[air] route RYR6GY: Dublin to Milan
[air] 25 aircraft within 100 km (10 stored)
[air] free heap 189900 bytes
_PowerOn : 172001
_refresh : 26018997
_PowerOff : 139001
[air] render total 27425 ms (panel 26022 ms)
[air] deep sleep 300 s
```

The panel refresh dominates. Wi-Fi/TLS/fetch, route lookups and JSON parsing
happen before the refresh and are not visible in the panel BUSY time. The page
uses the paged GxEPD2 API (160 bands of 3 px), so it transfers the same 192 KB
full-panel framebuffer.

The feed is scanned directly from the TLS stream one aircraft object at a time.
The firmware never buffers the full body or builds a document for all aircraft;
it keeps only the nearest `AIR_TRAFFIC_MAX_AIRCRAFT`. Peak heap use is therefore
independent of the radius and does not hit the old `No body memory` failure.

Demo mode skips adsb.lol/adsbdb and the location lookup. It may briefly join the
saved Wi-Fi network to fetch card photos, then falls back to the bundled images:

```text
image : air traffic (demo)
[air] demo feed: 5 aircraft
[air] demo render total 27392 ms (panel 26015 ms)
```

The demo panel hibernates but the ESP32 stays awake, so the image remains and
RESET redraws it.

Map mode rasterises the embedded Natural Earth vectors after Wi-Fi is
disconnected:

```text
[air] vector map q=-1035..-130, 8463..8858 in 6656 ms, heap 127748
[air] render total 37123 ms (panel 26023 ms)
```

The background is a 600x360 two-bit buffer (54 KB) with four inks, scaled to
the 800x480 panel. There is no map download and no JPEG decode, so no PSRAM is
required.

## Limitations

- Aircraft images are 320x128 live thumbnails or 480x192 bundled fallbacks,
  six-colour and card-sized; they are not full-resolution photographs. Live
  thumbnails depend on JetPhotos/Planespotters; when they are unavailable the
  bundled per-type photos are shown. No airline logos, painted skies or AI
  artwork.
- Routes are best-effort. adsbdb may not know a flight, and route lookups add a
  second API dependency and up to five HTTPS requests per wake; the RTC cache
  reduces repeat queries. Distance/bearing is always shown when no route is
  available.
- Map mode uses Natural Earth 1:50m base data and 1:10m district boundaries and
  places. It draws coastlines, country and district borders, rivers, district
  names and major city names. City names use a lower-contrast green; aircraft
  identifiers use black labels, white text and country flags. It deliberately
  omits roads and raster tile labels, so it is cleaner but less detailed than a
  street map. The 54 KB map buffer is allocated after Wi-Fi is disconnected; if
  that fails the panel shows `MAP DATA UNAVAILABLE`.
- TLS certificate verification is disabled (`setInsecure()`) because the
  firmware does not ship a CA bundle. The feed is public and read-only, but a
  man-in-the-middle could alter the displayed data. The public image proxy
  (`images.weserv.nl`) is fetched over plain HTTP because it is decorative and
  read-only; the aircraft metadata APIs remain on HTTPS.
- One location per device. Moving the display requires reconfiguring the
  portal.
- The public feed can be unavailable or rate-limited; the panel shows an error
  page and retries on the next wake.
- No OTA with `huge_app.csv`; flash over USB.
- Deep sleep resets the ESP32. The first update after wake reconnects Wi-Fi and
  can take a few seconds before the panel starts refreshing.
- The panel must not be refreshed below 0 °C, and repeated full refreshes are
  slow (about 26 s each).

## Troubleshooting

| Symptom | Check |
| --- | --- |
| Setup AP not visible | Wait for the ~27 s setup-page refresh; the AP starts afterwards. Reset and try again. |
| Portal page does not load | Join `EInk-AirTraffic` first, then open `http://192.168.4.1`; some phones keep mobile data as the default route. |
| `Wi-Fi setup failed` | Wrong Wi-Fi password, 5 GHz-only network, or the portal timed out. Reset and retry. |
| `HTTP -1` / `HTTP -` | No internet, DNS failure, TLS handshake failure or adsb.lol unavailable. Check the network and press RESET. |
| `No body memory` | Firmware older than the streaming parser tried to buffer the whole feed; the response was larger than the free heap. Update the firmware and reduce the radius if needed. |
| `Bad feed` | The adsb.lol response was truncated, malformed or timed out mid-stream. The next wake retries; check the network if it persists. |
| `NO AIRCRAFT` | Wrong latitude/longitude, radius too small, or genuinely quiet airspace. Reconfigure with a larger radius. |
| Wrong UTC time | The time comes from the feed's `now` field, not an RTC. A stale or malformed feed shows the wrong time. |
| Panel shows the setup page again | Location was never saved or the portal timed out. Reconfigure and wait for the fetch/render. |

## Attribution

- [adsb.lol](https://adsb.lol) for the community ADS-B feed.
- [tzapu/WiFiManager](https://github.com/tzapu/WiFiManager) for the captive
  portal.
- [bblanchon/ArduinoJson](https://github.com/bblanchon/ArduinoJson) for JSON
  parsing.
- [dmellok/tesserae-widget-overhead](https://github.com/dmellok/tesserae-widget-overhead)
  for the visual idea. The Overhead widget is AGPL-3.0; this firmware is an
  independent implementation and does not include its code.
