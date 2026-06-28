// 128x64 SSD1306 OLED renderer.
//
// Two render modes:
//
//   CARD     - one aircraft, glance-able layout (big callsign + status strip).
//              Carousel rotates through nearby planes every N seconds.
//
//   TICKER   - continuous horizontal scroll, all planes on one line, barbershop
//              pole style:  "QF28 PER>SYD B738 FL360 120km   EK420 DXB>SYD ..."
//              Reads like an airport departures board.
//
// Pick the mode in the config portal.

#include <U8g2lib.h>
#include "Aircraft.h"

extern const String& settingsAeroApiKey();
extern bool          settingsAeroApiEnabled();

// Default I2C pins: SDA=21, SCL=22 on most ESP32 dev boards.
static U8G2_SSD1306_128X64_NONAME_F_HW_I2C g_oled(U8G2_R0, U8X8_PIN_NONE);

// --- Boot / message --------------------------------------------------------

void displayBegin() {
  g_oled.begin();
  g_oled.setBusClock(400000);
  g_oled.clearBuffer();
  g_oled.setFont(u8g2_font_helvB10_tr);
  g_oled.drawStr(0, 30, "FlightBoard");
  g_oled.setFont(u8g2_font_6x10_tr);
  g_oled.drawStr(0, 50, "Booting...");
  g_oled.sendBuffer();
}

static void drawCentered(const char* s, int y) {
  int w = g_oled.getStrWidth(s);
  int x = (128 - w) / 2;
  if (x < 0) x = 0;
  g_oled.drawStr(x, y, s);
}

void displayBoot(const char* status) {
  g_oled.clearBuffer();
  g_oled.setFont(u8g2_font_helvB10_tr);
  drawCentered("FlightBoard", 24);
  g_oled.setFont(u8g2_font_5x7_tr);
  drawCentered(status, 44);
  g_oled.sendBuffer();
}

void displayEmpty(const char* line1, const char* line2) {
  g_oled.clearBuffer();
  g_oled.setFont(u8g2_font_helvB10_tr);
  drawCentered(line1, 28);
  g_oled.setFont(u8g2_font_5x7_tr);
  if (line2) drawCentered(line2, 50);
  g_oled.sendBuffer();
}

// --- Card mode (one aircraft full-screen) ---------------------------------

static int g_scrollX = 0;
static unsigned long g_lastScrollMs = 0;

void displayAircraft(const Aircraft& a, const char* title) {
  g_oled.clearBuffer();

  g_oled.setFont(u8g2_font_5x7_tr);
  g_oled.drawStr(0, 7, title);
  // Show a small FA dot in the title bar if enriched, so user knows where the
  // extra detail came from.
  if (a.enriched) g_oled.drawBox(122, 1, 4, 4);
  g_oled.drawHLine(0, 9, 128);

  // Big callsign
  g_oled.setFont(u8g2_font_logisoso16_tr);
  const char* cs = a.callsign.length() ? a.callsign.c_str() : "-";
  int csWidth = g_oled.getStrWidth(cs);
  int csX;
  if (csWidth <= 128) {
    csX = (128 - csWidth) / 2;
  } else {
    unsigned long now = millis();
    if (now - g_lastScrollMs > 60) {
      g_scrollX -= 1;
      g_lastScrollMs = now;
      if (g_scrollX < -(csWidth + 20)) g_scrollX = 128;
    }
    csX = g_scrollX;
  }
  g_oled.drawStr(csX, 28, cs);

  // Route line if enriched, else type+reg
  g_oled.setFont(u8g2_font_5x7_tr);
  char meta[48] = "";
  if (a.enriched && a.origin.length() && a.dest.length()) {
    // PER>SYD  B738
    snprintf(meta, sizeof(meta), "%s > %s",
             a.origin.c_str(), a.dest.c_str());
  } else if (a.type.length() && a.reg.length()) {
    snprintf(meta, sizeof(meta), "%s %s", a.type.c_str(), a.reg.c_str());
  } else if (a.type.length()) {
    snprintf(meta, sizeof(meta), "%s", a.type.c_str());
  } else if (a.reg.length()) {
    snprintf(meta, sizeof(meta), "%s", a.reg.c_str());
  }
  if (meta[0]) drawCentered(meta, 41);

  // Status strip
  g_oled.drawHLine(0, 46, 128);
  g_oled.setFont(u8g2_font_6x10_tr);

  char alt[16], spd[16], dst[16];
  if (a.on_ground) snprintf(alt, sizeof(alt), "GROUND");
  else if (a.alt_ft >= 18000) snprintf(alt, sizeof(alt), "FL%d", a.alt_ft / 100);
  else if (a.alt_ft > 0) snprintf(alt, sizeof(alt), "%dft", a.alt_ft);
  else snprintf(alt, sizeof(alt), "-");

  if (a.speed_kt > 0) snprintf(spd, sizeof(spd), "%dkt", a.speed_kt);
  else snprintf(spd, sizeof(spd), "-");

  if (a.dist_km > 0) snprintf(dst, sizeof(dst), "%.0fkm", a.dist_km);
  else snprintf(dst, sizeof(dst), "");

  g_oled.drawStr(0, 58, alt);
  int spdW = g_oled.getStrWidth(spd);
  g_oled.drawStr((128 - spdW) / 2, 58, spd);
  int dstW = g_oled.getStrWidth(dst);
  g_oled.drawStr(128 - dstW, 58, dst);

  g_oled.sendBuffer();
}

