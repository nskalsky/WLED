#pragma once
#include "wled.h"
#include <map>

#ifndef SKY_CFG_DIR
  #define SKY_CFG_DIR "/skyaware"
#endif
#ifndef SKY_MAP_PATH
  #define SKY_MAP_PATH "/skyaware/map.json"
#endif

enum SkyMapMode : uint8_t { SKY_MAP_CUSTOM = 0, SKY_MAP_PRESET = 1 };

struct SkyMapConfig {
  SkyMapMode mode = SKY_MAP_CUSTOM;
  String     preset;
};

// Externs your SkyAware.cpp should provide:
extern std::map<uint8_t, std::map<uint16_t, String>> segMap;
extern SkyMapConfig g_sa_cfg;

// Ensure dir exists
static inline bool sa_fsEnsureDir(const char* path) {
#ifdef WLED_FS_MKDIR
  if (!WLED_FS.exists(path)) return WLED_FS.mkdir(path);
  return true;
#else
  if (WLED_FS.exists(path)) return true;
  File f = WLED_FS.open(String(path) + "/.keep", "w");
  if (!f) return false;
  f.close();
  WLED_FS.remove(String(path) + "/.keep");
  return true;
#endif
}

static inline bool SA_SaveMapToFs() {
  if (!sa_fsEnsureDir(SKY_CFG_DIR)) return false;
  DynamicJsonDocument d(4096);
  JsonObject root = d.to<JsonObject>();
  JsonObject map  = root.createNestedObject("map");

  for (auto &segPair : segMap) {
    String line;
    uint16_t maxIdx = 0;
    for (auto &kv : segPair.second) if (kv.first > maxIdx) maxIdx = kv.first;
    for (uint16_t i=0; i<=maxIdx; i++) {
      if (i) line += ',';
      auto it = segPair.second.find(i);
      if (it != segPair.second.end()) line += it->second;
    }
    char key[6]; snprintf(key, sizeof(key), "%u", (unsigned)segPair.first);
    map[key] = line;
  }

  File f = WLED_FS.open(SKY_MAP_PATH, "w");
  if (!f) return false;
  bool ok = (serializeJson(d, f) > 0);
  f.close();
  return ok;
}

static inline bool SA_LoadMapFromFs() {
  if (!WLED_FS.exists(SKY_MAP_PATH)) return false;
  File f = WLED_FS.open(SKY_MAP_PATH, "r"); if (!f) return false;

  DynamicJsonDocument d(8192);
  DeserializationError e = deserializeJson(d, f);
  f.close();
  if (e) return false;

  segMap.clear();
  JsonObject map = d["map"].as<JsonObject>();
  for (JsonPair p : map) {
    uint8_t seg = (uint8_t) strtoul(p.key().c_str(), nullptr, 10);
    if (!p.value().is<const char*>()) continue;
    String csv = p.value().as<const char*>();
    std::map<uint16_t, String> inner;

    uint16_t idx = 0;
    int start = 0;
    while (start <= (int)csv.length()) {
      int comma = csv.indexOf(',', start);
      if (comma < 0) comma = csv.length();
      String ap = csv.substring(start, comma);
      ap.trim(); ap.toUpperCase();
      if (ap.length()) inner[idx] = ap;
      idx++; start = comma + 1;
      if (start > (int)csv.length()) break;
    }
    if (!inner.empty()) segMap[seg] = std::move(inner);
  }
  return true;
}
