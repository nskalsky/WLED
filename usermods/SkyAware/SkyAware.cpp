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
  #define SKYAWARE_LOG_CAP (11 * 1024)
#endif

// ----------------------- ring log -----------------------------
struct SA_RingLog {
  static const size_t CAP = SKYAWARE_LOG_CAP;
  char    buf[CAP];
  size_t  head = 0;
  size_t  size = 0;
  void clear() { head = 0; size = 0; }
  void push(char c) {
    buf[(head + size) % CAP] = c;
    if (size < CAP) size++;
    else head = (head + 1) % CAP;
  }
  void write(const char* s, size_t n) { for (size_t i = 0; i < n; i++) push(s[i]); }
  void writeStr(const char* s) { write(s, strlen(s)); }
  void dumpTo(Print& out) { for (size_t i = 0; i < size; i++) out.write(buf[(head + i) % CAP]); }
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
    return RGBW32(255,255,255,0);
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

  // ===== LED IDENTIFICATION FEATURE (Multi-Airport Mode) =====
  // Overlays a blinking LED on top of segment rendering
  
// Overlays a cyan pixel at the absolute LED index, independent of mapping.
void updateLedIdentification() {
  if (!ledIdent.active) return;

  // Simple blink: ~2 Hz (on 3 frames, off 1)
  bool on = ((millis() >> 8) & 0x03) != 0;
  if (on) strip.setPixelColor(ledIdent.ledIndex, RGBW32(0,255,255,0)); // cyan
  else    strip.setPixelColor(ledIdent.ledIndex, 0);

  strip.trigger();
}


void startLedIdentification(uint16_t ledAbsIndex) {
  if (ledAbsIndex >= strip.getLength()) {
    SA_DBG("invalid LED index: %u (max %u)\n", ledAbsIndex, strip.getLength() - 1);
    return;
  }
  ledIdent.active   = true;
  ledIdent.ledIndex = ledAbsIndex;
  ledIdent.originalCategory = ""; // default; set below if we find an airport

  // Try to find mapped airport (optional: to preserve/restore category nicely)
  uint16_t numSegs = strip.getSegmentsNum();
  for (uint16_t si = 0; si < numSegs; ++si) {
    Segment& seg = strip.getSegment(si);
    if (ledAbsIndex < seg.start || ledAbsIndex >= seg.stop) continue;

    SegmentMap& sm = segMaps[si];
    uint16_t ledPosInSeg = ledAbsIndex - seg.start;

    if (sm.mode == MAP_MULTIPLE && ledPosInSeg < sm.perLed.size()) {
      const LedEntry& le = sm.perLed[ledPosInSeg];
      if (le.type == LT_ICAO && le.value.length()) {
        String ap = le.value;
        ledIdent.originalCategory = getCatForAirport(ap);
        setCatForAirport(ap, "IDENT", true); // keeps UI chips consistent, but overlay wins
      }
    } else if (sm.mode == MAP_SINGLE) {
      String ap = sm.wholeAirport.length() ? sm.wholeAirport : airportId;
      ledIdent.originalCategory = getCatForAirport(ap);
      setCatForAirport(ap, "IDENT", true);
    }
    break; // found segment containing the LED
  }

  // Ensure the cyan overlay is visible right away
  updateLedIdentification();
}


  void stopLedIdentification() {
    if (!ledIdent.active) return;
    
    // Restore the original category
    uint16_t numSegs = strip.getSegmentsNum();
    for (uint16_t si = 0; si < numSegs; ++si) {
      Segment& seg = strip.getSegment(si);
      uint16_t start = seg.start, stop = seg.stop;
      
      if (ledIdent.ledIndex >= start && ledIdent.ledIndex < stop) {
        SegmentMap& sm = segMaps[si];
        uint16_t ledPosInSeg = ledIdent.ledIndex - start;
        
        if (sm.mode == MAP_MULTIPLE && ledPosInSeg < sm.perLed.size()) {
          const LedEntry& le = sm.perLed[ledPosInSeg];
          if (le.type == LT_ICAO && le.value.length()) {
            String ap = le.value;
            setCatForAirport(ap, ledIdent.originalCategory, true);
            SA_DBG("LED %u stopped: airport %s restored to %s\n", 
                   ledIdent.ledIndex, ap.c_str(), ledIdent.originalCategory.c_str());
            break;
          }
        } else if (sm.mode == MAP_SINGLE) {
          String ap = sm.wholeAirport.length() ? sm.wholeAirport : airportId;
          setCatForAirport(ap, ledIdent.originalCategory, true);
          SA_DBG("LED %u stopped: airport %s restored to %s\n", 
                 ledIdent.ledIndex, ap.c_str(), ledIdent.originalCategory.c_str());
          break;
        }
      }
    }
    
    ledIdent.active = false;
  }

