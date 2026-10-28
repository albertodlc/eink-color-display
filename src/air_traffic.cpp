#include "air_traffic.h"

#include <ArduinoJson.h>
#include <HTTPClient.h>
#include <JPEGDEC.h>
#include <Network.h>  // WiFi's public headers require the core Networking library
#include <Preferences.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <WiFiManager.h>
#include <ctype.h>
#include <esp_attr.h>
#include <esp_random.h>
#include <esp_sleep.h>

#include <algorithm>
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "air_traffic_config.h"
#include "fallback_photos.h"

namespace {

struct AirTrafficConfig {
  bool valid;
  double lat;
  double lon;
  int radius_km;
};

constexpr int16_t kHeaderTitleY = 8;
constexpr int16_t kHeaderTimeY = 12;
constexpr int16_t kHeaderRuleY = 30;
constexpr int16_t kBodyTop = 34;
constexpr int16_t kBodyBottom = 470;
constexpr int16_t kGridCardW = 379;
constexpr int16_t kGridCardH = 214;
constexpr int16_t kGridGapX = 10;
constexpr int16_t kGridGapY = 8;

// Photo cards: 280x96, four bits per pixel, six-colour palette indices.
constexpr int16_t kPhotoW = 320;
constexpr int16_t kPhotoH = 128;
constexpr uint16_t kPhotoBytes = (kPhotoW * kPhotoH * 3 + 7) / 8 + 2;
constexpr size_t kJpegBufferSize = 32 * 1024;

// Measured Spectra 6 appearance (Zephyr reTerminal E1002 reference panel).
// If this panel looks too bright, try the darker approximation used by the
// older image demo: {20,20,20}, {235,235,230}, {40,110,60}, {40,60,120},
// {180,30,40}, {230,200,40}.
const uint8_t kPhotoPalette[6][3] = {
    {0, 0, 0},       {245, 245, 240}, {60, 140, 75},
    {40, 85, 180},   {196, 50, 58},   {230, 200, 40},
};
const uint16_t kPhotoColors[6] = {
    GxEPD_BLACK, GxEPD_WHITE, GxEPD_GREEN,
    GxEPD_BLUE,  GxEPD_RED,   GxEPD_YELLOW,
};

struct PhotoCard {
  bool valid;
  uint8_t pixels[kPhotoBytes];
};

enum class PhotoFormat : uint8_t { Packed3, Rgb332 };

// A packed image plus its dimensions. Live photos are 3-bit palette indices
// (dithered at source resolution); bundled fallbacks are RGB332 data dithered
// at the final card resolution.
struct PhotoView {
  const uint8_t* pixels;
  uint16_t width;
  uint16_t height;
  PhotoFormat format;
};

// Allocate each 15 KB packed card separately. The heap is fragmented after
// Wi-Fi/TLS, so this is much more reliable than one large block.
PhotoCard* g_photos[AIR_TRAFFIC_PAGE_AIRCRAFT] = {};

bool ensure_photo_store() {
  bool ok = true;
  for (uint8_t i = 0; i < AIR_TRAFFIC_PAGE_AIRCRAFT; i++) {
    if (!g_photos[i]) {
      g_photos[i] = (PhotoCard*)malloc(sizeof(PhotoCard));
    }
    if (g_photos[i]) {
      g_photos[i]->valid = false;
    } else {
      ok = false;
    }
  }
  return ok;
}

const AirTrafficData* g_traffic = nullptr;

bool intersects(int16_t top, int16_t bottom, int16_t page_top,
                int16_t page_bottom) {
  return top < page_bottom && page_top < bottom;
}

void distance_bearing(double lat1, double lon1, double lat2, double lon2,
                      float& km, float& bearing) {
  const double rad = PI / 180.0;
  const double dlat = (lat2 - lat1) * rad;
  const double dlon = (lon2 - lon1) * rad;
  const double a = sin(dlat / 2) * sin(dlat / 2) +
                   cos(lat1 * rad) * cos(lat2 * rad) *
                       sin(dlon / 2) * sin(dlon / 2);
  const double c = 2 * atan2(sqrt(a), sqrt(1 - a));
  km = (float)(6371.0 * c);
  const double y = sin(dlon) * cos(lat2 * rad);
  const double x = cos(lat1 * rad) * sin(lat2 * rad) -
                   sin(lat1 * rad) * cos(lat2 * rad) * cos(dlon);
  bearing = (float)(fmod(atan2(y, x) / rad + 360.0, 360.0));
}

struct TypeName {
  const char* code;
  const char* name;
};

const TypeName kTypeNames[] = {
    {"A319", "Airbus A319"},      {"A320", "Airbus A320"},
    {"A321", "Airbus A321"},      {"A20N", "Airbus A320neo"},
    {"A21N", "Airbus A321neo"},   {"A332", "Airbus A330-200"},
    {"A333", "Airbus A330-300"},  {"A359", "Airbus A350-900"},
    {"A35K", "Airbus A350-1000"}, {"BCS1", "Airbus A220-100"},
    {"BCS3", "Airbus A220-300"},  {"B38M", "Boeing 737 MAX 8"},
    {"B39M", "Boeing 737 MAX 9"}, {"B737", "Boeing 737-700"},
    {"B738", "Boeing 737-800"},   {"B739", "Boeing 737-900"},
    {"B744", "Boeing 747-400"},   {"B748", "Boeing 747-8"},
    {"B752", "Boeing 757-200"},   {"B763", "Boeing 767-300"},
    {"B772", "Boeing 777-200"},   {"B77W", "Boeing 777-300ER"},
    {"B788", "Boeing 787-8"},     {"B789", "Boeing 787-9"},
    {"B78X", "Boeing 787-10"},    {"E170", "Embraer E170"},
    {"E75L", "Embraer E175"},     {"E190", "Embraer E190"},
    {"E195", "Embraer E195"},     {"AT76", "ATR 72-600"},
    {"DH8D", "Dash 8 Q400"},      {"CRJ9", "Bombardier CRJ-900"},
};

void fill_type_name(const char* code, char* out, size_t size) {
  if (!out || size == 0) return;
  if (!code || !code[0] || strcmp(code, "?") == 0) {
    snprintf(out, size, "Unknown type");
    return;
  }
  for (const TypeName& entry : kTypeNames) {
    if (strcmp(code, entry.code) == 0) {
      snprintf(out, size, "%s", entry.name);
      return;
    }
  }
  snprintf(out, size, "%s", code);
}

void format_thousands(int value, char* out, size_t size) {
  if (value < 0) {
    snprintf(out, size, "%d", value);
  } else if (value < 1000) {
    snprintf(out, size, "%d", value);
  } else {
    snprintf(out, size, "%d,%03d", value / 1000, value % 1000);
  }
}

void format_altitude(const AirTrafficAircraft& aircraft, char* out,
                     size_t size) {
  if (aircraft.ground) {
    snprintf(out, size, "on the ground");
    return;
  }
  if (aircraft.altitude_ft < 0) {
    snprintf(out, size, "altitude unknown");
    return;
  }
  char altitude[10];
  format_thousands(aircraft.altitude_ft, altitude, sizeof(altitude));
  const char* trend = "level";
  if (aircraft.vertical_rate_fpm > 300) {
    trend = "climbing";
  } else if (aircraft.vertical_rate_fpm < -300) {
    trend = "descending";
  }
  snprintf(out, size, "%s ft %s", altitude, trend);
}

void format_distance(const AirTrafficAircraft& aircraft, char* out,
                     size_t size) {
  snprintf(out, size, "%d km / %d deg", aircraft.distance_km,
           aircraft.bearing_deg);
}

void format_last_line(const AirTrafficAircraft& aircraft, char* out,
                      size_t size) {
  if (aircraft.route[0]) {
    snprintf(out, size, "%s", aircraft.route);
  } else {
    format_distance(aircraft, out, size);
  }
}

struct RouteCacheEntry {
  char callsign[10];
  char route[40];
  uint32_t fetched;
};

// Survives deep sleep (but not a power cycle). Avoids querying adsbdb for the
// same flight on every five-minute wake.
RTC_DATA_ATTR RouteCacheEntry g_route_cache[12];
constexpr uint32_t kRouteCacheTtlS = 2 * 60 * 60;

bool route_cache_get(const char* callsign, uint32_t now, char* out,
                     size_t size) {
  for (const RouteCacheEntry& entry : g_route_cache) {
    if (!entry.callsign[0] || strcmp(entry.callsign, callsign) != 0) continue;
    if (now == 0 || entry.fetched == 0 ||
        (now >= entry.fetched && now - entry.fetched < kRouteCacheTtlS)) {
      snprintf(out, size, "%s", entry.route);
      return true;
    }
    return false;
  }
  return false;
}

void route_cache_put(const char* callsign, const char* route, uint32_t now) {
  int slot = -1;
  for (int i = 0; i < 12; i++) {
    if (!g_route_cache[i].callsign[0] ||
        strcmp(g_route_cache[i].callsign, callsign) == 0) {
      slot = i;
      break;
    }
  }
  if (slot < 0) {
    slot = 0;
    for (int i = 1; i < 12; i++) {
      if (g_route_cache[i].fetched < g_route_cache[slot].fetched) slot = i;
    }
  }
  snprintf(g_route_cache[slot].callsign, sizeof(g_route_cache[slot].callsign),
           "%s", callsign);
  snprintf(g_route_cache[slot].route, sizeof(g_route_cache[slot].route), "%s",
           route);
  g_route_cache[slot].fetched = now;
}

void resolve_route(AirTrafficAircraft& aircraft, uint32_t now) {
  aircraft.route[0] = 0;
  if (!aircraft.callsign[0]) return;
  bool has_digit = false;
  for (const char* p = aircraft.callsign; *p; ++p) {
    if (isdigit((unsigned char)*p)) {
      has_digit = true;
      break;
    }
  }
  if (!has_digit) return;

  if (route_cache_get(aircraft.callsign, now, aircraft.route,
                      sizeof(aircraft.route))) {
    return;
  }

  char url[128];
  snprintf(url, sizeof(url), "https://api.adsbdb.com/v0/callsign/%s",
           aircraft.callsign);
  String payload;
  int code = -1;
  for (int attempt = 0; attempt < 2; attempt++) {
    WiFiClientSecure client;
    client.setInsecure();
    HTTPClient http;
    http.setTimeout(6000);
    http.setUserAgent("eink-color-display/1.0 (ESP32)");
    http.setFollowRedirects(HTTPC_FORCE_FOLLOW_REDIRECTS);
    if (!http.begin(client, url)) {
      Serial.printf("[air] route %s: begin failed (attempt %d)\n",
                    aircraft.callsign, attempt + 1);
      delay(200);
      continue;
    }
    code = http.GET();
    if (code == 200) {
      payload = http.getString();
      http.end();
      break;
    }
    Serial.printf("[air] route %s: HTTP %d (attempt %d)\n", aircraft.callsign,
                  code, attempt + 1);
    http.end();
    delay(200);
  }
  if (code != 200 || payload.length() == 0) return;

  JsonDocument filter;
  filter["response"]["flightroute"]["origin"]["municipality"] = true;
  filter["response"]["flightroute"]["origin"]["iata_code"] = true;
  filter["response"]["flightroute"]["destination"]["municipality"] = true;
  filter["response"]["flightroute"]["destination"]["iata_code"] = true;

  JsonDocument doc;
  const DeserializationError error =
      deserializeJson(doc, payload, DeserializationOption::Filter(filter));
  if (!error) {
    const char* origin =
        doc["response"]["flightroute"]["origin"]["municipality"] | "";
    const char* dest =
        doc["response"]["flightroute"]["destination"]["municipality"] | "";
    if (!origin[0]) {
      origin = doc["response"]["flightroute"]["origin"]["iata_code"] | "";
    }
    if (!dest[0]) {
      dest = doc["response"]["flightroute"]["destination"]["iata_code"] | "";
    }
    if (origin[0] && dest[0]) {
      snprintf(aircraft.route, sizeof(aircraft.route), "%s to %s", origin,
               dest);
    }
  }
  Serial.printf("[air] route %s: %s\n", aircraft.callsign,
                aircraft.route[0] ? aircraft.route : "unknown");
  route_cache_put(aircraft.callsign, aircraft.route, now);
}

uint8_t photo_nearest_index(uint8_t r, uint8_t g, uint8_t b) {
  uint8_t best = 1;
  uint32_t best_distance = 0xFFFFFFFFu;
  for (uint8_t i = 0; i < 6; i++) {
    const int dr = (int)r - kPhotoPalette[i][0];
    const int dg = (int)g - kPhotoPalette[i][1];
    const int db = (int)b - kPhotoPalette[i][2];
    const uint32_t distance =
        (uint32_t)(3 * dr * dr + 4 * dg * dg + 2 * db * db);
    if (distance < best_distance) {
      best_distance = distance;
      best = i;
    }
  }
  return best;
}

uint8_t photo_index_dither(uint8_t r, uint8_t g, uint8_t b, int16_t x,
                           int16_t y) {
  // A light ordered dither at the final display resolution. It is used for the
  // bundled fallback images, which are stored as smooth RGB332 data.
  static const int8_t kBayer4[4][4] = {
      {-8, 0, -6, 2},
      {4, -4, 6, -2},
      {-6, 2, -8, 0},
      {6, -2, 4, -4},
  };
  const int offset = kBayer4[y & 3][x & 3];
  int rr = (int)r + offset;
  int gg = (int)g + offset;
  int bb = (int)b + offset;
  if (rr < 0) rr = 0;
  if (rr > 255) rr = 255;
  if (gg < 0) gg = 0;
  if (gg > 255) gg = 255;
  if (bb < 0) bb = 0;
  if (bb > 255) bb = 255;
  return photo_nearest_index((uint8_t)rr, (uint8_t)gg, (uint8_t)bb);
}

void rgb332_to_rgb(uint8_t packed, int& r, int& g, int& b) {
  r = ((packed >> 5) & 7) * 255 / 7;
  g = ((packed >> 2) & 7) * 255 / 7;
  b = (packed & 3) * 255 / 3;
}

void photo_set(PhotoCard& photo, int x, int y, uint8_t index) {
  const uint32_t bit = ((uint32_t)y * kPhotoW + x) * 3;
  const uint16_t byte = (uint16_t)(bit >> 3);
  const uint8_t shift = (uint8_t)(bit & 7);
  uint32_t value = (uint32_t)photo.pixels[byte] |
                   ((uint32_t)photo.pixels[byte + 1] << 8) |
                   ((uint32_t)photo.pixels[byte + 2] << 16);
  const uint32_t mask = 7u << shift;
  value = (value & ~mask) | ((uint32_t)(index & 7) << shift);
  photo.pixels[byte] = (uint8_t)value;
  photo.pixels[byte + 1] = (uint8_t)(value >> 8);
  photo.pixels[byte + 2] = (uint8_t)(value >> 16);
}

void photo_clear(PhotoCard& photo) {
  for (int y = 0; y < kPhotoH; y++) {
    for (int x = 0; x < kPhotoW; x++) {
      photo_set(photo, x, y, 1);
    }
  }
}

PhotoCard* g_active_photo = nullptr;
int g_jpeg_out_w = 0;
int g_jpeg_out_h = 0;
int g_jpeg_crop_x = 0;
int g_jpeg_crop_y = 0;
int g_jpeg_crop_w = 0;
int g_jpeg_crop_h = 0;

int jpeg_draw_callback(JPEGDRAW* draw) {
  if (!g_active_photo) return 1;
  for (int yy = 0; yy < draw->iHeight; yy++) {
    const int py = draw->y + yy;
    if (py < g_jpeg_crop_y || py >= g_jpeg_crop_y + g_jpeg_crop_h) continue;
    int ty0 = (int)((int64_t)(py - g_jpeg_crop_y) * kPhotoH / g_jpeg_crop_h);
    int ty1 = (int)((int64_t)(py + 1 - g_jpeg_crop_y) * kPhotoH /
                       g_jpeg_crop_h) - 1;
    if (ty1 < ty0) ty1 = ty0;
    if (ty0 < 0) ty0 = 0;
    if (ty1 >= kPhotoH) ty1 = kPhotoH - 1;

    for (int xx = 0; xx < draw->iWidthUsed; xx++) {
      const int px = draw->x + xx;
      if (px < g_jpeg_crop_x || px >= g_jpeg_crop_x + g_jpeg_crop_w) continue;
      int tx0 = (int)((int64_t)(px - g_jpeg_crop_x) * kPhotoW /
                         g_jpeg_crop_w);
      int tx1 = (int)((int64_t)(px + 1 - g_jpeg_crop_x) * kPhotoW /
                         g_jpeg_crop_w) - 1;
      if (tx1 < tx0) tx1 = tx0;
      if (tx0 < 0) tx0 = 0;
      if (tx1 >= kPhotoW) tx1 = kPhotoW - 1;

      const uint16_t rgb565 = draw->pPixels[yy * draw->iWidth + xx];
      // Replicate the high bits into the low bits so 5/6-bit channels use
      // the full 0-255 range instead of stopping at 248/252.
      const uint8_t r5 = (uint8_t)((rgb565 >> 11) & 0x1F);
      const uint8_t g6 = (uint8_t)((rgb565 >> 5) & 0x3F);
      const uint8_t b5 = (uint8_t)(rgb565 & 0x1F);
      const uint8_t r = (uint8_t)((r5 << 3) | (r5 >> 2));
      const uint8_t g = (uint8_t)((g6 << 2) | (g6 >> 4));
      const uint8_t b = (uint8_t)((b5 << 3) | (b5 >> 2));
      const uint8_t index =
          photo_index_dither(r, g, b, (int16_t)tx0, (int16_t)ty0);
      for (int ty = ty0; ty <= ty1; ty++) {
        for (int tx = tx0; tx <= tx1; tx++) {
          photo_set(*g_active_photo, tx, ty, index);
        }
      }
    }
  }
  return 1;
}

bool process_photo(const uint8_t* jpeg_bytes, size_t jpeg_size,
                   PhotoCard& photo) {
  if (jpeg_size < 1000) return false;
  Serial.printf("[air] jpeg: %u bytes, %02X %02X\n", (unsigned)jpeg_size,
                jpeg_bytes[0], jpeg_bytes[1]);

  JPEGDEC* decoder = new JPEGDEC();
  if (!decoder) return false;
  uint8_t* data = const_cast<uint8_t*>(jpeg_bytes);
  if (!decoder->openRAM(data, (int)jpeg_size, jpeg_draw_callback)) {
    Serial.println("[air] jpeg: open failed");
    delete decoder;
    return false;
  }
  if (decoder->getJPEGType() == JPEG_MODE_PROGRESSIVE) {
    Serial.println("[air] jpeg: progressive");
    delete decoder;
    return false;
  }

  const int source_w = decoder->getWidth();
  const int source_h = decoder->getHeight();
  if (source_w < 100 || source_h < 32) return false;

  // The Weserv proxy returns a kPhotoW x kPhotoH baseline JPEG. Take a
  // slightly jittered crop and let the paper dither hide the lost resolution.
  g_jpeg_out_w = source_w;
  g_jpeg_out_h = source_h;
  int crop_w = (source_w * 96) / 100;
  int crop_h = (source_h * 96) / 100;
  if (crop_h < 32) crop_h = source_h;
  if (crop_w < 100) crop_w = source_w;
  if (crop_w > source_w) crop_w = source_w;
  if (crop_h > source_h) crop_h = source_h;

  g_jpeg_crop_x = (source_w - crop_w) / 2 + ((int)(esp_random() % 5) - 2);
  g_jpeg_crop_y = (source_h - crop_h) / 2 + ((int)(esp_random() % 3) - 1);
  g_jpeg_crop_w = crop_w;
  g_jpeg_crop_h = crop_h;
  if (g_jpeg_crop_x < 0) g_jpeg_crop_x = 0;
  if (g_jpeg_crop_y < 0) g_jpeg_crop_y = 0;
  if (g_jpeg_crop_x + g_jpeg_crop_w > g_jpeg_out_w) {
    g_jpeg_crop_x = g_jpeg_out_w - g_jpeg_crop_w;
  }
  if (g_jpeg_crop_y + g_jpeg_crop_h > g_jpeg_out_h) {
    g_jpeg_crop_y = g_jpeg_out_h - g_jpeg_crop_h;
  }

  photo_clear(photo);
  g_active_photo = &photo;
  const int ok = decoder->decode(0, 0, 0);
  g_active_photo = nullptr;
  delete decoder;
  if (!ok) {
    Serial.println("[air] jpeg: decode failed");
    return false;
  }
  photo.valid = true;
  Serial.printf("[air] jpeg: %dx%d -> %dx%d\n", source_w, source_h,
                g_jpeg_out_w, g_jpeg_out_h);
  return true;
}

void log_host_dns(const char* host) {
  IPAddress ip;
  if (WiFi.hostByName(host, ip)) {
    Serial.printf("[net] %s -> %s\n", host, ip.toString().c_str());
  } else {
    Serial.printf("[net] %s -> DNS failed\n", host);
  }
}

bool jetapi_thumbnail(const char* registration, char* out, size_t size) {
  log_host_dns("www.jetapi.dev");
  char api[160];
  snprintf(api, sizeof(api),
           "https://www.jetapi.dev/api?reg=%s&photos=2&flights=0",
           registration);

  String payload;
  int code = -1;
  for (int attempt = 0; attempt < 2; attempt++) {
    WiFiClientSecure client;
    client.setInsecure();
    HTTPClient http;
    http.setTimeout(8000);
    http.setUserAgent("eink-color-display/1.0 (ESP32)");
    http.setFollowRedirects(HTTPC_FORCE_FOLLOW_REDIRECTS);
    if (!http.begin(client, api)) {
      Serial.printf("[air] jetapi %s: begin failed (attempt %d)\n",
                    registration, attempt + 1);
      delay(300);
      continue;
    }
    code = http.GET();
    if (code == 200) {
      payload = http.getString();
      http.end();
      break;
    }
    Serial.printf("[air] jetapi %s: HTTP %d (%s) (attempt %d)\n",
                  registration, code, http.errorToString(code).c_str(),
                  attempt + 1);
    http.end();
    delay(300);
  }
  if (code != 200 || payload.length() == 0) return false;

  JsonDocument filter;
  filter["JetPhotos"]["Images"][0]["Thumbnail"] = true;
  filter["JetPhotos"]["Images"][0]["Image"] = true;
  JsonDocument doc;
  if (deserializeJson(doc, payload, DeserializationOption::Filter(filter))) {
    Serial.printf("[air] jetapi %s: bad JSON\n", registration);
    return false;
  }

  const char* thumb = doc["JetPhotos"]["Images"][0]["Thumbnail"] | "";
  const char* full = doc["JetPhotos"]["Images"][0]["Image"] | "";
  if (thumb[0]) {
    strncpy(out, thumb, size - 1);
    out[size - 1] = 0;
    return true;
  }
  if (full[0]) {
    const char* marker = strstr(full, "/full/");
    if (!marker) return false;
    const size_t prefix = (size_t)(marker - full);
    if (prefix + 6 >= size) return false;
    memcpy(out, full, prefix);
    memcpy(out + prefix, "/400/", 5);
    strncpy(out + prefix + 5, marker + 6, size - prefix - 6);
    out[size - 1] = 0;
    return true;
  }
  Serial.printf("[air] jetapi %s: no photo\n", registration);
  return false;
}

bool planespotters_thumbnail(const char* registration, char* out, size_t size) {
  log_host_dns("api.planespotters.net");
  char api[160];
  snprintf(api, sizeof(api),
           "https://api.planespotters.net/pub/photos/reg/%s", registration);

  WiFiClientSecure client;
  client.setInsecure();
  HTTPClient http;
  http.setTimeout(6000);
  http.setUserAgent(
      "tesserae-overhead/0.1 "
      "(+https://github.com/dmellok/tesserae-widget-overhead)");
  http.setFollowRedirects(HTTPC_FORCE_FOLLOW_REDIRECTS);
  if (!http.begin(client, api)) return false;

  const int code = http.GET();
  if (code != 200) {
    Serial.printf("[air] planespotters %s: HTTP %d (%s)\n", registration,
                  code, http.errorToString(code).c_str());
    http.end();
    return false;
  }
  const String payload = http.getString();
  http.end();

  JsonDocument filter;
  filter["photos"][0]["thumbnail_large"]["src"] = true;
  filter["photos"][0]["thumbnail"]["src"] = true;
  JsonDocument doc;
  if (deserializeJson(doc, payload, DeserializationOption::Filter(filter))) {
    return false;
  }
  const char* url = doc["photos"][0]["thumbnail_large"]["src"] | "";
  if (!url[0]) url = doc["photos"][0]["thumbnail"]["src"] | "";
  if (!url[0]) return false;
  strncpy(out, url, size - 1);
  out[size - 1] = 0;
  return true;
}

void weserv_url(const char* source, char* out, size_t size) {
  snprintf(out, size,
           "http://images.weserv.nl/?url=%s&w=%d&h=%d&fit=cover"
           "&output=jpg&q=80",
           source, (int)kPhotoW, (int)kPhotoH);
}

bool download_photo(const char* url, uint8_t*& buffer, size_t capacity,
                    size_t& length) {
  length = 0;
  buffer = nullptr;
  // The image proxy is public and read-only. Plain HTTP avoids a second TLS
  // handshake next to the live photo buffers; the metadata APIs stay on HTTPS.
  WiFiClient client;
  HTTPClient http;
  http.setTimeout(20000);
  http.setUserAgent(
      "Mozilla/5.0 (X11; Linux x86_64) AppleWebKit/537.36 "
      "(KHTML, like Gecko) Chrome/120 Safari/537.36");
  http.addHeader("Referer", "https://www.jetphotos.com/");
  http.setFollowRedirects(HTTPC_FORCE_FOLLOW_REDIRECTS);
  Serial.printf("[air] photo URL: %s\n", url);
  if (!http.begin(client, url)) {
    Serial.println("[air] photo: begin failed");
    return false;
  }

  const int code = http.GET();
  const int size = http.getSize();
  if (code != 200) {
    Serial.printf("[air] photo: HTTP %d\n", code);
    http.end();
    return false;
  }
  if (size <= 0 || (size_t)size > capacity) {
    Serial.printf("[air] photo: size %d (capacity %u)\n", size,
                  (unsigned)capacity);
    http.end();
    return false;
  }
  Serial.printf("[air] photo: HTTP 200, size %d\n", size);

  buffer = (uint8_t*)malloc((size_t)size);
  if (!buffer) {
    Serial.printf("[air] photo: body allocation failed, heap %u\n",
                  (unsigned)ESP.getFreeHeap());
    http.end();
    return false;
  }

  WiFiClient* stream = http.getStreamPtr();
  if (!stream) {
    free(buffer);
    buffer = nullptr;
    http.end();
    return false;
  }
  size_t total = 0;
  const uint32_t started = millis();
  while (total < (size_t)size && millis() - started < 20000) {
    const int available = stream->available();
    if (available <= 0) {
      delay(1);
      continue;
    }
    size_t want = (size_t)available;
    if (want > (size_t)size - total) want = (size_t)size - total;
    const size_t got = stream->read(buffer + total, want);
    total += got;
  }
  http.end();
  length = total;
  Serial.printf("[air] photo: read %u/%d bytes\n", (unsigned)total, size);
  if (total != (size_t)size) {
    free(buffer);
    buffer = nullptr;
    return false;
  }
  return true;
}

struct PhotoUrlCache {
  char registration[10];
  char url[160];
  uint32_t fetched;
};

RTC_DATA_ATTR PhotoUrlCache g_photo_cache[AIR_TRAFFIC_PAGE_AIRCRAFT];
constexpr uint32_t kPhotoCacheTtlS = 24 * 60 * 60;

bool photo_cache_get(const char* registration, uint32_t now, char* out,
                     size_t size) {
  for (const PhotoUrlCache& entry : g_photo_cache) {
    if (!entry.registration[0] ||
        strcmp(entry.registration, registration) != 0) {
      continue;
    }
    if (now == 0 || entry.fetched == 0 ||
        (now >= entry.fetched && now - entry.fetched < kPhotoCacheTtlS)) {
      strncpy(out, entry.url, size - 1);
      out[size - 1] = 0;
      return out[0] != 0;
    }
    return false;
  }
  return false;
}

void photo_cache_put(const char* registration, const char* url, uint32_t now) {
  int slot = -1;
  for (int i = 0; i < AIR_TRAFFIC_PAGE_AIRCRAFT; i++) {
    if (!g_photo_cache[i].registration[0] ||
        strcmp(g_photo_cache[i].registration, registration) == 0) {
      slot = i;
      break;
    }
  }
  if (slot < 0) {
    slot = 0;
    for (int i = 1; i < AIR_TRAFFIC_PAGE_AIRCRAFT; i++) {
      if (g_photo_cache[i].fetched < g_photo_cache[slot].fetched) slot = i;
    }
  }
  snprintf(g_photo_cache[slot].registration,
           sizeof(g_photo_cache[slot].registration), "%s", registration);
  snprintf(g_photo_cache[slot].url, sizeof(g_photo_cache[slot].url), "%s",
           url);
  g_photo_cache[slot].fetched = now;
}

void photo_cache_invalidate(const char* registration) {
  for (PhotoUrlCache& entry : g_photo_cache) {
    if (entry.registration[0] &&
        strcmp(entry.registration, registration) == 0) {
      entry.fetched = 0;
      entry.url[0] = 0;
      return;
    }
  }
}

bool find_photo_url(const char* registration, uint32_t now, char* out,
                    size_t size) {
  char source[160];
  if (photo_cache_get(registration, now, source, sizeof(source))) {
    Serial.printf("[air] photo %s: cached source\n", registration);
    weserv_url(source, out, size);
    return true;
  }

  if (!jetapi_thumbnail(registration, source, sizeof(source))) {
    if (!planespotters_thumbnail(registration, source, sizeof(source))) {
      Serial.printf("[air] photo %s: no source photo\n", registration);
      return false;
    }
  }
  Serial.printf("[air] photo %s: source %s\n", registration, source);
  photo_cache_put(registration, source, now);
  weserv_url(source, out, size);
  return true;
}

struct PhotoUrl {
  char url[256];
  bool valid;
};

void fetch_photos(AirTrafficData& data) {
  // Resolve all source URLs first, while the full photo heap is still free.
  // The TLS handshake for JetAPI/Planespotters needs more headroom than the
  // image downloads that follow.
  static PhotoUrl photo_urls[AIR_TRAFFIC_PAGE_AIRCRAFT];
  for (uint8_t i = 0; i < AIR_TRAFFIC_PAGE_AIRCRAFT; i++) {
    photo_urls[i].valid = false;
    if (i >= data.count) continue;
    AirTrafficAircraft& aircraft = data.aircraft[i];
    if (!aircraft.registration[0]) continue;
    photo_urls[i].valid =
        find_photo_url(aircraft.registration, data.feed_time,
                       photo_urls[i].url, sizeof(photo_urls[i].url));
  }

  if (!ensure_photo_store()) {
    Serial.printf("[air] photo buffers: allocation failed, heap %u\n",
                  (unsigned)ESP.getFreeHeap());
    return;
  }
  Serial.printf("[air] photo buffers: heap %u\n",
                (unsigned)ESP.getFreeHeap());
  for (uint8_t i = 0; i < AIR_TRAFFIC_PAGE_AIRCRAFT; i++) {
    if (g_photos[i]) g_photos[i]->valid = false;
  }

  for (uint8_t i = 0; i < data.count && i < AIR_TRAFFIC_PAGE_AIRCRAFT; i++) {
    if (!photo_urls[i].valid) continue;
    AirTrafficAircraft& aircraft = data.aircraft[i];

    // download_photo() allocates the exact body size only after the TLS
    // handshake, so the handshake sees the full remaining heap.
    uint8_t* jpeg = nullptr;
    size_t jpeg_size = 0;
    bool downloaded =
        download_photo(photo_urls[i].url, jpeg, kJpegBufferSize, jpeg_size);
    if (!downloaded) {
      delay(250);
      downloaded =
          download_photo(photo_urls[i].url, jpeg, kJpegBufferSize, jpeg_size);
    }
    if (!downloaded) {
      photo_cache_invalidate(aircraft.registration);
      free(jpeg);
      continue;
    }

    const uint32_t start = millis();
    const bool decoded = process_photo(jpeg, jpeg_size, *g_photos[i]);
    free(jpeg);
    if (!decoded) {
      photo_cache_invalidate(aircraft.registration);
      Serial.printf("[air] photo %s: decode failed\n", aircraft.registration);
      continue;
    }
    Serial.printf("[air] photo %s: %u bytes, %lu ms\n",
                  aircraft.registration, (unsigned)jpeg_size,
                  (unsigned long)(millis() - start));
    Serial.flush();
  }
}

void format_time_date(uint32_t epoch, char* time_out, size_t time_size,
                      char* date_out, size_t date_size) {
  if (epoch == 0) {
    snprintf(time_out, time_size, "--:-- UTC");
    snprintf(date_out, date_size, "--- -- ---");
    return;
  }
  time_t value = (time_t)epoch;
  struct tm tm;
  gmtime_r(&value, &tm);
  snprintf(time_out, time_size, "%02d:%02d UTC", tm.tm_hour, tm.tm_min);
  strftime(date_out, date_size, "%a %d %b", &tm);
}

int16_t text_width(const char* text, uint8_t size) {
  return (int16_t)(strlen(text) * 6 * size);
}

void draw_centered(epd::Display& display, int16_t x, int16_t y, int16_t width,
                   const char* text, uint8_t size, uint16_t color) {
  display.setTextSize(size);
  display.setTextColor(color);
  display.setCursor(x + (width - text_width(text, size)) / 2, y);
  display.print(text);
}

void draw_right(epd::Display& display, int16_t right, int16_t y,
                const char* text, uint8_t size, uint16_t color) {
  display.setTextSize(size);
  display.setTextColor(color);
  display.setCursor(right - text_width(text, size), y);
  display.print(text);
}

// Callsign and aircraft model on one line: the callsign stays prominent and
// the model uses a slightly smaller size so longer names still fit.
void draw_flight_and_type(epd::Display& display, int16_t x, int16_t y,
                          int16_t width, const char* callsign,
                          const char* type_name, uint8_t callsign_size,
                          uint8_t type_size) {
  int16_t callsign_w = text_width(callsign, callsign_size);
  int16_t type_w = text_width(type_name, type_size);
  constexpr int16_t kGap = 8;
  int16_t total = callsign_w + kGap + type_w;

  while (type_size > 1 && total > width) {
    type_size--;
    type_w = text_width(type_name, type_size);
    total = callsign_w + kGap + type_w;
  }
  if (total > width && callsign_size > 1) {
    callsign_size--;
    callsign_w = text_width(callsign, callsign_size);
    total = callsign_w + kGap + type_w;
  }

  const int16_t start = x + (width - total) / 2;
  display.setTextColor(GxEPD_BLACK);
  display.setTextSize(callsign_size);
  display.setCursor(start, y);
  display.print(callsign);
  display.setTextSize(type_size);
  display.setCursor(start + callsign_w + kGap,
                    y + (int16_t)(callsign_size - type_size) * 8);
  display.print(type_name);
}

// Altitude/trend and route or distance on one compact line.
void draw_flight_details(epd::Display& display, int16_t x, int16_t y,
                         int16_t width, const char* altitude,
                         const char* last_line, uint8_t size) {
  char text[96];
  snprintf(text, sizeof(text), "%s - %s", altitude, last_line);
  if (text_width(text, size) > width && size > 1) size = 1;
  draw_centered(display, x, y, width, text, size, GxEPD_BLACK);
}

struct TypePhoto {
  const char* code;
  const uint8_t* pixels;
};

const TypePhoto kTypePhotos[] = {
    // Airbus and Boeing airliners.
    {"A318", kFallbackA320}, {"A319", kFallbackA320},
    {"A320", kFallbackA320}, {"A321", kFallbackA320},
    {"A19N", kFallbackA320}, {"A20N", kFallbackA320},
    {"A21N", kFallbackA320},
    {"B736", kFallbackB738}, {"B737", kFallbackB738},
    {"B738", kFallbackB738}, {"B739", kFallbackB738},
    {"B38M", kFallbackB738}, {"B39M", kFallbackB738},
    {"B752", kFallbackB752}, {"B753", kFallbackB752},
    {"A332", kFallbackA333}, {"A333", kFallbackA333},
    {"A339", kFallbackA333}, {"A342", kFallbackA333},
    {"A343", kFallbackA333}, {"A345", kFallbackA333},
    {"A346", kFallbackA333}, {"A359", kFallbackA333},
    {"A35K", kFallbackA333}, {"A388", kFallbackA333},
    {"B744", kFallbackA333}, {"B748", kFallbackA333},
    {"B762", kFallbackA333}, {"B763", kFallbackA333},
    {"B764", kFallbackA333}, {"B772", kFallbackA333},
    {"B773", kFallbackA333}, {"B77L", kFallbackA333},
    {"B77W", kFallbackA333}, {"B788", kFallbackA333},
    {"B789", kFallbackA333}, {"B78X", kFallbackA333},

    // Regional jets.
    {"BCS1", kFallbackE190}, {"BCS3", kFallbackE190},
    {"CRJ1", kFallbackE190}, {"CRJ2", kFallbackE190},
    {"CRJ7", kFallbackE190}, {"CRJ9", kFallbackE190},
    {"CRJX", kFallbackE190},
    {"E135", kFallbackE190}, {"E145", kFallbackE190},
    {"E170", kFallbackE190}, {"E175", kFallbackE190},
    {"E190", kFallbackE190}, {"E195", kFallbackE190},
    {"E75L", kFallbackE190}, {"E75S", kFallbackE190},

    // Turboprops.
    {"AT45", kFallbackAT72}, {"AT72", kFallbackAT72},
    {"AT76", kFallbackAT72},
    {"DH8A", kFallbackDH8D}, {"DH8B", kFallbackDH8D},
    {"DH8C", kFallbackDH8D}, {"DH8D", kFallbackDH8D},
    {"DHC6", kFallbackDH8D},
    {"BE20", kFallbackDH8D}, {"BE30", kFallbackDH8D},
    {"BE9L", kFallbackDH8D}, {"BE10", kFallbackDH8D},
    {"PC12", kFallbackDH8D},
    {"TBM7", kFallbackDH8D}, {"TBM8", kFallbackDH8D},
    {"TBM9", kFallbackDH8D},
    {"C208", kFallbackDH8D},

    // Business jets.
    {"C25A", kFallbackC25B}, {"C25B", kFallbackC25B},
    {"C25C", kFallbackC25B},
    {"C500", kFallbackC25B}, {"C510", kFallbackC25B},
    {"C550", kFallbackC25B}, {"C560", kFallbackC25B},
    {"C56X", kFallbackC25B}, {"C680", kFallbackC25B},
    {"C68A", kFallbackC25B}, {"C700", kFallbackC25B},
    {"E50P", kFallbackC25B}, {"E55P", kFallbackC25B},
    {"GLF4", kFallbackC25B}, {"GLF5", kFallbackC25B},
    {"GLF6", kFallbackC25B}, {"GL7T", kFallbackC25B},
    {"LJ35", kFallbackC25B}, {"LJ45", kFallbackC25B},
    {"LJ60", kFallbackC25B},
    {"CL30", kFallbackC25B}, {"CL35", kFallbackC25B},
    {"F2TH", kFallbackC25B}, {"F900", kFallbackC25B},
    {"FA7X", kFallbackC25B},
    {"HDJT", kFallbackC25B}, {"PC24", kFallbackC25B},

    // Light singles.
    {"C150", kFallbackC172}, {"C152", kFallbackC172},
    {"C170", kFallbackC172}, {"C172", kFallbackC172},
    {"C182", kFallbackC172}, {"C206", kFallbackC172},
    {"C210", kFallbackC172},
    {"P28A", kFallbackC172}, {"P28B", kFallbackC172},
    {"PA28", kFallbackC172}, {"PA32", kFallbackC172},
    {"SR22", kFallbackC172}, {"DA40", kFallbackC172},
    {"DV20", kFallbackC172},
    {"BE58", kFallbackC172}, {"C310", kFallbackC172},
    {"C414", kFallbackC172}, {"C421", kFallbackC172},
    {"PA34", kFallbackC172}, {"DA42", kFallbackC172},
    {"GLID", kFallbackC172}, {"AS21", kFallbackC172},
    {"DG80", kFallbackC172}, {"LS8", kFallbackC172},

    // Helicopters.
    {"A109", kFallbackA139}, {"A119", kFallbackA139},
    {"A139", kFallbackA139}, {"A169", kFallbackA139},
    {"A189", kFallbackA139},
    {"EC35", kFallbackA139}, {"EC45", kFallbackA139},
    {"EC55", kFallbackA139},
    {"H125", kFallbackA139}, {"H130", kFallbackA139},
    {"H135", kFallbackA139}, {"H145", kFallbackA139},
    {"AS50", kFallbackA139}, {"AS65", kFallbackA139},
    {"B06", kFallbackA139}, {"B206", kFallbackA139},
    {"B212", kFallbackA139}, {"B412", kFallbackA139},
    {"B407", kFallbackA139}, {"B429", kFallbackA139},
    {"CH47", kFallbackA139}, {"H47", kFallbackA139},
    {"EC20", kFallbackA139}, {"EC25", kFallbackA139},
    {"EC30", kFallbackA139},
    {"H60", kFallbackA139}, {"UH60", kFallbackA139},
    {"UH1", kFallbackA139}, {"H1", kFallbackA139},
    {"AW101", kFallbackA139}, {"AW169", kFallbackA139},
    {"AW189", kFallbackA139},
    {"MD52", kFallbackA139},
    {"R22", kFallbackA139}, {"R44", kFallbackA139},
    {"R66", kFallbackA139},
    {"S70", kFallbackA139}, {"S76", kFallbackA139},
    {"S92", kFallbackA139},
};

PhotoView fallback_photo_for_type(const char* type) {
  const uint8_t* pixels = kFallbackA320;
  if (type && type[0]) {
    for (const TypePhoto& entry : kTypePhotos) {
      if (strcmp(type, entry.code) == 0) {
        pixels = entry.pixels;
        break;
      }
    }
  }
  return {pixels, kFallbackPhotoWidth, kFallbackPhotoHeight,
          PhotoFormat::Rgb332};
}

bool load_config(AirTrafficConfig& config) {
  Preferences prefs;
  if (!prefs.begin("airtraffic", false)) return false;
  config.valid = prefs.getBool("valid", false);
  config.lat = prefs.getDouble("lat", 0.0);
  config.lon = prefs.getDouble("lon", 0.0);
  config.radius_km = prefs.getInt("radius", AIR_TRAFFIC_RADIUS_KM);
  // Older builds could persist a radius outside the portal's 1-200 km range.
  // Such a feed can be hundreds of KB and exhaust the ESP32 heap, so fall back
  // to the compile-time default instead.
  if (config.radius_km < 1 || config.radius_km > 200) {
    config.radius_km = AIR_TRAFFIC_RADIUS_KM;
  }

  // One-time migration from the old 50 km default. Custom radii are kept.
  constexpr uint8_t kConfigRevision = 2;
  if (prefs.getUChar("rev", 0) < kConfigRevision) {
    if (config.radius_km == 50) {
      config.radius_km = AIR_TRAFFIC_RADIUS_KM;
      prefs.putInt("radius", config.radius_km);
    }
    prefs.putUChar("rev", kConfigRevision);
  }

  prefs.end();
  return config.valid;
}

void save_config(const AirTrafficConfig& config) {
  Preferences prefs;
  if (!prefs.begin("airtraffic", false)) return;
  prefs.putBool("valid", true);
  prefs.putDouble("lat", config.lat);
  prefs.putDouble("lon", config.lon);
  prefs.putInt("radius", config.radius_km);
  prefs.end();
}

bool connect_and_configure(AirTrafficConfig& config, bool force_portal) {
  WiFi.mode(WIFI_STA);
  WiFi.setSleep(false);

  // On every normal wake use the saved credentials directly. WiFiManager is
  // only needed for first-time setup/reconfiguration; keeping its web server
  // out of the heap leaves room for the three TLS endpoints (adsb.lol,
  // JetAPI/Planespotters and Weserv).
  if (!force_portal) {
    WiFi.begin();
    const uint32_t started = millis();
    while (WiFi.status() != WL_CONNECTED && millis() - started < 10000) {
      delay(200);
    }
    if (WiFi.status() == WL_CONNECTED) {
      Serial.printf("[air] Wi-Fi saved credentials, heap %u\n",
                    (unsigned)ESP.getFreeHeap());
      return true;
    }
    Serial.println("[air] saved Wi-Fi failed, trying WiFiManager");
  }

  char lat_buf[16];
  char lon_buf[16];
  char radius_buf[8];
  snprintf(lat_buf, sizeof(lat_buf), "%.5f", config.valid ? config.lat : 0.0);
  snprintf(lon_buf, sizeof(lon_buf), "%.5f", config.valid ? config.lon : 0.0);
  snprintf(radius_buf, sizeof(radius_buf), "%d", config.radius_km);

  WiFiManager manager;
  manager.setConfigPortalTimeout(AIR_TRAFFIC_PORTAL_TIMEOUT_S);
  manager.setTitle("EInk Air Traffic");
  manager.setDebugOutput(false);

  WiFiManagerParameter lat_param("lat", "Latitude", lat_buf, 15);
  WiFiManagerParameter lon_param("lon", "Longitude", lon_buf, 15);
  WiFiManagerParameter radius_param("radius", "Radius (km)", radius_buf, 7);
  manager.addParameter(&lat_param);
  manager.addParameter(&lon_param);
  manager.addParameter(&radius_param);

  Serial.printf("[air] Wi-Fi %s\n", force_portal ? "portal" : "connect");
  Serial.flush();

  // BOOT opens the portal without erasing the saved Wi-Fi network, so the
  // location can be changed without retyping the credentials.
  const bool connected = force_portal
                             ? manager.startConfigPortal(AIR_TRAFFIC_AP_NAME)
                             : manager.autoConnect(AIR_TRAFFIC_AP_NAME);
  if (!connected) {
    Serial.println("[air] Wi-Fi setup failed");
    return false;
  }

  const double lat = atof(lat_param.getValue());
  const double lon = atof(lon_param.getValue());
  const int radius = atoi(radius_param.getValue());
  if (!isfinite(lat) || !isfinite(lon) || fabs(lat) > 90.0 ||
      fabs(lon) > 180.0 || radius < 1 || radius > 200) {
    Serial.println("[air] invalid location/radius");
    return false;
  }

  // Avoid an NVS write on every 5-minute wake when nothing changed.
  const bool changed = !config.valid || fabs(config.lat - lat) > 1e-7 ||
                       fabs(config.lon - lon) > 1e-7 ||
                       config.radius_km != radius;
  config.valid = true;
  config.lat = lat;
  config.lon = lon;
  config.radius_km = radius;
  if (changed) save_config(config);
  Serial.printf("[air] location %.5f, %.5f radius %d km\n", lat, lon, radius);
  return true;
}

void render_setup_page(epd::Display& display) {
  display.firstPage();
  do {
    display.fillScreen(GxEPD_WHITE);
    display.fillRect(0, 0, epd::kWidth, 80, GxEPD_BLACK);
    display.setTextColor(GxEPD_WHITE);
    display.setTextSize(4);
    display.setCursor(24, 14);
    display.print("AIR TRAFFIC");
    display.setTextColor(GxEPD_YELLOW);
    display.setTextSize(2);
    display.setCursor(24, 52);
    display.print("SETUP");

    display.setTextColor(GxEPD_BLACK);
    display.setTextSize(2);
    display.setCursor(40, 118);
    display.print("1. Join Wi-Fi network");
    display.setTextSize(3);
    display.setTextColor(GxEPD_BLUE);
    display.setCursor(60, 150);
    display.print(AIR_TRAFFIC_AP_NAME);

    display.setTextColor(GxEPD_BLACK);
    display.setTextSize(2);
    display.setCursor(40, 220);
    display.print("2. Open http://192.168.4.1");
    display.setCursor(40, 268);
    display.print("3. Enter Wi-Fi, latitude,");
    display.setCursor(40, 296);
    display.print("   longitude and radius");
    display.setCursor(40, 344);
    display.print("4. Save. The panel will refresh.");

    display.fillRect(0, 416, epd::kWidth, 64, GxEPD_BLACK);
    display.setTextSize(1);
    display.setTextColor(GxEPD_WHITE);
    display.setCursor(16, 432);
    display.print("No Tesserae server. Direct adsb.lol feed.");
    display.setCursor(16, 450);
    display.print("Reconfigure: tap RESET, then hold BOOT 2 s.");
  } while (display.nextPage());
}

void render_error_page(epd::Display& display, const char* message) {
  display.firstPage();
  do {
    display.fillScreen(GxEPD_WHITE);
    display.fillRect(0, 0, epd::kWidth, 80, GxEPD_BLACK);
    display.setTextColor(GxEPD_WHITE);
    display.setTextSize(4);
    display.setCursor(24, 14);
    display.print("AIR TRAFFIC");
    display.setTextColor(GxEPD_RED);
    display.setTextSize(2);
    display.setCursor(24, 52);
    display.print("NO DATA");

    display.setTextColor(GxEPD_BLACK);
    display.setTextSize(2);
    display.setCursor(40, 140);
    display.print(message ? message : "Unknown error");
    display.setCursor(40, 200);
    display.print("Check Wi-Fi and location.");
    display.setCursor(40, 248);
    display.print("The next wake will retry.");

    display.fillRect(0, 416, epd::kWidth, 64, GxEPD_BLACK);
    display.setTextSize(1);
    display.setTextColor(GxEPD_WHITE);
    display.setCursor(16, 440);
    display.print("Direct adsb.lol feed");
  } while (display.nextPage());
}

// The adsb.lol response is an object with an "ac" array. Building an
// ArduinoJson document for the whole feed costs ~45 KB at 50 km and ~110 KB at
// 200 km, which does not fit next to the Wi-Fi/TLS buffers on this board.
// Scan the stream instead, parse one aircraft object at a time, and keep only
// the nearest AIR_TRAFFIC_MAX_AIRCRAFT. Peak memory is one object plus one
// small document, independent of the radius.
class AdsbFeedReader {
 public:
  explicit AdsbFeedReader(Stream& stream) : stream_(stream) {}

