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
#if AIR_TRAFFIC_MODE == 6
#  define AIR_TRAFFIC_MAX_AIRCRAFT 40  // map view has room for more markers
#else
#  define AIR_TRAFFIC_MAX_AIRCRAFT 10
#endif
#define AIR_TRAFFIC_PAGE_AIRCRAFT 5  // maximum cards any mode can draw

#if AIR_TRAFFIC_MODE == 2
#  define AIR_TRAFFIC_ACTIVE_CARDS 1
#elif AIR_TRAFFIC_MODE == 3
#  define AIR_TRAFFIC_ACTIVE_CARDS 2
#elif AIR_TRAFFIC_MODE == 4
#  define AIR_TRAFFIC_ACTIVE_CARDS 3
#elif AIR_TRAFFIC_MODE == 5
#  define AIR_TRAFFIC_ACTIVE_CARDS 5
#elif AIR_TRAFFIC_MODE == 6
#  define AIR_TRAFFIC_ACTIVE_CARDS 1  // map mode does not fetch cards
#elif AIR_TRAFFIC_MODE == 1
#  define AIR_TRAFFIC_ACTIVE_CARDS AIR_TRAFFIC_DEMO_COUNT
#else
#  define AIR_TRAFFIC_ACTIVE_CARDS 4
#endif

// The poster arrays and the photo-card pool are sized for PAGE_AIRCRAFT.
// Clamp a larger demo count instead of letting it index past the arrays.
#if AIR_TRAFFIC_ACTIVE_CARDS > AIR_TRAFFIC_PAGE_AIRCRAFT
#  warning "AIR_TRAFFIC_ACTIVE_CARDS exceeds AIR_TRAFFIC_PAGE_AIRCRAFT; clamping"
#  undef AIR_TRAFFIC_ACTIVE_CARDS
#  define AIR_TRAFFIC_ACTIVE_CARDS AIR_TRAFFIC_PAGE_AIRCRAFT
#endif
#if AIR_TRAFFIC_ACTIVE_CARDS < 1
#  undef AIR_TRAFFIC_ACTIVE_CARDS
#  define AIR_TRAFFIC_ACTIVE_CARDS 1
#endif
