#include "wled.h"
#include <HTTPClient.h>
#include <WiFiClientSecure.h>
#include <stdarg.h>
#if defined(ARDUINO_ARCH_ESP32)
  #include <WiFi.h>
#else
  #include <ESP8266WiFi.h>
#endif

// ---------- ring log (8KB RAM; lower if you want) ----------
#ifndef SKYAWARE_LOG_CAP
  #define SKYAWARE_LOG_CAP (8 * 1024)
#endif
struct SA_RingLog {
  static const size_t CAP = SKYAWARE_LOG_CAP;
  char    buf[CAP];
  size_t  head = 0;     // index of oldest byte
  size_t  size = 0;     // number of bytes used
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

// ---------- logging ----------
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

class SkyAwareUsermod : public Usermod {
private:
  // ---- persisted config ----
  bool     enabled        = false; // <— stays idle until you turn this on
  bool     autoFetchBoot  = false; // optional boot-time fetch once Wi-Fi is up
  String   airportId      = "KPDX";
  uint16_t updateInterval = 5;     // minutes (min 1)
  static const uint32_t RETRY_DELAYS_MS[4];
  static const size_t   RETRY_COUNT = 4;

  // ---- runtime state ----
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

  // --- boot-time Wi-Fi settle logic (only if autoFetchBoot==true) ---
  bool          bootFetchPending   = false;
  bool          wasWifiConnected   = false;
  unsigned long wifiConnectedSince = 0;
  static constexpr uint32_t BOOT_WIFI_SETTLE_MS = 5000; // wait 5s after Wi-Fi up

  // ---- retry scheduler ----
  uint32_t nextScheduledAt = 0;   // next regular tick (start of next window)
  uint32_t windowDeadline  = 0;   // end of the retry window (== nextScheduledAt)
  uint32_t nextAttemptAt   = 0;   // when to attempt next fetch (regular or retry)
  size_t   retryIndex      = 0;   // which backoff step we’re on
  bool     inRetryWindow   = false;

  // Track interval changes without reboot
  uint32_t prevPeriodMs    = 0;

  // Last known good (held to mask transient outages)
  String   lastGoodCat     = "UNKNOWN";
  bool     haveGoodCat     = false;

  // safe millis compare
  static inline bool timeReached(uint32_t t) {
    return (int32_t)(millis() - t) >= 0;
  }

  // compute current window length in ms (follows config at runtime)
  uint32_t periodMs() const {
    return (uint32_t)max<uint16_t>(1, updateInterval) * 60000UL;
  }

  // (re)arm schedule to a stable cadence without drift
  void initOrRealignSchedule(bool startImmediate=false) {
    const uint32_t now = millis();
    const uint32_t period = periodMs();

    // first-time init or interval changed
    if (nextScheduledAt == 0 || prevPeriodMs != period) {
      prevPeriodMs = period;
      nextScheduledAt = now + period;
    }
    // ensure future
    while (timeReached(nextScheduledAt)) nextScheduledAt += period;

    windowDeadline = nextScheduledAt;
    nextAttemptAt  = startImmediate ? now : nextScheduledAt; // manual refresh can force immediate
    inRetryWindow  = false;
    retryIndex     = 0;
  }

  // Color mapping
  static uint32_t colorForCategory(const String& cat) {
    if (cat.equalsIgnoreCase("VFR"))   return RGBW32(  0,255,  0,0);
    if (cat.equalsIgnoreCase("MVFR"))  return RGBW32(  0,  0,255,0);
    if (cat.equalsIgnoreCase("IFR"))   return RGBW32(255,  0,  0,0);
    if (cat.equalsIgnoreCase("LIFR"))  return RGBW32(255,  0,255,0);
    return RGBW32(255,224,160,0); // warm-ish white for unknown/error
  }

  static const char* colorNameForCategory(const String& cat) {
    if (cat.equalsIgnoreCase("VFR"))   return "GREEN";
    if (cat.equalsIgnoreCase("MVFR"))  return "BLUE";
    if (cat.equalsIgnoreCase("IFR"))   return "RED";
    if (cat.equalsIgnoreCase("LIFR"))  return "MAGENTA";
    return "AMBER";
  }

