// FlightAware AeroAPI enrichment client with cost controls.
//
// Philosophy: adsb.lol is free and gives us position data. AeroAPI is paid
// ($0.005 per /flights/{ident} call). So we use AeroAPI as a sparing
// *enrichment* layer, never as the primary source.
//
// FOUR layers of cost control:
//
//   1. Per-callsign cache (TTL 6h). Once we've enriched QFA9, we don't ask
//      again for 6 hours -- airline/aircraft type don't change mid-flight.
//
//   2. Rolling hourly budget. If you'd exceed `max_calls_per_hour` in the
//      last 60min, we skip the call. Default 20/hr.
//
//   3. *** HARD MONTHLY SPEND CAP. *** Default $5/month = 1000 calls. Counter
//      is persisted to NVS keyed by year-month, so reboots can't reset it,
//      and the counter auto-resets at the start of each new calendar month
//      (determined by NTP-synced time). Every call is rejected before HTTP
//      once this cap is reached. This is the cap the user actually cares
//      about -- everything else is preventative.
//
//   4. Opt-in. If no key or `aeroapi_enabled = false`, this module is a no-op.

#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <Preferences.h>
#include <time.h>
#include "Aircraft.h"
#include "AirportMap.h"

// Settings accessors (defined in ConfigPortal.cpp)
extern const String& settingsAeroApiKey();
extern bool          settingsAeroApiEnabled();
extern int           settingsAeroApiCallsPerHour();
extern int           settingsAeroApiMonthlyCapCents();

// Per-call cost in cents (AeroAPI /flights/{ident} = $0.005 = 0.5 cents).
// Stored as a fixed-point integer of tenths-of-cents so 0.5c works cleanly.
static const int CALL_COST_TENTHS_CENT = 5;     // 0.5 cents = 5 tenths

// --- Cache ---------------------------------------------------------------
//
// 32 slots * ~80 bytes each = 2.5KB RAM. Plenty for typical operation
// (you rarely see more than 8 distinct callsigns within a 50km radius).
//
struct CacheEntry {
  String   callsign;
  String   origin;
  String   dest;
  String   airline;
  String   type;
  unsigned long fetchedMs;
  bool     valid = false;
};

static const int CACHE_SIZE = 32;
static const unsigned long CACHE_TTL_MS = 6UL * 60UL * 60UL * 1000UL;  // 6 hours
static CacheEntry g_cache[CACHE_SIZE];

// --- Budget --------------------------------------------------------------
//
// Ring buffer of call timestamps. To check budget: count timestamps within
// the last 60 minutes; if >= cap, deny. Cheap, no map/list overhead.
//
static const int CALL_LOG_SIZE = 60;            // ample for any sane cap
static unsigned long g_callLog[CALL_LOG_SIZE];
static int g_callLogHead = 0;

static int callsInLastHour() {
  unsigned long now = millis();
  const unsigned long ONE_HOUR_MS = 60UL * 60UL * 1000UL;
  int count = 0;
  for (int i = 0; i < CALL_LOG_SIZE; ++i) {
    if (g_callLog[i] == 0) continue;
    // millis() wraps every ~49 days; in that rare case timestamps wrap too --
    // treat wrapped-past entries as expired.
    if (g_callLog[i] > now) { g_callLog[i] = 0; continue; }
    // BUG FIX: in the first hour after boot, now < ONE_HOUR_MS and the old
    // code did (now - ONE_HOUR_MS) which underflowed an unsigned long to a
    // huge number, so NO recorded call ever counted. Result: "0/20 this
    // hour" forever, even when calls were firing.
    //
    // Correct framing: a call counts if (now - call_time) <= 1 hour.
    if ((now - g_callLog[i]) <= ONE_HOUR_MS) count++;
  }
  return count;
}

static void recordCall() {
  g_callLog[g_callLogHead] = millis();
  g_callLogHead = (g_callLogHead + 1) % CALL_LOG_SIZE;
}

// Public: how many AeroAPI calls in the last hour (for the status page).
int aeroApiCallsThisHour() { return callsInLastHour(); }

