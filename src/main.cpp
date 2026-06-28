// FlightBoard ESP32 firmware - free-first with optional AeroAPI enrichment.
//
// Cooperative loop:
//   - service the config HTTP server every iteration
//   - poll adsb.lol every POLL_INTERVAL_MS (free, primary source)
//   - enrich up to MAX_FRESH_ENRICH_PER_POLL new callsigns from AeroAPI
//     (skipped entirely if AeroAPI is off or budget is exhausted)
//   - render every RENDER_INTERVAL_MS (cheap, mainly drives ticker scroll)

#include <Arduino.h>
#include <WiFi.h>
#include "Aircraft.h"

// --- Display ---
extern void displayBegin();
extern void displayBoot(const char* status);
extern void displayAircraft(const Aircraft& a, const char* title);
extern void displayEmpty(const char* line1, const char* line2);
extern void displayTicker(const Aircraft list[], int count,
                          const char* header, int scrollPxPerStep);

// --- Config / settings ---
extern bool   startConfigPortal();
extern void   serviceHttp();
extern const  String& settingsMode();
extern const  String& settingsDisplay();
extern const  String& settingsTrackedFlight();
extern float  settingsLat();
extern float  settingsLon();
extern float  settingsRadiusKm();
extern int    settingsRotateMs();
extern int    settingsTickerSpeed();
extern bool   settingsAeroApiEnabled();

// --- Data sources ---
extern int  fetchNearby(float lat, float lon, float radiusKm,
                        Aircraft out[], int maxOut);
extern bool fetchByCallsign(const String& userInput, Aircraft& out);

// --- AeroAPI enrichment ---
extern int  enrichAircraft(Aircraft& a);                       // -1 / 0 / 1
extern int  enrichBatch(Aircraft list[], int count, int maxNewCalls);

// --- Tuning ---------------------------------------------------------------
static const int          MAX_NEARBY                = 10;
static const unsigned long POLL_INTERVAL_MS         = 8000;
static const int          MAX_FRESH_ENRICH_PER_POLL = 2;
// Why 2? Even at one poll every 8 seconds, 2 fresh AeroAPI calls per poll
// is 900/hour worst case -- but cache hits and the per-hour budget gate
// pull this back hard. In normal operation you'll see 5-15 calls/hr.
static const unsigned long RENDER_INTERVAL_MS       = 50;
// Faster than before (was 80ms) so the ticker scrolls smoothly.

// --- State ---------------------------------------------------------------
static Aircraft   g_nearby[MAX_NEARBY];
static int        g_nearbyCount = 0;
static Aircraft   g_tracked;
static bool       g_trackedFound = false;
static int        g_carouselIdx = 0;

static unsigned long g_lastFetchMs  = 0;
static unsigned long g_lastRotateMs = 0;
static unsigned long g_lastRenderMs = 0;

void setup() {
  Serial.begin(115200);
  delay(200);
  Serial.println("\n=== FlightBoard ESP32 ===");

  displayBegin();
  if (!startConfigPortal()) {
    while (true) { delay(1000); }
  }

  g_lastFetchMs = millis() - POLL_INTERVAL_MS;   // force immediate poll
  displayBoot("Loading flights...");
}

// Free-first: hit adsb.lol, then enrich just a few uncached callsigns
// from AeroAPI if it's enabled and we have budget.
static void doPoll() {
  String mode = settingsMode();

  if (mode == "flight") {
    if (settingsTrackedFlight().length() == 0) {
      g_trackedFound = false;
      return;
    }
    g_trackedFound = fetchByCallsign(settingsTrackedFlight(), g_tracked);
    // For the tracked flight we're happy to spend 1 call to enrich it -- the
    // user explicitly picked it, so the enrichment is worth ~$0.005 of detail.
    if (g_trackedFound && settingsAeroApiEnabled()) {
      enrichAircraft(g_tracked);
    }
    return;
  }

  g_nearbyCount = fetchNearby(settingsLat(), settingsLon(),
                              settingsRadiusKm(), g_nearby, MAX_NEARBY);
  if (g_nearbyCount < 0) g_nearbyCount = 0;
  if (g_carouselIdx >= g_nearbyCount) g_carouselIdx = 0;

  // Batch-enrich, but cap fresh HTTP calls to stay cheap.
  if (g_nearbyCount > 0 && settingsAeroApiEnabled()) {
    enrichBatch(g_nearby, g_nearbyCount, MAX_FRESH_ENRICH_PER_POLL);
  }
}

static void renderCurrent() {
  String mode    = settingsMode();
  String display = settingsDisplay();

  // -------- Flight tracking mode --------
  if (mode == "flight") {
    if (!g_trackedFound) {
      displayEmpty(settingsTrackedFlight().length() ? settingsTrackedFlight().c_str() : "No flight set",
                   "Not airborne / no coverage");
      return;
    }
    if (display == "ticker") {
      // Single-flight ticker: still scroll, but only one entry.
      displayTicker(&g_tracked, 1, "TRACKING", settingsTickerSpeed());
    } else {
      displayAircraft(g_tracked, "TRACKING");
    }
    return;
  }

  // -------- Nearby mode --------
  if (g_nearbyCount == 0) {
    char line2[40];
    snprintf(line2, sizeof(line2), "within %.0f km", settingsRadiusKm());
    displayEmpty("No aircraft", line2);
    return;
  }

  if (display == "ticker") {
    char header[24];
    snprintf(header, sizeof(header), "NEARBY %.0fkm", settingsRadiusKm());
    displayTicker(g_nearby, g_nearbyCount, header, settingsTickerSpeed());
  } else {
    char title[24];
    snprintf(title, sizeof(title), "NEARBY %d/%d",
             g_carouselIdx + 1, g_nearbyCount);
    displayAircraft(g_nearby[g_carouselIdx], title);
  }
}

void loop() {
  serviceHttp();

  unsigned long now = millis();

  // 1. Poll data sources
  if (now - g_lastFetchMs >= POLL_INTERVAL_MS) {
    g_lastFetchMs = now;
    doPoll();
    g_lastRenderMs = 0;       // force re-render right after a fetch
  }

  // 2. Rotate carousel (card display, nearby mode only)
  if (settingsMode() != "flight" && settingsDisplay() == "card" &&
      g_nearbyCount > 1 &&
      (now - g_lastRotateMs) >= (unsigned long)settingsRotateMs()) {
    g_lastRotateMs = now;
    g_carouselIdx = (g_carouselIdx + 1) % g_nearbyCount;
    g_lastRenderMs = 0;
  }

  // 3. Re-render (drives ticker scroll & marquee callsign)
  if (now - g_lastRenderMs >= RENDER_INTERVAL_MS) {
    g_lastRenderMs = now;
    renderCurrent();
  }

  delay(2);
}
