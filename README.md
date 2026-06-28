# FlightBoard

Standalone flight-tracking firmware for an **ESP32 + 0.96" SSD1306 OLED**. Hits [adsb.lol](https://adsb.lol) directly over WiFi for live aircraft, serves its own config web page from your phone — no Pi, no server, no cloud account. Optional [FlightAware AeroAPI](https://flightaware.com/aeroapi/portal) enrichment for route info (PER>SYD), with a hard **$5/month spend cap** that physically can't be exceeded.

## What you'll need

| Part | AU source | Approx price |
|---|---|---|
| ESP32 dev board (WROOM-32 / NodeMCU-32S) | [Core Electronics](https://core-electronics.com.au/) or [Little Bird](https://littlebirdelectronics.com.au/) | $15–25 AUD |
| 0.96" 128×64 SSD1306 OLED (I2C, 4-pin) | [Core Electronics](https://core-electronics.com.au/) | $8–12 AUD |
| 4 × jumper wires (female-female) | Bunch | $2 |
| USB-C / micro-USB cable | — | — |

Total: **~$25–40 AUD**. No soldering required if the OLED has header pins pre-fitted.

## Wiring

The OLED uses I²C — only 4 wires.

| OLED pin | ESP32 pin |
|---|---|
| VCC  | 3V3 |
| GND  | GND |
| SDA  | GPIO 21 |
| SCL  | GPIO 22 |

If your ESP32 board uses different default I²C pins (Heltec boards do — they're 4/15), edit the `Wire.begin(...)` call in `src/Display.cpp`.

## Building & flashing

### Option A: PlatformIO (recommended)

1. Install [VS Code](https://code.visualstudio.com/) and the [PlatformIO IDE extension](https://platformio.org/install/ide?install=vscode).
2. Open this `esp32/` folder as a PlatformIO project.
3. Plug in the ESP32 via USB.
4. Click **PlatformIO → Upload**. First build takes ~3 minutes; subsequent builds ~10 seconds.
5. Click **PlatformIO → Monitor** to watch the serial log.

### Option B: Arduino IDE

1. Install [Arduino IDE 2.x](https://www.arduino.cc/en/software).
2. Add ESP32 board support: `File → Preferences → Additional Boards Manager URLs` → paste `https://espressif.github.io/arduino-esp32/package_esp32_index.json`. Then `Tools → Board → Boards Manager` → search "esp32" → install.
3. Install libraries (`Tools → Manage Libraries`):
   - `U8g2` by oliver
   - `ArduinoJson` by Benoit Blanchon (v7+)
   - `WiFiManager` by tzapu (v2.0.17+)
4. Open the project; `Tools → Board → ESP32 Dev Module`, set the port, click Upload.

## First boot — WiFi setup

1. Plug in the ESP32. The OLED shows **"Connecting WiFi..."** briefly, then **"WiFi setup needed"**.
2. On your phone, open WiFi and connect to **`FlightBoard-Setup`** (no password).
3. A captive portal pops up. If not, open Safari and go to `http://192.168.4.1`.
4. Tap **Configure WiFi**, pick your home WiFi, enter password, save.
5. The ESP32 reboots and connects to your network within ~10 seconds.

## Day-to-day — control from your phone

Once on WiFi, the device hosts a control page at:

- **`http://flightboard.local`** (iPhones support mDNS out of the box)
- **`http://<esp32-ip>`** if mDNS doesn't work on your router

The page lets you:

- Switch **Nearby** / **Track flight** modes
- Set tracked callsign (QF9, EK420 — IATA *or* ICAO callsigns both work)
- Set centre lat/lon and radius (defaults to Perth, WA, 50 km)
- Pick **Ticker** or **Card** display mode
- Tune ticker speed or carousel rotation
- Toggle **FlightAware enrichment** on/off, paste your AeroAPI key, set hourly call cap

All settings persist in ESP32 flash (NVS) — survive power loss, reboots, and firmware updates.

## Display modes

### Ticker mode (default)

Continuous horizontal scroll across the OLED, departure-board style:

```
NEARBY 50km                          .
___________________________________
|                                 |
|  QF28  PER>SYD  B738  FL360 120km     *    EK420 ...
|                                 |
|---------------------------------|
| 4 aircraft                FA on |
|_________________________________|
```

Tunable speed (1–6 px/step). If FlightAware enrichment is on, you'll see real airport codes (`PER>SYD`); otherwise just the callsign, type, altitude, and distance.

### Card mode

One aircraft full-screen with a big bold callsign and a status strip:

```
NEARBY 2/4                          .
___________________________________
|                                 |
|         QFA9                    |     <- big callsign, scrolls if long
|         YPPH > YSSY             |     <- route (if enriched) or type+reg
|---------------------------------|
| FL360       450kt          82km |
|_________________________________|
```

The carousel rotates through the closest planes every N seconds (configurable).

## FlightAware enrichment — how the cost controls work

adsb.lol is free and always primary. AeroAPI is optional, paid (~$0.005 per `/flights/{ident}` call), and the firmware *aggressively* avoids spending money. Four layers, in order of severity:

1. **Per-callsign cache (6 hours)** — Once we've enriched `QFA9`, we don't ask again for 6 hours. Airline and aircraft type don't change mid-flight.
2. **Per-poll fresh-call ceiling** — At most 2 new (uncached) callsigns get a fresh API call per 8-second poll cycle.
3. **Hourly soft cap** — Default 20 calls/hour. Once hit, new callsigns wait until next hour (rolling 60-min window).
4. **Monthly HARD cap — the one you actually care about.** Default **$5.00/month = 1,000 calls** (each call is $0.005 = 0.5¢). Once hit, the firmware will **refuse to fire any more AeroAPI requests until the calendar month rolls over.** No retries, no exceptions.

### What makes the monthly cap *actually* hard

- The counter lives in **NVS flash**, not RAM. Power loss, brownouts, crashes — none of it resets the count.
- The counter is **keyed by year-month** (`202606`). It only auto-resets when the calendar month genuinely changes (verified via NTP-synced time).
- The call is **charged BEFORE the HTTP request fires.** If the ESP32 reboots mid-call, you've still paid for it on our side, matching what FlightAware bills.
- **Without a synced clock, the firmware refuses all calls** (fail-safe — won't accidentally double-spend across an unknown month boundary).
- A bad-key 401 still counts. We assume FlightAware bills it.

### Spend math

| Limit | Default | Maximum theoretical spend |
|---|---|---|
| Per call | $0.005 | — |
| Hourly soft cap | 20 / hr | $0.10 / hr → $2.40 / day → $72 / mo |
| **Monthly HARD cap** | **$5.00** | **$5.00 / month, full stop** |
| Realistic spend (with 6h cache) | — | **$0.50–$2.00 / month** |

The config page shows a live progress bar that **goes amber at 70%** and **red at 90%** of your monthly cap. You can change the cap any time (in 50¢ steps), or set it to **$0** to disable spending entirely while leaving the key configured.

### Manual reset

FlightAware's billing month and your device's calendar month should agree, but if they ever drift (e.g. you registered on the 15th and FlightAware bills monthly from then), tap **Reset monthly counter** on the config page to align them.

### Get a key

[flightaware.com/aeroapi](https://flightaware.com/aeroapi/portal). Personal tier = $5/month free credit (≈1,000 enrichments). If you also feed ADS-B data back to FlightAware via a Pi, it's $20/month free credit. With the default $5 cap you'll never go a cent over the free tier.

## Forgetting WiFi

On the config page, tap **Forget WiFi**. The device clears credentials and reboots into setup-AP mode.

## Troubleshooting

| Symptom | Likely cause |
|---|---|
| OLED stays blank | I²C wiring; try swapping SDA/SCL; check 3V3 not 5V |
| Stuck on "Connecting WiFi…" | Wrong password; **Forget WiFi** and reconfigure |
| "No aircraft within 50km" but flights show online | adsb.lol coverage gap (common in WA/remote areas). Increase radius to 200 km, or add an AeroAPI key for FlightAware coverage |
| Config page not loading at `flightboard.local` | mDNS unsupported on your network. Use `http://<ip>` |
| AeroAPI "used" counter not moving | Check key is correct and **Enable** is ticked; try a callsign you know is airborne |
| Ticker scrolls too fast / too slow | Adjust **Ticker speed** (1=slow, 6=fast) |
| Random reboots while polling | USB power supply too weak. Use a 2 A wall adapter |

## File layout

```
flightboard-esp32/
├── platformio.ini
├── README.md             # this file
├── docs/
│   └── SHOPPING_LIST.md  # parts list with AU sources
├── include/
│   ├── Aircraft.h        # compact struct with optional enrichment fields
│   ├── CallsignMap.h     # IATA <-> ICAO airline codes (QF <-> QFA)
│   └── AirportMap.h      # ICAO -> IATA airport codes (YPPH -> PER)
└── src/
    ├── main.cpp          # cooperative loop: poll, enrich, render
    ├── AdsbClient.cpp    # free adsb.lol HTTPS client (primary source)
    ├── AeroApiClient.cpp # paid AeroAPI client with cache + budget gate
    ├── Display.cpp       # SSD1306 renderer (ticker + card modes)
    └── ConfigPortal.cpp # WiFiManager + control web page + NVS persistence
```