  bool parse(AirTrafficData& data, const AirTrafficConfig& config) {
    JsonDocument filter;
    filter["hex"] = true;
    filter["r"] = true;
    filter["flight"] = true;
    filter["t"] = true;
    filter["alt_baro"] = true;
    filter["baro_rate"] = true;
    filter["lat"] = true;
    filter["lon"] = true;
    filter["dst"] = true;
    filter["dir"] = true;

    JsonDocument doc;
    uint64_t now = 0;
    bool saw_ac = false;

    if (!skip_space() || get() != '{') return false;
    for (;;) {
      if (!skip_space()) return false;
      const int c = peek();
      if (c == '}') {
        get();
        break;
      }
      if (c == ',') {
        get();
        continue;
      }

      char key[16];
      if (!read_string(key, sizeof(key))) return false;
      if (!skip_space() || get() != ':') return false;

      if (strcmp(key, "ac") == 0) {
        saw_ac = true;
        if (!skip_space()) return false;
        if (peek() == '[') {
          if (!parse_aircraft_array(data, config, filter, doc)) return false;
        } else if (!skip_value()) {
          return false;
        }
      } else if (strcmp(key, "now") == 0) {
        if (!read_uint64(now)) return false;
      } else if (!skip_value()) {
        return false;
      }
    }

    data.feed_time =
        (uint32_t)(now > 100000000000ULL ? now / 1000ULL : now);
    return saw_ac;
  }

