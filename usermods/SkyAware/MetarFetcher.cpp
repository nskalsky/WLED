// usermods/skyaware/MetarFetcher.cpp
#include "MetarFetcher.h"
#include "wled.h"

#include <HTTPClient.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>

#include <map>
#include <vector>
#include <algorithm>

// ===== lwIP DNS (ESP32) =====
#if defined(ARDUINO_ARCH_ESP32)
extern "C" {
  #include "lwip/dns.h"
  #include "lwip/ip_addr.h"
  #include "lwip/ip4_addr.h"
}
#endif

// -------------------- Config/persist paths --------------------
#ifndef SKY_CFG_DIR
  #define SKY_CFG_DIR "/skyaware"
#endif
#define METAR_CFG_PATH "/skyaware/metar.json"

// -------------------- Logging (runtime level + ring buffer) --------------------
#ifndef METAR_LOG_LEVEL_DEFAULT
  #define METAR_LOG_LEVEL_DEFAULT 1  // 0=OFF, 1=INFO, 2=DEBUG, 3=TRACE
#endif

static uint8_t g_logLevel = METAR_LOG_LEVEL_DEFAULT;

static const size_t SAF_LOG_CAP = 64;
static String g_logRing[SAF_LOG_CAP];
static size_t g_logIdx = 0;
static size_t g_logCount = 0;

static const char* lvlName(uint8_t lvl){
  switch(lvl){ case 0:return "OFF"; case 1:return "INFO"; case 2:return "DEBUG"; case 3:return "TRACE"; default:return "?"; }
}
static void saf_log_line(uint8_t lvl, const String& line) {
  if (lvl > g_logLevel) return;
  Serial.println(line);
  g_logRing[g_logIdx] = line;
  g_logIdx = (g_logIdx + 1) % SAF_LOG_CAP;
  if (g_logCount < SAF_LOG_CAP) g_logCount++;
}
static void saf_logf(uint8_t lvl, const char* fmt, ...) {
  if (lvl > g_logLevel) return;
  char buf[256];
  va_list ap; va_start(ap, fmt);
  vsnprintf(buf, sizeof(buf), fmt, ap);
  va_end(ap);
  saf_log_line(lvl, String(buf));
}

// -------------------- Settings (persisted) --------------------
struct SAF_Settings {
  bool     enable    = false;      // default disabled
  uint32_t freqMs    = 150000;     // 2.5 minutes
  uint16_t batchSize = 10;         // airports per HTTP query
};

static SAF_Settings         g_cfg;
static void*                g_ctx = nullptr;
static SAF_CollectIcaosFn   g_collect = nullptr;
static SAF_ApplyCategoryFn  g_apply   = nullptr;
static uint32_t             g_nextDue = 0;

// Work queue
static std::vector<String>  g_cycleIcaos;
static size_t               g_cyclePos = 0;
static bool                 g_inFlight = false;

// diag state
static String   g_lastUrl;
static int      g_lastHttpCode = 0;
static size_t   g_lastBodyLen  = 0;
static uint16_t g_lastMetarCount = 0;
static String   g_lastErr;
static uint32_t g_lastRunMs  = 0;
static uint32_t g_cycles     = 0;
static String   g_lastBodySample;

// ------------- DNS strategy -------------
enum SAF_DnsMode : uint8_t { DNS_DHCP_FIRST=0, DNS_GOOGLE_FIRST=1 };
static SAF_DnsMode g_dnsMode = DNS_DHCP_FIRST;

static IPAddress g_dhcpDns0, g_dhcpDns1;
static const IPAddress DNS_GOOGLE(8,8,8,8);
static const IPAddress DNS_GOOGLE_2(8,8,4,4);

static inline bool ip_is_set(const IPAddress& ip) {
  return !(ip[0]==0 && ip[1]==0 && ip[2]==0 && ip[3]==0);
}

static void saf_dns_captureDhcp()
{
  g_dhcpDns0 = WiFi.dnsIP(0);
  g_dhcpDns1 = WiFi.dnsIP(1);
  saf_logf(2, "[METAR] DHCP DNS: %s, %s", g_dhcpDns0.toString().c_str(), g_dhcpDns1.toString().c_str());
}

#if defined(ARDUINO_ARCH_ESP32)
static ip_addr_t to_ip_addr(const IPAddress& ip) {
  ip_addr_t a;
  IP4_ADDR(&a.u_addr.ip4, ip[0], ip[1], ip[2], ip[3]);
  #ifdef IP_SET_TYPE_VAL
    IP_SET_TYPE_VAL(a, IPADDR_TYPE_V4);
  #else
    a.type = IPADDR_TYPE_V4;
  #endif
  return a;
}
#endif