// --- Monthly hard spend cap ---------------------------------------------
//
// Persisted to NVS so it survives reboots, brownouts, and crashes mid-month.
// We store two values: the year-month tag (e.g. 202606) and the count of
// calls made in that month. If the device boots into a different month than
// what's stored, we reset the counter automatically.
//
// Time comes from NTP (configured at WiFi connect). If NTP isn't yet ready
// the first time we check, we get back an obviously-wrong year (1970) -- we
// treat that as "don't reset, but also don't allow new calls" so a clock
// failure can never silently nuke the counter or open the gate.

static Preferences  g_faPrefs;
static bool         g_faPrefsReady = false;
static uint32_t     g_monthTag = 0;      // YYYYMM
static uint32_t     g_monthCount = 0;    // calls so far this month

static void ensureFaPrefs() {
  if (g_faPrefsReady) return;
  g_faPrefs.begin("fa", false);
  g_faPrefsReady = true;
  g_monthTag   = g_faPrefs.getUInt("mtag",  0);
  g_monthCount = g_faPrefs.getUInt("mcount", 0);
}

// Returns 0 if the clock isn't trustworthy yet (NTP not synced).
static uint32_t currentMonthTag() {
  time_t now = time(nullptr);
  if (now < 1700000000) return 0;   // before 2023 means NTP hasn't fired
  struct tm tm;
  localtime_r(&now, &tm);
  return (uint32_t)(tm.tm_year + 1900) * 100 + (uint32_t)(tm.tm_mon + 1);
}

// Roll the counter if we've crossed into a new month.
static void rolloverIfNeeded() {
  ensureFaPrefs();
  uint32_t tag = currentMonthTag();
  if (tag == 0) return;             // clock not ready; leave counter as-is
  if (tag != g_monthTag) {
    g_monthTag   = tag;
    g_monthCount = 0;
    g_faPrefs.putUInt("mtag",   g_monthTag);
    g_faPrefs.putUInt("mcount", g_monthCount);
    log_i("AeroAPI: new month %lu, counter reset", (unsigned long)tag);
  }
}

// How many cents we've spent this month.
int aeroApiSpendThisMonthCents() {
  ensureFaPrefs();
  rolloverIfNeeded();
  // tenths-of-cent * count -> cents (round up to be conservative on display)
  uint32_t tenths = (uint32_t)CALL_COST_TENTHS_CENT * g_monthCount;
  return (int)((tenths + 9) / 10);
}

// How many calls we've made this month.
int aeroApiCallsThisMonth() {
  ensureFaPrefs();
  rolloverIfNeeded();
  return (int)g_monthCount;
}

// Would the next call exceed the user's configured monthly cap?
// Returns true if blocked.
static bool monthlyCapExhausted() {
  ensureFaPrefs();
  rolloverIfNeeded();

  // If clock isn't yet trustworthy, refuse to spend money. The user would
  // rather see bare data than have us blow the cap because we lost track
  // of what month it is.
  if (g_monthTag == 0) {
    log_w("AeroAPI: clock not ready, refusing to spend until NTP syncs");
    return true;
  }

  int capCents = settingsAeroApiMonthlyCapCents();
  if (capCents <= 0) return true;   // cap of 0 = disable spending entirely

  // Convert cap to whole calls. We round DOWN -- err on the side of staying
  // under, never over. $5.00 cap / $0.005 per call = 1000 calls exactly.
  // The check is `>=`, so a cap of 1000 allows calls #1..#1000 (spending
  // exactly $5.00) and blocks #1001+.
  uint32_t maxCalls = ((uint32_t)capCents * 10) / CALL_COST_TENTHS_CENT;
  return g_monthCount >= maxCalls;
}

// Increment the monthly counter and flush to NVS. Called BEFORE the HTTP
// request fires, so a mid-call crash still leaves us correctly counted.
static void chargeOneCall() {
  ensureFaPrefs();
  rolloverIfNeeded();
  g_monthCount++;
  g_faPrefs.putUInt("mtag",   g_monthTag);
  g_faPrefs.putUInt("mcount", g_monthCount);
}

// Reset for the current month (exposed via the config page button).
void aeroApiResetMonthly() {
  ensureFaPrefs();
  g_monthCount = 0;
  g_faPrefs.putUInt("mcount", 0);
  log_i("AeroAPI: monthly counter manually reset");
}

// --- Cache helpers -------------------------------------------------------

