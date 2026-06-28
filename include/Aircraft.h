// Compact aircraft struct optimised for ESP32 RAM constraints.
// We only keep what the 128x64 OLED actually shows.

#pragma once
#include <Arduino.h>

struct Aircraft {
  String callsign;   // e.g. "QFA9"
  String type;       // e.g. "B789" (from adsb.lol)
  String reg;        // tail number, e.g. "VH-ZNJ" (from adsb.lol)
  float lat = 0;
  float lon = 0;
  int alt_ft = 0;          // baro altitude
  int speed_kt = 0;
  int heading_deg = 0;
  bool on_ground = false;
  float dist_km = 0;       // populated by caller after fetch

  // --- AeroAPI enrichment (optional; empty until we look it up) ---
  // We only fill these in if (a) AeroAPI is enabled, (b) we have budget,
  // and (c) the callsign isn't already cached.
  String origin;     // ICAO origin airport, e.g. "YPPH"
  String dest;       // ICAO destination, e.g. "YSSY"
  String airline;    // short airline name, e.g. "Qantas"
  bool   enriched = false;   // true once AeroAPI returned data for this callsign
};