static void saf_dns_apply(SAF_DnsMode mode)
{
#if defined(ARDUINO_ARCH_ESP32)
  if (mode == DNS_GOOGLE_FIRST) {
    ip_addr_t prim = to_ip_addr(DNS_GOOGLE);
    IPAddress useSecondary = ip_is_set(g_dhcpDns0) ? g_dhcpDns0 : DNS_GOOGLE_2;
    ip_addr_t sec  = to_ip_addr(useSecondary);
    dns_setserver(0, &prim);
    dns_setserver(1, &sec);
    saf_logf(1, "[METAR] DNS set: primary=%s secondary=%s",
             DNS_GOOGLE.toString().c_str(), useSecondary.toString().c_str());
  } else {
    IPAddress s0ip = ip_is_set(g_dhcpDns0) ? g_dhcpDns0 : DNS_GOOGLE;
    IPAddress s1ip = ip_is_set(g_dhcpDns1) ? g_dhcpDns1 : DNS_GOOGLE_2;
    ip_addr_t s0 = to_ip_addr(s0ip);
    ip_addr_t s1 = to_ip_addr(s1ip);
    dns_setserver(0, &s0);
    dns_setserver(1, &s1);
    saf_logf(1, "[METAR] DNS restored: primary=%s secondary=%s",
             s0ip.toString().c_str(), s1ip.toString().c_str());
  }
#endif
  g_dnsMode = mode;
}

// -------------------- Persist helpers --------------------
static void saf_loadCfg() {
  if (!WLED_FS.exists(METAR_CFG_PATH)) return;
  File f = WLED_FS.open(METAR_CFG_PATH, "r");
  if (!f) return;
  DynamicJsonDocument d(256);
  if (deserializeJson(d, f) == DeserializationError::Ok) {
    if (d["enable"].is<bool>())      g_cfg.enable = d["enable"].as<bool>();
    if (d["freqMs"].is<uint32_t>())  g_cfg.freqMs = d["freqMs"].as<uint32_t>();
    if (d["batch"].is<uint16_t>())   g_cfg.batchSize = d["batch"].as<uint16_t>();
  }
  f.close();
}

static void saf_saveCfg() {
  if (!WLED_FS.exists(SKY_CFG_DIR)) WLED_FS.mkdir(SKY_CFG_DIR);
  File f = WLED_FS.open(METAR_CFG_PATH, "w");
  if (!f) return;
  DynamicJsonDocument d(256);
  d["enable"] = g_cfg.enable;
  d["freqMs"] = g_cfg.freqMs;
  d["batch"]  = g_cfg.batchSize;
  serializeJson(d, f);
  f.close();
}

static inline uint32_t saf_nowSec() {
  time_t t = time(nullptr);
  return (t > 100000) ? (uint32_t)t : (millis()/1000);
}

// -------------------- ICAO collection (unique, upper) --------------------
static void saf_collectUniqueUpper(std::vector<String>& out) {
  out.clear();
  if (!g_collect) return;

  std::vector<String> raw;
  g_collect(g_ctx, raw);

  std::map<String, bool, std::less<String>> seen;
  for (auto s : raw) {
    s.trim(); s.toUpperCase();
    if (s.length() != 4) continue;
    if (seen.find(s) != seen.end()) continue;
    seen[s] = true;
    out.push_back(s);
  }
  saf_logf(1, "[METAR] collect: %u unique ICAO", (unsigned)out.size());
}

// -------------------- HTTP helpers --------------------
static const char* UA_MAC_SAFARI =
  "Mozilla/5.0 (Macintosh; Intel Mac OS X 10_15_7) AppleWebKit/605.1.15 "
  "(KHTML, like Gecko) Version/15.1 Safari/605.1.15";

struct ParsedUrl {
  String scheme, host, path; bool ok=false;
};
static ParsedUrl parseUrl(const String& url) {
  ParsedUrl pu;
  int p = url.indexOf("://"); if (p<0) return pu;
  pu.scheme = url.substring(0,p);
  int hs=p+3; int slash=url.indexOf('/',hs); if (slash<0) return pu;
  pu.host = url.substring(hs,slash);
  pu.path = url.substring(slash);
  pu.ok = (pu.scheme.length() && pu.host.length() && pu.path.length());
  return pu;
}