 private:
  int read_byte() {
    char c = 0;
    return stream_.readBytes(&c, 1) == 1 ? (int)(uint8_t)c : -1;
  }

  int get() {
    if (lookahead_ != -2) {
      const int c = lookahead_;
      lookahead_ = -2;
      return c;
    }
    return read_byte();
  }

  int peek() {
    if (lookahead_ == -2) lookahead_ = read_byte();
    return lookahead_;
  }

  bool skip_space() {
    for (;;) {
      const int c = peek();
      if (c == ' ' || c == '\t' || c == '\r' || c == '\n') {
        get();
        continue;
      }
      return c >= 0;
    }
  }

  bool read_string(char* out, size_t size) {
    if (!skip_space() || get() != '"') return false;
    size_t n = 0;
    for (;;) {
      int c = get();
      if (c < 0) return false;
      if (c == '"') break;
      if (c == '\\') {
        const int escaped = get();
        if (escaped < 0) return false;
        switch (escaped) {
          case '"':
            c = '"';
            break;
          case '\\':
            c = '\\';
            break;
          case '/':
            c = '/';
            break;
          case 'b':
            c = '\b';
            break;
          case 'f':
            c = '\f';
            break;
          case 'n':
            c = '\n';
            break;
          case 'r':
            c = '\r';
            break;
          case 't':
            c = '\t';
            break;
          case 'u': {
            uint32_t code = 0;
            for (int i = 0; i < 4; i++) {
              const int h = get();
              if (h < 0) return false;
              code <<= 4;
              if (h >= '0' && h <= '9') {
                code |= (uint32_t)(h - '0');
              } else if (h >= 'a' && h <= 'f') {
                code |= (uint32_t)(h - 'a' + 10);
              } else if (h >= 'A' && h <= 'F') {
                code |= (uint32_t)(h - 'A' + 10);
              } else {
                return false;
              }
            }
            c = code < 0x80 ? (int)code : '?';
            break;
          }
          default:
            return false;
        }
      }
      if (out && n + 1 < size) out[n++] = (char)c;
    }
    if (out && size > 0) out[n] = 0;
    return true;
  }

