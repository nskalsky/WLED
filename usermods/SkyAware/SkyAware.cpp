#include "wled.h"
#include <HTTPClient.h>
#include <WiFiClientSecure.h>
#include <stdarg.h>
#include <vector>
#if defined(ARDUINO_ARCH_ESP32)
  #include <WiFi.h>
#else
  #include <ESP8266WiFi.h>
#endif

// lwIP DNS access
extern "C" {
  #include "lwip/dns.h"
  #include "lwip/ip_addr.h"
  #include "lwip/ip4_addr.h"
}

#include "skyaware_html.h"   // UI HTML (PROGMEM)

// ======================= Config toggles =======================
#ifndef SA_FORCE_DNS_ONCE_PER_WINDOW
  #define SA_FORCE_DNS_ONCE_PER_WINDOW 1
#endif

#ifndef SKYAWARE_LOG_CAP
  #define SKYAWARE_LOG_CAP (9 * 1024)
#endif

// ---------- SkyAware multi-fetch config & helpers (file-scope) ----------
namespace {
  constexpr size_t SKY_MULTI_BATCH   = 12;    // ICAOs per HTTP request
  constexpr size_t SKY_JSON_DOC_SIZE = 2048;  // ArduinoJson capacity per batch

  // Build the ArduinoJson filter we use for METAR array items
  static inline void buildMetarFilter(StaticJsonDocument<256>& filter) {
    JsonArray farr = filter.to<JsonArray>();
    JsonObject any = farr.createNestedObject();
    // Station identifiers (endpoint can vary field names)
    any["icaoId"] = true; any["station"] = true; any["station_id"] = true;
    any["icao"]   = true; any["id"]      = true;
    // Flight category variants
    any["fltCat"] = true; any["flight_category"] = true; any["fltcat"] = true;
  }

  // Join a slice of airports into a CSV for ?ids=...
  static inline String joinIdsCsv(const std::vector<String>& airports, size_t start, size_t count) {
    String ids;
    for (size_t j = 0; j < count; ++j) {
      if (j) ids += ',';
      ids += airports[start + j];
    }
    return ids;
  }
} // namespace

// ---- SkyAware FS + JSON persistence helpers ----
#ifndef SKY_CFG_DIR
  #define SKY_CFG_DIR "/skyaware"
#endif
#ifndef SKY_CFG_CAP
  #define SKY_CFG_CAP 16384
#endif

static bool sa_fsEnsureDir(const char* path) {
#ifdef WLED_USE_FS
  if (!WLED_FS.exists(path)) return WLED_FS.mkdir(path);
  return true;
#else
  return false;
#endif
}

static constexpr const char* SKY_CFG_FILE = SKY_CFG_DIR "/mapping.json";

static bool saveSkyAwareFile(JsonObject src) {
#ifdef WLED_USE_FS
  if (!sa_fsEnsureDir(SKY_CFG_DIR)) return false;
  File f = WLED_FS.open(SKY_CFG_FILE, "w");
  if (!f) return false;
  serializeJson(src, f);
  f.close();
  return true;
#else
  (void)src;
  return false;
#endif
}

static bool loadSkyAwareFile(JsonDocument& doc) {
#ifdef WLED_USE_FS
  if (!WLED_FS.exists(SKY_CFG_FILE)) return false;
  File f = WLED_FS.open(SKY_CFG_FILE, "r");
  if (!f) return false;
  DeserializationError e = deserializeJson(doc, f);
  f.close();
  return !e;
#else
  (void)doc;
  return false;
#endif
}




// ----------------------- ring log -----------------------------
struct SA_RingLog {
  static const size_t CAP = SKYAWARE_LOG_CAP;
  char    buf[CAP];
  size_t  head = 0;
  size_t  size = 0;
  void clear() { head = 0; size = 0; }
  void write(const char* s, size_t n){
    for(size_t i=0;i<n;i++){
      buf[(head + size) % CAP] = s[i];
      if (size < CAP) size++;
      else head = (head + 1) % CAP;
    }
  }
  void dumpTo(Print& p) const {
    for (size_t i=0; i<size; i++){
      p.print(buf[(head + i) % CAP]);
    }
  }
};
static SA_RingLog SA_LOGBUF;

static void SA_MEMLOGF(const char* fmt, ...) {
  char tmp[256];
  va_list ap; va_start(ap, fmt);
  int n = vsnprintf(tmp, sizeof(tmp), fmt, ap);
  va_end(ap);
  if (n <= 0) return;
  if (n >= (int)sizeof(tmp)) n = sizeof(tmp) - 1;
  SA_LOGBUF.write(tmp, (size_t)n);
}