static String buildAwcApiUrl(const String& csvStations) {
  String u(F("https://aviationweather.gov/api/data/metar?format=json&age=2&ids="));
  u += csvStations;
  return u;
}

static bool httpGetCore(HTTPClient& http, WiFiClientSecure& client, const String& url,
                        String& outBody, int& outCode, bool useHttp10, bool followRedirects)
{
  client.setInsecure(); // HTTPS, but allow insecure cert
  http.setUserAgent(UA_MAC_SAFARI);
  http.addHeader("Accept", "application/json");
  http.addHeader("Referer", "https://aviationweather.gov/");
  http.addHeader("Accept-Language", "en-US,en;q=0.9");
  http.addHeader("Connection", "close");
  http.setTimeout(15000);
  http.setReuse(true);
  if (useHttp10) http.useHTTP10(true);
  if (followRedirects) http.setFollowRedirects(HTTPC_FORCE_FOLLOW_REDIRECTS);

  if (!http.begin(client, url)) { outCode = -1; return false; }
  int code = http.GET();
  outCode = code;
  if (code != HTTP_CODE_OK) { http.end(); return false; }
  outBody = http.getString();
  http.end();
  return true;
}

// HTTPS-only with DNS priming & temporary google-first if needed
static bool httpGetJsonHttpsDnsFallback(const String& urlIn, String& outBody, int& outCode, String& outErr) {
  outErr = ""; outCode = 0; outBody = "";
  ParsedUrl pu = parseUrl(urlIn);
  if (!pu.ok) { outErr=F("bad URL"); outCode=-1; return false; }

  auto tryOnce = [&](bool http10, String& errOut)->bool {
    WiFiClientSecure c; HTTPClient http;
    bool ok = httpGetCore(http, c, urlIn, outBody, outCode, http10, true);
    if (ok) return true;

    if (outCode == -1) {
      IPAddress ip;
      if (WiFi.hostByName(pu.host.c_str(), ip) == 1) {
        saf_logf(2, "[METAR] DNS prime (%s) %s -> %s",
                 (g_dnsMode==DNS_GOOGLE_FIRST)?"gFirst":"dhcpFirst",
                 pu.host.c_str(), ip.toString().c_str());
        WiFiClientSecure c2; HTTPClient http2;
        if (httpGetCore(http2, c2, urlIn, outBody, outCode, http10, true)) return true;
      }
#if defined(ARDUINO_ARCH_ESP32)
      // temporarily flip to google-first
      SAF_DnsMode original = g_dnsMode;
      ip_addr_t primG = to_ip_addr(DNS_GOOGLE);
      IPAddress secIP = ip_is_set(g_dhcpDns0) ? g_dhcpDns0 : DNS_GOOGLE_2;
      ip_addr_t secG  = to_ip_addr(secIP);
      dns_setserver(0, &primG);
      dns_setserver(1, &secG);
      IPAddress ip2;
      if (WiFi.hostByName(pu.host.c_str(), ip2) == 1) {
        saf_logf(2, "[METAR] DNS prime (temp gFirst) %s -> %s",
                 pu.host.c_str(), ip2.toString().c_str());
        WiFiClientSecure c3; HTTPClient http3;
        bool ok3 = httpGetCore(http3, c3, urlIn, outBody, outCode, http10, true);
        saf_dns_apply(original); // restore
        if (ok3) return true;
      } else {
        saf_dns_apply(original);
      }
#endif
    }
    errOut = String(F("HTTPS")) + (http10 ? F("(1.0)") : F("(1.1)")) + F(" fail code=") + outCode;
    return false;
  };

  if (tryOnce(false, outErr)) return true;
  {
    String e2;
    if (tryOnce(true, e2)) return true;
    if (outErr.length()) outErr += F(" | ");
    outErr += e2;
  }
  return false;
}