  bool skip_value() {
    if (!skip_space()) return false;
    const int first = peek();
    if (first == '"') return read_string(nullptr, 0);
    if (first == '{' || first == '[') {
      const char open = (char)get();
      const char close = open == '{' ? '}' : ']';
      int depth = 1;
      while (depth > 0) {
        const int c = get();
        if (c < 0) return false;
        if (c == '"') {
          for (;;) {
            const int s = get();
            if (s < 0) return false;
            if (s == '\\') {
              if (get() < 0) return false;
              continue;
            }
            if (s == '"') break;
          }
        } else if (c == open) {
          depth++;
        } else if (c == close) {
          depth--;
        }
      }
      return true;
    }
    int c = first;
    while (c >= 0 && c != ',' && c != '}' && c != ']' && c != ' ' &&
           c != '\t' && c != '\r' && c != '\n') {
      get();
      c = peek();
    }
    return true;
  }

  bool read_uint64(uint64_t& out) {
    if (!skip_space()) return false;
    uint64_t value = 0;
    bool any = false;
    int c = peek();
    while (c >= '0' && c <= '9') {
      value = value * 10 + (uint64_t)(c - '0');
      get();
      any = true;
      c = peek();
    }
    if (!any) return false;
    out = value;
    return true;
  }

  bool parse_aircraft_array(AirTrafficData& data,
                            const AirTrafficConfig& config,
                            JsonDocument& filter, JsonDocument& doc) {
    if (!skip_space() || get() != '[') return false;
    // An aircraft object is ~700 bytes in the current feed; keep enough room
    // for extra fields without putting the buffer on the 8 KB task stack.
    static char object[2048];
    for (;;) {
      if (!skip_space()) return false;
      const int c = peek();
      if (c == ']') {
        get();
        return true;
      }
      if (c == ',') {
        get();
        continue;
      }
      if (c == '{') {
        size_t length = 0;
        if (capture_object(object, sizeof(object), length)) {
          parse_aircraft(object, length, data, config, filter, doc);
        }
        continue;
      }
      if (!skip_value()) return false;
    }
  }

