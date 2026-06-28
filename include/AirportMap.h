// ICAO -> IATA airport lookup for friendlier ticker display.
//
// AeroAPI returns ICAO codes (YPPH, YSSY, KLAX). The ticker reads better
// with the 3-letter IATA equivalent (PER, SYD, LAX). This table covers
// the ~80 busiest airports an Australian viewer is likely to see.
//
// On ESP32, const data is in flash anyway -- no PROGMEM dance needed.

#pragma once
#include <Arduino.h>

struct AirportPair { const char* icao; const char* iata; };

static const AirportPair AIRPORTS[] = {
  // Australia
  {"YPPH","PER"},{"YSSY","SYD"},{"YMML","MEL"},{"YBBN","BNE"},{"YPAD","ADL"},
  {"YPDN","DRW"},{"YBCS","CNS"},{"YBCG","OOL"},{"YBHM","HBA"},{"YPLM","LST"},
  {"YBRK","ROK"},{"YBMK","MKY"},{"YBTL","TSV"},{"YBNA","BNK"},{"YPKG","KGI"},
  {"YPKA","KTA"},{"YPED","EDR"},{"YBAS","ASP"},{"YBHI","BHQ"},{"YSCB","CBR"},
  {"YMHB","HBA"},{"YBNS","NSO"},{"YPGV","GOV"},{"YBKT","BKQ"},{"YBPN","PPP"},
  // NZ
  {"NZAA","AKL"},{"NZWN","WLG"},{"NZCH","CHC"},{"NZQN","ZQN"},{"NZDN","DUD"},
  // Asia-Pacific hubs
  {"WSSS","SIN"},{"VHHH","HKG"},{"RJTT","HND"},{"RJAA","NRT"},{"RKSI","ICN"},
  {"ZBAA","PEK"},{"ZSPD","PVG"},{"ZGGG","CAN"},{"VTBS","BKK"},{"WMKK","KUL"},
  {"WIII","CGK"},{"RPLL","MNL"},{"VVTS","SGN"},{"VNKT","KTM"},{"VECC","CCU"},
  {"VABB","BOM"},{"VIDP","DEL"},{"VTSP","HKT"},
  // Middle East
  {"OMDB","DXB"},{"OMAA","AUH"},{"OTHH","DOH"},{"OERK","RUH"},{"OEJN","JED"},
  // Europe
  {"EGLL","LHR"},{"EGKK","LGW"},{"EHAM","AMS"},{"LFPG","CDG"},{"EDDF","FRA"},
  {"EDDM","MUC"},{"LEMD","MAD"},{"LIRF","FCO"},{"LSZH","ZRH"},{"EKCH","CPH"},
  {"ESSA","ARN"},{"LOWW","VIE"},{"EBBR","BRU"},{"LTBA","IST"},{"LTFM","IST"},
  // North America
  {"KLAX","LAX"},{"KSFO","SFO"},{"KSEA","SEA"},{"KORD","ORD"},{"KJFK","JFK"},
  {"KATL","ATL"},{"KDFW","DFW"},{"KIAH","IAH"},{"KDEN","DEN"},{"KMIA","MIA"},
  {"KPHX","PHX"},{"KLAS","LAS"},{"KBOS","BOS"},{"CYYZ","YYZ"},{"CYVR","YVR"},
  // South Pacific & other
  {"NFFN","NAN"},{"NSFA","APW"},{"NTAA","PPT"},{"FAOR","JNB"},{"FACT","CPT"},
};
static const size_t AIRPORTS_COUNT = sizeof(AIRPORTS) / sizeof(AIRPORTS[0]);

// Returns IATA if known, else returns the original ICAO string.
inline String icaoToIata(const String& icao) {
  if (icao.length() != 4) return icao;
  for (size_t i = 0; i < AIRPORTS_COUNT; ++i) {
    if (icao.equals(AIRPORTS[i].icao)) return String(AIRPORTS[i].iata);
  }
  return icao;
}