// -------------------- JSON parsing (AWC) + fallback scanner --------------------
static uint16_t parseAwcByScan(const String& s) {
  uint16_t applied = 0;
  size_t pos = 0;
  auto up = [](String x){ x.trim(); x.toUpperCase(); return x; };

  while (true) {
    // Prefer AWC names: "icaoId":"XXXX"
    size_t k1 = s.indexOf("\"icaoId\":\"", pos);
    if (k1 == (size_t)-1) {
      // try legacy "station":"XXXX"
      k1 = s.indexOf("\"station\":\"", pos);
      if (k1 == (size_t)-1) break;
      size_t v1 = k1 + 11; // len("\"station\":\"")
      size_t e1 = s.indexOf('"', v1); if (e1==(size_t)-1) break;
      String icao = up(s.substring(v1, e1));

      size_t k2 = s.indexOf("\"flight_category\":\"", e1);
      if (k2 == (size_t)-1) { pos = e1; continue; }
      size_t v2 = k2 + 19; // len("\"flight_category\":\"")
      size_t e2 = s.indexOf('"', v2); if (e2==(size_t)-1) break;
      String cat = up(s.substring(v2, e2));

      if (icao.length()==4 && cat.length()>0) {
        if (g_logLevel >= 3) saf_logf(3, "[APPLY] %s -> %s (scan)", icao.c_str(), cat.c_str());
        if (g_apply) g_apply(g_ctx, icao, cat, saf_nowSec());
        applied++;
      }
      pos = e2;
      continue;
    }

    // AWC fields
    size_t v1 = k1 + 10; // len("\"icaoId\":\"")
    size_t e1 = s.indexOf('"', v1); if (e1==(size_t)-1) break;
    String icao = up(s.substring(v1, e1));

    size_t k2 = s.indexOf("\"fltCat\":\"", e1);
    if (k2 == (size_t)-1) { pos = e1; continue; }
    size_t v2 = k2 + 10; // len("\"fltCat\":\"")
    size_t e2 = s.indexOf('"', v2); if (e2==(size_t)-1) break;
    String cat = up(s.substring(v2, e2));

    if (icao.length()==4 && cat.length()>0) {
      if (g_logLevel >= 3) saf_logf(3, "[APPLY] %s -> %s (scan)", icao.c_str(), cat.c_str());
      if (g_apply) g_apply(g_ctx, icao, cat, saf_nowSec());
      applied++;
    }
    pos = e2;
  }
  return applied;
}

static uint16_t parseAwcApiMetars(const String& body) {
  DynamicJsonDocument d(16384);
  DeserializationError err = deserializeJson(d, body);
  if (err) {
    g_lastErr = String(F("json: ")) + err.c_str();
    return parseAwcByScan(body);
  }

  JsonArray arr;
  if (d.is<JsonArray>()) {
    arr = d.as<JsonArray>();
  } else if (d["data"].is<JsonArray>()) {
    arr = d["data"].as<JsonArray>();
  } else {
    // Unexpected envelope -> fallback scan
    return parseAwcByScan(body);
  }

  uint16_t applied = 0;
  for (JsonVariant v : arr) {
    const char* s_api = v["icaoId"].as<const char*>();
    const char* c_api = v["fltCat"].as<const char*>();
    const char* s_legacy1 = v["station"].as<const char*>();
    const char* s_legacy2 = v["station_id"].as<const char*>();
    const char* c_legacy  = v["flight_category"].as<const char*>();

    String icao = s_api ? String(s_api)
                        : (s_legacy1 ? String(s_legacy1)
                                     : (s_legacy2 ? String(s_legacy2) : String()));
    String cat  = c_api ? String(c_api)
                        : (c_legacy ? String(c_legacy) : String());

    icao.trim(); icao.toUpperCase();
    cat.trim();  cat.toUpperCase();

    if (icao.length()==4 && cat.length()>0) {
      if (g_logLevel >= 3) saf_logf(3, "[APPLY] %s -> %s", icao.c_str(), cat.c_str());
      if (g_apply) g_apply(g_ctx, icao, cat, saf_nowSec());
      applied++;
    }
  }
  return applied;
}