  bool capture_object(char* out, size_t capacity, size_t& length) {
    if (!skip_space() || peek() != '{') return false;
    size_t n = 0;
    int depth = 0;
    bool in_string = false;
    bool escaped = false;
    bool too_big = false;
    for (;;) {
      const int c = get();
      if (c < 0) return false;
      if (!too_big && n + 1 < capacity) {
        out[n++] = (char)c;
      } else {
        too_big = true;
      }
      if (in_string) {
        if (escaped) {
          escaped = false;
        } else if (c == '\\') {
          escaped = true;
        } else if (c == '"') {
          in_string = false;
        }
        continue;
      }
      if (c == '"') {
        in_string = true;
      } else if (c == '{') {
        depth++;
      } else if (c == '}') {
        depth--;
        if (depth == 0) break;
      }
    }
    out[n] = 0;
    length = n;
    return !too_big;
  }

  void parse_aircraft(const char* json, size_t length, AirTrafficData& data,
                      const AirTrafficConfig& config, JsonDocument& filter,
                      JsonDocument& doc) {
    doc.clear();
    if (deserializeJson(doc, json, length,
                        DeserializationOption::Filter(filter))) {
      return;
    }

    float km = 0.0f;
    float bearing = 0.0f;
    const float dst_nm = doc["dst"] | -1.0f;
    if (dst_nm >= 0.0f) {
      km = dst_nm * 1.852f;
      bearing = doc["dir"] | 0.0f;
    } else {
      const double lat = doc["lat"] | 0.0;
      const double lon = doc["lon"] | 0.0;
      if (lat == 0.0 && lon == 0.0) return;
      distance_bearing(config.lat, config.lon, lat, lon, km, bearing);
    }
    if (km > config.radius_km + 0.5f) return;
    data.total_within_radius++;

    AirTrafficAircraft out = {};
    const char* flight = doc["flight"] | "";
    if (flight && flight[0]) {
      strncpy(out.callsign, flight, sizeof(out.callsign) - 1);
      for (int i = (int)strlen(out.callsign) - 1;
           i >= 0 && out.callsign[i] == ' '; i--) {
        out.callsign[i] = 0;
      }
    }
    if (!out.callsign[0]) {
      const char* hex = doc["hex"] | "";
      size_t n = 0;
      for (; hex && hex[n] && n < sizeof(out.callsign) - 1; n++) {
        char c = hex[n];
        out.callsign[n] = (c >= 'a' && c <= 'z') ? (char)(c - 'a' + 'A') : c;
      }
      out.callsign[n] = 0;
    }

    const char* registration = doc["r"] | "";
    strncpy(out.registration, registration, sizeof(out.registration) - 1);

    const char* type = doc["t"] | "?";
    strncpy(out.type, (type && type[0]) ? type : "?", sizeof(out.type) - 1);
    fill_type_name(out.type, out.type_name, sizeof(out.type_name));

    JsonVariant altitude = doc["alt_baro"];
    if (altitude.is<const char*>()) {
      const char* value = altitude.as<const char*>();
      out.ground = value && strcmp(value, "ground") == 0;
      out.altitude_ft = -1;
    } else if (altitude.is<int>()) {
      out.altitude_ft = altitude.as<int>();
    } else if (altitude.is<float>()) {
      out.altitude_ft = (int)altitude.as<float>();
    }

    JsonVariant rate = doc["baro_rate"];
    if (rate.is<int>()) {
      out.vertical_rate_fpm = rate.as<int>();
    } else if (rate.is<float>()) {
      out.vertical_rate_fpm = (int)lroundf(rate.as<float>());
    }

    out.distance_km = (int)lroundf(km);
    out.bearing_deg = ((int)lroundf(bearing)) % 360;

    if (data.count < AIR_TRAFFIC_MAX_AIRCRAFT) {
      data.aircraft[data.count++] = out;
    } else {
      int worst = 0;
      for (int i = 1; i < data.count; i++) {
        if (data.aircraft[i].distance_km >
            data.aircraft[worst].distance_km) {
          worst = i;
        }
      }
      if (out.distance_km < data.aircraft[worst].distance_km) {
        data.aircraft[worst] = out;
      }
    }
  }