#ifdef WLED_DEBUG
  #define SA_DBG(fmt, ...) do { \
      SA_MEMLOGF("[%9lu] [SkyAware] " fmt, millis(), ##__VA_ARGS__); \
      Serial.printf("[SkyAware] " fmt, ##__VA_ARGS__); \
    } while(0)
#else
  #define SA_DBG(fmt, ...) do { SA_MEMLOGF("[%9lu] [SkyAware] " fmt, millis(), ##__VA_ARGS__); } while(0)
#endif

#ifndef USERMOD_ID_SKYAWARE
  #define USERMOD_ID_SKYAWARE 0x5CA7
#endif

// ======================= Helpers ==============================
static inline String upperTrim(String s) { s.trim(); s.toUpperCase(); return s; }
static inline bool isAlphaNumDash(char c) { return (c>='A'&&c<='Z')||(c>='0'&&c<='9')||c=='-'||c=='_'; }
static inline bool timeReached(uint32_t t) { return (int32_t)(millis() - t) >= 0; }

// ============================================================
//                     SkyAware Usermod (WLED 0.16.x)
// ============================================================
class SkyAwareUsermod : public Usermod {
public:
  enum MapMode : uint8_t { MAP_SINGLE = 0, MAP_MULTIPLE = 1 };

  enum LedType : uint8_t { LT_SKIP = 0, LT_ICAO = 1, LT_INDICATOR = 2 };

  struct LedEntry { LedType type = LT_SKIP; String value; };
  struct SegmentMap {
    MapMode mode = MAP_SINGLE;
    String  wholeAirport;
    std::vector<LedEntry> perLed;
  };

  // ---- persistence buffer & helpers ----
  StaticJsonDocument<SKY_CFG_CAP> _fileDoc;
  bool saveToFile() {
    _fileDoc.clear();
    JsonObject root = _fileDoc.to<JsonObject>();
    JsonObject top = root.createNestedObject("skyAwareUsermod");
    top["Enabled"]                = enabled;
    top["Auto Fetch on Boot"]     = autoFetchBoot;
    top["Airport ID"]             = airportId;
    top["Update Frequency (min)"] = updateInterval;
    JsonArray maps = top.createNestedArray("Mappings");
    uint16_t numSegs = strip.getSegmentsNum();
    for (uint16_t si=0; si<numSegs; ++si) {
      JsonObject m = maps.createNestedObject();
      m["Segment"] = si;
      m["Mode"]    = modeLabel(segMaps[si].mode);
      if (segMaps[si].mode == MAP_SINGLE) {
        m["Airport"] = segMaps[si].wholeAirport;
      } else {
        Segment& seg = strip.getSegment(si);
        uint16_t start = seg.start, stop = seg.stop;
        uint16_t len = (stop>start)?(stop-start):0;
        if (segMaps[si].perLed.size()!=len) segMaps[si].perLed.resize(len);
        String csv;
        for (uint16_t i=0;i<len;i++){
          if (i) csv += ",";
          const LedEntry& le = segMaps[si].perLed[i];
          if (le.type==LT_SKIP)      csv += "-";
          else if (le.type==LT_ICAO) csv += le.value;
          else                       csv += "-";
        }
        m["CSV"] = csv;
      }
    }
    return saveSkyAwareFile(root);
  }
  bool loadFromFile() {
    _fileDoc.clear();
    if (!loadSkyAwareFile(_fileDoc)) return false;
    JsonObject root = _fileDoc.as<JsonObject>();
    return readFromConfig(root);
  }

  
  // (duplicate persistence block removed)
private:
  // persisted base config
  bool     enabled        = false;
  bool     autoFetchBoot  = false;
  String   airportId      = "KHIO";
  uint16_t updateInterval = 5;     // minutes (>=1)
  static const uint32_t RETRY_DELAYS_MS[4];
  static const size_t   RETRY_COUNT = 4;

  // mapping
  std::vector<SegmentMap> segMaps;

  // runtime state
  unsigned long lastRunMs        = 0;
  bool          busy             = false;
  bool          refreshNow       = false;

  String lastMetarRaw;
  String metarStation, metarTimeZ, metarWind, metarVis, metarClouds, metarTempDew, metarAltim, metarWx, metarRemarks;

  String        lastCat          = "UNKNOWN";
  int           lastHttp         = 0;
  String        lastErr;
  String        lastUrl;
  String        lastBodyPreview;
  unsigned long lastOkMs         = 0;
  unsigned long lastFetchStartMs = 0;
  unsigned long lastFetchDurMs   = 0;
  uint32_t      successCount     = 0;
  uint32_t      failureCount     = 0;

  // boot settle
  bool          bootFetchPending   = false;
  bool          wasWifiConnected   = false;
  unsigned long wifiConnectedSince = 0;
  static constexpr uint32_t BOOT_WIFI_SETTLE_MS = 5000;

  // retry scheduler
  uint32_t nextScheduledAt = 0;
  uint32_t windowDeadline  = 0;
  uint32_t nextAttemptAt   = 0;
  size_t   retryIndex      = 0;
  bool     inRetryWindow   = false;
  uint32_t prevPeriodMs    = 0;

  // per-window DNS forcing
  bool     forcedDnsThisWindow = false;

  // LED identification state (for multi-airport LED identification feature)
  struct LedIdentifier {
    bool active = false;
    uint16_t ledIndex = 0;
    String originalCategory;
  } ledIdent;

  // per-airport cache
  struct ApCache { String id, cat; bool good=false; unsigned long okMs=0; };
  std::vector<ApCache> apCache;

  // network diag
  struct NetDiag {
    String host, ip, redirect;
    int    httpCode = 0;
    int    bytes = 0;
    uint32_t dnsMs=0, tcpMs=0, tlsMs=0, httpMs=0;
    String  errStage, errDetail;
    bool    ok = false;
    String  dnsProvider;  // "system", "dns.google", ...
    bool    dnsFallbackUsed = false;
    void clear(){*this = NetDiag();}
  } lastDiag;

  // ---------- schedule ----------
  uint32_t periodMs() const { return (uint32_t)max<uint16_t>(1, updateInterval) * 60000UL; }

  void initOrRealignSchedule(bool startImmediate=false) {
    const uint32_t now = millis();
    const uint32_t period = periodMs();
    if (nextScheduledAt == 0 || prevPeriodMs != period) {
      prevPeriodMs = period;
      nextScheduledAt = now + period;
    }
    while (timeReached(nextScheduledAt)) nextScheduledAt += period;
    windowDeadline = nextScheduledAt;
    nextAttemptAt  = startImmediate ? now : nextScheduledAt;
    inRetryWindow  = false;
    retryIndex     = 0;
    forcedDnsThisWindow = false;
  }

  // ---------- color mapping ----------
  static uint32_t colorForCategory(const String& cat) {
    if (cat.equalsIgnoreCase("VFR"))   return RGBW32(  0,255,  0,0);
    if (cat.equalsIgnoreCase("MVFR"))  return RGBW32(  0,  0,255,0);
    if (cat.equalsIgnoreCase("IFR"))   return RGBW32(255,  0,  0,0);
    if (cat.equalsIgnoreCase("LIFR"))  return RGBW32(255,  0,255,0);
    if (cat.equalsIgnoreCase("IDENT")) return RGBW32(  0,255,255,0);  // Cyan - unique identifier color
    return RGBW32(0,0,0,0);
  }

  String getCatForAirport(const String& ap) {
    String a = ap; a.trim(); a.toUpperCase();
    for (auto& e : apCache) if (e.id == a) return e.good ? e.cat : String("UNKNOWN");
    return "UNKNOWN";
  }
  void setCatForAirport(const String& ap, const String& cat, bool good) {
    String a = ap; a.trim(); a.toUpperCase();
    for (auto& e : apCache) if (e.id == a) { e.cat = cat; e.good = good && !cat.equalsIgnoreCase("UNKNOWN"); e.okMs = e.good ? millis() : 0; return; }
    ApCache e; e.id=a; e.cat=cat; e.good=good && !cat.equalsIgnoreCase("UNKNOWN"); e.okMs = e.good ? millis() : 0; apCache.push_back(e);
  }

  // ===== LED OVERRIDE SYSTEM (Direct Per-LED Control) =====
  // This allows arbitrary control of individual LEDs without affecting airport mappings
  struct LedOverride {
    uint16_t ledIndex;
    uint32_t color;
    unsigned long activeSince;
    unsigned long duration;  // 0 = indefinite
    
    bool isExpired() const {
      if (duration == 0) return false;
      return (millis() - activeSince) >= duration;
    }
  };
  
  std::vector<LedOverride> ledOverrides;
  
  // Add or replace an LED override
  void setLedOverride(uint16_t ledIndex, uint32_t color, unsigned long durationMs = 0) {
    if (ledIndex >= strip.getLength()) {
      SA_DBG("setLedOverride: invalid LED index %u (max %u)\n", 
             ledIndex, strip.getLength() - 1);
      return;
    }
    
    // Check if override already exists for this LED
    for (size_t i = 0; i < ledOverrides.size(); ++i) {
      if (ledOverrides[i].ledIndex == ledIndex) {
        // Update existing
        ledOverrides[i].color = color;
        ledOverrides[i].activeSince = millis();
        ledOverrides[i].duration = durationMs;
        SA_DBG("setLedOverride: updated LED %u to 0x%08x (%lu ms)\n", 
               ledIndex, color, durationMs);
        return;
      }
    }
    
    // Add new override
    ledOverrides.push_back({
      ledIndex,
      color,
      millis(),
      durationMs
    });
    
    SA_DBG("setLedOverride: new LED %u set to 0x%08x (%lu ms)\n", 
           ledIndex, color, durationMs);
  }
  
  // Remove override for a specific LED
  void clearLedOverride(uint16_t ledIndex) {
    for (size_t i = 0; i < ledOverrides.size(); ++i) {
      if (ledOverrides[i].ledIndex == ledIndex) {
        SA_DBG("clearLedOverride: LED %u cleared\n", ledIndex);
        ledOverrides.erase(ledOverrides.begin() + i);
        return;
      }
    }
  }
  
  // Remove all overrides
  void clearAllLedOverrides() {
    size_t count = ledOverrides.size();
    ledOverrides.clear();
    if (count > 0) {
      SA_DBG("clearAllLedOverrides: cleared %u overrides\n", count);
    }
  }
  
  // Apply active LED overrides to the display
  void applyLedOverrides() {
    if (ledOverrides.empty()) return;
    
    std::vector<size_t> expired;
    
    for (size_t i = 0; i < ledOverrides.size(); ++i) {
      LedOverride& ov = ledOverrides[i];
      
      // Check expiration
      if (ov.isExpired()) {
        expired.push_back(i);
        SA_DBG("applyLedOverrides: LED %u expired\n", ov.ledIndex);
        continue;
      }
      
      // Apply override color
      strip.setPixelColor(ov.ledIndex, ov.color);
    }
    
    // Remove expired overrides (iterate backwards to preserve indices)
    for (int i = (int)expired.size() - 1; i >= 0; --i) {
      ledOverrides.erase(ledOverrides.begin() + expired[i]);
    }
    
    if (!expired.empty()) {
      SA_DBG("applyLedOverrides: removed %u expired\n", expired.size());
    }
  }
  
  // New LED identification using override system
  void startLedIdentification(uint16_t ledIndex, unsigned long durationMs = 30000) {
    if (ledIndex >= strip.getLength()) {
      SA_DBG("startLedIdentification: invalid LED index %u (max %u)\n", 
             ledIndex, strip.getLength() - 1);
      return;
    }
    
    uint32_t cyanColor = RGBW32(0, 255, 255, 0);
    setLedOverride(ledIndex, cyanColor, durationMs);
    
    ledIdent.active = true;
    ledIdent.ledIndex = ledIndex;
    
    SA_DBG("startLedIdentification: LED %u cyan override (%lu ms)\n", 
           ledIndex, durationMs);
  }
  
  // Stop identifying a specific LED
  void stopLedIdentification() {
    if (!ledIdent.active) return;
    
    clearLedOverride(ledIdent.ledIndex);
    ledIdent.active = false;
    
    SA_DBG("stopLedIdentification: stopped LED %u\n", ledIdent.ledIndex);
  }
  
  // Update blinking effect for identified LEDs
  void updateLedIdentificationBlink() {
    static unsigned long lastBlink = 0;
    static bool blinkOn = true;
    
    // Toggle every 250ms for ~2Hz effect
    if (millis() - lastBlink >= 250) {
      lastBlink = millis();
      blinkOn = !blinkOn;
    }
    
    if (!blinkOn) {
      // Turn off identified LEDs temporarily
      for (const auto& ov : ledOverrides) {
        strip.setPixelColor(ov.ledIndex, 0);
      }
      strip.trigger();
    }
  }
  
  // ===== END LED OVERRIDE SYSTEM =====

  // ---------- segment helpers ----------
  // Return segment length; if invalid (start>=stop), fall back to the whole strip and signal fallback.
  static inline uint16_t safeSegLen(const Segment& seg, bool& usedFallback) {
    if (seg.stop > seg.start) { usedFallback = false; return (uint16_t)(seg.stop - seg.start); }
    usedFallback = true;
    return strip.getLength();
  }

  // Paint MULTIPLE segment using segment-local indexing.
  void paintMultiSegment(Segment& seg, const SegmentMap& sm, const String& fallbackAp) {
    seg.setOption(SEG_OPTION_FREEZE, true);
    uint16_t len = (seg.stop > seg.start) ? (seg.stop - seg.start) : 0;
    for (uint16_t i = 0; i < len; i++) seg.setPixelColor(i, 0);

    for (uint16_t i = 0; i < len; i++) {
      uint32_t col = 0;
      const LedEntry& le = sm.perLed[i];
      if (le.type == LT_ICAO) {
        const String& ap = le.value.length() ? le.value : fallbackAp;
        col = colorForCategory(getCatForAirport(ap));
      }
      seg.setPixelColor(i, col);
    }
  }

  void applyLayoutColors() {
    uint16_t numSegs = strip.getSegmentsNum();
    if (segMaps.size() < numSegs) syncSegMapSize();

    for (uint16_t si=0; si<numSegs; ++si) {
      Segment& seg = strip.getSegment(si);
      SegmentMap& sm = segMaps[si];

      seg.setOption(SEG_OPTION_ON, true);
      seg.setMode(FX_MODE_STATIC);

      if (sm.mode == MAP_SINGLE) {
        seg.setOption(SEG_OPTION_FREEZE, false);
        const String ap = sm.wholeAirport.length() ? sm.wholeAirport : airportId;
        seg.setColor(0, colorForCategory(getCatForAirport(ap)));
      } else {
        uint16_t start = seg.start, stop = seg.stop;
        uint16_t len = (stop > start) ? (stop - start) : 0;
        if (sm.perLed.size() != len) segMaps[si].perLed.resize(len);
        paintMultiSegment(seg, sm, airportId);
      }
    }

       // Apply LED overrides AFTER normal rendering
    applyLedOverrides();
    
    strip.trigger();
    
 
  }

  // ---------- JSON helpers ----------
  static String readFlightCategory(JsonObject o) {
    if (!o.isNull()) {
      if (o.containsKey("fltCat"))          return String((const char*)o["fltCat"]);
      if (o.containsKey("flight_category")) return String((const char*)o["flight_category"]);
      if (o.containsKey("fltcat"))          return String((const char*)o["fltcat"]);
    }
    return "UNKNOWN";
  }
  static String readStation(JsonObject o) {
    const char* keys[] = {"icaoId","station","station_id","icao","id"};
    for (size_t i=0;i<sizeof(keys)/sizeof(keys[0]);++i) if (o.containsKey(keys[i])) return String((const char*)o[keys[i]]);
    return "";
  }
  static String readMetarRaw(JsonObject o) {
    const char* keys[] = {"rawOb","raw_text","raw","metar","raw_ob","metar_text"};
    for (size_t i=0;i<sizeof(keys)/sizeof(keys[0]);++i) if (o.containsKey(keys[i])) return String((const char*)o[keys[i]]);
    return "";
  }

  static bool isAllDigits(const String& s){ for (size_t i=0;i<s.length();++i) if (s[i]<'0'||s[i]>'9') return false; return s.length()>0; }
  static bool looksLikeTimeZ(const String& tok){ if(!tok.endsWith("Z")) return false; String c=tok.substring(0,tok.length()-1); return c.length()==6&&isAllDigits(c); }
  static bool looksLikeWind(const String& tok){ if(!tok.endsWith("KT")) return false; if(tok.startsWith("VRB")) return true; if(tok.length()<7) return false; return isAllDigits(tok.substring(0,3)); }
  static bool looksLikeVis(const String& tok){ return tok.endsWith("SM"); }
  static bool looksLikeCloud(const String& tok){ if(tok.startsWith("VV")) return tok.length()>=4&&isAllDigits(tok.substring(2,5)); if(tok.startsWith("FEW")||tok.startsWith("SCT")||tok.startsWith("BKN")||tok.startsWith("OVC")) return true; return false; }
  static bool looksLikeTempDew(const String& tok){
    int s = tok.indexOf('/'); if (s<=0 || s>=(int)tok.length()-1) return false;
    auto ok=[&](String x){ if(x.startsWith("M")) x.remove(0,1); return x.length()>=1&&x.length()<=2&&isAllDigits(x); };
    return ok(tok.substring(0,s)) && ok(tok.substring(s+1));
  }
  static bool looksLikeAltim(const String& tok){ if(tok.startsWith("A")&&tok.length()==5&&isAllDigits(tok.substring(1))) return true; if(tok.startsWith("Q")&&tok.length()==5&&isAllDigits(tok.substring(1))) return true; return false; }
  static bool looksLikeStation(const String& tok){ if(tok.length()<4||tok.length()>5) return false; for(size_t i=0;i<tok.length();++i){char c=tok[i]; if(c<'A'||c>'Z') return false;} return true; }
  static bool looksLikeWx(const String& tok){
    const char* wx[] = {"RA","DZ","SN","SG","PL","GR","GS","IC","UP","BR","FG","FU","VA","DU","SA","HZ","PY","TS","SH","FZ","PO","SQ","FC","SS","DS"};
    String t=tok; t.replace("+",""); t.replace("-",""); t.replace("VC","");
    for(size_t i=0;i<sizeof(wx)/sizeof(wx[0]);++i) if (t.indexOf(wx[i])!=-1) return true; return false;
  }
  void parseMetarIntoFields(const String& metar){
    metarStation=metarTimeZ=metarWind=metarVis=metarClouds=metarTempDew=metarAltim=metarWx=metarRemarks="";
    if (!metar.length()) return;
    const int MAXT=80; String toks[MAXT]; int nt=0;
    for (int i=0,n=metar.length(); i<n && nt<MAXT;){
      while(i<n && metar[i]==' ') i++; int j=i; while(j<n && metar[j]!=' ') j++;
      if (j>i) toks[nt++]=metar.substring(i,j); i=j+1;
    }
    int idxRMK=-1; for(int k=0;k<nt;k++) if(toks[k]=="RMK"){ idxRMK=k; break; }
    for (int k=0;k<nt;k++){
      const String& t=toks[k];
      if (t=="METAR"||t=="SPECI"||t=="AUTO"||t=="=") continue;
      if (metarStation==""&&looksLikeStation(t)){metarStation=t;continue;}
      if (metarTimeZ==""&&looksLikeTimeZ(t))   {metarTimeZ=t;continue;}
      if (metarWind==""&&looksLikeWind(t))     {metarWind=t;continue;}
      if (metarVis==""&&looksLikeVis(t)){ if(k>0&&isAllDigits(toks[k-1])) metarVis=toks[k-1]+" "+t; else metarVis=t; continue; }
      if (looksLikeCloud(t)){ if(metarClouds.length()) metarClouds+=' '; metarClouds+=t; continue; }
      if (metarTempDew==""&&looksLikeTempDew(t)){metarTempDew=t;continue;}
      if (metarAltim==""&&looksLikeAltim(t))  {metarAltim=t;continue;}
      if (looksLikeWx(t)){ if(metarWx.length()) metarWx+=' '; metarWx+=t; continue; }
    }
    if (idxRMK>=0 && idxRMK<nt-1){ String r; for(int k=idxRMK+1;k<nt;k++){ if(r.length()) r+=' '; r+=toks[k]; } metarRemarks=r; }
  }

  static String httpcErrName(int code) {
    if (code >= 0) return String(code);
    switch(code){
      case -1:  return "HTTPC_ERROR_CONNECTION_REFUSED(-1)";
      case -2:  return "HTTPC_ERROR_SEND_HEADER_FAILED(-2)";
      case -3:  return "HTTPC_ERROR_SEND_PAYLOAD_FAILED(-3)";
      case -4:  return "HTTPC_ERROR_NOT_CONNECTED(-4)";
      case -5:  return "HTTPC_ERROR_CONNECTION_LOST(-5)";
      case -6:  return "HTTPC_ERROR_NO_STREAM(-6)";
      case -7:  return "HTTPC_ERROR_NO_HTTP_SERVER(-7)";
      case -8:  return "HTTPC_ERROR_TOO_LESS_RAM(-8)";
      case -9:  return "HTTPC_ERROR_ENCODING(-9)";
      case -10: return "HTTPC_ERROR_STREAM_WRITE(--10)";
      case -11: return "HTTPC_ERROR_READ_TIMEOUT(-11)";
      default:  return String("HTTPClient_ERR(")+code+")";
    }
  }

  // ======================= DOH helpers ========================
  bool dohResolveOnce_IP(const String& host, IPAddress& out, String& provider, uint32_t& tookMs,
                         const char* providerHost, const char* providerIP, const char* pathPrefix,
                         const char* provName)
  {
    IPAddress dohIP; if (!dohIP.fromString(providerIP)) return false;
    WiFiClientSecure cli; cli.setInsecure();
  #if defined(ARDUINO_ARCH_ESP32)
    cli.setHandshakeTimeout(8);
  #endif
    HTTPClient http;
    http.setConnectTimeout(5000);
    http.setReuse(false);
    String path = String(pathPrefix) + host;
    uint32_t t0 = millis();
    if (!http.begin(cli, dohIP.toString(), 443, path, true)) { http.end(); return false; }
    http.addHeader("Host", providerHost, true, false);
    http.addHeader("Accept", "application/dns-json");
    int code = http.GET();
    if (code != HTTP_CODE_OK) { http.end(); return false; }
    DynamicJsonDocument d(1536);
    DeserializationError jerr = deserializeJson(d, http.getStream());
    http.end();
    tookMs = millis() - t0;
    if (jerr) return false;
    JsonArray ans = d["Answer"].as<JsonArray>();
    if (ans.isNull() || ans.size()==0) return false;
    for (JsonVariant v : ans) {
      JsonObject o = v.as<JsonObject>();
      if (o.isNull()) continue;
      int t = o["type"] | 0;
      String data = o["data"] | "";
      if (t == 1 && out.fromString(data)) { provider = provName; return true; }
    }
    for (JsonVariant v : ans) {
      String data = v["data"] | "";
      if (out.fromString(data)) { provider = provName; return true; }
    }
    return false;
  }

  bool resolveHostWithFallback(const String& host, IPAddress& ipOut, String& providerOut, uint32_t& dnsMsOut) {
    uint32_t t0 = millis();
    if (WiFi.hostByName(host.c_str(), ipOut) == 1) { dnsMsOut = millis() - t0; providerOut="system"; lastDiag.dnsFallbackUsed=false; return true; }
    if (dohResolveOnce_IP(host, ipOut, providerOut, dnsMsOut, "dns.google", "8.8.8.8", "/resolve?type=A&name=", "dns.google")) { lastDiag.dnsFallbackUsed=true; return true; }
    if (dohResolveOnce_IP(host, ipOut, providerOut, dnsMsOut, "cloudflare-dns.com", "1.1.1.1", "/dns-query?type=A&name=", "cloudflare-dns")) { lastDiag.dnsFallbackUsed=true; return true; }
    if (dohResolveOnce_IP(host, ipOut, providerOut, dnsMsOut, "dns.quad9.net", "9.9.9.9", "/dns-query?type=A&name=", "quad9")) { lastDiag.dnsFallbackUsed=true; return true; }
    dnsMsOut = millis() - t0; return false;
  }

  // NOTE: accepts optional ArduinoJson filter (DeserializationOption::Filter)
  bool fetchJsonWithDiag(const String& url,
                         DynamicJsonDocument& doc,
                         bool wantPreview = true,
                         const JsonDocument* filter = nullptr)
  {
    lastDiag.clear();
    lastUrl = url;
    lastErr = ""; lastHttp = 0; lastBodyPreview = "";
    lastFetchStartMs = millis();

    const String scheme = "https://";
    if (!url.startsWith(scheme)) { lastErr = "URL must be https://"; return false; }
    int p = url.indexOf('/', scheme.length());
    const String hostPort = (p>0) ? url.substring(scheme.length(), p) : url.substring(scheme.length());
    const String path     = (p>0) ? url.substring(p) : "/";
    String host = hostPort; int port = 443;
    int c = hostPort.indexOf(':');
    if (c>0) { host = hostPort.substring(0, c); port = hostPort.substring(c+1).toInt(); if (port <= 0) port = 443; }
    lastDiag.host = host;

    IPAddress ip; uint32_t dnsT = 0; String provider;
    if (!resolveHostWithFallback(host, ip, provider, dnsT)) {
      lastDiag.errStage = "dns"; lastDiag.errDetail = "DNS failed (system+DoH)";
      lastDiag.dnsMs = dnsT; SA_DBG("DNS failed for %s (system+DoH)\n", host.c_str());
      return false;
    }
    lastDiag.ip = ip.toString();
    lastDiag.dnsMs = dnsT;
    lastDiag.dnsProvider = provider;

#if SA_FORCE_DNS_ONCE_PER_WINDOW
    if (lastDiag.dnsFallbackUsed && !forcedDnsThisWindow) {
#else
    if (lastDiag.dnsFallbackUsed) {
#endif
      ip_addr_t d1, d2;
      ipaddr_aton("1.1.1.1", &d1);
      ipaddr_aton("8.8.8.8", &d2);
      dns_setserver(0, &d1);
      dns_setserver(1, &d2);
      forcedDnsThisWindow = true;
      SA_DBG("DNS: forced to 1.1.1.1 / 8.8.8.8 (once/window)\n");
    }

    { // TCP probe
      WiFiClient tcp; uint32_t t0=millis();
      bool ok = tcp.connect(ip, port, 4000);
      lastDiag.tcpMs = millis()-t0;
      if (!ok) { lastDiag.errStage="tcp"; lastDiag.errDetail="TCP connect failed"; return false; }
      tcp.stop();
    }
    { // TLS probe with SNI
      WiFiClientSecure tls; tls.setInsecure();
    #if defined(ARDUINO_ARCH_ESP32)
      tls.setHandshakeTimeout(10);
    #endif
      uint32_t t0=millis();
      bool ok=tls.connect(host.c_str(), port);
      lastDiag.tlsMs = millis()-t0;
      if(!ok){ lastDiag.errStage="tls"; lastDiag.errDetail="TLS handshake failed"; return false; }
      tls.stop();
    }

    WiFiClientSecure client; client.setInsecure();
  #if defined(ARDUINO_ARCH_ESP32)
    client.setHandshakeTimeout(10);
  #endif
    HTTPClient http;
    http.setUserAgent("Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36...");
   // http.setUserAgent("WLED-SkyAware/0.6 (+esp)");
    http.setConnectTimeout(8000);
    http.setReuse(false);
    http.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);
    const char* hdrs[] = {"Location","Content-Type","Content-Length","Server"};
    http.collectHeaders(hdrs, 4);

    uint32_t t0 = millis();
    if (!http.begin(client, url)) {
      lastDiag.errStage="http"; lastDiag.errDetail="http.begin failed (hostname)";
      lastDiag.httpMs = millis() - t0;
      return false;
    }

    int code = http.GET();
    lastHttp = code;
    lastDiag.httpCode = code;
    lastDiag.httpMs = millis() - t0;

    if (http.hasHeader("Location")) lastDiag.redirect = http.header("Location");

    if (code != HTTP_CODE_OK) {
      lastErr = httpcErrName(code);
      String prev; int limit = 128;
      WiFiClient& es = http.getStream();
      while (es.connected() && es.available() && (limit--)>0) prev += (char)es.read();
      lastBodyPreview = prev;
      lastDiag.bytes = prev.length();
      http.end();
      failureCount++;
      lastFetchDurMs = millis() - lastFetchStartMs;
      return false;
    }

    { // parse JSON
      WiFiClient& stream = http.getStream();
      if (!wantPreview) lastBodyPreview = "";

      DeserializationError jerr = filter
        ? deserializeJson(doc, stream, DeserializationOption::Filter(*filter))
        : deserializeJson(doc, stream);

      http.end();
      if (jerr) {
        lastDiag.errStage="json"; lastDiag.errDetail = String("ArduinoJson: ") + jerr.c_str();
        failureCount++; lastFetchDurMs = millis() - lastFetchStartMs; lastErr = lastDiag.errDetail;
        return false;
      }
      lastDiag.bytes = http.getSize();
    }

    lastDiag.ok = true;
    lastFetchDurMs = millis() - lastFetchStartMs;
    return true;
  }

  // ======================= METAR fetchers =====================
  bool fetchMetarOnce() {
    if (!WLED_CONNECTED) { lastErr="WiFi not connected"; return false; }
    String primary = getPrimaryAirportForSingle();
    lastUrl = "https://aviationweather.gov/api/data/metar?format=json&ids=" + primary;

    // Filter for single: station + category + raw METAR fields
    StaticJsonDocument<256> filter;
    JsonArray farr = filter.to<JsonArray>();
    JsonObject any = farr.createNestedObject();
    any["icaoId"] = true; any["station"] = true; any["station_id"] = true;
    any["icao"]   = true; any["id"]      = true;
    any["fltCat"] = true; any["flight_category"] = true; any["fltcat"] = true;
    any["rawOb"]  = true; any["raw_text"] = true; any["raw"] = true;
    any["metar"]  = true; any["raw_ob"]   = true; any["metar_text"] = true;

    DynamicJsonDocument doc(1024);
    bool ok = fetchJsonWithDiag(lastUrl, doc, true, &filter);
    lastHttp = lastDiag.httpCode;
    if (!ok) { failureCount++; return false; }
    if (!doc.is<JsonArray>()) { lastErr="JSON not array"; failureCount++; return false; }
    JsonArray arr = doc.as<JsonArray>();
    if (arr.isNull() || arr.size()==0) { lastErr="empty array"; failureCount++; return false; }
    JsonObject first = arr[0].as<JsonObject>(); if (first.isNull()) { lastErr="first not object"; failureCount++; return false; }

    String stn = readStation(first);
    String cat = readFlightCategory(first);
    if (stn.length()) {
      lastCat = cat;
      setCatForAirport(stn, cat, true);
      lastMetarRaw = readMetarRaw(first);
      parseMetarIntoFields(lastMetarRaw);
      lastOkMs = millis();
    }
    successCount++;
    return true;
  }

// Build the ArduinoJson filter we use for METAR array items
static inline void buildMetarFilter(StaticJsonDocument<256>& filter) {
  JsonArray farr = filter.to<JsonArray>();
  JsonObject any = farr.createNestedObject();
  // Station identifiers (the endpoint can use different field names)
  any["icaoId"] = true; any["station"] = true; any["station_id"] = true;
  any["icao"]   = true; any["id"]      = true;
  // Flight category variants
  any["fltCat"] = true; any["flight_category"] = true; any["fltcat"] = true;
}

// Join a slice of airports into a CSV for ?ids=...
static inline String joinIdsCsv(const std::vector<String>& airports, size_t start, size_t count) {
  String ids;
  for (size_t j = 0; j < count; ++j) {
    if (j) ids += ',';
    ids += airports[start + j];
  }
  return ids;
}

// ======== Drop-in replacement ========
bool fetchMetarMulti(const std::vector<String>& airports)
{
  // Basic guards
  if (!WLED_CONNECTED) { lastErr = F("WiFi not connected"); failureCount++; return false; }
  if (airports.empty()) { lastErr = F("no airports"); return true; }

  // Prepare common filter once
  StaticJsonDocument<256> filter;
  buildMetarFilter(filter);

  bool anyOk = false;
  uint16_t goodObjects = 0, badObjects = 0;
  const size_t total = airports.size();

  // Process in batches to avoid large JSON docs/URLs
  for (size_t off = 0; off < total; off += SKY_MULTI_BATCH) {
    const size_t take = (off + SKY_MULTI_BATCH <= total) ? SKY_MULTI_BATCH : (total - off);
    const String ids  = joinIdsCsv(airports, off, take);

    lastUrl = F("https://aviationweather.gov/api/data/metar?format=json&ids=");
    lastUrl += ids;

    // Small and predictable memory per batch
    DynamicJsonDocument doc(SKY_JSON_DOC_SIZE);

    // Perform request + JSON parse with diagnostics (existing helper)
    // wantPreview=true keeps timing/diag fields populated
    bool ok = fetchJsonWithDiag(lastUrl, doc, /*wantPreview=*/true, &filter);

    if (!ok) {
      // Network/HTTP/parse error—fetchJsonWithDiag filled lastErr/lastHttp/lastDiag
      SA_DBG("multi: batch off=%u take=%u failed: http=%d err=%s\n",
             (unsigned)off, (unsigned)take, (int)lastHttp, lastErr.c_str());
      // Continue to next batch; we still may succeed overall
      continue;
    }

    // Expect an array of minimal objects after filter
    if (!doc.is<JsonArray>()) {
      lastErr = F("JSON not array (multi)");
      SA_DBG("multi: batch off=%u take=%u bad shape\n", (unsigned)off, (unsigned)take);
      continue;
    }

    JsonArray arr = doc.as<JsonArray>();
    if (arr.isNull()) {
      lastErr = F("null array (multi)");
      SA_DBG("multi: batch off=%u take=%u null array\n", (unsigned)off, (unsigned)take);
      continue;
    }

    // Walk filtered objects: extract station + category
    uint16_t batchGood = 0, batchBad = 0;
    for (JsonVariant v : arr) {
      JsonObject o = v.as<JsonObject>();
      if (o.isNull()) { batchBad++; continue; }

      String stn = readStation(o);          // existing helper in your code
      String cat = readFlightCategory(o);   // existing helper in your code
      if (!stn.length()) { batchBad++; continue; }

      setCatForAirport(stn, cat, /*overwrite=*/true);
      batchGood++;
    }

    if (batchGood) anyOk = true;
    goodObjects += batchGood;
    badObjects  += batchBad;

    SA_DBG("multi: batch off=%u take=%u ok=%u bad=%u\n",
           (unsigned)off, (unsigned)take, (unsigned)batchGood, (unsigned)batchBad);
  }

  // Summarize result
  if (anyOk) {
    // Pick a primary to mirror single-airport fields (first of original list)
    lastCat    = getCatForAirport(airports[0]);
    lastMetarRaw = "";          // we don't keep raw METAR in multi mode
    lastOkMs   = millis();
    successCount++;
    SA_DBG("multi: success objects=%u bad=%u\n", (unsigned)goodObjects, (unsigned)badObjects);
    return true;
  }

  failureCount++;
  // Keep lastErr/lastHttp from the last failed batch; add context
  if (lastErr.length() == 0) lastErr = F("no batches succeeded");
  SA_DBG("multi: FAIL (all batches); lastHttp=%d err=%s\n", (int)lastHttp, lastErr.c_str());
  return false;
}



  // ======================= mapping helpers ====================
  void gatherAllAirports(std::vector<String>& out) {
    out.clear();
    auto pushUnique=[&](const String& s){
      if (!s.length()) return;
      String a = upperTrim(s);
      if (a=="SKIP"||a=="-") return;
      for (auto& t : out) if (t==a) return;
      out.push_back(a);
    };

    uint16_t numSegs = strip.getSegmentsNum();
    if (segMaps.size() < numSegs) syncSegMapSize();

    bool anySingle = false;
    bool anyMultiple = false;

    for (uint16_t si=0; si<numSegs; ++si) {
      SegmentMap& sm = segMaps[si];
      if (sm.mode == MAP_SINGLE) {
        anySingle = true;
        String ap = sm.wholeAirport.length() ? sm.wholeAirport : airportId;
        pushUnique(ap);
      } else {
        anyMultiple = true;
        Segment& seg = strip.getSegment(si);
        uint16_t len = (seg.stop>seg.start)?(seg.stop-seg.start):0;
        if (sm.perLed.size()!=len) sm.perLed.resize(len);
        for (auto& le : sm.perLed) if (le.type==LT_ICAO) pushUnique(le.value);
      }
    }

    if (!anySingle && !anyMultiple && airportId.length()) pushUnique(airportId);
  }

  String getPrimaryAirportForSingle() {
    uint16_t numSegs = strip.getSegmentsNum();
    if (segMaps.size() < numSegs) syncSegMapSize();
    for (uint16_t si=0; si<numSegs; ++si) {
      if (segMaps[si].mode == MAP_SINGLE) {
        String ap = segMaps[si].wholeAirport.length()? segMaps[si].wholeAirport : airportId;
        if (ap.length()) return upperTrim(ap);
      }
    }
    return upperTrim(airportId);
  }

  static const char* modeLabel(MapMode m) { return (m==MAP_SINGLE) ? "SINGLE" : "MULTIPLE"; }

  // ======================= JSON writers =======================

  // Flattened /segments producer (FULL per-LED data). INDICATOR -> "-" ; includes fallback flag.
  void writeSegmentsJson(Print& out) {
    uint16_t numSegs = strip.getSegmentsNum();
    if (segMaps.size() < numSegs) syncSegMapSize();

    out.print("{\"segments\":[");
    for (uint16_t si=0; si<numSegs; ++si) {
      if (si) out.print(",");
      Segment& seg = strip.getSegment(si);
      bool usedFallback=false;
      uint16_t len = safeSegLen(seg, usedFallback);

      out.print("{\"index\":");   out.print(si);
      out.print(",\"length\":");  out.print(len);
      out.print(",\"mode\":\"");  out.print(modeLabel(segMaps[si].mode)); out.print("\"");
      out.print(",\"fallback\":"); out.print(usedFallback ? "true":"false");

      SegmentMap& sm = segMaps[si];

      if (sm.mode == MAP_SINGLE) {
        // Provide airport-only for SINGLE; editor can convert to MULTIPLE if needed.
        String ap = sm.wholeAirport.length()? sm.wholeAirport : airportId;
        out.print(",\"airport\":\""); out.print(ap); out.print("\"");
      } else {
        // MULTIPLE: ensure vector length & emit per-LED list
        if (sm.perLed.size() != len) segMaps[si].perLed.resize(len);
        out.print(",\"leds\":[");
        for (uint16_t i=0;i<len;i++) {
          if (i) out.print(",");
          const LedEntry& le = sm.perLed[i];
          if (le.type == LT_ICAO) { out.print("\""); out.print(le.value); out.print("\""); }
          else { out.print("\"-\""); } // SKIP or INDICATOR => "-"
        }
        out.print("]");
      }

      out.print("}");
    }
    out.print("]}");
  }

  // Summary-only mapping info for /state (no per-LED arrays).
  void writeMappingsSummaryJson(Print& out) {
    uint16_t numSegs = strip.getSegmentsNum();
    if (segMaps.size() < numSegs) syncSegMapSize();

    out.print("[");
    for (uint16_t si=0; si<numSegs; ++si) {
      if (si) out.print(",");
      Segment& seg = strip.getSegment(si);
      bool usedFallback=false;
      uint16_t len = safeSegLen(seg, usedFallback);

      out.print("{\"segment\":");   out.print(si);
      out.print(",\"mode\":\"");    out.print(modeLabel(segMaps[si].mode)); out.print("\"");
      out.print(",\"length\":");    out.print(len);
      out.print(",\"fallback\":");  out.print(usedFallback ? "true":"false");
      out.print("}");
    }
    out.print("]");
  }

  // Streamed /state writer (debug + summary only)
  void writeStateJson(Print& out) {
    out.print("{\"SkyAware\":{");

    out.print("\"enabled\":");      out.print(enabled ? "true":"false");
    out.print(",\"autoBoot\":");    out.print(autoFetchBoot ? "true":"false");
    out.print(",\"airport\":\"");   out.print(airportId); out.print("\"");
    out.print(",\"category\":\"");  out.print(lastCat); out.print("\"");
    out.print(",\"interval\":");    out.print(updateInterval);
    out.print(",\"http\":");        out.print(lastHttp);
    out.print(",\"err\":\"");       out.print(lastErr); out.print("\"");
    out.print(",\"url\":\"");       out.print(lastUrl); out.print("\"");
    out.print(",\"bodyPrev\":\"");  out.print(lastBodyPreview); out.print("\"");
    out.print(",\"lastOkSec\":");   out.print((int)(lastOkMs/1000));
    out.print(",\"fetchMs\":");     out.print((int)lastFetchDurMs);
    out.print(",\"ok\":");          out.print(successCount);
    out.print(",\"fail\":");        out.print(failureCount);
    out.print(",\"bootWaitMs\":");  out.print((int)BOOT_WIFI_SETTLE_MS);
    out.print(",\"pendingFetch\":");out.print(refreshNow ? "true":"false");

    // scheduler
    out.print(",\"nowSec\":");         out.print((int)(millis()/1000));
    out.print(",\"nextAttemptSec\":"); out.print((int)(nextAttemptAt/1000));
    out.print(",\"windowEndSec\":");   out.print((int)(windowDeadline/1000));
    out.print(",\"periodSec\":");      out.print((int)(periodMs()/1000));
    out.print(",\"retryIndex\":");     out.print((int)retryIndex);
    out.print(",\"inRetryWindow\":");  out.print(inRetryWindow ? "true":"false");

    // mode + primary
    bool anySingle=false, anyMultiple=false;
    {
      uint16_t numSegs = strip.getSegmentsNum();
      if (segMaps.size() < numSegs) syncSegMapSize();
      for (uint16_t si=0; si<numSegs; ++si) ((segMaps[si].mode==MAP_SINGLE)? anySingle: anyMultiple)=true;
    }
    out.print(",\"mode\":\""); out.print(anyMultiple ? "MULTIPLE" : "SINGLE"); out.print("\"");
    out.print(",\"primary\":\""); out.print(anyMultiple ? "" : getPrimaryAirportForSingle()); out.print("\"");

    // wanted (airports)
    {
      std::vector<String> aps; gatherAllAirports(aps);
      out.print(",\"wanted\":[");
      for (size_t i=0;i<aps.size();++i){ if(i) out.print(","); out.print("\""); out.print(aps[i]); out.print("\""); }
      out.print("]");
    }

    // WiFi info
    out.print(",\"ssid\":\""); out.print(WiFi.SSID()); out.print("\"");
    out.print(",\"rssi\":");   out.print(WiFi.RSSI());
    out.print(",\"staIP\":\"");out.print(WiFi.localIP().toString()); out.print("\"");

    // METAR detail (single-primary view)
    out.print(",\"metarRaw\":\""); out.print(lastMetarRaw); out.print("\"");
    out.print(",\"metar\":{");
    out.print("\"station\":\""); out.print(metarStation); out.print("\"");
    out.print(",\"timeZ\":\"");  out.print(metarTimeZ);   out.print("\"");
    out.print(",\"wind\":\"");   out.print(metarWind);    out.print("\"");
    out.print(",\"vis\":\"");    out.print(metarVis);     out.print("\"");
    out.print(",\"clouds\":\""); out.print(metarClouds);  out.print("\"");
    out.print(",\"tempDew\":\"");out.print(metarTempDew); out.print("\"");
    out.print(",\"altim\":\"");  out.print(metarAltim);   out.print("\"");
    out.print(",\"wx\":\"");     out.print(metarWx);      out.print("\"");
    out.print(",\"remarks\":\"");out.print(metarRemarks); out.print("\"");
    out.print("}");

    // mappings summary only (no per-LEDs)
    out.print(",\"mappings\":");
    writeMappingsSummaryJson(out);

    // airports cache
    out.print(",\"airports\":[");
    for (size_t i=0;i<apCache.size();++i) {
      if (i) out.print(",");
      auto& e = apCache[i];
      out.print("{\"id\":\""); out.print(e.id); out.print("\"");
      out.print(",\"cat\":\""); out.print(e.cat); out.print("\"");
      out.print(",\"good\":"); out.print(e.good ? "true":"false");
      out.print(",\"okSec\":"); out.print((int)(e.okMs/1000));
      out.print("}");
    }
    out.print("]");

    // net diag
    out.print(",\"net\":{");
    out.print("\"host\":\""); out.print(lastDiag.host); out.print("\"");
    out.print(",\"ip\":\"");  out.print(lastDiag.ip);   out.print("\"");
    out.print(",\"http\":");  out.print(lastDiag.httpCode);
    out.print(",\"redirect\":\""); out.print(lastDiag.redirect); out.print("\"");
    out.print(",\"dnsMs\":"); out.print((int)lastDiag.dnsMs);
    out.print(",\"tcpMs\":"); out.print((int)lastDiag.tcpMs);
    out.print(",\"tlsMs\":"); out.print((int)lastDiag.tlsMs);
    out.print(",\"httpMs\":");out.print((int)lastDiag.httpMs);
    out.print(",\"bytes\":"); out.print(lastDiag.bytes);
    out.print(",\"ok\":");    out.print(lastDiag.ok ? "true":"false");
    out.print(",\"stage\":\"");  out.print(lastDiag.errStage);  out.print("\"");
    out.print(",\"detail\":\""); out.print(lastDiag.errDetail); out.print("\"");
    out.print(",\"dnsProvider\":\""); out.print(lastDiag.dnsProvider); out.print("\"");
    out.print(",\"dnsFallback\":"); out.print(lastDiag.dnsFallbackUsed ? "true":"false");
    out.print("}");

    out.print("}}");
  }

  // ======================= config I/O =========================
  void syncSegMapSize() {
    uint16_t numSegs = strip.getSegmentsNum();
    if (segMaps.size() == numSegs) return;
    size_t old = segMaps.size();
    segMaps.resize(numSegs);
    for (size_t i=old; i<segMaps.size(); ++i) {
      segMaps[i].mode = MAP_SINGLE;
      segMaps[i].wholeAirport = airportId;
      segMaps[i].perLed.clear();
      // Note: perLed will be resized lazily where needed; no heavy allocations here
    }
  }

  void parseLedCsv(uint16_t segIndex, const String& csv) {
    syncSegMapSize();
    Segment& seg = strip.getSegment(segIndex);
    bool fb=false;
    uint16_t len = safeSegLen(seg, fb);

    segMaps[segIndex].mode = MAP_MULTIPLE;
    segMaps[segIndex].perLed.clear();
    segMaps[segIndex].perLed.resize(len);

    uint16_t idx = 0;
    for (int i=0,n=csv.length(); i<n && idx<len;){
      int j=i; while(j<n && csv[j]!=',') j++;
      String tok = csv.substring(i,j); tok.trim();
      LedEntry le;
      if (tok.length()==0 || tok=="-" || tok.equalsIgnoreCase("SKIP")) {
        le.type = LT_SKIP;
      } else if (tok.startsWith("IND:")) {
        // per your choice: treat INDICATOR as SKIP for now
        le.type = LT_SKIP; le.value="";
      } else {
        String ap = upperTrim(tok);
        bool ok=true;
        if (ap.length()<3 || ap.length()>8) ok=false;
        for (size_t k=0; k<ap.length() && ok; ++k) ok = isAlphaNumDash(ap[k]);
        if (ok) { le.type = LT_ICAO; le.value = ap; } else { le.type = LT_SKIP; }
      }
      segMaps[segIndex].perLed[idx++] = le;
      i = (j<n)? (j+1) : j;
    }
  }

  // ======================= core hooks =========================
public:
  void setup() override {
    SA_DBG("setup: airport=%s, interval=%u min, enabled=%d, autoFetch=%d\n",
           airportId.c_str(), updateInterval, (int)enabled, (int)autoFetchBoot);
    // attempt to load persisted mapping (if exists)
    loadFromFile();


    for (uint16_t i=0;i<strip.getLength();++i) strip.setPixelColor(i, RGBW32(255,255,255,0));
    strip.trigger();

    extern AsyncWebServer server;

    // ===== UI static page =====
    server.on("/skyaware", HTTP_GET, [](AsyncWebServerRequest* r){
      size_t htmlLen = strlen_P((PGM_P)SKY_UI_HTML);
      AsyncWebServerResponse* res = r->beginResponse_P(
        200, F("text/html"),
        reinterpret_cast<const uint8_t*>(SKY_UI_HTML),
        htmlLen
      );
      r->send(res);
    });

    // ===== API =====
    server.on("/api/skyaware/enable", HTTP_GET, [this](AsyncWebServerRequest* r){
      bool on = r->hasParam("on") && (r->getParam("on")->value() == "1");
      enabled = on; r->send(200, "text/plain", enabled ? "SkyAware: enabled" : "SkyAware: disabled");
    });

    server.on("/api/skyaware/refresh", HTTP_GET, [this](AsyncWebServerRequest* r){
      refreshNow = true; r->send(200, "text/plain", "SkyAware: refresh queued");
    });

    server.on("/api/skyaware/set", HTTP_GET, [this](AsyncWebServerRequest* r){
      if (!r->hasParam("airport")) { r->send(400, "text/plain", "missing airport"); return; }
      String a = upperTrim(r->getParam("airport")->value());
      if (a == "" || a.length()<3 || a.length()>8) { r->send(400, "text/plain", "bad airport"); return; }
      if (!a.equals(airportId)) {
        SA_DBG("airport change via /set: %s -> %s\n", airportId.c_str(), a.c_str());
        airportId = a;
        syncSegMapSize();
        for (auto& sm : segMaps) if (sm.mode==MAP_SINGLE && !sm.wholeAirport.length()) sm.wholeAirport=a;
          refreshNow = true;
          initOrRealignSchedule(true);
      }
      r->send(200, "text/plain", airportId);
    });

    // Geometry + mapping (FLATTENED with full per-LED list)
    server.on("/api/skyaware/segments", HTTP_GET, [this](AsyncWebServerRequest* r){
      AsyncResponseStream* res = r->beginResponseStream("application/json");
      writeSegmentsJson(*res);
      r->send(res);
    });

    // /map accepts MULTIPLE CSV or SINGLE with airport
    server.on("/api/skyaware/map", HTTP_POST, [this](AsyncWebServerRequest* r){
      auto bad=[&](const char* m){ r->send(400,"text/plain",m); };
      if (!r->hasParam("seg", true)) { bad("missing seg"); return; }
      uint16_t segIdx = (uint16_t) r->getParam("seg", true)->value().toInt();
      if (segIdx >= strip.getSegmentsNum()) { bad("seg out of range"); return; }
      String modeStr = r->hasParam("mode", true) ? upperTrim(r->getParam("mode", true)->value()) : "SINGLE";
      syncSegMapSize();
      if (modeStr=="SINGLE") {
        String ap = r->hasParam("airport", true) ? upperTrim(r->getParam("airport", true)->value()) : "";
        if (!ap.length()) ap = airportId;
        segMaps[segIdx].mode = MAP_SINGLE; segMaps[segIdx].wholeAirport = ap; segMaps[segIdx].perLed.clear();
      } else if (modeStr=="MULTIPLE") {
        String csv = r->hasParam("leds", true) ? r->getParam("leds", true)->value() : "";
        parseLedCsv(segIdx, csv);
      } else { bad("mode must be SINGLE or MULTIPLE"); return; }
      refreshNow = true;
      initOrRealignSchedule(true); 
      {
        bool ok = saveToFile();
        AsyncResponseStream* res = r->beginResponseStream("application/json");
        res->printf("{\"status\":\"ok\",\"saved\":%s,\"seg\":%u,\"mode\":\"%s\"}",
                    ok?"true":"false", segIdx, modeStr.c_str());
        r->send(res);
      }

  });
    // HTTPS probe + state + log
    server.on("/api/skyaware/https_test", HTTP_GET, [this](AsyncWebServerRequest* r){
      std::vector<String> aps; gatherAllAirports(aps);
      String ids; for (size_t i=0;i<aps.size();++i){ if(i) ids+=','; ids+=aps[i]; }
      String url = "https://aviationweather.gov/api/data/metar?format=json&ids=" + (ids.length()?ids:getPrimaryAirportForSingle());
      StaticJsonDocument<256> filter;
      JsonArray farr = filter.to<JsonArray>();
      JsonObject any = farr.createNestedObject();
      any["icaoId"]=true; any["station"]=true; any["station_id"]=true;
      any["icao"]=true;   any["id"]=true;
      any["fltCat"]=true; any["flight_category"]=true; any["fltcat"]=true;

      DynamicJsonDocument tmp(256);
      bool ok = fetchJsonWithDiag(url, tmp, true, &filter);
      StaticJsonDocument<512> d; JsonObject o = d.to<JsonObject>();
      o["ok"]  = ok; o["err"] = lastErr; o["http"] = lastHttp; o["url"] = lastUrl;
      JsonObject nd = o.createNestedObject("net");
      nd["host"] = lastDiag.host; nd["ip"]=lastDiag.ip; nd["http"]=lastDiag.httpCode;
      nd["redirect"]=lastDiag.redirect; nd["dnsMs"]=(int)lastDiag.dnsMs; nd["tcpMs"]=(int)lastDiag.tcpMs;
      nd["tlsMs"]=(int)lastDiag.tlsMs; nd["httpMs"]=(int)lastDiag.httpMs; nd["bytes"]=lastDiag.bytes;
      nd["ok"]=lastDiag.ok; nd["stage"]=lastDiag.errStage; nd["detail"]=lastDiag.errDetail;
      nd["dnsProvider"]=lastDiag.dnsProvider; nd["dnsFallback"]=lastDiag.dnsFallbackUsed;
      o["bodyPrev"] = lastBodyPreview;
      AsyncResponseStream* res = r->beginResponseStream("application/json"); serializeJson(d, *res); r->send(res);
    });

    server.on("/api/skyaware/state", HTTP_GET, [this](AsyncWebServerRequest* r){
      AsyncResponseStream* res = r->beginResponseStream("application/json");
      writeStateJson(*res);
      r->send(res);
    });

    server.on("/api/skyaware/log", HTTP_GET, [](AsyncWebServerRequest* r){
      AsyncResponseStream* res = r->beginResponseStream("text/plain; charset=utf-8"); SA_LOGBUF.dumpTo(*res); r->send(res);
    });
    server.on("/api/skyaware/log/clear", HTTP_GET, [](AsyncWebServerRequest* r){ SA_LOGBUF.clear(); r->send(200,"text/plain","SkyAware log cleared"); });

    // ===== LED OVERRIDE ENDPOINTS =====
    
    // Start LED identification with cyan color and optional timeout
    server.on("/api/skyaware/led/identify", HTTP_GET, [this](AsyncWebServerRequest* r){
      if (!r->hasParam("idx")) {
        r->send(400, "application/json", "{\"error\":\"missing idx parameter\"}");
        return;
      }
      
      uint16_t idx = (uint16_t)strtoul(r->getParam("idx")->value().c_str(), nullptr, 10);
      unsigned long duration = 30000;  // 30s default
      
      if (r->hasParam("duration")) {
        duration = strtoul(r->getParam("duration")->value().c_str(), nullptr, 10);
      }
      
      SA_DBG("API: LED identify - idx=%u, duration=%lu ms\n", idx, duration);
      startLedIdentification(idx, duration);
      strip.trigger();
      
      r->send(200, "application/json", 
        "{\"status\":\"identifying\",\"led\":" + String(idx) + 
        ",\"duration\":" + String(duration) + "}");
    });
    
    // Stop LED identification (clear override)
    server.on("/api/skyaware/led/stop", HTTP_GET, [this](AsyncWebServerRequest* r){
      uint16_t idx = ledIdent.active ? ledIdent.ledIndex : 0;
      if (r->hasParam("idx")) {
        idx = (uint16_t)strtoul(r->getParam("idx")->value().c_str(), nullptr, 10);
      }
      
      SA_DBG("API: LED stop - idx=%u\n", idx);
      stopLedIdentification();
      applyLayoutColors();  // Re-render with normal colors
      
      r->send(200, "application/json", "{\"status\":\"stopped\",\"led\":" + String(idx) + "}");
    });
    
    // Set arbitrary LED color (new feature)
    server.on("/api/skyaware/led/setcolor", HTTP_GET, [this](AsyncWebServerRequest* r){
      if (!r->hasParam("idx") || !r->hasParam("color")) {
        r->send(400, "application/json", 
          "{\"error\":\"missing idx or color parameter (format: 0xRRGGBB or 0xRRGGBBWW)\"}");
        return;
      }
      
      uint16_t idx = (uint16_t)strtoul(r->getParam("idx")->value().c_str(), nullptr, 10);
      uint32_t color = strtoul(r->getParam("color")->value().c_str(), nullptr, 16);
      unsigned long duration = 0;  // indefinite by default
      
      if (r->hasParam("duration")) {
        duration = strtoul(r->getParam("duration")->value().c_str(), nullptr, 10);
      }
      
      SA_DBG("API: LED setcolor - idx=%u, color=0x%08x, duration=%lu ms\n", 
             idx, color, duration);
      
      setLedOverride(idx, color, duration);
      strip.trigger();
      
      r->send(200, "application/json", 
        "{\"status\":\"set\",\"led\":" + String(idx) + 
        ",\"color\":\"0x" + String(color, HEX) + "\"}");
    });
    
    // Clear specific LED override
    server.on("/api/skyaware/led/clearoverride", HTTP_GET, [this](AsyncWebServerRequest* r){
      if (!r->hasParam("idx")) {
        r->send(400, "application/json", "{\"error\":\"missing idx parameter\"}");
        return;
      }
      
      uint16_t idx = (uint16_t)strtoul(r->getParam("idx")->value().c_str(), nullptr, 10);
      
      SA_DBG("API: LED clearoverride - idx=%u\n", idx);
      clearLedOverride(idx);
      applyLayoutColors();
      
      r->send(200, "application/json", "{\"status\":\"cleared\",\"led\":" + String(idx) + "}");
    });
    
    // Clear all LED overrides
    server.on("/api/skyaware/led/clearall", HTTP_GET, [this](AsyncWebServerRequest* r){
      SA_DBG("API: LED clearall\n");
      clearAllLedOverrides();
      applyLayoutColors();
      
      r->send(200, "application/json", "{\"status\":\"all_cleared\"}");
    });
    
    // Get status of all active LED overrides
    server.on("/api/skyaware/led/status", HTTP_GET, [this](AsyncWebServerRequest* r){
      AsyncResponseStream* res = r->beginResponseStream("application/json");
      
      res->print("{\"overrides\":[");
      for (size_t i = 0; i < ledOverrides.size(); ++i) {
        if (i > 0) res->print(",");
        const LedOverride& ov = ledOverrides[i];
        unsigned long remaining = (ov.duration > 0) ? 
          (ov.duration - (millis() - ov.activeSince)) : 0;
        res->print("{\"led\":");
        res->print((int)ov.ledIndex);
        res->print(",\"color\":\"0x");
        res->print(String(ov.color, HEX));
        res->print("\",\"duration\":");
        res->print((int)ov.duration);
        res->print(",\"remaining\":");
        res->print((int)remaining);
        res->print("}");
      }
      res->print("],\"total\":");
      res->print((int)ledOverrides.size());
      res->print("}");
      
      r->send(res);
    });
    
    // ===== END LED OVERRIDE ENDPOINTS =====

    // Simple LED test - cycle through each LED by temporarily setting its airport to IDENT
    server.on("/api/skyaware/led/test", HTTP_GET, [this](AsyncWebServerRequest* r){
      uint16_t total = strip.getLength();
      SA_DBG("LED test starting: %u LEDs\n", total);
      
      bool wasEnabled = enabled;
      enabled = false;
      
      // Cycle through each LED
      for (uint16_t i = 0; i < total; i++) {
        // Set a dummy airport to IDENT (cyan) for this test
        setCatForAirport("TEST", "IDENT", true);
        applyLayoutColors();  // This WORKS - we know it does
        delay(300);
        
        // Clear it
        setCatForAirport("TEST", "UNKNOWN", true);
        applyLayoutColors();
        delay(50);
      }
      
      // Restore
      enabled = wasEnabled;
      if (enabled) {
        applyLayoutColors();
      }
      
      r->send(200, "application/json", "{\"status\":\"test_complete\",\"leds\":" + String(total) + "}");
    });

    // Back-compat redirects
    auto redir=[&](AsyncWebServerRequest* r, const char* to){
      auto* resp=r->beginResponse(302,"text/plain",to); resp->addHeader("Location",to); r->send(resp);
    };
    server.on("/um/skyaware/state.json",  HTTP_GET, [=](AsyncWebServerRequest* r){ redir(r,"/api/skyaware/state"); });
    server.on("/um/skyaware/segments.json",HTTP_GET, [=](AsyncWebServerRequest* r){ redir(r,"/api/skyaware/segments"); });
    server.on("/um/skyaware/https_test",  HTTP_GET, [=](AsyncWebServerRequest* r){ redir(r,"/api/skyaware/https_test"); });
    server.on("/um/skyaware/log.txt",     HTTP_GET, [=](AsyncWebServerRequest* r){ redir(r,"/api/skyaware/log"); });
    server.on("/um/skyaware/log/clear",   HTTP_GET, [=](AsyncWebServerRequest* r){ redir(r,"/api/skyaware/log/clear"); });
    server.on("/um/skyaware/map",         HTTP_POST,[=](AsyncWebServerRequest* r){ redir(r,"/api/skyaware/map"); });
    server.on("/um/skyaware/refresh",     HTTP_GET, [=](AsyncWebServerRequest* r){ redir(r,"/api/skyaware/refresh"); });
    server.on("/um/skyaware/enable",      HTTP_GET, [=](AsyncWebServerRequest* r){ redir(r,"/api/skyaware/enable"); });
    server.on("/um/skyaware/set",         HTTP_GET, [=](AsyncWebServerRequest* r){ redir(r,"/api/skyaware/set"); });
    server.on("/skyaware/status",         HTTP_GET, [=](AsyncWebServerRequest* r){ redir(r,"/skyaware"); });

    // boot schedule
    bootFetchPending = autoFetchBoot;
    initOrRealignSchedule(false);
    syncSegMapSize();
    
    // Initialize LED override system
    ledOverrides.clear();
    SA_DBG("LED override system initialized\n");

    SA_DBG("setup done\n");
  }

  void loop() override {
    static bool firstLoop = true; if (firstLoop) { firstLoop=false; SA_DBG("loop entered\n"); }
    
    if (busy) return;

    // boot settle
    if (bootFetchPending) {
      if (WLED_CONNECTED) {
        if (!wasWifiConnected) { wasWifiConnected=true; wifiConnectedSince=millis(); SA_DBG("WiFi connected; settling %u ms\n",(unsigned)BOOT_WIFI_SETTLE_MS); }
        if (millis()-wifiConnectedSince >= BOOT_WIFI_SETTLE_MS) { SA_DBG("Boot-time fetch now\n"); refreshNow=true; bootFetchPending=false; }
      } else { if (wasWifiConnected) SA_DBG("WiFi dropped; waiting again\n"); wasWifiConnected=false; }
    }

    if (!enabled && !refreshNow) return;

    const uint32_t now = millis();
    const uint32_t period = periodMs();
    if (nextScheduledAt == 0 || prevPeriodMs != period) initOrRealignSchedule(false);
    if (refreshNow) { nextAttemptAt=now; windowDeadline=nextScheduledAt; inRetryWindow=true; retryIndex=0; refreshNow=false; }
    if (!timeReached(nextAttemptAt)) {
      // Still update LED identification blink even when not fetching
      updateLedIdentificationBlink();
      return;
    }

    SA_DBG("attempt (window ends in %lus)\n",
           (unsigned long)((windowDeadline > now) ? ((windowDeadline - now)/1000) : 0));

    busy = true;
    std::vector<String> airports; gatherAllAirports(airports);
    bool ok = (airports.size()<=1) ? fetchMetarOnce() : fetchMetarMulti(airports);
    busy = false;

    if (ok) {
      applyLayoutColors();
      // Note: LED overrides are now applied inside applyLayoutColors()
      updateLedIdentificationBlink();
      inRetryWindow=false; retryIndex=0; forcedDnsThisWindow=false;
      while (timeReached(nextScheduledAt)) nextScheduledAt += period;
      windowDeadline=nextScheduledAt; nextAttemptAt=nextScheduledAt; lastRunMs=now;
      SA_DBG("ok: next in %lus (airports=%u)\n",
             (unsigned long)((nextAttemptAt > millis()) ? ((nextAttemptAt - millis())/1000) : 0),
             (unsigned)airports.size());
      return;
    }

    if (!inRetryWindow) { inRetryWindow=true; retryIndex=0; windowDeadline=nextScheduledAt; }
    if (timeReached(windowDeadline)) {
      inRetryWindow=false; retryIndex=0; forcedDnsThisWindow=false;
      bool anyGood=false; for (auto& e: apCache) if (e.good) { anyGood=true; break; }
      if (!anyGood) { for (uint16_t i=0;i<strip.getLength();++i) strip.setPixelColor(i, RGBW32(0,0,0,0)); strip.trigger(); }
      else applyLayoutColors();
      // Update LED identification blink AFTER segment rendering
      updateLedIdentificationBlink();
      nextScheduledAt += period; windowDeadline=nextScheduledAt; nextAttemptAt=nextScheduledAt; lastRunMs=now;
      return;
    }

    if (retryIndex + 1 < RETRY_COUNT) retryIndex++;
    uint32_t candidate = millis() + RETRY_DELAYS_MS[retryIndex];
    nextAttemptAt = (candidate < windowDeadline) ? candidate : windowDeadline;
  }

  // ---------- config JSON ----------
  void addToConfig(JsonObject& root) override {
    syncSegMapSize();
    JsonObject top = root.createNestedObject("skyAwareUsermod");
    top["Enabled"]               = enabled;
    top["Auto Fetch on Boot"]    = autoFetchBoot;
    top["Airport ID"]            = airportId;
    top["Update Frequency (min)"]= updateInterval;

    JsonArray maps = top.createNestedArray("Mappings");
    uint16_t numSegs = strip.getSegmentsNum();
    for (uint16_t si=0; si<numSegs; ++si) {
      JsonObject m = maps.createNestedObject();
      m["Segment"] = si;
      m["Mode"]    = modeLabel(segMaps[si].mode);
      if (segMaps[si].mode == MAP_SINGLE) {
        m["Airport"] = segMaps[si].wholeAirport;
      } else {
        // save as CSV for compactness
        Segment& seg = strip.getSegment(si);
        uint16_t start = seg.start, stop = seg.stop, len = (stop>start)?(stop-start):0;
        if (segMaps[si].perLed.size()!=len) segMaps[si].perLed.resize(len);
        String csv;
        for (uint16_t i=0;i<len;i++) {
          if (i) csv += ",";
          const LedEntry& le = segMaps[si].perLed[i];
          if (le.type==LT_SKIP) csv += "-";
          else if (le.type==LT_ICAO) csv += le.value;
          else csv += "-"; // IND=* stored as "-" for now
        }
        m["CSV"] = csv;
      }
    }
  }

  bool readFromConfig(JsonObject& root) override {
    JsonObject top = root["skyAwareUsermod"];
    bool configComplete = !top.isNull();

    const String   oldAirport   = airportId;
    const uint16_t oldInterval  = updateInterval;

    configComplete &= getJsonValue(top["Enabled"],               enabled,       false);
    configComplete &= getJsonValue(top["Auto Fetch on Boot"],    autoFetchBoot, false);
    configComplete &= getJsonValue(top["Airport ID"],            airportId,     "KPDX");
    configComplete &= getJsonValue(top["Update Frequency (min)"],updateInterval,(uint16_t)5);

    if (updateInterval < 1) updateInterval = 1;
    airportId = upperTrim(airportId);

    syncSegMapSize();
    if (top.containsKey("Mappings") && top["Mappings"].is<JsonArray>()) {
      JsonArray maps = top["Mappings"].as<JsonArray>();
      for (JsonObject m : maps) {
        uint16_t si = m["Segment"] | 0;
        if (si >= strip.getSegmentsNum()) continue;
        String mode = m["Mode"] | "SINGLE";
        if (mode == "SINGLE") {
          segMaps[si].mode = MAP_SINGLE;
          segMaps[si].wholeAirport = upperTrim(String(m["Airport"] | ""));
          if (!segMaps[si].wholeAirport.length()) segMaps[si].wholeAirport = airportId;
          segMaps[si].perLed.clear();
        } else if (mode == "MULTIPLE") {
          segMaps[si].mode = MAP_MULTIPLE;
          String csv = m["CSV"] | "";
          parseLedCsv(si, csv);
        }
      }
    }

    if (updateInterval != oldInterval) initOrRealignSchedule(false);
    if (!airportId.equals(oldAirport)) {
      SA_DBG("airport change via settings: %s -> %s\n", oldAirport.c_str(), airportId.c_str());
      refreshNow = true;
      initOrRealignSchedule(true);
    }

    SA_DBG("config: enabled=%d, autoBoot=%d, airport=%s, interval=%u (complete=%d)\n",
           (int)enabled, (int)autoFetchBoot, airportId.c_str(), updateInterval, (int)configComplete);
    return configComplete;
  }

  void addToJsonInfo(JsonObject& obj) override {
    JsonObject s = obj.createNestedObject("SkyAware");
    s["enabled"]    = enabled;
    s["autoBoot"]   = autoFetchBoot;
    s["airport"]    = airportId;
    s["category"]   = lastCat;
    s["interval"]   = updateInterval;
    s["http"]       = lastHttp;
    s["err"]        = lastErr;
    s["url"]        = lastUrl;
    s["bodyPrev"]   = lastBodyPreview;
    s["lastOkSec"]  = (int)(lastOkMs/1000);
    s["fetchMs"]    = (int)lastFetchDurMs;
    s["ok"]         = successCount;
    s["fail"]       = failureCount;
    s["bootWaitMs"] = (int)BOOT_WIFI_SETTLE_MS;

    JsonArray a = s.createNestedArray("mappedAirports");
    std::vector<String> aps; gatherAllAirports(aps);
    for (auto& id : aps) { JsonObject o = a.createNestedObject(); o["id"]=id; o["cat"]=getCatForAirport(id); }

    JsonObject nd = s.createNestedObject("net");
    nd["host"]=lastDiag.host; nd["ip"]=lastDiag.ip; nd["http"]=lastDiag.httpCode;
    nd["redirect"]=lastDiag.redirect; nd["dnsMs"]=(int)lastDiag.dnsMs; nd["tcpMs"]=(int)lastDiag.tcpMs;
    nd["tlsMs"]=(int)lastDiag.tlsMs; nd["httpMs"]=(int)lastDiag.httpMs; nd["bytes"]=lastDiag.bytes;
    nd["ok"]=lastDiag.ok; nd["stage"]=lastDiag.errStage; nd["detail"]=lastDiag.errDetail;
    nd["dnsProvider"]=lastDiag.dnsProvider; nd["dnsFallback"]=lastDiag.dnsFallbackUsed;
  }

  uint16_t getId() override { return USERMOD_ID_SKYAWARE; }
};

static SkyAwareUsermod skyaware;
REGISTER_USERMOD(skyaware);
const uint32_t SkyAwareUsermod::RETRY_DELAYS_MS[4] = { 0, 30000, 90000, 180000 };