  // ===== END LED IDENTIFICATION =====

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

  bool fetchMetarMulti(const std::vector<String>& airports) {
    if (!WLED_CONNECTED) { lastErr="WiFi not connected"; return false; }
    if (airports.empty()) return true;

    String ids;
    for (size_t i=0;i<airports.size();++i) { if (i) ids+=','; ids += airports[i]; }

    lastUrl = "https://aviationweather.gov/api/data/metar?format=json&ids=" + ids;

    // Filter for multi: only station id + category
    StaticJsonDocument<256> filter;
    JsonArray farr = filter.to<JsonArray>();
    JsonObject any = farr.createNestedObject();
    any["icaoId"] = true; any["station"] = true; any["station_id"] = true;
    any["icao"]   = true; any["id"]      = true;
    any["fltCat"] = true; any["flight_category"] = true; any["fltcat"] = true;

    DynamicJsonDocument doc(1024);
    bool ok = fetchJsonWithDiag(lastUrl, doc, true, &filter);
    lastHttp = lastDiag.httpCode;
    if (!ok) { failureCount++; return false; }
    if (!doc.is<JsonArray>()) { lastErr="JSON not array"; failureCount++; return false; }
    JsonArray arr = doc.as<JsonArray>();
    if (arr.isNull()) { lastErr="null array"; failureCount++; return false; }

    bool anyOk=false;
    for (JsonVariant v : arr) {
      JsonObject o = v.as<JsonObject>();
      if (o.isNull()) continue;
      String stn = readStation(o);
      String cat = readFlightCategory(o);
      if (stn.length()) {
        setCatForAirport(stn, cat, true);
        anyOk = true;
      }
    }
    if (anyOk) {
      String prim = airports[0];
      lastCat = getCatForAirport(prim);
      lastMetarRaw = ""; metarStation=metarTimeZ=metarWind=metarVis=metarClouds=metarTempDew=metarAltim=metarWx=metarRemarks="";
      lastOkMs = millis();
    } else { lastErr="no stations parsed"; failureCount++; return false; }
    successCount++;
    return true;
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
      r->send(200,"text/plain","OK");
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

    // ===== LED IDENTIFICATION ENDPOINTS =====
    
    // LED identification endpoint - SIMPLE GET based to avoid POST issues
  server.on("/api/skyaware/led/identify", HTTP_GET, [this](AsyncWebServerRequest* r){
  if (!r->hasParam("idx")) { r->send(400, "application/json", "{\"error\":\"missing idx parameter\"}"); return; }
  uint16_t idx = (uint16_t) strtoul(r->getParam("idx")->value().c_str(), nullptr, 10);
  SA_DBG("LED identify GET request: idx=%u\n", idx);
  startLedIdentification(idx);
  applyLayoutColors();      // paint normal mapping
  updateLedIdentification(); // then overlay cyan at absolute index
  r->send(200, "application/json", "{\"status\":\"identifying\",\"led\":" + String(idx) + "}");
});


    // Cancel LED identification endpoint
    server.on("/api/skyaware/led/stop", HTTP_GET, [this](AsyncWebServerRequest* r){
      SA_DBG("LED identify STOP request\n");
      stopLedIdentification();
      applyLayoutColors();  // Apply immediately!
      r->send(200, "application/json", "{\"status\":\"stopped\"}");
    });

    // ===== END LED IDENTIFICATION ENDPOINTS =====

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
      // Still update LED identification even when not fetching
      updateLedIdentification();
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
      // Update LED identification AFTER segment rendering so it overlays on top
      updateLedIdentification();
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
      if (!anyGood) { for (uint16_t i=0;i<strip.getLength();++i) strip.setPixelColor(i, RGBW32(255,255,255,0)); strip.trigger(); }
      else applyLayoutColors();
      // Update LED identification AFTER segment rendering
      updateLedIdentification();
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