  void applyCategoryColor(const String& cat) {
    uint32_t c = colorForCategory(cat);
    Segment& seg = strip.getSegment(0);
    seg.setOption(SEG_OPTION_ON, true);
    seg.setMode(FX_MODE_STATIC);
    seg.setColor(0, c);
    strip.trigger();
    SA_DBG("applyCategoryColor: %s -> %s\n", cat.c_str(), colorNameForCategory(cat));
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

  static String readMetarRaw(JsonObject o) {
    const char* keys[] = {"rawOb","raw_text","raw","metar","raw_ob","metar_text"};
    for (size_t i=0;i<sizeof(keys)/sizeof(keys[0]);++i) {
      if (o.containsKey(keys[i])) return String((const char*)o[keys[i]]);
    }
    return "";
  }

  static bool isAllDigits(const String& s) {
    for (size_t i=0;i<s.length();++i) if (s[i]<'0'||s[i]>'9') return false;
    return s.length() > 0;
  }

  static bool looksLikeTimeZ(const String& tok) {
    // e.g. 191753Z : 6 digits + 'Z'
    if (!tok.endsWith("Z")) return false;
    String core = tok.substring(0, tok.length()-1);
    return core.length()==6 && isAllDigits(core);
  }

  static bool looksLikeWind(const String& tok) {
    // 00000KT, 20012G20KT, VRB03KT, 12012KT
    if (!tok.endsWith("KT")) return false;
    if (tok.startsWith("VRB")) return true;
    if (tok.length() < 7) return false; // e.g. dddffKT minimal length
    String d = tok.substring(0,3);
    return isAllDigits(d);
  }

  static bool looksLikeVis(const String& tok) {
    // 10SM, 3/4SM, 1 1/2SM (will capture the first part 1 or 1 1/2 later)
    return tok.endsWith("SM");
  }

  static bool looksLikeCloud(const String& tok) {
    // FEW/SCT/BKN/OVCxxx, VVxxx
    if (tok.startsWith("VV"))  return tok.length()>=4 && isAllDigits(tok.substring(2,5));
    if (tok.startsWith("FEW") || tok.startsWith("SCT") || tok.startsWith("BKN") || tok.startsWith("OVC")) return true;
    return false;
  }

  static bool looksLikeTempDew(const String& tok) {
    // M05/M10, 18/12, 03/M01
    int slash = tok.indexOf('/');
    if (slash <= 0 || slash >= (int)tok.length()-1) return false;
    // tolerate 'M' prefix
    auto okHalf = [](const String& x){
      String y=x;
      if (y.startsWith("M")) y.remove(0,1);
      return y.length()>=1 && y.length()<=2 && isAllDigits(y);
    };
    return okHalf(tok.substring(0,slash)) && okHalf(tok.substring(slash+1));
  }

  static bool looksLikeAltim(const String& tok) {
    // A2992, Q1013
    if (tok.startsWith("A") && tok.length()==5 && isAllDigits(tok.substring(1))) return true;
    if (tok.startsWith("Q") && tok.length()==5 && isAllDigits(tok.substring(1))) return true;
    return false;
  }

  static bool looksLikeStation(const String& tok) {
    if (tok.length()<4 || tok.length()>5) return false;
    for (size_t i=0;i<tok.length();++i) {
      char c=tok[i];
      if (c<'A'||c>'Z') return false;
    }
    return true;
  }

  static bool looksLikeWx(const String& tok) {
    // Common weather codes (not exhaustive but useful)
    const char* wx[] = {"RA","DZ","SN","SG","PL","GR","GS","IC","UP",
                        "BR","FG","FU","VA","DU","SA","HZ","PY",
                        "TS","SH","FZ","PO","SQ","FC","SS","DS"};
    String t = tok; t.replace("+",""); t.replace("-",""); t.replace("VC","");
    // e.g. "-RA", "SHRA", "TSRA", "FZRASN"
    for (size_t i=0;i<sizeof(wx)/sizeof(wx[0]);++i) {
      if (t.indexOf(wx[i])!=-1) return true;
    }
    return false;
  }

  void parseMetarIntoFields(const String& metar) {
    metarStation = metarTimeZ = metarWind = metarVis = metarClouds = metarTempDew = metarAltim = metarWx = metarRemarks = "";
    if (metar.length()==0) return;

    // Split by spaces (no std::vector needed)
    const int MAXT = 80;
    String toks[MAXT];
    int nt = 0;
    int i = 0, n = metar.length();
    while (i < n && nt < MAXT) {
      while (i < n && metar[i] == ' ') i++;
      int j = i;
      while (j < n && metar[j] != ' ') j++;
      if (j > i) toks[nt++] = metar.substring(i, j);
      i = j + 1;
    }

    // Find remarks start, if present
    int idxRMK = -1;
    for (int k = 0; k < nt; k++) {
      if (toks[k] == "RMK") { idxRMK = k; break; }
    }

    // Extract key fields
    for (int k = 0; k < nt; k++) {
      const String& t = toks[k];

      // Skip common non-station leading tokens
      if (t == "METAR" || t == "SPECI" || t == "AUTO" || t == "=") continue;

      if (metarStation == "" && looksLikeStation(t)) { metarStation = t; continue; }
      if (metarTimeZ  == "" && looksLikeTimeZ(t))    { metarTimeZ  = t; continue; }
      if (metarWind   == "" && looksLikeWind(t))     { metarWind   = t; continue; }

      if (metarVis == "" && looksLikeVis(t)) {
        // Handle “1 1/2SM” pattern (stitch with previous numeric token)
        if (k > 0 && isAllDigits(toks[k-1])) metarVis = toks[k-1] + " " + t;
        else metarVis = t;
        continue;
      }

      if (looksLikeCloud(t)) {
        if (metarClouds.length()) metarClouds += " ";
        metarClouds += t;
        continue;
      }

      if (metarTempDew == "" && looksLikeTempDew(t)) { metarTempDew = t; continue; }
      if (metarAltim   == "" && looksLikeAltim(t))   { metarAltim   = t; continue; }

      if (looksLikeWx(t)) {
        if (metarWx.length()) metarWx += " ";
        metarWx += t;
        continue;
      }
    }

    // Stitch remarks
    if (idxRMK >= 0 && idxRMK < nt - 1) {
      String r;
      for (int k = idxRMK + 1; k < nt; k++) {
        if (r.length()) r += ' ';
        r += toks[k];
      }
      metarRemarks = r;
    }
  }

  bool fetchMetarOnce() {
    if (!WLED_CONNECTED) {
      lastErr = "WiFi not connected";
      SA_DBG("%s\n", lastErr.c_str());
      return false;
    }

    lastUrl = "https://aviationweather.gov/api/data/metar?format=json&ids=" + airportId;
    lastErr = ""; lastHttp = 0; lastBodyPreview = "";
    lastFetchStartMs = millis();
    SA_DBG("fetch: %s\n", lastUrl.c_str());

    WiFiClientSecure client;
    client.setInsecure();
    HTTPClient http;
    http.setUserAgent("WLED-SkyAware/0.1 (+esp)");
    http.setConnectTimeout(8000);
    http.setReuse(false);
    http.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);

    if (!http.begin(client, lastUrl)) {
      lastErr = "http.begin() failed";
      SA_DBG("%s\n", lastErr.c_str());
      return false;
    }

    int code = http.GET();
    lastHttp = code;
    SA_DBG("HTTP %d\n", code);

    if (code != HTTP_CODE_OK) {
      lastErr = String("HTTP ") + code;
      http.end();
      lastFetchDurMs = millis() - lastFetchStartMs;
      failureCount++;
      SA_DBG("fail: %s (dur=%lums)\n", lastErr.c_str(), lastFetchDurMs);
      return false;
    }

    // stream parse to avoid big stack/String copies
    WiFiClient& stream = http.getStream();
    DynamicJsonDocument doc(2048); // single-station METAR JSON
    DeserializationError err = deserializeJson(doc, stream);
    http.end(); // close network before we do more work

    if (err) {
      lastErr = String("JSON: ") + err.c_str();
      lastFetchDurMs = millis() - lastFetchStartMs;
      failureCount++;
      SA_DBG("parse error: %s (dur=%lums)\n", lastErr.c_str(), lastFetchDurMs);
      return false;
    }

    if (!doc.is<JsonArray>()) {
      lastErr = "JSON not array";
      lastFetchDurMs = millis() - lastFetchStartMs;
      failureCount++;
      SA_DBG("%s (dur=%lums)\n", lastErr.c_str(), lastFetchDurMs);
      return false;
    }

    JsonArray arr = doc.as<JsonArray>();
    if (arr.isNull() || arr.size() == 0) {
      lastErr = "empty array";
      lastFetchDurMs = millis() - lastFetchStartMs;
      failureCount++;
      SA_DBG("%s (dur=%lums)\n", lastErr.c_str(), lastFetchDurMs);
      return false;
    }

    JsonObject first = arr[0].as<JsonObject>();
    if (first.isNull()) {
      lastErr = "first element not object";
      lastFetchDurMs = millis() - lastFetchStartMs;
      failureCount++;
      SA_DBG("%s (dur=%lums)\n", lastErr.c_str(), lastFetchDurMs);
      return false;
    }

#ifdef WLED_DEBUG
    { String firstStr; serializeJson(first, firstStr);
      SA_DBG("first METAR json: %s\n", firstStr.c_str()); }
#endif

    String cat = readFlightCategory(first);
    SA_DBG("flight_category=%s\n", cat.c_str());

    lastCat = cat;
    applyCategoryColor(cat);             // show the *good* result now

    // Record last-known-good (for outage masking)
    lastGoodCat = cat;
    haveGoodCat = !cat.equalsIgnoreCase("UNKNOWN");

    lastOkMs = millis();
    lastFetchDurMs = lastOkMs - lastFetchStartMs;
    successCount++;

#if defined(ARDUINO_ARCH_ESP32)
    SA_DBG("ok: dur=%lums, success=%lu, failure=%lu, heap=%lu\n",
           lastFetchDurMs, (unsigned long)successCount, (unsigned long)failureCount,
           (unsigned long)ESP.getFreeHeap());
#else
    SA_DBG("ok: dur=%lums, success=%lu, failure=%lu\n",
           lastFetchDurMs, (unsigned long)successCount, (unsigned long)failureCount);
#endif

    lastMetarRaw = readMetarRaw(first);
    parseMetarIntoFields(lastMetarRaw);

    return true;
  }