// -------------------- HTTP API (settings, DNS, debug, diag) --------------------
static void registerHttp(AsyncWebServer& server) {
  // status
  server.on("/skyaware.metar/status", HTTP_GET, [](AsyncWebServerRequest* req){
    DynamicJsonDocument d(256);
    d["ok"]     = true;
    d["enable"] = g_cfg.enable;
    d["freqMs"] = g_cfg.freqMs;
    d["batch"]  = g_cfg.batchSize;
    d["dnsMode"]= (g_dnsMode == DNS_GOOGLE_FIRST) ? "google-first" : "dhcp-first";
    d["logLevel"] = g_logLevel;
    d["logLevelName"] = lvlName(g_logLevel);
    String out; serializeJson(d, out);
    auto* res = req->beginResponse(200, "application/json", out);
    res->addHeader("Cache-Control","no-store");
    req->send(res);
  });

  // config
  server.on("/skyaware.metar/config", HTTP_POST, [](AsyncWebServerRequest* req){
    if (req->hasArg("enable")) {
      String v = req->arg("enable");
      g_cfg.enable = !(v == "0" || v.equalsIgnoreCase("false"));
    }
    if (req->hasArg("freqMs")) {
      uint32_t ms = (uint32_t) strtoul(req->arg("freqMs").c_str(), nullptr, 10);
      if (ms >= 15000) g_cfg.freqMs = ms;
    }
    if (req->hasArg("batch")) {
      uint16_t b = (uint16_t) strtoul(req->arg("batch").c_str(), nullptr, 10);
      if (b == 0) b = 1;
      if (b > 50) b = 50;
      g_cfg.batchSize = b;
    }
    saf_saveCfg();
    g_nextDue = 0;
    req->send(200, "application/json", "{\"ok\":true}");
  });

  // dns mode
  server.on("/skyaware.metar/dns", HTTP_POST, [](AsyncWebServerRequest* req){
    String mode = req->hasArg("mode") ? req->arg("mode") : "";
    mode.toLowerCase();
    if (mode == "google") saf_dns_apply(DNS_GOOGLE_FIRST);
    else if (mode == "dhcp") saf_dns_apply(DNS_DHCP_FIRST);
    else { req->send(400, "application/json", "{\"ok\":false,\"err\":\"mode must be 'google' or 'dhcp'\"}"); return; }
    req->send(200, "application/json", "{\"ok\":true}");
  });

  // debug level
  server.on("/skyaware.metar/debug", HTTP_GET, [](AsyncWebServerRequest* req){
    DynamicJsonDocument d(128);
    d["ok"] = true;
    d["level"] = g_logLevel;
    d["levelName"] = lvlName(g_logLevel);
    String out; serializeJson(d, out);
    req->send(200, "application/json", out);
  });
  server.on("/skyaware.metar/debug", HTTP_POST, [](AsyncWebServerRequest* req){
    if (!req->hasArg("level")) { req->send(400,"application/json","{\"ok\":false,\"err\":\"missing level\"}"); return; }
    uint8_t lv = (uint8_t) strtoul(req->arg("level").c_str(), nullptr, 10);
    if (lv > 3) lv = 3;
    g_logLevel = lv;
    String s = String("{\"ok\":true,\"level\":") + lv + ",\"levelName\":\"" + lvlName(lv) + "\"}";
    req->send(200, "application/json", s);
  });

  // logs ring
  server.on("/skyaware.metar/logs", HTTP_GET, [](AsyncWebServerRequest* req){
    DynamicJsonDocument d(4096);
    d["ok"] = true;
    d["level"] = g_logLevel;
    JsonArray a = d.createNestedArray("lines");
    size_t n = g_logCount;
    size_t start = (g_logCount < SAF_LOG_CAP) ? 0 : g_logIdx;
    for (size_t i = 0; i < n; i++) {
      size_t idx = (start + i) % SAF_LOG_CAP;
      a.add(g_logRing[idx]);
    }
    String out; serializeJson(d, out);
    req->send(200, "application/json", out);
  });

  // diag
  server.on("/skyaware.metar/diag", HTTP_GET, [](AsyncWebServerRequest* req){
    DynamicJsonDocument d(1024);
    d["ok"] = true;
    d["inFlight"] = g_inFlight;
    d["nextDueMs"] = g_nextDue;
    d["cyclePos"]  = (uint32_t)g_cyclePos;
    d["cycleSize"] = (uint32_t)g_cycleIcaos.size();
    d["cycles"]    = g_cycles;
    JsonObject last = d.createNestedObject("last");
    last["url"]       = g_lastUrl;
    last["http"]      = g_lastHttpCode;
    last["bytes"]     = (uint32_t)g_lastBodyLen;
    last["metars"]    = g_lastMetarCount;
    last["runMs"]     = g_lastRunMs;
    last["error"]     = g_lastErr;
    last["bodySample"]= g_lastBodySample;
    // include a tiny tail of logs
    JsonArray tail = d.createNestedArray("logTail");
    size_t toEmit = (g_logCount>15)?15:g_logCount;
    for (size_t i=toEmit; i>0; --i) {
      size_t idx = (g_logIdx + SAF_LOG_CAP - i) % SAF_LOG_CAP;
      tail.add(g_logRing[idx]);
    }
    String out; serializeJson(d, out);
    auto* res = req->beginResponse(200, "application/json", out);
    res->addHeader("Cache-Control","no-store");
    req->send(res);
  });

  // force now
  server.on("/skyaware.metar/force", HTTP_POST, [](AsyncWebServerRequest* req){
    g_nextDue = 0;
    req->send(200, "application/json", "{\"ok\":true,\"forced\":true}");
  });
}