  Stream& stream_;
  int lookahead_ = -2;
};

bool fetch_traffic(AirTrafficData& data, const AirTrafficConfig& config) {
  memset(&data, 0, sizeof(data));
  data.lat = config.lat;
  data.lon = config.lon;
  data.radius_km = config.radius_km;
  data.ok = false;

  if (WiFi.status() != WL_CONNECTED) {
    snprintf(data.error, sizeof(data.error), "Wi-Fi not connected");
    return false;
  }

  const int nm = (int)ceilf(config.radius_km / 1.852f);
  char url[160];
  snprintf(url, sizeof(url),
           "https://api.adsb.lol/v2/lat/%.5f/lon/%.5f/dist/%d", config.lat,
           config.lon, nm);
  Serial.printf("[air] GET %s\n", url);
  Serial.flush();

  {
  WiFiClientSecure client;
  client.setInsecure();
  HTTPClient http;
  http.setTimeout(12000);
  http.setUserAgent("eink-color-display/1.0 (ESP32)");
  if (!http.begin(client, url)) {
    snprintf(data.error, sizeof(data.error), "HTTP begin failed");
    return false;
  }

  const int code = http.GET();
  if (code != 200) {
    snprintf(data.error, sizeof(data.error), "HTTP %d", code);
    http.end();
    return false;
  }

  const int body_size = http.getSize();
  Serial.printf("[air] payload size %d\n", body_size);
  Serial.flush();
  if (body_size <= 0) {
    snprintf(data.error, sizeof(data.error), "No body size");
    http.end();
    return false;
  }

  WiFiClient* stream = http.getStreamPtr();
  if (!stream) {
    snprintf(data.error, sizeof(data.error), "No HTTP stream");
    http.end();
    return false;
  }

  // Parse one aircraft object at a time so the heap cost does not grow with
  // the radius. The reader keeps only the nearest AIR_TRAFFIC_MAX_AIRCRAFT.
  AdsbFeedReader reader(*stream);
  const uint32_t parse_started = millis();
  const bool parsed = reader.parse(data, config);
  const uint32_t parse_ms = millis() - parse_started;
  http.end();
  client.stop();
  if (!parsed) {
    Serial.printf("[air] feed parse failed after %lu ms (payload %d, heap %u)\n",
                  (unsigned long)parse_ms, body_size,
                  (unsigned)ESP.getFreeHeap());
    snprintf(data.error, sizeof(data.error), "Bad feed");
    return false;
  }

  }

  std::sort(data.aircraft, data.aircraft + data.count,
            [](const AirTrafficAircraft& a, const AirTrafficAircraft& b) {
              return a.distance_km < b.distance_km;
            });
  data.ok = true;
  // Routes first while the heap is still roomy; photos need the large working
  // buffers and are fetched afterwards.
  for (uint8_t i = 0; i < data.count && i < AIR_TRAFFIC_PAGE_AIRCRAFT; i++) {
    resolve_route(data.aircraft[i], data.feed_time);
  }
  fetch_photos(data);
  Serial.printf("[air] %u aircraft within %d km (%u stored)\n",
                data.total_within_radius, config.radius_km, data.count);
  return true;
}

void sample_rgb332(const PhotoView& photo, int16_t dx, int16_t dy,
                   int16_t out_w, int16_t out_h, int& r, int& g, int& b) {
  const int32_t fx = ((int32_t)dx * photo.width << 8) / out_w;
  const int32_t fy = ((int32_t)dy * photo.height << 8) / out_h;
  const int16_t x0 = (int16_t)(fx >> 8);
  const int16_t y0 = (int16_t)(fy >> 8);
  int16_t x1 = x0 + 1;
  int16_t y1 = y0 + 1;
  if (x1 >= (int16_t)photo.width) x1 = (int16_t)photo.width - 1;
  if (y1 >= (int16_t)photo.height) y1 = (int16_t)photo.height - 1;
  const int32_t wx = fx & 0xFF;
  const int32_t wy = fy & 0xFF;

  const uint8_t* row0 = photo.pixels + (uint32_t)y0 * photo.width;
  const uint8_t* row1 = photo.pixels + (uint32_t)y1 * photo.width;
  int r00, g00, b00, r10, g10, b10;
  int r01, g01, b01, r11, g11, b11;
  rgb332_to_rgb(row0[x0], r00, g00, b00);
  rgb332_to_rgb(row0[x1], r10, g10, b10);
  rgb332_to_rgb(row1[x0], r01, g01, b01);
  rgb332_to_rgb(row1[x1], r11, g11, b11);

  const int32_t w00 = (256 - wx) * (256 - wy);
  const int32_t w10 = wx * (256 - wy);
  const int32_t w01 = (256 - wx) * wy;
  const int32_t w11 = wx * wy;
  r = (r00 * w00 + r10 * w10 + r01 * w01 + r11 * w11) >> 16;
  g = (g00 * w00 + g10 * w10 + g01 * w01 + g11 * w11) >> 16;
  b = (b00 * w00 + b10 * w10 + b01 * w01 + b11 * w11) >> 16;
}

void draw_photo(epd::Display& display, const PhotoView& photo, int16_t box_x,
                int16_t box_y, int16_t box_w, int16_t box_h, float scale,
                int16_t page_top, int16_t page_bottom) {
  if (!photo.pixels || photo.width == 0 || photo.height == 0 ||
      scale <= 0.0f) {
    return;
  }

  int16_t pw = box_w;
  int16_t ph = (int16_t)((int32_t)box_w * photo.height / photo.width);
  if (ph > box_h) {
    ph = box_h;
    pw = (int16_t)((int32_t)box_h * photo.width / photo.height);
  }
  pw = (int16_t)(pw * scale);
  ph = (int16_t)(ph * scale);
  if (pw < 2 || ph < 2) return;

  const int16_t px = box_x + (box_w - pw) / 2;
  const int16_t py = box_y + (box_h - ph) / 2;
  if (py >= page_bottom || py + ph <= page_top) return;

  display.drawRect(px - 1, py - 1, pw + 2, ph + 2, GxEPD_BLACK);

  int16_t start = page_top - py;
  if (start < 0) start = 0;
  int16_t end = page_bottom - py;
  if (end > ph) end = ph;

  if (photo.format == PhotoFormat::Packed3) {
    for (int16_t dy = start; dy < end; dy++) {
      const int16_t sy = (int16_t)((int32_t)dy * photo.height / ph);
      for (int16_t dx = 0; dx < pw; dx++) {
        const int16_t sx = (int16_t)((int32_t)dx * photo.width / pw);
        const uint32_t bit = ((uint32_t)sy * photo.width + sx) * 3;
        const uint32_t byte = bit >> 3;
        const uint8_t shift = (uint8_t)(bit & 7);
        uint32_t value = (uint32_t)photo.pixels[byte] |
                         ((uint32_t)photo.pixels[byte + 1] << 8) |
                         ((uint32_t)photo.pixels[byte + 2] << 16);
        uint8_t index = (uint8_t)((value >> shift) & 7);
        if (index > 5) index = 1;
        display.drawPixel(px + dx, py + dy, kPhotoColors[index]);
      }
    }
    return;
  }

  {
    // Sample the smooth RGB332 image with bilinear interpolation, then run
    // Floyd-Steinberg to the six inks at the final display resolution. This
    // avoids resampling an already-dithered bitmap and keeps the aircraft
    // edges clean while still mixing the limited palette.
    const size_t error_values = (size_t)pw * 3 * 2;
    int16_t* errors =
        (int16_t*)malloc(error_values * sizeof(int16_t));
    if (!errors) {
      for (int16_t dy = start; dy < end; dy++) {
        for (int16_t dx = 0; dx < pw; dx++) {
          int r, g, b;
          sample_rgb332(photo, dx, dy, pw, ph, r, g, b);
          const uint8_t index = photo_index_dither(
              (uint8_t)r, (uint8_t)g, (uint8_t)b, px + dx, py + dy);
          display.drawPixel(px + dx, py + dy, kPhotoColors[index]);
        }
      }
      return;
    }
    memset(errors, 0, error_values * sizeof(int16_t));
    int16_t* current = errors;
    int16_t* following = errors + (size_t)pw * 3;

    for (int16_t dy = 0; dy < ph; dy++) {
      for (int16_t dx = 0; dx < pw; dx++) {
        int r, g, b;
        sample_rgb332(photo, dx, dy, pw, ph, r, g, b);
        const size_t e = (size_t)dx * 3;
        int rr = r + current[e];
        int gg = g + current[e + 1];
        int bb = b + current[e + 2];
        if (rr < 0) rr = 0;
        if (rr > 255) rr = 255;
        if (gg < 0) gg = 0;
        if (gg > 255) gg = 255;
        if (bb < 0) bb = 0;
        if (bb > 255) bb = 255;

        const uint8_t index = photo_nearest_index(
            (uint8_t)rr, (uint8_t)gg, (uint8_t)bb);
        const int er = rr - kPhotoPalette[index][0];
        const int eg = gg - kPhotoPalette[index][1];
        const int eb = bb - kPhotoPalette[index][2];

        if (dx + 1 < pw) {
          current[e + 3] += (int16_t)(er * 7 / 16);
          current[e + 4] += (int16_t)(eg * 7 / 16);
          current[e + 5] += (int16_t)(eb * 7 / 16);
        }
        if (dy + 1 < ph) {
          if (dx > 0) {
            following[e - 3] += (int16_t)(er * 3 / 16);
            following[e - 2] += (int16_t)(eg * 3 / 16);
            following[e - 1] += (int16_t)(eb * 3 / 16);
          }
          following[e] += (int16_t)(er * 5 / 16);
          following[e + 1] += (int16_t)(eg * 5 / 16);
          following[e + 2] += (int16_t)(eb * 5 / 16);
          if (dx + 1 < pw) {
            following[e + 3] += (int16_t)(er * 1 / 16);
            following[e + 4] += (int16_t)(eg * 1 / 16);
            following[e + 5] += (int16_t)(eb * 1 / 16);
          }
        }

        if (dy >= start && dy < end) {
          display.drawPixel(px + dx, py + dy, kPhotoColors[index]);
        }
      }
      int16_t* swap = current;
      current = following;
      following = swap;
      memset(following, 0, (size_t)pw * 3 * sizeof(int16_t));
    }
    free(errors);
  }
}

struct PosterCard {
  int16_t x;
  int16_t y;
  int16_t w;
  int16_t h;
  uint8_t index;
  uint8_t kind;  // 0 = grid, 1 = single feature, 2 = feature row
};

uint8_t build_layout(const AirTrafficData& data, PosterCard* cards) {
  const uint8_t shown = data.count > AIR_TRAFFIC_PAGE_AIRCRAFT
                            ? AIR_TRAFFIC_PAGE_AIRCRAFT
                            : data.count;
  if (shown == 0) return 0;
  if (shown == 1) {
    // Give the single aircraft almost the whole body area.
    cards[0] = {20, 34, 760, 400, 0, 1};
    return 1;
  }
  if (shown == 2) {
    cards[0] = {20, 34, 760, 214, 0, 2};
    cards[1] = {20, 256, 760, 214, 1, 2};
    return 2;
  }

  const uint8_t rows = (shown + 1) / 2;
  const int16_t total_h = rows * kGridCardH + (rows - 1) * kGridGapY;
  const int16_t top = kBodyTop + (kBodyBottom - kBodyTop - total_h) / 2;
  for (uint8_t i = 0; i < shown; i++) {
    const uint8_t row = i / 2;
    const uint8_t col = i % 2;
    const bool last_odd = (shown & 1) && (i == shown - 1);
    const int16_t x = last_odd
                          ? (epd::kWidth - kGridCardW) / 2
                          : 16 + col * (kGridCardW + kGridGapX);
    const int16_t y = top + row * (kGridCardH + kGridGapY);
    cards[i] = {x, y, kGridCardW, kGridCardH, i, 0};
  }
  return shown;
}

void draw_card(epd::Display& display, const PosterCard& card,
               const AirTrafficAircraft& aircraft, int16_t page_top,
               int16_t page_bottom) {
  const PhotoCard* live_photo = nullptr;
  if (card.index < AIR_TRAFFIC_PAGE_AIRCRAFT) {
    live_photo = g_photos[card.index];
  }
  const PhotoView photo =
      (live_photo && live_photo->valid)
          ? PhotoView{live_photo->pixels, (uint16_t)kPhotoW,
                      (uint16_t)kPhotoH, PhotoFormat::Packed3}
          : fallback_photo_for_type(aircraft.type);

  char altitude[32];
  char last_line[48];
  format_altitude(aircraft, altitude, sizeof(altitude));
  format_last_line(aircraft, last_line, sizeof(last_line));

  if (card.kind == 1) {
    const int16_t box_x = card.x + 6;
    const int16_t box_y = card.y + 10;
    const int16_t box_w = card.w - 12;
    const int16_t box_h = 300;
    if (photo.pixels) {
      draw_photo(display, photo, box_x, box_y, box_w, box_h, 1.0f,
                 page_top, page_bottom);
    }

    draw_flight_and_type(display, card.x, card.y + 310, card.w,
                         aircraft.callsign, aircraft.type_name, 3, 2);
    draw_flight_details(display, card.x, card.y + 338, card.w, altitude,
                        last_line, 2);
  } else if (card.kind == 2) {
    const int16_t box_x = card.x + 10;
    const int16_t box_y = card.y + 8;
    const int16_t box_w = 450;
    const int16_t box_h = 190;
    if (photo.pixels) {
      draw_photo(display, photo, box_x, box_y, box_w, box_h, 1.0f,
                 page_top, page_bottom);
    }

    const int16_t text_x = card.x + 475;
    const int16_t text_w = card.w - 490;
    draw_flight_and_type(display, text_x, card.y + 84, text_w,
                         aircraft.callsign, aircraft.type_name, 3, 2);
    draw_flight_details(display, text_x, card.y + 116, text_w, altitude,
                        last_line, 1);
  } else {
    const int16_t box_x = card.x + 2;
    const int16_t box_y = card.y + 4;
    const int16_t box_w = card.w - 4;
    const int16_t box_h = 154;
    if (photo.pixels) {
      draw_photo(display, photo, box_x, box_y, box_w, box_h, 1.0f,
                 page_top, page_bottom);
    }

    draw_flight_and_type(display, card.x, card.y + 160, card.w,
                         aircraft.callsign, aircraft.type_name, 2, 1);
    draw_flight_details(display, card.x, card.y + 180, card.w, altitude,
                        last_line, 1);
  }
}

void draw_header(epd::Display& display, const AirTrafficData& data) {
  // N counts every aircraft inside the selected radius, not just the nearest
  // ones that fit on the poster.
  char title[80];
  snprintf(title, sizeof(title),
           "WITHIN %.0f KM - %u AIRCRAFT IN THE AIR NOW", data.radius_km,
           (unsigned)data.total_within_radius);
  display.setTextSize(2);
  display.setTextColor(GxEPD_BLACK);
  display.setCursor(18, kHeaderTitleY);
  display.print(title);

  char time_text[16];
  char date_text[16];
  format_time_date(data.feed_time, time_text, sizeof(time_text), date_text,
                   sizeof(date_text));
  char right[40];
  snprintf(right, sizeof(right), "%s - %s", time_text, date_text);
  draw_right(display, epd::kWidth - 18, kHeaderTimeY, right, 1,
             GxEPD_BLACK);

  display.drawFastHLine(18, kHeaderRuleY, epd::kWidth - 36, GxEPD_BLACK);
}

void draw_empty_page(epd::Display& display, const AirTrafficData& data) {
  char time_text[16];
  char date_text[16];
  format_time_date(data.feed_time, time_text, sizeof(time_text), date_text,
                   sizeof(date_text));
  draw_centered(display, 0, 210, epd::kWidth, "NO AIRCRAFT IN RANGE", 3,
                GxEPD_BLACK);
  char sub[64];
  snprintf(sub, sizeof(sub), "within %.0f km - %s", data.radius_km, time_text);
  draw_centered(display, 0, 250, epd::kWidth, sub, 2, GxEPD_BLACK);
}

void draw_poster_page(epd::Display& display, uint8_t page) {
  if (!g_traffic) return;
  const AirTrafficData& data = *g_traffic;
  const int16_t page_top = page * display.pageHeight();
  int16_t page_bottom = page_top + display.pageHeight();
  if (page_bottom > display.height()) page_bottom = display.height();

  if (intersects(0, kHeaderRuleY + 2, page_top, page_bottom)) {
    draw_header(display, data);
  }

  PosterCard cards[AIR_TRAFFIC_PAGE_AIRCRAFT];
  const uint8_t shown = build_layout(data, cards);
  if (shown == 0) {
    if (intersects(190, 280, page_top, page_bottom)) {
      draw_empty_page(display, data);
    }
  } else {
    for (uint8_t i = 0; i < shown; i++) {
      const PosterCard& card = cards[i];
      if (!intersects(card.y, card.y + card.h, page_top, page_bottom)) {
        continue;
      }
      draw_card(display, card, data.aircraft[card.index], page_top,
                page_bottom);
    }
  }
}

void render_traffic(epd::Display& display, const AirTrafficData& data) {
  g_traffic = &data;
  display.firstPage();
  uint8_t physical_page = 0;
  do {
#if EPD_ROTATE_180
    // The driver flips coordinates inside the page buffer, so draw the
    // logical pages in reverse order while the panel pages go top-to-bottom.
    const uint8_t page =
        (uint8_t)(display.pages() - 1 - physical_page);
#else
    const uint8_t page = physical_page;
#endif
    draw_poster_page(display, page);
    physical_page++;
  } while (display.nextPage());
}

#if AIR_TRAFFIC_DEMO
void fill_demo_data(AirTrafficData& data) {
  struct DemoAircraft {
    const char* callsign;
    const char* registration;
    const char* type;
    int altitude_ft;
    int vertical_rate_fpm;
    int distance_km;
    int bearing_deg;
    const char* route;
  };

  // Four aircraft from the Overhead reference print, with real registrations so
  // JetAPI can find a photo. The demo time is 2026-10-22 18:28 UTC.
  static const DemoAircraft kDemo[] = {
      {"VOZ858", "VH-VUH", "B738", 7200, -1200, 12, 210, "Sydney to Melbourne"},
      {"QFA693", "VH-XZP", "B738", 6000, 1400, 18, 160, "Melbourne to Adelaide"},
      {"VOZ875", "VH-YFC", "B738", 15450, 1800, 24, 190, "Melbourne to Sydney"},
      {"JST521", "VH-VQF", "A320", 2975, -900, 27, 130, "Sydney to Melbourne"},
      {"QFA476", "", "B738", 21750, 1600, 34, 250, "Perth to Melbourne"},
      {"QFA478", "", "B738", 10900, 1200, 41, 240, "Melbourne to Sydney"},
  };

  memset(&data, 0, sizeof(data));
  data.demo = true;
  data.ok = true;
  data.radius_km = AIR_TRAFFIC_RADIUS_KM;
  data.feed_time = 1792693680UL;  // 2026-10-22 18:28:00 UTC
  int count = AIR_TRAFFIC_DEMO_COUNT;
  if (count < 0) count = 0;
  if (count > (int)(sizeof(kDemo) / sizeof(kDemo[0]))) {
    count = (int)(sizeof(kDemo) / sizeof(kDemo[0]));
  }
  for (int i = 0; i < count; i++) {
    AirTrafficAircraft& out = data.aircraft[data.count++];
    snprintf(out.callsign, sizeof(out.callsign), "%s", kDemo[i].callsign);
    snprintf(out.registration, sizeof(out.registration), "%s",
             kDemo[i].registration);
    snprintf(out.type, sizeof(out.type), "%s", kDemo[i].type);
    fill_type_name(out.type, out.type_name, sizeof(out.type_name));
    out.altitude_ft = kDemo[i].altitude_ft;
    out.vertical_rate_fpm = kDemo[i].vertical_rate_fpm;
    out.distance_km = kDemo[i].distance_km;
    out.bearing_deg = kDemo[i].bearing_deg;
    snprintf(out.route, sizeof(out.route), "%s", kDemo[i].route);
  }
  data.total_within_radius = data.count;
}
#endif

void hibernate_and_sleep(epd::Display& display) {
  display.hibernate();
#if AIR_TRAFFIC_SLEEP
  Serial.printf("[air] deep sleep %d s\n", AIR_TRAFFIC_UPDATE_S);
  Serial.flush();
  esp_sleep_enable_timer_wakeup((uint64_t)AIR_TRAFFIC_UPDATE_S * 1000000ULL);
  esp_deep_sleep_start();
#endif
}

}  // namespace

