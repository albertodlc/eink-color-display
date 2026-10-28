#pragma once

#include <Arduino.h>

#include "air_traffic_config.h"
#include "epd_config.h"

struct AirTrafficAircraft {
  char callsign[10];
  char registration[10];
  char type[6];
  char type_name[24];
  int altitude_ft;       // -1 when unknown
  int vertical_rate_fpm; // negative = descending
  int distance_km;
  int bearing_deg;
  int heading_deg;      // ground track from the feed, -1 when unknown
  char route[40];  // "Dublin to Milan" from adsbdb, empty if unknown
  bool ground;
};

struct AirTrafficData {
  AirTrafficAircraft aircraft[AIR_TRAFFIC_MAX_AIRCRAFT];
  uint8_t count;               // stored and sorted, capped at MAX_AIRCRAFT
  uint16_t total_within_radius;  // may be larger than count
  float radius_km;
  double lat;
  double lon;
  uint32_t feed_time;  // epoch seconds from adsb.lol
  bool ok;
  bool demo;  // true when the built-in sample feed is being rendered
  char error[48];
};

// Full standalone cycle: load/setup config, connect Wi-Fi, fetch adsb.lol,
// render one air-traffic page, hibernate and optionally deep sleep.
void airTrafficRun(epd::Display& display);