// --- Ticker mode (continuous horizontal scroll) ---------------------------
//
// We build one long string from all aircraft and scroll it leftward across
// the screen. When it disappears off the left edge, the next aircraft's text
// is already visible behind it -- giving the seamless barbershop-pole effect.
//
// The string is regenerated when the underlying data changes (different
// aircraft list, or new enrichment); otherwise we just shift the X offset.

#include "AirportMap.h"

static String   g_tickerText;
static int      g_tickerX = 128;     // pixel offset; starts off-right
static int      g_tickerW = 0;       // width of the rendered string in pixels
static unsigned long g_lastTickerStepMs = 0;
static String   g_tickerSig;         // signature of the last data we built from

// Build the ticker string from an aircraft list, plus a signature so we know
// when to rebuild.
static String buildTicker(const Aircraft list[], int count) {
  String s = "";
  for (int i = 0; i < count; ++i) {
    const Aircraft& a = list[i];
    String cs = a.callsign; cs.trim();
    if (cs.length() == 0) cs = "----";
    s += cs;

    if (a.enriched && a.origin.length() && a.dest.length()) {
      s += "  ";
      s += icaoToIata(a.origin);
      s += ">";
      s += icaoToIata(a.dest);
    }
    if (a.type.length()) { s += "  "; s += a.type; }

    s += "  ";
    if (a.on_ground) s += "GND";
    else if (a.alt_ft >= 18000) { s += "FL"; s += String(a.alt_ft / 100); }
    else if (a.alt_ft > 0)       { s += String(a.alt_ft); s += "ft"; }

    if (a.dist_km > 0) {
      s += "  ";
      s += String((int)(a.dist_km + 0.5f));
      s += "km";
    }

    if (i < count - 1) s += "   *   ";   // separator between flights
  }
  s += "        ";   // tail gap so the loop reads cleanly
  return s;
}

static String tickerSignature(const Aircraft list[], int count) {
  String sig = "";
  for (int i = 0; i < count; ++i) {
    sig += list[i].callsign;
    sig += list[i].enriched ? "1" : "0";
    sig += String(list[i].alt_ft);
    sig += "|";
  }
  return sig;
}

void displayTicker(const Aircraft list[], int count,
                   const char* header, int scrollPxPerStep) {
  // Top header (small)
  g_oled.clearBuffer();
  g_oled.setFont(u8g2_font_5x7_tr);
  g_oled.drawStr(0, 7, header);
  g_oled.drawHLine(0, 9, 128);

  if (count == 0) {
    g_oled.setFont(u8g2_font_helvB10_tr);
    drawCentered("No aircraft", 36);
    g_oled.setFont(u8g2_font_5x7_tr);
    drawCentered("waiting for data...", 56);
    g_oled.sendBuffer();
    return;
  }

  // Rebuild ticker text if data changed
  String sig = tickerSignature(list, count);
  if (sig != g_tickerSig) {
    g_tickerSig = sig;
    g_tickerText = buildTicker(list, count);
    g_oled.setFont(u8g2_font_inb16_mr);   // INconsolata bold 16px -- great for tickers
    g_tickerW = g_oled.getStrWidth(g_tickerText.c_str());
    g_tickerX = 128;   // restart from right edge
  }

  // Scroll
  unsigned long now = millis();
  if (now - g_lastTickerStepMs >= 30) {     // step every 30ms
    g_lastTickerStepMs = now;
    g_tickerX -= scrollPxPerStep;
    if (g_tickerX <= -g_tickerW) g_tickerX = 128;
  }

  // Draw the big scrolling text
  g_oled.setFont(u8g2_font_inb16_mr);
  // Draw twice: once at g_tickerX, once at g_tickerX + g_tickerW, so the loop
  // is seamless (barbershop effect).
  g_oled.drawStr(g_tickerX, 36, g_tickerText.c_str());
  g_oled.drawStr(g_tickerX + g_tickerW, 36, g_tickerText.c_str());

  // Bottom status strip: aircraft count + AeroAPI badge
  g_oled.setFont(u8g2_font_5x7_tr);
  g_oled.drawHLine(0, 48, 128);
  char foot[32];
  snprintf(foot, sizeof(foot), "%d aircraft", count);
  g_oled.drawStr(0, 58, foot);

  if (settingsAeroApiEnabled() && settingsAeroApiKey().length()) {
    g_oled.drawStr(128 - g_oled.getStrWidth("FA on"), 58, "FA on");
  }

  g_oled.sendBuffer();
}