static CacheEntry* findCached(const String& callsign) {
  unsigned long now = millis();
  for (int i = 0; i < CACHE_SIZE; ++i) {
    if (!g_cache[i].valid) continue;
    // Handle millis() wrap by treating future timestamps as stale.
    if (g_cache[i].fetchedMs > now) { g_cache[i].valid = false; continue; }
    if ((now - g_cache[i].fetchedMs) > CACHE_TTL_MS) {
      g_cache[i].valid = false;
      continue;
    }
    if (g_cache[i].callsign == callsign) return &g_cache[i];
  }
  return nullptr;
}

static CacheEntry* findFreeCacheSlot() {
  // Prefer invalid slots; otherwise overwrite the oldest.
  unsigned long oldest = ULONG_MAX;
  int oldestIdx = 0;
  for (int i = 0; i < CACHE_SIZE; ++i) {
    if (!g_cache[i].valid) return &g_cache[i];
    if (g_cache[i].fetchedMs < oldest) {
      oldest = g_cache[i].fetchedMs;
      oldestIdx = i;
    }
  }
  return &g_cache[oldestIdx];
}

// Copy cached enrichment into an Aircraft.
static void applyCacheTo(Aircraft& a, const CacheEntry& c) {
  a.origin   = c.origin;
  a.dest     = c.dest;
  a.airline  = c.airline;
  if (c.type.length() && a.type.length() == 0) a.type = c.type;
  a.enriched = true;
}

// --- The actual HTTP call ------------------------------------------------

static WiFiClientSecure g_tls;
static bool g_tlsReady = false;
static void ensureTLS() {
  if (g_tlsReady) return;
  g_tls.setInsecure();   // matches TheFlightWall's pattern; avoids cert bundling
  g_tlsReady = true;
}

// Returns true if the call succeeded AND populated the cache entry.
static bool callAeroApi(const String& callsign, CacheEntry& out) {
  if (WiFi.status() != WL_CONNECTED) return false;
  if (settingsAeroApiKey().length() == 0) return false;

  // *** HARD MONTHLY SPEND CAP ***
  // This is the final gate. If we'd exceed the user's $/month limit on
  // this call, refuse. No retries, no edge cases -- the cap is the cap.
  if (monthlyCapExhausted()) {
    log_w("AeroAPI: monthly cap reached (%d calls = $%.2f), refusing %s",
          (int)g_monthCount, g_monthCount * 0.005, callsign.c_str());
    return false;
  }

  ensureTLS();

  // Strip trailing spaces adsb.lol leaves behind ("QFA9   " -> "QFA9")
  String ident = callsign;
  ident.trim();
  if (ident.length() == 0) return false;

  char url[160];
  snprintf(url, sizeof(url),
           "https://aeroapi.flightaware.com/aeroapi/flights/%s", ident.c_str());

  HTTPClient http;
  http.setTimeout(8000);
  if (!http.begin(g_tls, url)) return false;
  http.addHeader("x-apikey", settingsAeroApiKey());
  http.addHeader("Accept", "application/json");

  // Charge the call to both meters BEFORE firing the request. If we crash
  // or lose power mid-call we've still counted it -- bias is always toward
  // staying under the cap. A bad-key 401 still costs because FlightAware
  // counts it on their side too.
  chargeOneCall();
  recordCall();

  int code = http.GET();
  if (code != 200) {
    log_w("AeroAPI %s -> HTTP %d (charged, %d/mo)",
          ident.c_str(), code, (int)g_monthCount);
    http.end();
    return false;
  }

  JsonDocument doc;
  DeserializationError err = deserializeJson(doc, http.getStream());
  http.end();
  if (err) { log_w("AeroAPI parse: %s", err.c_str()); return false; }

  JsonArrayConst flights = doc["flights"].as<JsonArrayConst>();
  if (flights.size() == 0) return false;

  // AeroAPI returns multiple entries (past + current + scheduled). Prefer
  // the one in-flight if we can detect it; otherwise fall back to flights[0].
  JsonObjectConst f = flights[0].as<JsonObjectConst>();
  for (JsonObjectConst candidate : flights) {
    const char* status = candidate["status"] | "";
    if (strstr(status, "Air") || strstr(status, "En Route") || strstr(status, "Taxi")) {
      f = candidate;
      break;
    }
  }

  out.callsign = callsign;
  out.airline  = (const char*)(f["operator_iata"] | f["operator"] | "");
  // AeroAPI's "ident" already contains the callsign; we want a friendly
  // airline name. operator_iata is the 2-letter code (QF, EK, SQ) which
  // looks fine on a small OLED ticker.
  out.type     = (const char*)(f["aircraft_type"] | "");

  // AeroAPI's airport objects can have any of: code_icao, code_iata, code_lid,
  // or just `code`. Try them all in friendly-first order so the OLED gets the
  // best label even when ICAO is missing (small/regional airports often only
  // have IATA). icaoToIata() called downstream is a no-op if we already
  // handed it a 3-letter code.
  JsonObjectConst origin = f["origin"].as<JsonObjectConst>();
  JsonObjectConst dest   = f["destination"].as<JsonObjectConst>();
  out.origin = (const char*)(origin["code_iata"] | origin["code_icao"]
                           | origin["code"]      | origin["code_lid"] | "");
  out.dest   = (const char*)(dest["code_iata"]   | dest["code_icao"]
                           | dest["code"]        | dest["code_lid"]   | "");

  // Log for diagnostics so the serial console shows what came back.
  log_i("AeroAPI %s -> %s>%s op=%s type=%s",
        callsign.c_str(),
        out.origin.length() ? out.origin.c_str() : "?",
        out.dest.length()   ? out.dest.c_str()   : "?",
        out.airline.length() ? out.airline.c_str() : "-",
        out.type.length()    ? out.type.c_str()    : "-");

  out.fetchedMs = millis();
  out.valid     = true;
  return true;
}