  // ---- immediate fetch trigger (NEW) ----
  void triggerImmediateFetch(const char* why) {
    SA_DBG("immediate fetch queued (%s)\n", why ? why : "n/a");
    refreshNow = true;            // loop() consumes this
    initOrRealignSchedule(true);  // nextAttemptAt = now, cadence preserved
  }

  void writeStateJson(Print& out) {
    StaticJsonDocument<2048> d;
    JsonObject s = d.createNestedObject("SkyAware");
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
    s["pendingFetch"] = (bool)refreshNow; // <— NEW: reflect queued fetch

    // METAR details
    s["metarRaw"] = lastMetarRaw;
    JsonObject mp = s.createNestedObject("metar");
    mp["station"] = metarStation;
    mp["timeZ"]   = metarTimeZ;
    mp["wind"]    = metarWind;
    mp["vis"]     = metarVis;
    mp["clouds"]  = metarClouds;
    mp["tempDew"] = metarTempDew;
    mp["altim"]   = metarAltim;
    mp["wx"]      = metarWx;
    mp["remarks"] = metarRemarks;

    serializeJson(d, out);
  }

public:
  // ---------- core hooks ----------
  void setup() override {
    SA_DBG("setup: airport=%s, interval=%u min, enabled=%d, autoFetch=%d\n",
           airportId.c_str(), updateInterval, (int)enabled, (int)autoFetchBoot);

    extern AsyncWebServer server;  // WLED global server object

    // Enable/disable at runtime (doesn't persist)
    server.on("/um/skyaware/enable", HTTP_GET, [this](AsyncWebServerRequest* r){
      bool on = r->hasParam("on") && (r->getParam("on")->value() == "1");
      enabled = on;
      SA_DBG("runtime enable=%d\n", (int)enabled);
      r->send(200, "text/plain", enabled ? "SkyAware: enabled" : "SkyAware: disabled");
    });

    // Simple numeric endpoint: returns successful METAR fetches since boot
    server.on("/um/skyaware/okcount", HTTP_GET, [this](AsyncWebServerRequest* r){
      r->send(200, "text/plain", String(successCount));
    });

    // Force a fetch now (works even if enabled=false)
    server.on("/um/skyaware/refresh", HTTP_GET, [this](AsyncWebServerRequest* r){
      SA_DBG("manual refresh requested\n");
      refreshNow = true;
      r->send(200, "text/plain", "SkyAware: refresh queued");
    });

    // (NEW) Change airport via API and fetch immediately
    server.on("/um/skyaware/set", HTTP_GET, [this](AsyncWebServerRequest* r){
      if (!r->hasParam("airport")) { r->send(400, "text/plain", "missing airport"); return; }
      String a = r->getParam("airport")->value(); a.trim(); a.toUpperCase();
      if (a == "" || a.length()<3 || a.length()>8) { r->send(400, "text/plain", "bad airport"); return; }
      if (!a.equals(airportId)) {
        SA_DBG("airport change via /set: %s -> %s\n", airportId.c_str(), a.c_str());
        airportId = a;
        triggerImmediateFetch("/um/skyaware/set");
      }
      r->send(200, "text/plain", airportId);
    });

    // State JSON
    server.on("/um/skyaware/state.json", HTTP_GET, [this](AsyncWebServerRequest* r){
      AsyncResponseStream* res = r->beginResponseStream("application/json");
      writeStateJson(*res);
      r->send(res);
    });

    // Diagnostics: Wi-Fi + loop gating
    server.on("/um/skyaware/diag.json", HTTP_GET, [this](AsyncWebServerRequest* r){
      StaticJsonDocument<512> d;
      JsonObject j = d.to<JsonObject>();
      j["wledConnected"] = (bool)WLED_CONNECTED;
      j["wifiStatus"]    = (int)WiFi.status();     // WL_CONNECTED == 3
      j["staIP"]         = WiFi.localIP().toString();
      j["apIP"]          = WiFi.softAPIP().toString();
      j["ssid"]          = WiFi.SSID();
      j["rssi"]          = WiFi.RSSI();
      j["bootPending"]   = bootFetchPending;
      j["wasWifi"]       = wasWifiConnected;
      j["sinceMs"]       = (int)(millis() - wifiConnectedSince);
      j["lastRunMs"]     = (int)lastRunMs;
      j["refreshNow"]    = refreshNow;
      j["busy"]          = busy;
      AsyncResponseStream* res = r->beginResponseStream("application/json");
      serializeJson(d, *res);
      r->send(res);
    });

    // Ring log endpoints
    server.on("/um/skyaware/log.txt", HTTP_GET, [](AsyncWebServerRequest* r){
      AsyncResponseStream* res = r->beginResponseStream("text/plain; charset=utf-8");
      SA_LOGBUF.dumpTo(*res);
      r->send(res);
    });
    server.on("/um/skyaware/log/clear", HTTP_GET, [](AsyncWebServerRequest* r){
      SA_LOGBUF.clear();
      r->send(200, "text/plain", "SkyAware log cleared");
    });

    server.on("/um/skywizard/get", HTTP_GET, [this](AsyncWebServerRequest* r){
      String json = String("{\"airport\":\"") + airportId + "\"}";
      r->send(200, "application/json", json);
    });

    // Let the captive page save the SAME settings as /settings/um
    server.on("/um/skywizard/save", HTTP_POST, [this](AsyncWebServerRequest* r){
      auto get = [&](const char* name)->String{
        if (r->hasParam(name, true)) return r->getParam(name, true)->value();
        if (r->hasParam(name))       return r->getParam(name)->value();
        return String();
      };

      // Only set what you care about (airport). You can add more later.
      String a = get("airport");
      a.trim(); a.toUpperCase();

      // basic validation: 3–8 alnum
      bool ok = (a.length()>=3 && a.length()<=8);
      for (size_t i=0; i<a.length() && ok; i++) {
        char c=a[i]; ok = ((c>='A'&&c<='Z')||(c>='0'&&c<='9'));
      }
      if (ok) {
        if (!a.equals(airportId)) {              // only trigger if it actually changed
          SA_DBG("airport change: %s -> %s\n", airportId.c_str(), a.c_str());
          airportId = a;                          // update runtime field used by your usermod
          triggerImmediateFetch("wizard.save airport changed");
        }
      }

      // go where you want next (default to Wi-Fi setup)
      String redir = get("redir");
      if (!redir.length()) redir = "/settings/wifi";
      auto* resp = r->beginResponse(302, "text/plain", "saved");
      resp->addHeader("Location", redir);
      r->send(resp);
    });

    server.on("/skyaware/status", HTTP_GET, [this](AsyncWebServerRequest* r){
      String html =
        "<!doctype html><meta name=viewport content='width=device-width,initial-scale=1'>"
        "<style>"
          "body{font-family:system-ui,-apple-system,Segoe UI,Roboto,Helvetica,Arial,sans-serif;padding:16px;line-height:1.35;background:#fafafa;color:#111}"
          "button{padding:8px 14px;border-radius:10px;border:1px solid #bbb;cursor:pointer;background:#fff}"
          "pre{max-height:40vh;overflow:auto;background:#111;color:#0f0;padding:8px;border-radius:10px}"
          ".row{display:flex;flex-wrap:wrap;gap:16px;align-items:center}"
          ".card{border:1px solid #e5e5e5;border-radius:14px;padding:12px 14px;background:#fff;box-shadow:0 1px 2px rgba(0,0,0,.04)}"
          ".big{font-size:1.8rem;font-weight:700}"
          ".muted{color:#666}"
          ".badge{display:inline-block;border-radius:999px;padding:6px 10px;border:1px solid #ccc;background:#fff}"
          ".grid{display:grid;grid-template-columns:repeat(auto-fit,minmax(180px,1fr));gap:12px;margin-top:10px}"
          ".kv{border:1px solid #eee;border-radius:12px;padding:10px;background:#fff}"
          ".kv h4{margin:0 0 6px 0;font-size:.85rem;color:#555;font-weight:600}"
          ".kv .v{font-family:ui-monospace,Menlo,Consolas,monospace}"
          ".catTag{display:inline-block;padding:6px 14px;border-radius:999px;border:1px solid #ccc;background:#f7f7f7;min-width:110px;text-align:center}"
          ".catTag.VFR{background:#0a0;color:#fff;border-color:#070}"
          ".catTag.MVFR{background:#08f;color:#fff;border-color:#06c}"
          ".catTag.IFR{background:#d00;color:#fff;border-color:#a00}"
          ".catTag.LIFR{background:#c0f;color:#111;border-color:#90c}"
          ".catTag.UNKNOWN{background:#f6c57f;color:#111;border-color:#dea54a}"
        "</style>"
        "<h2>SkyAware Debug</h2>"

        "<div class='row'>"
          "<button onclick=\"fetch('/um/skyaware/refresh').then(()=>setTimeout(load,400))\">Fetch Now</button>"
          "<label style='display:inline-flex;align-items:center;gap:.5rem;margin-left:1rem'>"
            "<input type='checkbox' id='en'> Enabled"
          "</label>"
          "<a href='/settings/um' style='margin-left:auto'>← Back to Usermods</a>"
        "</div>"

        "<div class='row' style='margin-top:12px'>"
          "<div class='card'><div class='muted'>Valid METAR fetches since boot</div>"
            "<div class='big' id='okCount'>0</div></div>"
          "<div class='card'><div class='muted'>Failures since boot</div>"
            "<div class='big' id='failCount'>0</div></div>"
          "<div class='card'><div class='muted'>Last fetch (ms)</div>"
            "<div class='big' id='fetchMs'>0</div></div>"
          "<div class='card'><div class='muted'>Last HTTP</div>"
            "<div class='big' id='httpCode'>0</div></div>"
          "<div class='card'><div class='muted'>Category</div>"
            "<div class='big'><span id='catTag' class='catTag UNKNOWN'>UNKNOWN</span></div></div>"
        "</div>"

        "<p class='muted badge'>Also available: <a href='/um/skyaware/okcount' target=_blank>/um/skyaware/okcount</a>, "
        "<a href='/um/skyaware/diag.json' target=_blank>diag.json</a>, "
        "<a href='/um/skyaware/state.json' target=_blank>state.json</a></p>"

        "<h3>METAR</h3>"
        "<div class='card'>"
          "<div class='grid'>"
            "<div class='kv'><h4>Station</h4><div class='v' id='m_station'>—</div></div>"
            "<div class='kv'><h4>Time (Z)</h4><div class='v' id='m_time'>—</div></div>"
            "<div class='kv'><h4>Wind</h4><div class='v' id='m_wind'>—</div></div>"
            "<div class='kv'><h4>Visibility</h4><div class='v' id='m_vis'>—</div></div>"
            "<div class='kv'><h4>Clouds</h4><div class='v' id='m_clouds'>—</div></div>"
            "<div class='kv'><h4>Temp/Dew</h4><div class='v' id='m_tempdew'>—</div></div>"
            "<div class='kv'><h4>Altimeter</h4><div class='v' id='m_altim'>—</div></div>"
            "<div class='kv'><h4>Weather</h4><div class='v' id='m_wx'>—</div></div>"
            "<div class='kv' style='grid-column:1/-1'><h4>Remarks</h4><div class='v' id='m_rmk' style='white-space:pre-wrap'>—</div></div>"
          "</div>"
          "<div style='margin-top:10px'><h4 class='muted' style='margin:0 0 6px 0'>Raw</h4>"
            "<pre id='metarRaw'>—</pre></div>"
        "</div>"

        "<h3>State</h3><pre id='state'>loading…</pre>"

        "<h3>Live Log</h3>"
        "<p><a href='/um/skyaware/log.txt' target=_blank>Open raw log</a> "
        "<button onclick='fetch(\"/um/skyaware/log/clear\").then(()=>setTimeout(loadLog,300))'>Clear</button></p>"
        "<pre id='log'>loading…</pre>"

        "<script>"
          "function applyCatTag(cat){"
            "const el=document.getElementById('catTag');"
            "el.textContent = (cat||'UNKNOWN').toUpperCase();"
            "el.className = 'catTag ' + el.textContent;"
          "}"
          "function set(id,val){ document.getElementById(id).textContent = (val&&String(val).length)?val:'—'; }"
          "async function load(){"
            "try{"
              "const j=await fetch('/um/skyaware/state.json').then(r=>r.json());"
              "const s=j.SkyAware||{};"
              "document.getElementById('state').textContent=JSON.stringify(j,null,2);"
              "document.getElementById('en').checked=!!s.enabled;"
              "document.getElementById('okCount').textContent = s.ok||0;"
              "document.getElementById('failCount').textContent = s.fail||0;"
              "document.getElementById('fetchMs').textContent = s.fetchMs||0;"
              "document.getElementById('httpCode').textContent = s.http||0;"
              "applyCatTag(s.category||'UNKNOWN');"
              "set('metarRaw', s.metarRaw||'');"
              "const m=s.metar||{};"
              "set('m_station', m.station);"
              "set('m_time',    m.timeZ);"
              "set('m_wind',    m.wind);"
              "set('m_vis',     m.vis);"
              "set('m_clouds',  m.clouds);"
              "set('m_tempdew', m.tempDew);"
              "set('m_altim',   m.altim);"
              "set('m_wx',      m.wx);"
              "set('m_rmk',     m.remarks);"
            "}catch(e){"
              "document.getElementById('state').textContent='ERR '+e"
            "}"
          "}"
          "async function loadLog(){"
            "try{"
              "const t=await fetch('/um/skyaware/log.txt').then(r=>r.text());"
              "const lines=t.split('\\n');"
              "document.getElementById('log').textContent=lines.slice(-300).join('\\n');"
            "}catch(e){"
              "document.getElementById('log').textContent='ERR '+e"
            "}"
          "}"
          "document.getElementById('en').addEventListener('change',e=>{"
            "fetch('/um/skyaware/enable?on='+(e.target.checked?1:0)).then(()=>setTimeout(load,300));"
          "});"
          "setInterval(load,1500); setInterval(loadLog,1500); load(); loadLog();"
        "</script>";
      r->send(200, "text/html", html);
    });

    // Only arm boot-fetch if user asked for it
    bootFetchPending = autoFetchBoot;
    initOrRealignSchedule(false);
    SA_DBG("setup done (bootFetchPending=%d)\n", (int)bootFetchPending);
  } // <-- CLOSES setup()

