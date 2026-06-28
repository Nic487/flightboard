// IATA -> ICAO airline callsign prefix lookup table.
//
// ADS-B transmits the ICAO callsign (QFA123), not the IATA boarding-pass
// code (QF123). This table lets a user type "QF9" and have the firmware
// also try "QFA9" against the adsb.lol /callsign/ endpoint.
//
// IATA airline code -> ICAO callsign prefix (e.g. QF -> QFA).
// ADS-B transmits ICAO callsigns (QFA9), users type IATA (QF9). We translate both.
//
// Stored in PROGMEM so it doesn't eat precious RAM on the ESP32.

#pragma once
#include <Arduino.h>
#include <pgmspace.h>

struct CallsignMapping { const char iata[3]; const char icao[4]; };

static const CallsignMapping CALLSIGN_MAP[] PROGMEM = {
  // Australian + Oceania
  {"QF","QFA"},{"JQ","JST"},{"VA","VOZ"},{"TT","TGW"},{"ZL","RXA"},
  {"NZ","ANZ"},{"FJ","FJI"},{"PX","ANG"},{"IE","SOL"},
  // Asia-Pacific
  {"SQ","SIA"},{"MI","SLK"},{"TR","SCO"},{"CX","CPA"},{"KA","HDA"},
  {"UO","HKE"},{"HX","CRK"},{"CI","CAL"},{"BR","EVA"},{"JX","STR"},
  {"JL","JAL"},{"NH","ANA"},{"KE","KAL"},{"OZ","AAR"},
  {"TG","THA"},{"PG","BKP"},{"VZ","VTV"},{"VJ","VJC"},{"VN","HVN"},
  {"MH","MAS"},{"OD","MXD"},{"AK","AXM"},{"D7","XAX"},{"FD","AIQ"},
  {"GA","GIA"},{"JT","LNI"},{"SJ","SJY"},{"QG","CTV"},{"ID","BTK"},
  {"CZ","CSN"},{"MU","CES"},{"CA","CCA"},{"HU","CHH"},{"MF","CXA"},
  {"ZH","CSZ"},{"GS","GCR"},{"SC","CDG"},
  {"AI","AIC"},{"UK","VTI"},{"SG","SEJ"},{"QP","AKJ"},
  // Middle East
  {"EK","UAE"},{"EY","ETD"},{"QR","QTR"},{"SV","SVA"},{"GF","GFA"},
  {"WY","OMA"},{"KU","KAC"},{"RJ","RJA"},{"ME","MEA"},{"TK","THY"},
  {"PC","PGT"},
  // Europe
  {"BA","BAW"},{"VS","VIR"},{"U2","EZY"},{"FR","RYR"},{"LH","DLH"},
  {"LX","SWR"},{"OS","AUA"},{"SN","BEL"},{"EW","EWG"},
  {"AF","AFR"},{"KL","KLM"},{"AY","FIN"},{"SK","SAS"},{"IB","IBE"},
  {"UX","AEA"},{"VY","VLG"},{"AZ","ITY"},{"AT","RAM"},{"TP","TAP"},
  {"EI","EIN"},{"LO","LOT"},{"OK","CSA"},{"RO","ROT"},
  {"A3","AEE"},{"OA","OAL"},{"PS","AUI"},{"SU","AFL"},{"S7","SBI"},
  // Americas
  {"AA","AAL"},{"DL","DAL"},{"UA","UAL"},{"WN","SWA"},{"AS","ASA"},
  {"B6","JBU"},{"NK","NKS"},{"F9","FFT"},{"G4","AAY"},{"HA","HAL"},
  {"AC","ACA"},{"WS","WJA"},{"TS","TSC"},
  {"AM","AMX"},{"Y4","VOI"},
  {"LA","LAN"},{"AR","ARG"},{"AV","AVA"},{"AD","AZU"},{"JJ","TAM"},
  {"G3","GLO"},{"CM","CMP"},
  // Africa
  {"ET","ETH"},{"KQ","KQA"},{"SA","SAA"},{"MK","MAU"},{"MS","MSR"},
  // Cargo
  {"FX","FDX"},{"5X","UPS"},{"CV","CLX"},{"CK","CKK"},{"PO","PAC"},
  {"QY","BCS"},
};

// Build all the variants the firmware should try for a user-entered code.
// Returns up to 4 candidates. `out` must be sized for that.
inline int expandCallsign(const String& input, String out[4]) {
  int n = 0;
  String cs = input;
  cs.trim();
  cs.toUpperCase();
  // Strip whitespace
  String clean;
  for (size_t i = 0; i < cs.length(); ++i) if (cs[i] != ' ') clean += cs[i];
  if (clean.length() == 0) return 0;
  out[n++] = clean;

  // Strip leading zeros after a 2-char prefix (QF0123 -> QF123)
  if (clean.length() > 3 && isAlpha(clean[0]) && (isAlpha(clean[1]) || isDigit(clean[1]))) {
    String prefix = clean.substring(0, 2);
    String rest = clean.substring(2);
    while (rest.length() > 1 && rest[0] == '0') rest.remove(0, 1);
    String stripped = prefix + rest;
    if (stripped != clean && n < 4) out[n++] = stripped;
  }

  // IATA -> ICAO swap (QF123 -> QFA123)
  if (clean.length() >= 3) {
    String prefix = clean.substring(0, 2);
    String rest = clean.substring(2);
    if (rest.length() > 0 && isDigit(rest[0])) {
      char iata[3] = { prefix[0], prefix[1], 0 };
      char icaoBuf[4];
      for (size_t i = 0; i < sizeof(CALLSIGN_MAP) / sizeof(CALLSIGN_MAP[0]); ++i) {
        strncpy_P(icaoBuf, CALLSIGN_MAP[i].icao, 4);
        char iataPgm[3];
        strncpy_P(iataPgm, CALLSIGN_MAP[i].iata, 3);
        if (strcmp(iata, iataPgm) == 0) {
          String stripped = rest;
          while (stripped.length() > 1 && stripped[0] == '0') stripped.remove(0, 1);
          String candidate = String(icaoBuf) + stripped;
          if (n < 4) out[n++] = candidate;
          break;
        }
      }
    }
  }
  return n;
}
