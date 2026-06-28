// Configuration web portal served by the ESP32 itself.
//
// First boot: the device can't connect to WiFi (no credentials saved), so
// WiFiManager raises an AP called "FlightBoard-Setup". Connect with your
// phone -> captive portal pops up -> you pick your home WiFi.
//
// Once on WiFi: visit http://flightboard.local (mDNS) from any device on
// the network to change tracked flight, location, radius, mode.
//
// State persists in NVS (ESP32 flash), survives reboots and power loss.

#include <WiFi.h>
#include <WebServer.h>
#include <ESPmDNS.h>
#include <Preferences.h>
#include <WiFiManager.h>

extern void displayBoot(const char* status);
extern int  aeroApiCallsThisHour();
extern int  aeroApiCallsThisMonth();
extern int  aeroApiSpendThisMonthCents();
extern void aeroApiResetMonthly();

static Preferences g_prefs;
static WebServer g_http(80);

// Settings shared with main loop.
struct Settings {
  // Display behavior
  String mode;            // "nearby" | "flight"
  String display;         // "ticker" | "card"
  String trackedFlight;
  float  centerLat;
  float  centerLon;
  float  radiusKm;
  int    rotateMs;        // card-mode carousel speed
  int    tickerSpeed;     // pixels per 30ms step (1=slow, 4=fast)

  // AeroAPI enrichment
  bool   aeroApiEnabled;
  String aeroApiKey;
  int    aeroApiCallsPerHour;
  int    aeroApiMonthlyCapCents;   // HARD monthly spend cap in cents ($5 = 500)
} g_settings;

static void saveSettings() {
  g_prefs.putString("mode",        g_settings.mode);
  g_prefs.putString("display",     g_settings.display);
  g_prefs.putString("flight",      g_settings.trackedFlight);
  g_prefs.putFloat ("lat",         g_settings.centerLat);
  g_prefs.putFloat ("lon",         g_settings.centerLon);
  g_prefs.putFloat ("radius",      g_settings.radiusKm);
  g_prefs.putInt   ("rotate",      g_settings.rotateMs);
  g_prefs.putInt   ("tickspd",     g_settings.tickerSpeed);
  g_prefs.putBool  ("fa_on",       g_settings.aeroApiEnabled);
  g_prefs.putString("fa_key",      g_settings.aeroApiKey);
  g_prefs.putInt   ("fa_cap",      g_settings.aeroApiCallsPerHour);
  g_prefs.putInt   ("fa_mcap",     g_settings.aeroApiMonthlyCapCents);
}

static void loadSettings() {
  g_settings.mode                = g_prefs.getString("mode", "nearby");
  g_settings.display             = g_prefs.getString("display", "ticker");
  g_settings.trackedFlight       = g_prefs.getString("flight", "");
  g_settings.centerLat           = g_prefs.getFloat("lat", -31.9505f);
  g_settings.centerLon           = g_prefs.getFloat("lon", 115.8605f);
  g_settings.radiusKm            = g_prefs.getFloat("radius", 50.0f);
  g_settings.rotateMs            = g_prefs.getInt("rotate", 4000);
  g_settings.tickerSpeed         = g_prefs.getInt("tickspd", 2);
  g_settings.aeroApiEnabled        = g_prefs.getBool("fa_on", false);
  g_settings.aeroApiKey            = g_prefs.getString("fa_key", "");
  g_settings.aeroApiCallsPerHour   = g_prefs.getInt("fa_cap",  20);
  g_settings.aeroApiMonthlyCapCents = g_prefs.getInt("fa_mcap", 500);   // $5.00 default
}