void airTrafficRun(epd::Display& display) {
#if AIR_TRAFFIC_DEMO
  static AirTrafficData demo;
  fill_demo_data(demo);

  // The demo normally has no network path. Try the saved Wi-Fi credentials
  // briefly so the four cards can use real JetAPI photos; if that fails, the
  // bundled fallback photos are used.
  WiFi.mode(WIFI_STA);
  WiFi.begin();
  const uint32_t wifi_start = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - wifi_start < 10000) {
    delay(200);
  }
  if (WiFi.status() == WL_CONNECTED) {
    fetch_photos(demo);
    WiFi.disconnect(true);
  }

  Serial.printf("[air] demo feed: %u aircraft\n", demo.count);
  Serial.flush();
  const uint32_t demo_start = millis();
  render_traffic(display, demo);
  Serial.printf("[air] demo render total %lu ms (panel %lu ms)\n",
                (unsigned long)(millis() - demo_start),
                (unsigned long)display.epd2.lastRefreshMs());
  Serial.flush();
  display.hibernate();
  return;
#endif

  AirTrafficConfig config = {};
  config.radius_km = AIR_TRAFFIC_RADIUS_KM;
  load_config(config);
#if AIR_TRAFFIC_FORCE_RADIUS > 0
  if (config.valid && config.radius_km != AIR_TRAFFIC_FORCE_RADIUS) {
    config.radius_km = AIR_TRAFFIC_FORCE_RADIUS;
    save_config(config);
    Serial.printf("[air] radius migrated to %d km\n",
                  AIR_TRAFFIC_FORCE_RADIUS);
  }
