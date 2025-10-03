#include "wled.h"
#include <Preferences.h>

static Preferences skyPref; // NVS namespace "sky"

class UsermodSkyWizard : public Usermod {
private:
  bool   enabled         = true;   // checkbox in Config → Usermods
  String homeAirport;              // persisted + runtime
  bool   wizardSaved     = false;
  bool   routesAttached  = false;

  // --- utils ---
  static bool validAirport(const String& s) {
    if (s.length() < 3 || s.length() > 8) return false; // KPDX, PDX, EGLL, etc.
    for (size_t i=0;i<s.length();i++) {
      char c = s[i];
      if (!((c>='A'&&c<='Z')||(c>='0'&&c<='9'))) return false;
    }
    return true;
  }

  // --- persistence ---
  static String loadAirport() {
    String v;
    skyPref.begin("sky", true);
    v = skyPref.getString("airport", "");
    skyPref.end();
    v.trim();
    return v;
  }

  static void saveAirportNow(const String& s) {
    skyPref.begin("sky", false);
    skyPref.putString("airport", s);
    skyPref.end();
  }

  // --- HTTP handlers ---
  void handleSave(AsyncWebServerRequest* request) {
    if (!enabled) { request->send(403, "text/plain", "disabled"); return; }

    String a, redir = "/welcome.htm?saved=1";

    if (request->hasParam("airport", true))
      a = request->getParam("airport", true)->value();
    else if (request->hasParam("airport"))
      a = request->getParam("airport")->value();

    if (request->hasParam("redir", true)) {
      String r = request->getParam("redir", true)->value();
      if (r.length()) redir = r;
    } else if (request->hasParam("redir")) {
      String r = request->getParam("redir")->value();
      if (r.length()) redir = r;
    }

    a.trim(); a.toUpperCase();
    if (a.length()==0 || validAirport(a)) {
      homeAirport = a;           // runtime
      saveAirportNow(homeAirport); // persist immediately
      wizardSaved = true;
    }

    // 302 redirect so captive UI advances (e.g., to /settings/wifi)
    AsyncWebServerResponse *resp = request->beginResponse(302, "text/plain", "saved");
    resp->addHeader("Location", redir);
    request->send(resp);
  }

  void handleStatus(AsyncWebServerRequest* request){
    DynamicJsonDocument d(256);
    d["enabled"]     = enabled;
    d["wizardSaved"] = wizardSaved;
    d["homeAirport"] = homeAirport;
    String out; serializeJson(d,out);
    request->send(200,"application/json",out);
  }

  void handleGet(AsyncWebServerRequest* request){
    String cur = homeAirport;
    if (cur.isEmpty()) cur = loadAirport();
    String json = "{\"airport\":\"";
    for (size_t i=0;i<cur.length();i++){ char c=cur[i]; if (c=='"'||c=='\\') json+='\\'; json+=c; }
    json += "\"}";
    request->send(200,"application/json",json);
  }

  void attachWeb() {
    if (routesAttached || !enabled) return;

    // 'server' is a global AsyncWebServer object (not a pointer)
    server.on("/um/skywizard/save", HTTP_POST, [this](AsyncWebServerRequest* r){ handleSave(r); });
    server.on("/um/skywizard.json", HTTP_GET,  [this](AsyncWebServerRequest* r){ handleStatus(r); });
    server.on("/um/skywizard/get",  HTTP_GET,  [this](AsyncWebServerRequest* r){ handleGet(r); });

    routesAttached = true;
    Serial.println(F("[SkyWizard] endpoints ready"));
  }

public:
  void setup() override {
    Serial.println(F("[SkyWizard] setup"));
    homeAirport = loadAirport();  // make available immediately at boot
    attachWeb();
  }

  void loop() override {
    if (!routesAttached && enabled) attachWeb();
  }

  // Config → Usermods (optional, keeps manual edit path)
  void addToConfig(JsonObject& root) override {
    JsonObject uw = root.createNestedObject("SkyWizard");
    uw["enabled"]     = enabled;
    uw["homeAirport"] = homeAirport;
    uw["wizardSaved"] = wizardSaved;
  }

  bool readFromConfig(JsonObject& root) override {
    JsonObject uw = root["SkyWizard"];
    if (uw.isNull()) return false;
    enabled     = uw["enabled"]     | true;
    homeAirport = uw["homeAirport"] | homeAirport; // preserve if missing
    wizardSaved = uw["wizardSaved"] | wizardSaved;
    return true;
  }

  uint16_t getId() override { return 0xA901; }
};

Usermod* usermod_SkyWizard = new UsermodSkyWizard();
