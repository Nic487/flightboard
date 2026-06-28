# Shopping list — FlightBoard ESP32

Everything you need to build the ESP32 + OLED flight board. Total: **~$25–40 AUD**, all available locally in Perth.

## Required parts

| Part | Where | Approx (AUD) | Notes |
|---|---|---|---|
| ESP32 dev board (WROOM-32 / NodeMCU-32S) | [Core Electronics](https://core-electronics.com.au/) · [Little Bird](https://littlebirdelectronics.com.au/) · [Jaycar](https://www.jaycar.com.au/) | $15–25 | Any ESP32 with WiFi works. WROOM-32 is the most common. Avoid ESP32-S2/S3 unless you check pin compatibility |
| 0.96" 128×64 SSD1306 OLED, I²C, 4-pin | [Core Electronics](https://core-electronics.com.au/) · [Little Bird](https://littlebirdelectronics.com.au/) · eBay AU | $8–12 | Must be **I²C** (4 pins: VCC GND SDA SCL), not SPI (7 pins). White or blue, both fine |
| 4× female-female jumper wires | Jaycar · Bunnings | $2 | 10 cm length is plenty |
| USB cable (matches your ESP32 board) | — | — | Usually micro-USB; newer boards are USB-C |
| 5 V / 2 A USB wall adapter | Anywhere | $10 (or use a phone charger you already have) | A weak adapter causes random reboots during polling |

## Optional

| Part | Where | Approx (AUD) | Why |
|---|---|---|---|
| Small project enclosure / 3D-printed case | Thingiverse files · Jaycar boxes | $5–20 | Makes it look tidy on a shelf |
| Right-angle USB cable | Core Electronics | $5 | Lets the board sit flush against a wall |

## What you do NOT need

- ❌ Raspberry Pi
- ❌ SD card
- ❌ LED matrix or HDMI display
- ❌ Soldering iron (assuming the OLED ships with headers pre-fitted — most do)
- ❌ FlightAware account (works fine without; key is opt-in only)

## Bundled-deal tip

Core Electronics often have an "ESP32 + OLED starter pack" around **$28 AUD shipped**, which is the easiest single-cart purchase. Little Bird Electronics is also reliable and ships from Sydney to Perth in 2–3 days.

## Before you order

Double-check your ESP32 board's I²C pins. The firmware defaults to **SDA = GPIO 21, SCL = GPIO 22**, which is correct for the standard WROOM-32 / NodeMCU-32S. Heltec boards use 4/15 — you'd edit one line in `src/Display.cpp`.