// -------------------- lifecycle --------------------
void MetarFetcher_begin(AsyncWebServer& server,
                        void* ctx,
                        SAF_CollectIcaosFn collectCb,
                        SAF_ApplyCategoryFn applyCb)
{
  g_ctx     = ctx;
  g_collect = collectCb;
  g_apply   = applyCb;

  saf_loadCfg();
  registerHttp(server);

  saf_dns_captureDhcp();
  saf_dns_apply(DNS_DHCP_FIRST); // default: respect LAN/router DNS first

  g_nextDue = millis() + 5000; // start a bit after boot
  saf_logf(1, "[METAR] init enable=%d freqMs=%u batch=%u dns=%s log=%s",
           (int)g_cfg.enable, (unsigned)g_cfg.freqMs, (unsigned)g_cfg.batchSize,
           (g_dnsMode==DNS_GOOGLE_FIRST)?"google-first":"dhcp-first",
           lvlName(g_logLevel));
}

// -------------------- state machine (AWC API only) --------------------
void MetarFetcher_tick(void* /*ctx*/) {
  const uint32_t now = millis();

  if (!g_cfg.enable) {
    if (g_inFlight) saf_logf(1, "[METAR] disabled -> abort cycle");
    g_inFlight = false;
    g_cycleIcaos.clear();
    g_cyclePos = 0;
    g_nextDue = now + 1000;
    return;
  }

  if (!g_inFlight && now < g_nextDue) return;

  // Start cycle
  if (!g_inFlight) {
    saf_collectUniqueUpper(g_cycleIcaos);
    g_cyclePos = 0;
    g_inFlight = true;
    g_cycles++;
    if (g_cycleIcaos.empty()) {
      saf_logf(1, "[METAR] cycle: no ICAOs -> sleep %u ms", (unsigned)g_cfg.freqMs);
      g_inFlight = false;
      g_nextDue = now + g_cfg.freqMs;
      return;
    }
    saf_logf(1, "[METAR] cycle start: icao=%u batch=%u",
             (unsigned)g_cycleIcaos.size(), (unsigned)g_cfg.batchSize);
  }

  // One batch
  const size_t N    = g_cycleIcaos.size();
  const size_t from = g_cyclePos;
  const size_t to   = std::min(from + (size_t)g_cfg.batchSize, N);

  String csv;
  for (size_t i = from; i < to; i++) { if (i > from) csv += ','; csv += g_cycleIcaos[i]; }
  if (g_logLevel >= 3) saf_logf(3, "[METAR] stations=%s", csv.c_str());

  // AWC API
  String body; int code = 0;
  g_lastUrl = buildAwcApiUrl(csv);
  g_lastErr = "";
  g_lastBodyLen = 0;
  g_lastMetarCount = 0;
  uint32_t t0 = millis();

  saf_logf(2, "[METAR] GET %s", g_lastUrl.c_str());
  bool ok = httpGetJsonHttpsDnsFallback(g_lastUrl, body, code, g_lastErr);
  g_lastHttpCode = code;
  g_lastRunMs = millis() - t0;

  if (!ok) {
    if (g_lastErr.length() == 0) g_lastErr = String(F("HTTPS fail code=")) + code;
    saf_logf(1, "[METAR] ERROR %s", g_lastErr.c_str());
  } else {
    g_lastBodyLen = body.length();
    g_lastBodySample = (body.length() > 512) ? body.substring(0,512) : body;
    if (g_logLevel >= 2) {
      String samp = body.substring(0, (body.length()>120)?120:body.length());
      samp.replace("\n"," "); samp.replace("\r"," ");
      saf_logf(2, "[METAR] http=%d bytes=%u durMs=%u body[:120]=%s",
               g_lastHttpCode, (unsigned)g_lastBodyLen, (unsigned)g_lastRunMs, samp.c_str());
    }
    uint16_t cnt = parseAwcApiMetars(body);
    g_lastMetarCount = cnt;
    saf_logf(1, "[METAR] parsed=%u applied=%u", (unsigned)cnt, (unsigned)cnt);
  }

  // Advance
  g_cyclePos = to;

  if (g_cyclePos >= N) {
    saf_logf(1, "[METAR] cycle done; next in %u ms", (unsigned)g_cfg.freqMs);
    g_inFlight = false;
    g_cycleIcaos.clear();
    g_nextDue = now + g_cfg.freqMs;
  } else {
    g_nextDue = now + 1000; // 1s between batches
  }
}