#endif

  pinMode(0, INPUT_PULLUP);
  const bool boot_held = digitalRead(0) == LOW;
  const bool force_portal = !config.valid || boot_held;

  if (force_portal) {
    Serial.println("[air] setup portal");
    render_setup_page(display);
  }

  if (!connect_and_configure(config, force_portal)) {
    render_error_page(display, "Wi-Fi setup failed");
    hibernate_and_sleep(display);
    return;
  }

  // Keep the ~2.4 KB aircraft table out of the 8 KB loop-task stack; the Wi-Fi
  // manager and the TLS handshakes are stack-hungry.
  static AirTrafficData data;
  const bool fetched = fetch_traffic(data, config);
  // The radio is not needed while the panel refreshes (~26 s).
  // disconnect(true) already powers the interface down; calling
  // WiFi.mode(WIFI_OFF) as well triggers an "un-init" driver warning.
  WiFi.disconnect(true);
  Serial.printf("[air] free heap %u bytes\n", (unsigned)ESP.getFreeHeap());
  Serial.flush();
  if (fetched) {
    const uint32_t start = millis();
    render_traffic(display, data);
    Serial.printf("[air] render total %lu ms (panel %lu ms)\n",
                  (unsigned long)(millis() - start),
                  (unsigned long)display.epd2.lastRefreshMs());
  } else {
    Serial.printf("[air] fetch failed: %s\n", data.error);
    render_error_page(display, data.error);
  }
  Serial.flush();
  hibernate_and_sleep(display);
}