  void loop() override {
    static bool firstLoop = true;
    if (firstLoop) { firstLoop = false; SA_DBG("loop entered\n"); }
    if (busy) return;

    // Optional boot-time fetch after Wi-Fi comes up (only if autoFetchBoot)
    if (bootFetchPending) {
      if (WLED_CONNECTED) {
        if (!wasWifiConnected) {
          wasWifiConnected = true;
          wifiConnectedSince = millis();
          SA_DBG("WiFi connected; settling for %u ms\n", (unsigned)BOOT_WIFI_SETTLE_MS);
        }
        if (millis() - wifiConnectedSince >= BOOT_WIFI_SETTLE_MS) {
          SA_DBG("Boot-time fetch now\n");
          refreshNow = true;
          bootFetchPending = false;
        }
      } else {
        if (wasWifiConnected) SA_DBG("WiFi dropped; waiting again\n");
        wasWifiConnected = false;
      }
    }

    // If not enabled and no manual refresh queued, stay idle
    if (!enabled && !refreshNow) return;

    const uint32_t now    = millis();
    const uint32_t period = periodMs();

    // Initialize or realign schedule if needed (e.g., after settings change)
    if (nextScheduledAt == 0 || prevPeriodMs != period) {
      initOrRealignSchedule(false);
    }

    // Manual/Immediate refresh: run attempt now without disturbing cadence.
    if (refreshNow) {
      // Start a transient mini-window that ends at the current nextScheduledAt
      nextAttemptAt  = now;               // attempt now
      windowDeadline = nextScheduledAt;   // do not cross the regular boundary
      inRetryWindow  = true;              // allow backoffs if needed
      retryIndex     = 0;
      refreshNow     = false;
    }

    // Not time yet to attempt?
    if (!timeReached(nextAttemptAt)) return;

    // ===== Perform one attempt (either scheduled tick or in-window retry) =====
    SA_DBG("loop: attempt (window ends in %lus)\n",
           (unsigned long)((windowDeadline > now) ? ((windowDeadline - now)/1000) : 0));

    busy = true;
    const bool ok = fetchMetarOnce();
    busy = false;

    if (ok) {
      // SUCCESS: keep LEDs as set by fetch (lastGoodCat), clear retry state.
      inRetryWindow = false;
      retryIndex = 0;

      // Advance cadence to the next whole period boundary (no drift)
      while (timeReached(nextScheduledAt)) nextScheduledAt += period;

      windowDeadline = nextScheduledAt;
      nextAttemptAt  = nextScheduledAt;

      lastRunMs = now;  // observability (how long since last activity)

      SA_DBG("ok: kept %s, next @ +%lus\n",
             lastGoodCat.c_str(),
             (unsigned long)((nextAttemptAt > millis()) ? ((nextAttemptAt - millis())/1000) : 0));
      return;
    }

    // ===== FAILURE: do NOT flip LEDs yet; try retries within the same window =====
    if (!inRetryWindow) {
      // Enter retry mode for this window
      inRetryWindow = true;
      retryIndex = 0;
      windowDeadline = nextScheduledAt;   // finish all retries before the boundary
    }

    // If the window has expired, decide what to show and move to next window.
    if (timeReached(windowDeadline)) {
      inRetryWindow = false;
      retryIndex = 0;

      if (haveGoodCat) {
        // Keep last known good; do not override with UNKNOWN
        SA_DBG("window expired, keeping last good: %s\n", lastGoodCat.c_str());
      } else {
        // No good data at all in this boot/session → show UNKNOWN/AMBER
        applyCategoryColor("UNKNOWN");
        SA_DBG("window expired → UNKNOWN/AMBER\n");
      }

      nextScheduledAt += period;
      windowDeadline = nextScheduledAt;
      nextAttemptAt  = nextScheduledAt;
      lastRunMs = now;
      return;
    }

    // Schedule another retry inside the window (backoff and clamp)
    if (retryIndex + 1 < RETRY_COUNT) retryIndex++;
    uint32_t candidate = millis() + RETRY_DELAYS_MS[retryIndex];
    nextAttemptAt = (candidate < windowDeadline) ? candidate : windowDeadline;

    SA_DBG("retry %u/%u at +%lus (window ends in %lus)\n",
           (unsigned)retryIndex, (unsigned)(RETRY_COUNT-1),
           (unsigned long)((nextAttemptAt > millis()) ? ((nextAttemptAt - millis())/1000) : 0),
           (unsigned long)((windowDeadline > millis()) ? ((windowDeadline - millis())/1000) : 0));
  }

  // ---------- config UI (persists via /settings/um) ----------
  void addToConfig(JsonObject& root) override {
    JsonObject top = root.createNestedObject("skyAwareUsermod");
    top["Enabled"]               = enabled;
    top["Auto Fetch on Boot"]    = autoFetchBoot;
    top["Airport ID"]            = airportId;      // e.g., "KPDX"
    top["Update Frequency (min)"]= updateInterval; // minutes
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
    airportId.toUpperCase();

    // realign if interval changed
    if (updateInterval != oldInterval) initOrRealignSchedule(false);

    // immediate fetch if airport changed
    if (!airportId.equals(oldAirport)) {
      SA_DBG("airport change via settings: %s -> %s\n", oldAirport.c_str(), airportId.c_str());
      triggerImmediateFetch("settings.um airport changed");
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
  }

  uint16_t getId() override { return USERMOD_ID_SKYAWARE; }
};

static SkyAwareUsermod skyaware;
REGISTER_USERMOD(skyaware);
const uint32_t SkyAwareUsermod::RETRY_DELAYS_MS[4] = { 0, 30000, 90000, 180000 };
