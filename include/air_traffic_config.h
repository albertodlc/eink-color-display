#pragma once

// Standalone air-traffic page: no Tesserae server, no AI paintings/photos.
// The ESP32 queries the public adsb.lol feed directly and draws a radar + list.
//
// First boot (or BOOT held) starts a local setup portal:
//   AP: EInk-AirTraffic
//   URL: http://192.168.4.1
// Enter Wi-Fi, latitude, longitude and radius there. Values are stored in NVS.

// Compile-time demo feed: renders sample aircraft from the Overhead reference
// instead of contacting Wi-Fi/adsb.lol. Enable with -DAIR_TRAFFIC_DEMO=1.
#ifndef AIR_TRAFFIC_DEMO
#define AIR_TRAFFIC_DEMO 0
#endif
#ifndef AIR_TRAFFIC_DEMO_COUNT
#define AIR_TRAFFIC_DEMO_COUNT 4  // 1-4; exercises the adaptive layouts
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
#define AIR_TRAFFIC_MAX_AIRCRAFT 10
#define AIR_TRAFFIC_PAGE_AIRCRAFT 4  // aircraft visible on the print page
