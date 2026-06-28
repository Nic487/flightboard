// adsb.lol HTTPS client.
//
// adsb.lol is free, no key, no rate limits. It uses CORS-friendly headers
// but for an ESP32 we just use HTTPClient directly over TLS.
//
// Endpoints we hit:
//   GET /v2/point/{lat}/{lon}/{nm}  -- aircraft within N nautical miles
//   GET /v2/callsign/{cs}            -- look up a specific callsign

#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include "Aircraft.h"
#include "CallsignMap.h"

// adsb.lol doesn't require pinning, and pinning would mean shipping a cert
// that we'd have to rotate every year. setInsecure() is fine here -- the
// response is public data, the worst-case attacker shows us a fake plane.
static WiFiClientSecure g_tlsClient;
static bool g_tlsInitialised = false;

static void ensureTLS() {
  if (g_tlsInitialised) return;
  g_tlsClient.setInsecure();
  g_tlsInitialised = true;
}

// Compute great-circle distance in km between two points.
static float distKm(float lat1, float lon1, float lat2, float lon2) {
  const float R = 6371.0f;
  float dLat = (lat2 - lat1) * PI / 180.0f;
  float dLon = (lon2 - lon1) * PI / 180.0f;
  float a = sin(dLat / 2) * sin(dLat / 2) +
            cos(lat1 * PI / 180.0f) * cos(lat2 * PI / 180.0f) *
            sin(dLon / 2) * sin(dLon / 2);
  return R * 2 * atan2(sqrt(a), sqrt(1 - a));
}

// Fill an Aircraft from a single adsb.lol JSON object.
static void fillFromJson(JsonObjectConst o, Aircraft& a) {
  a.callsign = (const char*)(o["flight"] | "");
  a.callsign.trim();
  a.type = (const char*)(o["t"] | "");
  a.reg = (const char*)(o["r"] | "");
  a.lat = o["lat"] | 0.0f;
  a.lon = o["lon"] | 0.0f;
  const char* altStr = o["alt_baro"] | "";
  if (strcmp(altStr, "ground") == 0) { a.alt_ft = 0; a.on_ground = true; }
  else { a.alt_ft = o["alt_baro"] | 0; a.on_ground = false; }
  a.speed_kt = (int)(o["gs"] | 0.0f);
  a.heading_deg = (int)(o["track"] | 0.0f);
}

// Fetch the closest aircraft within `radiusKm` of (centerLat, centerLon).
// Returns the number filled into `out` (max `maxOut`), or -1 on error.
int fetchNearby(float centerLat, float centerLon, float radiusKm,
                Aircraft out[], int maxOut) {
  if (WiFi.status() != WL_CONNECTED) return -1;
  ensureTLS();

  // adsb.lol uses nautical miles, max 250
  int radiusNm = (int)min(250.0f, radiusKm / 1.852f);
  if (radiusNm < 1) radiusNm = 1;

  char url[160];
  snprintf(url, sizeof(url),
           "https://api.adsb.lol/v2/point/%.4f/%.4f/%d",
           centerLat, centerLon, radiusNm);

  HTTPClient http;
  http.setTimeout(8000);
  http.setUserAgent("flightboard-esp32/1.0");
  if (!http.begin(g_tlsClient, url)) { return -1; }
  int code = http.GET();
  if (code != 200) {
    log_w("adsb.lol nearby HTTP %d", code);
    http.end();
    return -1;
  }

  // 28 KB is enough for ~30 aircraft; larger payloads get truncated to maxOut anyway
  JsonDocument doc;
  DeserializationError err = deserializeJson(doc, http.getStream());
  http.end();
  if (err) { log_w("JSON parse: %s", err.c_str()); return -1; }

  JsonArrayConst ac = doc["ac"].as<JsonArrayConst>();
  int filled = 0;

  // First pass: parse and compute distances
  struct ParsedAc { Aircraft a; float dist; };
  static ParsedAc buf[40];
  int parsedCount = 0;
  for (JsonObjectConst o : ac) {
    if (parsedCount >= 40) break;
    Aircraft a;
    fillFromJson(o, a);
    if (a.lat == 0 && a.lon == 0) continue;
    a.dist_km = distKm(centerLat, centerLon, a.lat, a.lon);
    buf[parsedCount].a = a;
    buf[parsedCount].dist = a.dist_km;
    parsedCount++;
  }

  // Simple insertion sort by distance (parsedCount is tiny so this is fine)
  for (int i = 1; i < parsedCount; ++i) {
    ParsedAc key = buf[i];
    int j = i - 1;
    while (j >= 0 && buf[j].dist > key.dist) { buf[j + 1] = buf[j]; --j; }
    buf[j + 1] = key;
  }

  for (int i = 0; i < parsedCount && filled < maxOut; ++i) {
    out[filled++] = buf[i].a;
  }
  return filled;
}

// Single-flight lookup by callsign with IATA->ICAO expansion.
// Returns true if found and `out` is populated.
bool fetchByCallsign(const String& userInput, Aircraft& out) {
  if (WiFi.status() != WL_CONNECTED) return false;
  ensureTLS();

  String variants[4];
  int n = expandCallsign(userInput, variants);
  if (n == 0) return false;

  for (int i = 0; i < n; ++i) {
    char url[140];
    snprintf(url, sizeof(url),
             "https://api.adsb.lol/v2/callsign/%s", variants[i].c_str());

    HTTPClient http;
    http.setTimeout(8000);
    http.setUserAgent("flightboard-esp32/1.0");
    if (!http.begin(g_tlsClient, url)) continue;
    int code = http.GET();
    if (code != 200) { http.end(); continue; }

    JsonDocument doc;
    DeserializationError err = deserializeJson(doc, http.getStream());
    http.end();
    if (err) continue;
    JsonArrayConst ac = doc["ac"].as<JsonArrayConst>();
    if (ac.size() == 0) continue;

    fillFromJson(ac[0], out);
    return true;
  }
  return false;
}
