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
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <time.h>

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

// Mobile-friendly HTML. No external deps (works without internet).
// Single-page app with a hamburger drawer for advanced sections so the main
// view stays clean. All <input name="..."> attributes are unchanged from v1
// so handleSave() below still works without any modification.
static const char* HTML_PAGE = R"HTML(
<!doctype html>
<html><head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>FlightBoard</title>
<style>
  *{box-sizing:border-box}
  html,body{margin:0;padding:0}
  body{font-family:-apple-system,BlinkMacSystemFont,system-ui,"Segoe UI",sans-serif;background:#070b14;color:#e6e6e6;min-height:100vh;-webkit-font-smoothing:antialiased}
  .wrap{max-width:520px;margin:0 auto;padding:0 18px 40px;position:relative}

  /* ---------- Top bar with hamburger ---------- */
  .topbar{display:flex;align-items:center;gap:14px;padding:18px 0 16px;position:sticky;top:0;background:#070b14;z-index:5}
  .burger{width:42px;height:42px;display:flex;flex-direction:column;justify-content:center;align-items:center;gap:5px;background:#131829;border:1px solid #1f2738;border-radius:10px;cursor:pointer;flex-shrink:0}
  .burger span{display:block;width:18px;height:2px;background:#ffb347;border-radius:2px;transition:.2s}
  .burger:active{transform:scale(.96)}
  .brand{flex:1;min-width:0}
  .brand h1{color:#ffb347;font-size:20px;margin:0;font-weight:700;letter-spacing:-.2px}
  .brand .sub{color:#8b95a7;font-size:12px;margin-top:2px}
  .dot{width:8px;height:8px;border-radius:50%;background:#7ec97f;box-shadow:0 0 8px #7ec97f;flex-shrink:0}

  /* ---------- Drawer ---------- */
  .scrim{position:fixed;inset:0;background:rgba(0,0,0,.5);opacity:0;pointer-events:none;transition:.2s;z-index:9}
  .scrim.open{opacity:1;pointer-events:auto}
  .drawer{position:fixed;top:0;left:0;bottom:0;width:78%;max-width:300px;background:#0f1422;border-right:1px solid #1f2738;transform:translateX(-100%);transition:transform .25s ease;z-index:10;padding:20px 0;overflow-y:auto}
  .drawer.open{transform:translateX(0)}
  .drawer h3{color:#8b95a7;font-size:11px;text-transform:uppercase;letter-spacing:.8px;margin:0;padding:14px 20px 8px}
  .drawer a{display:flex;align-items:center;gap:12px;padding:12px 20px;color:#e6e6e6;text-decoration:none;font-size:15px;cursor:pointer;border-left:3px solid transparent}
  .drawer a:hover{background:#131829}
  .drawer a.active{border-left-color:#ffb347;background:#131829;color:#ffb347}
  .drawer a .ico{width:20px;text-align:center;font-size:16px}

  /* ---------- Section panels ---------- */
  .panel{display:none}
  .panel.active{display:block;animation:fade .2s ease}
  @keyframes fade{from{opacity:0;transform:translateY(4px)}to{opacity:1;transform:none}}

  /* ---------- Cards & inputs ---------- */
  h2{color:#ffb347;font-size:11px;margin:18px 0 10px;text-transform:uppercase;letter-spacing:.8px;font-weight:600}
  h2 .pill{display:inline-block;padding:2px 8px;background:#1f2738;border-radius:10px;font-size:10px;color:#8b95a7;margin-left:6px;font-weight:400;letter-spacing:.3px}
  .card{background:#131829;border:1px solid #1f2738;border-radius:14px;padding:16px;margin-bottom:14px}
  label{display:block;font-size:11px;color:#8b95a7;text-transform:uppercase;letter-spacing:.6px;margin-bottom:6px;font-weight:500}
  input,select{width:100%;padding:12px;background:#0a0e1a;border:1px solid #1f2738;border-radius:9px;color:#e6e6e6;font-size:16px;font-family:inherit;transition:border-color .15s}
  input:focus,select:focus{outline:none;border-color:#ffb347}
  .row{display:flex;gap:10px}
  .row > div{flex:1}
  .btn{width:100%;padding:13px;background:#ffb347;color:#0a0e1a;border:0;border-radius:9px;font-size:15px;font-weight:600;cursor:pointer;margin-top:6px;font-family:inherit;transition:.15s}
  .btn:active{transform:scale(.98)}
  .btn.ghost{background:#1f2738;color:#e6e6e6}
  .btn.danger{background:#1f2738;color:#ff8b8b}
  .btn.small{padding:10px;font-size:13px}
  .seg{display:flex;gap:8px;margin-bottom:14px}
  .seg button{flex:1;background:#0a0e1a;color:#8b95a7;border:1px solid #1f2738;padding:11px;border-radius:9px;font-size:14px;font-weight:500;cursor:pointer;font-family:inherit;margin:0}
  .seg button.active{background:#ffb347;color:#0a0e1a;border-color:#ffb347;font-weight:600}
  .hint{font-size:12px;color:#8b95a7;margin-top:6px;line-height:1.45}
  .switch{display:flex;align-items:center;justify-content:space-between;margin-bottom:8px}
  .switch label{margin:0;color:#e6e6e6;text-transform:none;font-size:14px;letter-spacing:0;font-weight:500}
  .switch input[type=checkbox]{width:auto;transform:scale(1.4);margin:0}
  .meter{height:6px;background:#0a0e1a;border-radius:3px;overflow:hidden;margin-top:6px}
  .meter-fill{height:100%;background:#7ec97f;transition:width .3s}

  /* ---------- Dashboard ---------- */
  .stat-grid{display:grid;grid-template-columns:1fr 1fr;gap:10px;margin-bottom:14px}
  .stat{background:#131829;border:1px solid #1f2738;border-radius:12px;padding:14px}
  .stat .k{font-size:10px;color:#8b95a7;text-transform:uppercase;letter-spacing:.6px;margin-bottom:4px}
  .stat .v{font-size:20px;font-weight:600;color:#e6e6e6;line-height:1.1}
  .stat .v.accent{color:#ffb347}
  .stat .sub{font-size:11px;color:#8b95a7;margin-top:3px}

  .status{font-size:13px;color:#7ec97f;margin:14px 0;text-align:center;padding:10px;background:rgba(126,201,127,.08);border-radius:8px;display:none}
  .status.show{display:block}

  hr{border:0;border-top:1px solid #1f2738;margin:18px 0}
  a.tlink{color:#ffb347;text-decoration:none}
</style></head>
<body>

<div class="scrim" id="scrim" onclick="toggleDrawer(false)"></div>

<nav class="drawer" id="drawer">
  <h3>FlightBoard</h3>
  <a data-tab="dashboard" class="active"><span class="ico">&#9992;</span> Dashboard</a>
  <a data-tab="source"><span class="ico">&#8982;</span> Source &amp; location</a>
  <a data-tab="display"><span class="ico">&#9635;</span> Display</a>
  <a data-tab="api"><span class="ico">&#9919;</span> API keys &amp; budget</a>
  <a data-tab="wifi"><span class="ico">&#10689;</span> WiFi</a>
  <a data-tab="about"><span class="ico">&#9432;</span> About</a>
</nav>

<div class="wrap">

<div class="topbar">
  <div class="burger" onclick="toggleDrawer()"><span></span><span></span><span></span></div>
  <div class="brand">
    <h1 id="hdr">Dashboard</h1>
    <div class="sub" id="hdrsub">Live status &amp; quick controls</div>
  </div>
  <div class="dot" title="Online"></div>
</div>

<div class="status" id="status">{{STATUS}}</div>

<form method="POST" action="/save" id="f">

<!-- ===================== DASHBOARD ===================== -->
<section class="panel active" data-panel="dashboard">
  <div class="stat-grid">
    <div class="stat">
      <div class="k">Mode</div>
      <div class="v accent" id="sMode">--</div>
      <div class="sub" id="sModeSub">--</div>
    </div>
    <div class="stat">
      <div class="k">Display</div>
      <div class="v" id="sDisplay">--</div>
      <div class="sub" id="sDisplaySub">--</div>
    </div>
    <div class="stat">
      <div class="k">Location</div>
      <div class="v" style="font-size:14px" id="sLoc">--, --</div>
      <div class="sub"><span id="sRad">--</span> km radius</div>
    </div>
    <div class="stat">
      <div class="k">AeroAPI</div>
      <div class="v" id="sFa" style="font-size:14px">--</div>
      <div class="sub" id="sFaSub">--</div>
    </div>
  </div>

  <div class="card">
    <label>Monthly AeroAPI spend</label>
    <div style="display:flex;justify-content:space-between;align-items:baseline;margin-top:2px">
      <div style="font-size:22px;font-weight:600">${{FA_MSPEND}}</div>
      <div style="font-size:13px;color:#8b95a7">of ${{FA_MCAP_DOL}} cap</div>
    </div>
    <div class="meter" style="margin-top:10px"><div class="meter-fill" style="width:{{FA_MPCT}}%;background:{{FA_MCOLOR}}"></div></div>
    <div class="hint" style="margin-top:8px">{{FA_MCALLS}} calls this month &middot; {{FA_USED}}/{{FA_CAP}} this hour</div>
  </div>

  <button type="button" class="btn ghost" onclick="go('source')">Edit source &amp; location</button>
</section>

<!-- ===================== SOURCE & LOCATION ===================== -->
<section class="panel" data-panel="source">
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
    <label>Location</label>
    <div class="row">
      <div><input name="lat" id="lat" value="{{LAT}}" type="number" step="0.0001" placeholder="Latitude"></div>
      <div><input name="lon" id="lon" value="{{LON}}" type="number" step="0.0001" placeholder="Longitude"></div>
    </div>
    <button type="button" class="btn ghost small" style="margin-top:10px" onclick="useMyLocation()" id="locBtn">&#8982; Use my location</button>
    <div class="hint" id="locHint">Tap to autofill from your phone or browser. Uses WiFi/IP if GPS is off.</div>
    <label style="margin-top:14px">Radius (km)</label>
    <input name="radius" value="{{RADIUS}}" type="number" min="5" max="400">
  </div>

  <button type="submit" class="btn">Save &amp; apply</button>
</section>

<!-- ===================== DISPLAY ===================== -->
<section class="panel" data-panel="display">
  <h2>OLED display mode</h2>
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
      <div class="hint">How long each plane stays on screen before the next slides in.</div>
    </div>
  </div>

  <h2>Future: external display <span class="pill">roadmap</span></h2>
  <div class="card">
    <div class="hint" style="margin-top:0">
      The ESP32 has no video output, so HDMI/USB-C&rarr;HDMI displays aren't supported
      directly. A Raspberry Pi running a slim companion dashboard (full radar map,
      stats screen) is the planned upgrade path. See the README for details.
    </div>
  </div>

  <button type="submit" class="btn">Save &amp; apply</button>
</section>

<!-- ===================== API & BUDGET ===================== -->
<section class="panel" data-panel="api">
  <h2>FlightAware AeroAPI <span class="pill">optional</span></h2>
  <div class="card">
    <div class="switch">
      <label>Enable AeroAPI enrichment</label>
      <input type="checkbox" name="fa_on" value="1" {{FA_ON}}>
    </div>
    <div class="hint" style="margin-bottom:14px">
      Free adsb.lol data is always primary. AeroAPI adds origin/destination
      (e.g. PER&gt;SYD) for nearby callsigns. ~$0.005 per lookup, cached 6h.
    </div>

    <label>AeroAPI key</label>
    <input name="fa_key" value="{{FA_KEY}}" placeholder="paste your x-apikey here" autocomplete="off">
    <div class="hint">Get one at <a class="tlink" href="https://flightaware.com/aeroapi" target="_blank">flightaware.com/aeroapi</a> &mdash; free tier is $5/month credit.</div>

    <label style="margin-top:14px">Hourly cap (calls)</label>
    <input name="fa_cap" value="{{FA_CAP}}" type="number" min="1" max="200">
    <div class="hint">Soft cap. Once hit, new callsigns wait until next hour.</div>

    <div class="hint" style="margin-top:14px">Used this hour: <b style="color:#e6e6e6">{{FA_USED}} / {{FA_CAP}}</b></div>
    <div class="meter"><div class="meter-fill" style="width:{{FA_PCT}}%"></div></div>
  </div>

  <h2>Hard monthly spend cap</h2>
  <div class="card">
    <label style="color:#ff8b8b">Maximum monthly spend</label>
    <div class="row">
      <div style="flex:0 0 auto;line-height:44px;color:#8b95a7;padding-right:6px;font-size:18px">$</div>
      <div><input name="fa_mcap_dollars" value="{{FA_MCAP_DOL}}" type="number" min="0" max="100" step="0.50"></div>
    </div>
    <div class="hint">
      Once this cap is hit, the firmware <b>refuses all AeroAPI calls until next month</b>.
      Survives reboots (counter is in flash). $0 disables spending entirely.
    </div>

    <div class="hint" style="margin-top:14px">
      This month: <b style="color:#e6e6e6">${{FA_MSPEND}}</b> of
      <b style="color:#e6e6e6">${{FA_MCAP_DOL}}</b>
      ({{FA_MCALLS}} calls)
    </div>
    <div class="meter"><div class="meter-fill" style="width:{{FA_MPCT}}%;background:{{FA_MCOLOR}}"></div></div>

    <button type="button" class="btn ghost small" style="margin-top:14px" onclick="if(confirm('Reset the monthly counter to zero? Use this only if FlightAware confirms your billing month rolled over differently.'))location.href='/fa-reset'">Reset monthly counter</button>
  </div>

  <h2>Troubleshooting</h2>
  <div class="card">
    <div class="hint" style="margin-top:0;margin-bottom:12px">Not seeing origin/destination on the OLED? Run the diagnostic to see exactly which step is failing (WiFi, NTP, key, budget, or the live API call).</div>
    <a href="/diag" class="btn ghost small" style="display:block;text-decoration:none;text-align:center;line-height:1">Run AeroAPI diagnostic</a>
    <div class="hint" style="margin-top:8px">A successful test costs $0.005.</div>
  </div>

  <button type="submit" class="btn">Save &amp; apply</button>
</section>

<!-- ===================== WIFI ===================== -->
<section class="panel" data-panel="wifi">
  <h2>WiFi</h2>
  <div class="card">
    <label>Currently connected</label>
    <div style="font-size:18px;color:#ffb347;font-weight:600">{{SSID}}</div>
    <div class="hint" style="margin-top:10px">Forgetting WiFi clears saved credentials and reboots the device. Reconnect to the <b>FlightBoard-Setup</b> network to set new ones.</div>
    <button type="button" class="btn danger" style="margin-top:14px" onclick="if(confirm('Reset WiFi credentials and restart?'))location.href='/wifi-reset'">Forget WiFi &amp; restart</button>
  </div>
</section>

<!-- ===================== ABOUT ===================== -->
<section class="panel" data-panel="about">
  <h2>About</h2>
  <div class="card">
    <div style="font-size:15px;margin-bottom:8px"><b style="color:#ffb347">FlightBoard ESP32</b></div>
    <div class="hint" style="margin-top:0">
      A standalone ESP32 + SSD1306 OLED that shows nearby aircraft from
      adsb.lol, with optional FlightAware AeroAPI enrichment for origin and
      destination airports. All data and credentials live in NVS flash;
      nothing leaves your network except calls to adsb.lol and (optionally)
      flightaware.com.
    </div>
    <hr>
    <div class="hint" style="margin-top:0">
      Open source &middot; MIT licensed &middot; see the GitHub repo for source, schematics,
      and the planned Raspberry Pi companion dashboard.
    </div>
  </div>
</section>

</form>

</div><!-- /wrap -->

<script>
function toggleDrawer(force){
  var d=document.getElementById('drawer'),s=document.getElementById('scrim');
  var open = force===undefined ? !d.classList.contains('open') : force;
  d.classList.toggle('open',open); s.classList.toggle('open',open);
}
function go(tab){
  document.querySelectorAll('.drawer a').forEach(function(a){a.classList.toggle('active',a.dataset.tab===tab);});
  document.querySelectorAll('.panel').forEach(function(p){p.classList.toggle('active',p.dataset.panel===tab);});
  var titles={dashboard:['Dashboard','Live status & quick controls'],source:['Source & location','What the OLED is tracking'],display:['Display','OLED behaviour'],api:['API keys & budget','FlightAware AeroAPI controls'],wifi:['WiFi','Network connection'],about:['About','FlightBoard ESP32']};
  if(titles[tab]){document.getElementById('hdr').textContent=titles[tab][0];document.getElementById('hdrsub').textContent=titles[tab][1];}
  toggleDrawer(false); window.scrollTo({top:0,behavior:'smooth'});
}
document.querySelectorAll('.drawer a').forEach(function(a){a.addEventListener('click',function(){go(a.dataset.tab);});});

function setMode(m){
  document.getElementById('mode').value=m;
  document.getElementById('m-nearby').classList.toggle('active',m==='nearby');
  document.getElementById('m-flight').classList.toggle('active',m==='flight');
  document.getElementById('card-flight').style.display = m==='flight'?'block':'none';
  document.getElementById('card-nearby').style.display = m==='nearby'?'block':'none';
  document.getElementById('sMode').textContent = m==='flight'?'Track':'Nearby';
  document.getElementById('sModeSub').textContent = m==='flight'?(document.querySelector('[name=flight]').value||'no callsign'):'planes around me';
}
function setDisplay(d){
  document.getElementById('display').value=d;
  document.getElementById('d-ticker').classList.toggle('active',d==='ticker');
  document.getElementById('d-card').classList.toggle('active',d==='card');
  document.getElementById('opt-ticker').style.display = d==='ticker'?'block':'none';
  document.getElementById('opt-card').style.display   = d==='card'?'block':'none';
  document.getElementById('sDisplay').textContent = d==='ticker'?'Ticker':'Card';
  document.getElementById('sDisplaySub').textContent = d==='ticker'?'continuous scroll':'one plane at a time';
}

function useMyLocation(){
  var btn=document.getElementById('locBtn'), hint=document.getElementById('locHint');
  if(!navigator.geolocation){hint.textContent='Geolocation not supported on this browser.';hint.style.color='#ff8b8b';return;}
  btn.textContent='Locating...'; btn.disabled=true;
  navigator.geolocation.getCurrentPosition(function(p){
    document.getElementById('lat').value=p.coords.latitude.toFixed(4);
    document.getElementById('lon').value=p.coords.longitude.toFixed(4);
    btn.textContent='Location filled'; hint.textContent='Accuracy: '+Math.round(p.coords.accuracy)+'m. Tap Save & apply to keep it.'; hint.style.color='#7ec97f';
    setTimeout(function(){btn.textContent='Use my location';btn.disabled=false;},2500);
  },function(e){
    btn.textContent='Use my location'; btn.disabled=false;
    hint.textContent='Could not get location: '+(e.message||'permission denied')+'. You can type it in manually.'; hint.style.color='#ff8b8b';
  },{enableHighAccuracy:false,timeout:8000,maximumAge:60000});
}

// Initial state
setMode(document.getElementById('mode').value);
setDisplay(document.getElementById('display').value);
document.getElementById('sLoc').textContent = document.querySelector('[name=lat]').value+', '+document.querySelector('[name=lon]').value;
document.getElementById('sRad').textContent = document.querySelector('[name=radius]').value;
var faOn = document.querySelector('[name=fa_on]').checked;
var faKey = document.querySelector('[name=fa_key]').value;
document.getElementById('sFa').textContent = faOn && faKey ? 'On' : 'Off';
document.getElementById('sFaSub').textContent = faOn && faKey ? 'enriching routes' : 'adsb.lol only';

var status=document.getElementById('status');
if(status.textContent.trim().length) status.classList.add('show');
setTimeout(function(){status.classList.remove('show');},4000);
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


// --- /diag --------------------------------------------------------------
// Walks through every step of the AeroAPI enrichment path and reports
// what's working and what isn't. Visit http://flightboard.local/diag.
// A successful live test costs $0.005 (one real AeroAPI call against QFA9).
static void handleDiag() {
  String r = "<!doctype html><html><head><meta charset='utf-8'>";
  r += "<meta name='viewport' content='width=device-width,initial-scale=1'>";
  r += "<title>FlightBoard diag</title><style>";
  r += "body{font-family:-apple-system,system-ui,sans-serif;background:#0a0e1a;color:#e6e6e6;padding:20px;max-width:560px;margin:auto;font-size:14px;line-height:1.55}";
  r += "h1{color:#ffb347;font-size:20px} h2{color:#ffb347;font-size:14px;text-transform:uppercase;letter-spacing:.5px;margin-top:24px}";
  r += ".ok{color:#7ec97f} .bad{color:#ff6b6b} .warn{color:#ffb347}";
  r += ".row{padding:8px 0;border-bottom:1px solid #1f2738;display:flex;justify-content:space-between;gap:10px}";
  r += ".k{color:#8b95a7} .v{font-family:Menlo,monospace;text-align:right;word-break:break-all}";
  r += "a{color:#ffb347} pre{background:#131829;padding:12px;border-radius:8px;overflow:auto;font-size:12px;white-space:pre-wrap}";
  r += "</style></head><body><h1>FlightBoard diagnostics</h1>";
  r += "<p><a href='/'>&larr; back</a></p>";

  // 1. WiFi
  r += "<h2>1. WiFi</h2>";
  r += "<div class='row'><span class='k'>Status</span><span class='v ";
  r += (WiFi.status() == WL_CONNECTED ? "ok'>connected" : "bad'>NOT CONNECTED");
  r += "</span></div>";
  r += "<div class='row'><span class='k'>SSID</span><span class='v'>" + WiFi.SSID() + "</span></div>";
  r += "<div class='row'><span class='k'>IP</span><span class='v'>" + WiFi.localIP().toString() + "</span></div>";
  long rssi = WiFi.RSSI();
  r += "<div class='row'><span class='k'>RSSI</span><span class='v ";
  if (rssi > -70) r += "ok'>"; else if (rssi > -80) r += "warn'>"; else r += "bad'>";
  r += String(rssi) + " dBm</span></div>";

  // 2. NTP clock
  r += "<h2>2. NTP clock <span style='font-size:11px;color:#8b95a7'>(required for monthly cap)</span></h2>";
  time_t now = time(nullptr);
  bool clockOk = (now > 1700000000);
  r += "<div class='row'><span class='k'>System time</span><span class='v ";
  r += (clockOk ? "ok'>" : "bad'>");
  if (clockOk) {
    struct tm tm; localtime_r(&now, &tm);
    char buf[32]; strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S", &tm);
    r += String(buf);
  } else {
    r += "NOT SYNCED (" + String((long)now) + ")";
  }
  r += "</span></div>";
  if (!clockOk) {
    r += "<div class='row'><span class='v warn'>AeroAPI calls are blocked until NTP syncs. Most common cause of missing destinations on a fresh boot.</span></div>";
  }

  // 3. AeroAPI settings
  r += "<h2>3. AeroAPI settings (loaded from NVS)</h2>";
  r += "<div class='row'><span class='k'>Enabled</span><span class='v ";
  r += (g_settings.aeroApiEnabled ? "ok'>YES" : "bad'>NO");
  r += "</span></div>";
  String k = g_settings.aeroApiKey;
  String kMasked;
  if (k.length() == 0) kMasked = "(EMPTY)";
  else if (k.length() < 8) kMasked = "(too short: " + String(k.length()) + " chars)";
  else kMasked = k.substring(0, 4) + "..." + k.substring(k.length() - 4) + "  (" + String(k.length()) + " chars)";
  r += "<div class='row'><span class='k'>Key</span><span class='v ";
  r += (k.length() >= 8 ? "ok'>" : "bad'>");
  r += kMasked + "</span></div>";
  r += "<div class='row'><span class='k'>Hourly cap</span><span class='v'>" + String(g_settings.aeroApiCallsPerHour) + "</span></div>";
  char buf2[16]; snprintf(buf2, sizeof(buf2), "$%d.%02d", g_settings.aeroApiMonthlyCapCents/100, g_settings.aeroApiMonthlyCapCents%100);
  r += "<div class='row'><span class='k'>Monthly cap</span><span class='v'>" + String(buf2) + "</span></div>";

  // 4. Budget right now
  r += "<h2>4. Budget right now</h2>";
  int monthSpend = aeroApiSpendThisMonthCents();
  int monthCap = g_settings.aeroApiMonthlyCapCents;
  bool monthCapHit = (monthCap <= 0) || (monthSpend >= monthCap);
  r += "<div class='row'><span class='k'>Hourly used</span><span class='v'>" + String(aeroApiCallsThisHour()) + " / " + String(g_settings.aeroApiCallsPerHour) + "</span></div>";
  r += "<div class='row'><span class='k'>Monthly spend</span><span class='v ";
  r += (monthCapHit ? "bad'>" : "ok'>");
  snprintf(buf2, sizeof(buf2), "$%d.%02d", monthSpend/100, monthSpend%100);
  r += String(buf2) + " / ";
  snprintf(buf2, sizeof(buf2), "$%d.%02d", monthCap/100, monthCap%100);
  r += String(buf2);
  if (monthCapHit) r += " (CAP HIT)";
  r += "</span></div>";
  r += "<div class='row'><span class='k'>Calls this month</span><span class='v'>" + String(aeroApiCallsThisMonth()) + "</span></div>";

  // 5. Live HTTPS test
  r += "<h2>5. Live test (one real call, costs $0.005)</h2>";
  if (!g_settings.aeroApiEnabled) {
    r += "<div class='row'><span class='v warn'>AeroAPI is disabled in settings. Enable it on the API panel first.</span></div>";
  } else if (k.length() < 8) {
    r += "<div class='row'><span class='v bad'>No key set. Paste your AeroAPI key on the API panel and tap Save.</span></div>";
  } else if (!clockOk) {
    r += "<div class='row'><span class='v bad'>Clock not synced; refusing to test (the monthly counter would not be reliable).</span></div>";
  } else {
    WiFiClientSecure tls; tls.setInsecure();
    HTTPClient http; http.setTimeout(8000);
    String url = "https://aeroapi.flightaware.com/aeroapi/flights/QFA9";
    r += "<div class='row'><span class='k'>URL</span><span class='v'>" + url + "</span></div>";
    if (!http.begin(tls, url)) {
      r += "<div class='row'><span class='v bad'>http.begin() failed (TLS init).</span></div>";
    } else {
      http.addHeader("x-apikey", k);
      http.addHeader("Accept", "application/json");
      unsigned long t0 = millis();
      int code = http.GET();
      unsigned long dt = millis() - t0;
      r += "<div class='row'><span class='k'>HTTP status</span><span class='v ";
      r += (code == 200 ? "ok'>" : "bad'>");
      r += String(code) + "  (" + String(dt) + " ms)</span></div>";
      String body = http.getString();
      http.end();
      if (code == 200) {
        r += "<div class='row'><span class='v ok'>KEY WORKS. AeroAPI accepted the request.</span></div>";
        if (body.length() > 700) body = body.substring(0, 700) + "\n... (truncated)";
        r += "<pre>" + body + "</pre>";
      } else if (code == 401 || code == 403) {
        r += "<div class='row'><span class='v bad'>Key REJECTED. Wrong, expired, or suspended account.</span></div>";
        r += "<pre>" + body + "</pre>";
      } else if (code < 0) {
        r += "<div class='row'><span class='v bad'>Network error (" + String(code) + "). DNS, TLS, or firewall.</span></div>";
      } else {
        r += "<div class='row'><span class='v warn'>Unexpected HTTP " + String(code) + ".</span></div>";
        r += "<pre>" + body + "</pre>";
      }
    }
  }

  r += "<p style='margin-top:24px;color:#8b95a7;font-size:12px'>Don't refresh /diag in a loop -- each successful test costs half a cent.</p>";
  r += "<p><a href='/'>&larr; back to FlightBoard</a></p>";
  r += "</body></html>";
  g_http.send(200, "text/html", r);
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
  g_http.on("/diag",       handleDiag);
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