// Minimal mobile-friendly HTML. No external deps (works without internet).
static const char* HTML_PAGE = R"HTML(
<!doctype html>
<html><head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>FlightBoard</title>
<style>
  *{box-sizing:border-box}
  body{font-family:-apple-system,system-ui,sans-serif;background:#0a0e1a;color:#e6e6e6;margin:0;padding:20px;max-width:480px;margin:auto}
  h1{color:#ffb347;font-size:22px;margin:0 0 4px}
  h2{color:#ffb347;font-size:14px;margin:0 0 10px;text-transform:uppercase;letter-spacing:.5px}
  .sub{color:#8b95a7;font-size:13px;margin-bottom:24px}
  .card{background:#131829;border:1px solid #1f2738;border-radius:12px;padding:16px;margin-bottom:14px}
  label{display:block;font-size:12px;color:#8b95a7;text-transform:uppercase;letter-spacing:.5px;margin-bottom:6px}
  input,select{width:100%;padding:11px;background:#0a0e1a;border:1px solid #1f2738;border-radius:8px;color:#e6e6e6;font-size:16px;font-family:inherit}
  input:focus,select:focus{outline:none;border-color:#ffb347}
  .row{display:flex;gap:10px}
  .row > div{flex:1}
  button{width:100%;padding:13px;background:#ffb347;color:#0a0e1a;border:0;border-radius:8px;font-size:16px;font-weight:600;cursor:pointer;margin-top:8px}
  button:active{transform:scale(.98)}
  .seg{display:flex;gap:8px;margin-bottom:14px}
  .seg button{flex:1;background:#131829;color:#e6e6e6;border:1px solid #1f2738;margin:0;font-weight:400}
  .seg button.active{background:#ffb347;color:#0a0e1a;border-color:#ffb347;font-weight:600}
  .hint{font-size:12px;color:#8b95a7;margin-top:6px}
  .status{font-size:12px;color:#7ec97f;margin-top:14px;text-align:center}
  .switch{display:flex;align-items:center;justify-content:space-between;margin-bottom:12px}
  .switch input[type=checkbox]{width:auto;transform:scale(1.4);margin:0}
  .meter{height:6px;background:#0a0e1a;border-radius:3px;overflow:hidden;margin-top:6px}
  .meter-fill{height:100%;background:#7ec97f;transition:width .3s}
  .pill{display:inline-block;padding:2px 8px;background:#1f2738;border-radius:10px;font-size:11px;color:#8b95a7}
</style></head>
<body>
<h1>FlightBoard</h1>
<div class="sub">ESP32 OLED control</div>

<form method="POST" action="/save">

<h2>Source</h2>
<div class="seg">
  <button type="button" id="m-nearby" onclick="setMode('nearby')">Nearby</button>
  <button type="button" id="m-flight" onclick="setMode('flight')">Track flight</button>
</div>
<input type="hidden" name="mode" id="mode" value="{{MODE}}">

<div class="card" id="card-flight" style="display:none">
  <label>Flight callsign</label>
  <input name="flight" value="{{FLIGHT}}" placeholder="QF9, EK420, SIA216" autocapitalize="characters" autocorrect="off">
  <div class="hint">Boarding-pass code (QF9) or ICAO callsign (QFA9) both work.</div>
</div>

<div class="card" id="card-nearby">
  <label>Location (lat, lon)</label>
  <div class="row">
    <div><input name="lat" value="{{LAT}}" type="number" step="0.0001"></div>
    <div><input name="lon" value="{{LON}}" type="number" step="0.0001"></div>
  </div>
  <div class="hint">Default: Perth, WA.</div>
  <label style="margin-top:14px">Radius (km)</label>
  <input name="radius" value="{{RADIUS}}" type="number" min="5" max="400">
</div>

<h2>Display</h2>
<div class="card">
  <div class="seg">
    <button type="button" id="d-ticker" onclick="setDisplay('ticker')">Ticker</button>
    <button type="button" id="d-card" onclick="setDisplay('card')">Card</button>
  </div>
  <input type="hidden" name="display" id="display" value="{{DISPLAY}}">

  <div id="opt-ticker">
    <label>Ticker speed</label>
    <input name="tickspd" value="{{TICKSPD}}" type="number" min="1" max="6">
    <div class="hint">Pixels per step. 1 = slow scroll, 4 = brisk, 6 = whoa.</div>
  </div>
  <div id="opt-card" style="display:none">
    <label>Carousel rotate (seconds)</label>
    <input name="rotate" value="{{ROTATE}}" type="number" min="2" max="30">
    <div class="hint">How long each plane stays on screen.</div>
  </div>
</div>

<h2>FlightAware enrichment <span class="pill">optional</span></h2>
<div class="card">
  <div class="switch">
    <label style="margin:0">Use AeroAPI for routes & airline</label>
    <input type="checkbox" name="fa_on" value="1" {{FA_ON}}>
  </div>
  <div class="hint" style="margin-bottom:12px">
    Free adsb.lol data is always primary. AeroAPI just adds origin/destination
    (e.g. PER&gt;SYD) for callsigns nearby. Each enrichment costs ~$0.005 and
    is cached for 6 hours.
  </div>

  <label>AeroAPI key</label>
  <input name="fa_key" value="{{FA_KEY}}" placeholder="paste your x-apikey here" autocomplete="off">
  <div class="hint">Get one at flightaware.com/aeroapi -- free tier is $5/month credit.</div>

  <label style="margin-top:14px">Hourly cap (calls)</label>
  <input name="fa_cap" value="{{FA_CAP}}" type="number" min="1" max="200">
  <div class="hint">Soft cap. Once reached, new callsigns wait until next hour.</div>

  <div class="hint" style="margin-top:14px">Used in last hour: <b style="color:#e6e6e6">{{FA_USED}} / {{FA_CAP}}</b></div>
  <div class="meter"><div class="meter-fill" style="width:{{FA_PCT}}%"></div></div>

  <hr style="border:0;border-top:1px solid #1f2738;margin:18px 0">

  <label style="color:#ff6b6b">Monthly spend cap (HARD)</label>
  <div class="row">
    <div style="flex:0 0 auto;line-height:42px;color:#8b95a7;padding-right:6px">$</div>
    <div><input name="fa_mcap_dollars" value="{{FA_MCAP_DOL}}" type="number" min="0" max="100" step="0.50"></div>
  </div>
  <div class="hint">
    Once this dollar cap is hit, the firmware will <b>refuse to make any more
    AeroAPI calls until next month</b>. Survives reboots (counter is in flash).
    Each /flights/{ident} call costs $0.005 = 0.5 cents. Set to $0 to disable
    spending entirely.
  </div>

  <div class="hint" style="margin-top:14px">
    This month: <b style="color:#e6e6e6">${{FA_MSPEND}}</b> of
    <b style="color:#e6e6e6">${{FA_MCAP_DOL}}</b>
    ({{FA_MCALLS}} calls)
  </div>
  <div class="meter"><div class="meter-fill" style="width:{{FA_MPCT}}%;background:{{FA_MCOLOR}}"></div></div>

  <button type="button" onclick="if(confirm('Reset the monthly counter to zero? Use this if FlightAware confirms your billing month rolled over differently.'))location.href='/fa-reset'" style="background:#1f2738;color:#e6e6e6;margin-top:12px;font-size:13px;padding:9px">Reset monthly counter</button>
</div>

<button type="submit">Save & apply</button>
</form>

<div class="card" style="margin-top:14px">
  <label>WiFi</label>
  <div style="font-size:14px">Connected: <span style="color:#ffb347">{{SSID}}</span></div>
  <button type="button" onclick="if(confirm('Reset WiFi credentials and restart?'))location.href='/wifi-reset'" style="background:#1f2738;color:#e6e6e6;margin-top:10px">Forget WiFi</button>
</div>

<div class="status">{{STATUS}}</div>

<script>
function setMode(m){
  document.getElementById('mode').value=m;
  document.getElementById('m-nearby').classList.toggle('active',m==='nearby');
  document.getElementById('m-flight').classList.toggle('active',m==='flight');
  document.getElementById('card-flight').style.display = m==='flight'?'block':'none';
  document.getElementById('card-nearby').style.display = m==='nearby'?'block':'none';
}
function setDisplay(d){
  document.getElementById('display').value=d;
  document.getElementById('d-ticker').classList.toggle('active',d==='ticker');
  document.getElementById('d-card').classList.toggle('active',d==='card');
  document.getElementById('opt-ticker').style.display = d==='ticker'?'block':'none';
  document.getElementById('opt-card').style.display   = d==='card'?'block':'none';
}
setMode(document.getElementById('mode').value);
setDisplay(document.getElementById('display').value);
</script>
</body></html>
)HTML";

static String renderPage(const String& status = "") {
  String page(HTML_PAGE);
  page.replace("{{MODE}}",    g_settings.mode);
  page.replace("{{DISPLAY}}", g_settings.display);
  page.replace("{{FLIGHT}}",  g_settings.trackedFlight);
  page.replace("{{LAT}}",     String(g_settings.centerLat, 4));
  page.replace("{{LON}}",     String(g_settings.centerLon, 4));
  page.replace("{{RADIUS}}",  String(g_settings.radiusKm, 0));
  page.replace("{{ROTATE}}",  String(g_settings.rotateMs / 1000));
  page.replace("{{TICKSPD}}", String(g_settings.tickerSpeed));
  page.replace("{{FA_ON}}",   g_settings.aeroApiEnabled ? "checked" : "");
  page.replace("{{FA_KEY}}",  g_settings.aeroApiKey);
  page.replace("{{FA_CAP}}",  String(g_settings.aeroApiCallsPerHour));
  int used = aeroApiCallsThisHour();
  int cap  = g_settings.aeroApiCallsPerHour > 0 ? g_settings.aeroApiCallsPerHour : 1;
  int pct  = (used * 100) / cap;
  if (pct > 100) pct = 100;
  page.replace("{{FA_USED}}", String(used));
  page.replace("{{FA_PCT}}",  String(pct));

  // Monthly cap rendering
  int mSpendCents  = aeroApiSpendThisMonthCents();
  int mCalls       = aeroApiCallsThisMonth();
  int mCapCents    = g_settings.aeroApiMonthlyCapCents;
  char mSpendBuf[16], mCapBuf[16];
  snprintf(mSpendBuf, sizeof(mSpendBuf), "%d.%02d", mSpendCents / 100, mSpendCents % 100);
  snprintf(mCapBuf,   sizeof(mCapBuf),   "%d.%02d", mCapCents / 100,   mCapCents % 100);
  int mPct = (mCapCents > 0) ? (mSpendCents * 100) / mCapCents : 100;
  if (mPct > 100) mPct = 100;
  // Color the bar red as we approach the cap.
  const char* mColor = "#7ec97f";        // green <70%
  if (mPct >= 90)      mColor = "#ff6b6b"; // red
  else if (mPct >= 70) mColor = "#ffb347"; // amber
  page.replace("{{FA_MSPEND}}",  String(mSpendBuf));
  page.replace("{{FA_MCAP_DOL}}",String(mCapBuf));
  page.replace("{{FA_MCALLS}}",  String(mCalls));
  page.replace("{{FA_MPCT}}",    String(mPct));
  page.replace("{{FA_MCOLOR}}",  String(mColor));

  page.replace("{{SSID}}",    WiFi.SSID());
  page.replace("{{STATUS}}",  status);
  return page;
}

static void handleRoot() { g_http.send(200, "text/html", renderPage()); }

static void handleSave() {
  if (g_http.hasArg("mode"))    g_settings.mode    = g_http.arg("mode");
  if (g_http.hasArg("display")) g_settings.display = g_http.arg("display");
  if (g_http.hasArg("flight")) {
    g_settings.trackedFlight = g_http.arg("flight");
    g_settings.trackedFlight.trim();
    g_settings.trackedFlight.toUpperCase();
  }
  if (g_http.hasArg("lat"))     g_settings.centerLat = g_http.arg("lat").toFloat();
  if (g_http.hasArg("lon"))     g_settings.centerLon = g_http.arg("lon").toFloat();
  if (g_http.hasArg("radius"))  g_settings.radiusKm  = g_http.arg("radius").toFloat();
  if (g_http.hasArg("rotate"))  g_settings.rotateMs  = max(2L, g_http.arg("rotate").toInt()) * 1000;
  if (g_http.hasArg("tickspd")) g_settings.tickerSpeed = constrain(g_http.arg("tickspd").toInt(), 1, 6);

  // Checkbox: present in form only when checked
  g_settings.aeroApiEnabled = g_http.hasArg("fa_on");
  if (g_http.hasArg("fa_key")) {
    g_settings.aeroApiKey = g_http.arg("fa_key");
    g_settings.aeroApiKey.trim();
  }
  if (g_http.hasArg("fa_cap")) {
    g_settings.aeroApiCallsPerHour = constrain(g_http.arg("fa_cap").toInt(), 1, 200);
  }
  if (g_http.hasArg("fa_mcap_dollars")) {
    // Dollars (with decimals) -> cents. Clamp 0..$100 just to keep the
    // worst-case bounded; user can edit NVS or reflash to go higher.
    float dollars = g_http.arg("fa_mcap_dollars").toFloat();
    if (dollars < 0)   dollars = 0;
    if (dollars > 100) dollars = 100;
    g_settings.aeroApiMonthlyCapCents = (int)(dollars * 100.0f + 0.5f);
  }

  saveSettings();
  g_http.send(200, "text/html", renderPage("Saved. New settings applied."));
}

static void handleFaReset() {
  aeroApiResetMonthly();
  g_http.send(200, "text/html",
    "<h2 style='font-family:sans-serif;color:#ffb347'>Monthly counter reset.</h2>"
    "<p style='font-family:sans-serif'><a href='/' style='color:#ffb347'>Back to FlightBoard</a></p>");
}

static void handleWifiReset() {
  WiFi.disconnect(true, true);
  g_http.send(200, "text/html",
    "<h2 style='font-family:sans-serif;color:#ffb347'>WiFi cleared.</h2>"
    "<p style='font-family:sans-serif'>Device will restart. Reconnect to the "
    "<b>FlightBoard-Setup</b> network to set new credentials.</p>");
  delay(800);
  ESP.restart();
}

bool startConfigPortal() {
  g_prefs.begin("fb", false);
  loadSettings();

  WiFi.mode(WIFI_STA);
  WiFi.setHostname("flightboard");

  WiFiManager wm;
  wm.setConfigPortalTimeout(180);
  wm.setConnectTimeout(20);
  wm.setClass("invert");

  displayBoot("Connecting WiFi...");
  bool ok = wm.autoConnect("FlightBoard-Setup");
  if (!ok) {
    displayBoot("WiFi setup needed");
    delay(2000);
    return false;
  }

  if (MDNS.begin("flightboard")) {
    MDNS.addService("http", "tcp", 80);
  }

  // Sync clock via NTP. Critical for the monthly cap rollover: without a
  // trustworthy date the AeroAPI client refuses to make any calls (fail-safe).
  // AWST is UTC+8 with no DST; gmtOffset_sec = 28800, daylightOffset_sec = 0.
  configTime(8 * 3600, 0, "pool.ntp.org", "time.nist.gov");

  g_http.on("/",           handleRoot);
  g_http.on("/save",       HTTP_POST, handleSave);
  g_http.on("/fa-reset",   handleFaReset);
  g_http.on("/wifi-reset", handleWifiReset);
  g_http.begin();
  return true;
}

void serviceHttp() { g_http.handleClient(); }

// --- Accessors -----------------------------------------------------------
const String& settingsMode()              { return g_settings.mode; }
const String& settingsDisplay()           { return g_settings.display; }
const String& settingsTrackedFlight()     { return g_settings.trackedFlight; }
float         settingsLat()               { return g_settings.centerLat; }
float         settingsLon()               { return g_settings.centerLon; }
float         settingsRadiusKm()          { return g_settings.radiusKm; }
int           settingsRotateMs()          { return g_settings.rotateMs; }
int           settingsTickerSpeed()       { return g_settings.tickerSpeed; }
bool          settingsAeroApiEnabled()    { return g_settings.aeroApiEnabled; }
const String& settingsAeroApiKey()        { return g_settings.aeroApiKey; }
int           settingsAeroApiCallsPerHour()    { return g_settings.aeroApiCallsPerHour; }
int           settingsAeroApiMonthlyCapCents() { return g_settings.aeroApiMonthlyCapCents; }