// Public API: try to enrich `a` in place.
// Returns:
//    1 if enriched from a fresh call
//    0 if enriched from cache (no cost)
//   -1 if not enriched (disabled, no key, budget exhausted, or call failed)
//
// Callers should treat 0/1 identically for display purposes; -1 means
// "show what adsb.lol gave you and don't sweat it".
int enrichAircraft(Aircraft& a) {
  if (!settingsAeroApiEnabled()) return -1;
  if (settingsAeroApiKey().length() == 0) return -1;
  if (a.callsign.length() == 0) return -1;

  String cs = a.callsign;
  cs.trim();

  // 1. Cache hit?
  CacheEntry* cached = findCached(cs);
  if (cached) {
    applyCacheTo(a, *cached);
    return 0;
  }

  // 2. Monthly hard cap?
  if (monthlyCapExhausted()) return -1;

  // 3. Hourly budget OK?
  int used = callsInLastHour();
  int cap  = settingsAeroApiCallsPerHour();
  if (cap > 0 && used >= cap) {
    log_i("AeroAPI hourly budget: %d/%d, skipping %s", used, cap, cs.c_str());
    return -1;
  }

  // 3. Make the call.
  CacheEntry fresh;
  if (!callAeroApi(cs, fresh)) return -1;

  // 4. Store in cache (overwriting oldest if full).
  CacheEntry* slot = findFreeCacheSlot();
  *slot = fresh;
  applyCacheTo(a, *slot);
  return 1;
}

// Convenience for the main loop: enrich up to maxNew uncached entries in
// a list, leaving the rest alone. This is how we keep cost predictable
// even when many planes are nearby -- only a small handful get a fresh
// call per poll cycle; the others either come from cache or stay bare.
int enrichBatch(Aircraft list[], int count, int maxNewCalls) {
  if (!settingsAeroApiEnabled()) return 0;
  if (settingsAeroApiKey().length() == 0) return 0;

  int enriched = 0;
  int freshCalls = 0;
  for (int i = 0; i < count; ++i) {
    // Cache lookups are free -- always try those.
    String cs = list[i].callsign; cs.trim();
    CacheEntry* cached = findCached(cs);
    if (cached) { applyCacheTo(list[i], *cached); enriched++; continue; }

    // Fresh calls cost money: respect the per-batch ceiling.
    if (freshCalls >= maxNewCalls) continue;

    // Monthly cap is hard -- once hit, stop trying new calls entirely.
    if (monthlyCapExhausted()) break;

    int r = enrichAircraft(list[i]);
    if (r == 1) { enriched++; freshCalls++; }
    else if (r == 0) { enriched++; }
    // If hourly budget exhausted, no point continuing this batch.
    if (r == -1 && callsInLastHour() >= settingsAeroApiCallsPerHour()) break;
  }
  return enriched;
}
