#pragma once

// Standalone air-traffic page: no Tesserae server, no AI paintings/photos.
// The ESP32 queries the public adsb.lol feed directly and draws a radar + list.
//
// First boot (or BOOT held) starts a local setup portal:
//   AP: EInk-AirTraffic
//   URL: http://192.168.4.1
// Enter Wi-Fi, latitude, longitude and radius there. Values are stored in NVS.

// Display mode. Set with -DAIR_TRAFFIC_MODE=<n>:
//   0 = auto live feed, up to 4 aircraft
//   1 = demo feed, AIR_TRAFFIC_DEMO_COUNT aircraft
//   2 = one aircraft with extra information
//   3 = two aircraft
//   4 = three aircraft
//   5 = five aircraft
//   6 = map centred on the configured location and radius
#ifndef AIR_TRAFFIC_MODE
// A legacy -DAIR_TRAFFIC_DEMO=1 build maps to mode 1.
#  if defined(AIR_TRAFFIC_DEMO) && AIR_TRAFFIC_DEMO
#    define AIR_TRAFFIC_MODE 1
#  else
#    define AIR_TRAFFIC_MODE 0
#  endif
#endif
#if AIR_TRAFFIC_MODE < 0 || AIR_TRAFFIC_MODE > 6
#  error "AIR_TRAFFIC_MODE must be 0 (auto), 1 (demo), 2 (one), 3 (two), 4 (three), 5 (five) or 6 (map)"
#endif

// Cycle the displayed view after every refresh. The initial mode is shown on
// power-on, then the sequence runs; after the last sequence entry the display
// returns to the initial mode and repeats. Override the sequence with
// -DAIR_TRAFFIC_CYCLE_SEQUENCE=2,6,3,4,5 and the start view with
// -DAIR_TRAFFIC_CYCLE_INITIAL_MODE=0. Mode 1 is the demo feed and is not in the
// default sequence.
#ifndef AIR_TRAFFIC_CYCLE_MODES
#  define AIR_TRAFFIC_CYCLE_MODES 1
#endif
#ifndef AIR_TRAFFIC_CYCLE_INITIAL_MODE
#  define AIR_TRAFFIC_CYCLE_INITIAL_MODE AIR_TRAFFIC_MODE
#endif
#ifndef AIR_TRAFFIC_CYCLE_SEQUENCE
#  define AIR_TRAFFIC_CYCLE_SEQUENCE 2, 6, 3, 4, 5
#endif
#if AIR_TRAFFIC_CYCLE_INITIAL_MODE < 0 || AIR_TRAFFIC_CYCLE_INITIAL_MODE > 6
#  error "AIR_TRAFFIC_CYCLE_INITIAL_MODE must be 0..6"
#endif
#if AIR_TRAFFIC_CYCLE_MODES
#  define AIR_TRAFFIC_ALL_MODES 1
#else
#  define AIR_TRAFFIC_ALL_MODES 0
#endif
#undef AIR_TRAFFIC_DEMO
#if AIR_TRAFFIC_MODE == 1
#  define AIR_TRAFFIC_DEMO 1
#else
#  define AIR_TRAFFIC_DEMO 0
#endif
#ifndef AIR_TRAFFIC_DEMO_COUNT
#define AIR_TRAFFIC_DEMO_COUNT 4  // 1-5; exercises the adaptive layouts
#endif
// One-time migration helper: set to a km value to write it into NVS at boot.
#ifndef AIR_TRAFFIC_FORCE_RADIUS
#define AIR_TRAFFIC_FORCE_RADIUS 0
#endif

#define AIR_TRAFFIC_AP_NAME "EInk-AirTraffic"
#define AIR_TRAFFIC_PORTAL_TIMEOUT_S 600
// Default radius. The setup portal still allows 1-200 km.
#define AIR_TRAFFIC_RADIUS_KM 100
#define AIR_TRAFFIC_UPDATE_S 300  // >= 180 s vendor recommendation
#define AIR_TRAFFIC_SLEEP 1       // deep sleep between updates
#if AIR_TRAFFIC_ALL_MODES || AIR_TRAFFIC_MODE == 6
#  define AIR_TRAFFIC_MAX_AIRCRAFT 40  // map view has room for more markers
#else
#  define AIR_TRAFFIC_MAX_AIRCRAFT 10
#endif
#define AIR_TRAFFIC_PAGE_AIRCRAFT 5  // maximum cards any mode can draw

#if AIR_TRAFFIC_ALL_MODES || AIR_TRAFFIC_DEMO
#  define AIR_TRAFFIC_INCLUDE_DEMO 1
#else
#  define AIR_TRAFFIC_INCLUDE_DEMO 0
#endif
