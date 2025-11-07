// usermods/skyaware/SkyAware.cpp
// SkyAware — Stage 0 (WLED 0.16.0-alpha compatible)
//
// - Owns/releases segments (STATIC + FREEZE). Firmware never repaints pixels.
// - UI at /skyaware: Map Profile dropdown (+ auto-apply for presets), Apply Profile button,
//   per-LED table (Airport column), ID buttons, CSV import/export.
// - Preset selected  -> table inputs READ-ONLY (and preset applied immediately).
// - Custom selected  -> table inputs EDITABLE; clicking "Apply Profile" gathers the table
//   and persists to /skyaware/map.json (+ sets /skyaware/config.json to Custom).
// - CSV import -> switches to Custom (RAM), updates /skyaware/config.json; Apply Profile to persist map.json.
//
// Persistence:
//   * /skyaware/config.json -> { "mapProfile": "Custom" | "<PresetName>" }
//   * /skyaware/map.json    -> { "map": { "0":"CSV...", "1":"CSV..." } } (for Custom)
//
// JSON endpoints live under /skyaware.api/*.

#include "wled.h"
#include <pgmspace.h>
#include <map>
#include <vector>
#include "preloadmaps.h"   // namespace SkyAwarePreloads { PRESET_COUNT; struct CsvMap{uint8_t segment; const char* csv;}; PRESETS[] }

#ifndef SKY_CFG_DIR
  #define SKY_CFG_DIR "/skyaware"
#endif
#ifndef SKY_MAP_PATH
  #define SKY_MAP_PATH "/skyaware/map.json"
#endif
#ifndef SKY_PROFILE_PATH
  #define SKY_PROFILE_PATH "/skyaware/config.json"
#endif

// ------------------- UI HTML (PROGMEM) -------------------
static const char SKYAWARE_HTML[] PROGMEM = R"HTML(
<!doctype html>
<html lang="en">
<head>
<meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1">
<title>SkyAware — Stage 0</title>
<style>
:root{--bg:#0f1115;--card:#171a21;--text:#e8ecf3;--muted:#a0a6b6;--btn:#2a3042;--btnh:#353c55;--hr:#242a3a;--cyan:#00ffff}
*{box-sizing:border-box}body{margin:0;background:var(--bg);color:var(--text);font:14px/1.45 system-ui,Segoe UI,Roboto,Helvetica,Arial,sans-serif}
header{padding:14px 18px;border-bottom:1px solid var(--hr)}h1{margin:0;font-size:18px}
main{padding:16px 18px}
.card{background:var(--card);border:1px solid #262b3c;border-radius:14px;box-shadow:0 4px 16px rgba(0,0,0,.25);padding:14px;margin:10px 0}
.row{display:flex;gap:12px;align-items:center;justify-content:space-between;flex-wrap:wrap;margin-bottom:8px}
.muted{color:var(--muted)}.cyan{color:var(--cyan)}
button{all:unset;background:var(--btn);color:var(--text);padding:6px 10px;border-radius:8px;cursor:pointer}
button:hover{background:var(--btnh)}
.segtitle{font-weight:600;margin-bottom:8px}
table{width:100%;border-collapse:collapse}
th,td{padding:8px;border-bottom:1px solid #262b3c;text-align:left}
.sw{display:inline-block;width:16px;height:16px;border-radius:4px;border:1px solid #2c3450;margin-right:8px;vertical-align:middle}
.badge-on{background:#203a20;border-color:#2a5a2a;color:#a9e6a9}
input[type=text], select{background:#20263a;border:1px solid #2c3450;border-radius:6px;color:var(--text);padding:6px 8px;min-width:160px}
input[readonly]{ background:#1a2030; color:#9aa3b2; cursor:not-allowed; }
.err{white-space:pre-wrap;background:#241a1a;border:1px solid #4a2d2d;color:#ffd6d6;border-radius:10px;padding:10px;margin-bottom:10px;display:none}
input[type="file"]{display:none}
label.filebtn{padding:6px 10px;border-radius:8px;background:#2a3042;cursor:pointer}
label.filebtn:hover{background:#353c55}
.small{font-size:12px}
</style>
</head>
<body>
<header><h1>SkyAware — Stage 0 (Owns Segments)</h1></header>
<main>
  <div id="err" class="err"></div>

  <!-- Map Profile selector + Apply -->
  <div class="card">
    <div class="row" style="align-items:flex-end">
      <div>
        <div class="small muted">Map Profile</div>
        <select id="mapProfile"></select>
        <div class="small muted">Preset = read-only. Custom = edit table then click <b>Apply Profile</b>.</div>
      </div>
      <div>
        <button id="applyProfile">Apply Profile</button>
      </div>
    </div>
  </div>

  <!-- Stage-0 controls -->
  <div class="card">
    <div class="row">
      <div class="muted">
        Table shows per-LED Airport IDs. Use <b>SKIP</b> or <b>-</b> to leave an LED off (only in Custom).<br>
        "ID" paints one LED <span class="cyan">CYAN</span> temporarily.
      </div>
      <div style="display:flex;gap:10px;align-items:center;flex-wrap:wrap">
        <label><input id="ownToggle" type="checkbox" checked /> Own segments</label>
        <button id="clear">Clear Identify</button>
        <input id="csvFile" type="file" accept=".csv" />
        <label class="filebtn" for="csvFile">Import CSV</label>
        <button id="exportCsv">Export CSV</button>
      </div>
    </div>
  </div>

  <div id="segs"></div>
</main>

<script>
const AUTO_CLEAR_MS = 10000; // set 0 to disable
const WARM_HEX = "FFD278";
const OFF_HEX  = "000000";

let currentProfileName = 'Custom';
let lastMeta = null; // cache of /json/skyaware for building CSV

function isEditable(){ return currentProfileName.toUpperCase() === 'CUSTOM'; }

function showErr(msg){ const e=document.getElementById('err'); e.textContent=msg; e.style.display='block'; }
function hideErr(){ const e=document.getElementById('err'); e.style.display='none'; e.textContent=''; }
async function fetchJSON(url, opts){ const r=await fetch(url,{cache:'no-store',...(opts||{})}); const t=await r.text(); const ct=r.headers.get('content-type')||''; if(!r.ok) throw new Error(url+" -> HTTP "+r.status+": "+t.slice(0,160)); if(!ct.includes('application/json')) throw new Error(url+" -> Expected JSON, got: "+t.slice(0,160)); return JSON.parse(t); }
async function jget(u){ return fetchJSON(u); }
async function jpost(u){ const r=await fetch(u,{method:'POST'}); if(!r.ok) throw new Error(await r.text()); return r.text(); }
async function postState(payload){ await fetch('/json/state',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify(payload)}); }
function hex(n){ return n.toString(16).padStart(2,'0'); }
function rgb2hex(r,g,b){ return '#'+hex(r)+hex(g)+hex(b); }
function stripHash(H){ return (H||'').replace(/^#/,'').toUpperCase(); }

let active = null; // {seg, idx, prevHex}
let autoTimer = null;
async function setLedHex(segId, idx, hexNoHash){ await postState({ seg: { id: segId, i: [ idx, hexNoHash ] } }); }
async function restoreActive(){ if (!active) return; try{ await setLedHex(active.seg, active.idx, active.prevHex);} finally{ active=null; if (autoTimer){ clearTimeout(autoTimer); autoTimer=null; } } }
function armAutoClear(){ if (!AUTO_CLEAR_MS) return; if (autoTimer) clearTimeout(autoTimer); autoTimer=setTimeout(async()=>{ try{ await restoreActive(); await refresh(); }catch(e){ showErr(e.message);} }, AUTO_CLEAR_MS); }

function segTable(seg, mapForSeg, editable){
  const wrap=document.createElement('div'); wrap.className='card';
  const title=document.createElement('div'); title.className='segtitle';
  title.textContent=`Segment ${seg.id} — start ${seg.start}, length ${seg.len}`;
  wrap.appendChild(title);

  const table=document.createElement('table');
  const thead=document.createElement('thead');
  thead.innerHTML='<tr><th style="width:72px">Index</th><th>Color</th><th style="min-width:160px">Airport</th><th style="width:92px">ID</th></tr>';
  table.appendChild(thead);

  const tbody=document.createElement('tbody');
  seg.leds.forEach(L=>{
    const tr=document.createElement('tr');

    const tdIdx=document.createElement('td'); tdIdx.textContent=L.i;

    const tdCol=document.createElement('td');
    const sw=document.createElement('span'); sw.className='sw';
    const hx=rgb2hex(L.r||0,L.g||0,L.b||0); sw.style.background=hx;
    const label=document.createElement('span'); label.textContent=hx.toUpperCase();
    tdCol.appendChild(sw); tdCol.appendChild(label);

    const tdAirport=document.createElement('td');
    const apKey = String(L.i);
    const input=document.createElement('input'); input.type='text'; input.placeholder= editable ? 'KPDX or SKIP' : 'Preset (read-only)';
    input.value = (mapForSeg && mapForSeg[apKey]) ? mapForSeg[apKey] : '';
    input.readOnly = !editable;
    if (!editable) { input.title = 'Preset profile: read-only'; input.style.opacity='0.7'; }
    input.id = `ap-${seg.id}-${L.i}`; // used by Apply Profile
    if (editable) {
      input.oninput=()=>{ input.value = input.value.toUpperCase(); };
    }
    tdAirport.appendChild(input);

    const tdBtn=document.createElement('td');
    const btn=document.createElement('button');
    const isActive = active && active.seg===seg.id && active.idx===L.i;
    const isSkip = (mapForSeg && (mapForSeg[apKey]||'').toUpperCase().trim()==='SKIP');
    btn.textContent = isActive ? 'ID (ON)' : 'ID';
    if (isActive) btn.classList.add('badge-on');
    if (isSkip) { btn.title='SKIP (disabled)'; btn.style.opacity='0.5'; }

    btn.onclick=async()=>{
      try{
        if (isSkip) return;
        const currentHex = stripHash(rgb2hex(L.r||0,L.g||0,L.b||0));
        if (active && active.seg===seg.id && active.idx===L.i) {
          await restoreActive();
        } else {
          if (active) await restoreActive();
          active = { seg: seg.id, idx: L.i, prevHex: currentHex };
          await setLedHex(seg.id, L.i, "00FFFF");
          armAutoClear();
        }
        await refresh();
      }catch(e){ showErr(e.message); }
    };
    tdBtn.appendChild(btn);

    tr.appendChild(tdIdx); tr.appendChild(tdCol); tr.appendChild(tdAirport); tr.appendChild(tdBtn);
    tbody.appendChild(tr);
  });

  table.appendChild(tbody);
  wrap.appendChild(table);
  return wrap;
}

async function refresh(){
  hideErr();
  const meta = await jget('/json/skyaware');
  lastMeta = meta;

  const ownT = document.getElementById('ownToggle');
  if (ownT) ownT.checked = !!meta.own;

  if (active) {
    const s = meta.segments.find(x=>x.id===active.seg);
    const L = s ? s.leds.find(x=>x.i===active.idx) : null;
    if (!L || !(L.r===0 && L.g===255 && L.b===255)) {
      active=null; if (autoTimer){ clearTimeout(autoTimer); autoTimer=null; }
    }
  }

  const c=document.getElementById('segs'); if (c) c.innerHTML='';
  const map = meta.map || {};
  const editable = isEditable();
  meta.segments.forEach(seg=>{
    const segMap = map[String(seg.id)] || {};
    c.appendChild(segTable(seg, segMap, editable));
  });
}

function collectCsvFromTable(){
  const out = {};
  if (!lastMeta) return out;
  for (const s of lastMeta.segments) {
    const parts = [];
    for (let i=0; i<s.len; i++) {
      const el = document.getElementById(`ap-${s.id}-${i}`);
      let v = el ? (el.value||'').toUpperCase().trim() : '';
      if (v === '-') v = 'SKIP';
      parts.push(v);
    }
    out[String(s.id)] = parts.join(',');
  }
  return out;
}

async function loadMapProfileUI() {
  try {
    const [presets, status] = await Promise.all([
      fetchJSON('/skyaware.api/presets'),
      fetchJSON('/skyaware.api/status')
    ]);

    const sel = document.getElementById('mapProfile');
    sel.innerHTML = '';
    for (const name of presets) {
      const opt = document.createElement('option'); opt.value = name; opt.textContent = name; sel.appendChild(opt);
    }
    currentProfileName = status.mapProfile || 'Custom';
    sel.value = currentProfileName;

    // AUTO-APPLY preset when selected; when Custom is selected, tell FW to load map.json into RAM
    sel.onchange = async (e) => {
      const val = e.target.value;
      currentProfileName = val;

      if (val === 'Custom') {
        try {
          const res = await fetch('/skyaware.api/apply', {
            method:'POST',
            headers:{'Content-Type':'application/x-www-form-urlencoded'},
            body:'mapProfile=Custom'
          });
          if (!res.ok) throw new Error(await res.text());
          await refresh(); // now shows map.json content (editable)
        } catch (err) {
          showErr('Switch to Custom failed: ' + err.message);
        }
        return;
      }
      try {
        const res = await fetch('/skyaware.api/apply', {
          method:'POST',
          headers:{'Content-Type':'application/x-www-form-urlencoded'},
          body:'mapProfile='+encodeURIComponent(val)
        });
        if (!res.ok) throw new Error(await res.text());
        await refresh(); // table now shows the preset mapping from device
      } catch (err) {
        showErr('Apply preset failed: ' + err.message);
      }
    };

    document.getElementById('applyProfile').onclick = async ()=>{
      try{
        if (currentProfileName === 'Custom') {
          const rows = collectCsvFromTable();
          const data = new URLSearchParams();
          data.set('mapProfile','Custom');
          Object.keys(rows).forEach(k => data.set('csv-'+k, rows[k]));
          const res = await fetch('/skyaware.api/apply', { method:'POST', headers:{'Content-Type':'application/x-www-form-urlencoded'}, body:data });
          if (!res.ok) throw new Error(await res.text());
        } else {
          // in case user clicks apply while in preset, re-assert preset
          const res = await fetch('/skyaware.api/apply', {
            method:'POST',
            headers:{'Content-Type':'application/x-www-form-urlencoded'},
            body:'mapProfile='+encodeURIComponent(currentProfileName)
          });
          if (!res.ok) throw new Error(await res.text());
        }
        await refresh();
      }catch(e){ showErr('Apply failed: ' + e.message); }
    };
  } catch (e) {
    showErr('Profile UI failed: ' + e.message);
  }
}

document.getElementById('ownToggle').onchange = async (e) => {
  try {
    const en = e.target.checked;
    await jpost(`/skyaware/own?enable=${en?1:0}`);
    const meta = await jget('/json/skyaware');
    if (en) {
      const segDefs = meta.segments.map(s => ({ id:s.id, fx:0, frz:true, col:[[WARM_HEX]] }));
      await postState({ on:true, bri:255, seg: segDefs });
      const map = meta.map || {};
      for (const s of meta.segments) {
        const m = map[String(s.id)] || {};
        const iArr = [];
        for (let li=0; li<s.len; li++){
          const v = (m[String(li)]||'').toUpperCase().trim();
          if (v === 'SKIP') { iArr.push(li, OFF_HEX); }
        }
        if (iArr.length) await postState({ seg:{ id:s.id, i:iArr } });
      }
    } else {
      await postState({ seg: meta.segments.map(s=>({id:s.id, frz:false})) });
    }
    await refresh();
  } catch (err) { showErr(err.message); e.target.checked = !e.target.checked; }
};

document.getElementById('clear').onclick = async () => {
  try{ if (active){ await restoreActive(); await refresh(); } }catch(e){ showErr(e.message); }
};

document.getElementById('csvFile').addEventListener('change', async (ev)=>{
  const f = ev.target.files[0]; if (!f) return;
  try{
    const text = await f.text();
    const r = await fetch('/skyaware/csv', { method:'POST', headers:{'Content-Type':'text/plain; charset=utf-8'}, body:text });
    if (!r.ok) throw new Error(await r.text());

    // After CSV import, force profile to Custom in UI and refresh
    currentProfileName = 'Custom';
    const sel = document.getElementById('mapProfile');
    if (sel) sel.value = 'Custom';
    await refresh();
  }catch(e){ showErr(e.message); }
  ev.target.value='';
});

document.getElementById('exportCsv').onclick = ()=>{ window.location.href = '/skyaware/csv'; };

refresh().catch(e=>showErr(e.message));
loadMapProfileUI().catch(e=>showErr(e.message));
</script>
</body>
</html>
)HTML";

// ------------------- Usermod -------------------
class SkyAwareUsermod : public Usermod {
public:
  bool initialized = false;
  bool ownSegmentsEnabled = true;

  // seg -> (idx -> airport)
  std::map<uint8_t, std::map<uint16_t, String>> segMap;

  // User-facing profile name ("Custom" or PRESET)
  String mapProfile = "Custom";

  void addToConfig(JsonObject& root) override { (void)root; } // no generic WLED UI

  bool readFromConfig(JsonObject& root) override {
    (void)root;

    // Load profile
    if (WLED_FS.exists(SKY_PROFILE_PATH)) {
      File f = WLED_FS.open(SKY_PROFILE_PATH, "r");
      if (f) {
        DynamicJsonDocument d(512);
        if (deserializeJson(d, f) == DeserializationError::Ok) {
          if (d["mapProfile"].is<const char*>()) mapProfile = d["mapProfile"].as<const char*>();
        }
        f.close();
      }
    }

    // Load map according to profile
    segMap.clear();
    if (mapProfile.equalsIgnoreCase("Custom")) {
      if (WLED_FS.exists(SKY_MAP_PATH)) {
        File f = WLED_FS.open(SKY_MAP_PATH, "r");
        if (f) {
          DynamicJsonDocument d(8192);
          if (deserializeJson(d, f) == DeserializationError::Ok) {
            JsonObject map = d["map"].as<JsonObject>();
            for (JsonPair p : map) {
              uint8_t seg = (uint8_t) strtoul(p.key().c_str(), nullptr, 10);
              if (!p.value().is<const char*>()) continue;
              String csv = p.value().as<const char*>();
              std::map<uint16_t, String> inner;
              uint16_t idx=0; int start=0;
              while (start <= (int)csv.length()) {
                int comma = csv.indexOf(',', start);
                if (comma < 0) comma = csv.length();
                String ap = csv.substring(start, comma); ap.trim(); ap.toUpperCase();
                if (ap == "-") ap = "SKIP";
                if (ap.length()) inner[idx] = ap;
                idx++; start = comma + 1;
                if (start > (int)csv.length()) break;
              }
              if (!inner.empty()) segMap[seg] = std::move(inner);
            }
          }
          f.close();
        }
      }
    } else {
      using namespace SkyAwarePreloads;
      for (uint8_t i=0; i<PRESET_COUNT; i++) {
        String pname = String(FPSTR(PRESETS[i].name));
        if (!pname.equalsIgnoreCase(mapProfile)) continue;
        const CsvMap* rows = PRESETS[i].rows;
        for (uint8_t r = 0; r < PRESETS[i].count; r++) {
          uint8_t seg = pgm_read_byte(&rows[r].segment);
          const char* csvPtr = (const char*) pgm_read_ptr(&rows[r].csv);
          String csv = String(FPSTR(csvPtr));
          std::map<uint16_t, String> inner;
          uint16_t idx=0; int start=0;
          while (start <= (int)csv.length()) {
            int comma = csv.indexOf(',', start);
            if (comma < 0) comma = csv.length();
            String ap = csv.substring(start, comma); ap.trim(); ap.toUpperCase();
            if (ap.length()) inner[idx] = ap;
            idx++; start = comma + 1;
            if (start > (int)csv.length()) break;
          }
          if (!inner.empty()) segMap[seg] = std::move(inner);
        }
        break;
      }
    }
    return true;
  }

  // ---- helpers ----
  static void addNoCache(AsyncWebServerResponse* res){
    res->addHeader("Cache-Control","no-store, no-cache, must-revalidate, max-age=0");
    res->addHeader("Pragma","no-cache");
    res->addHeader("Expires","0");
  }

  void sendHtml(AsyncWebServerRequest* r, const char* htmlPROGMEM) {
    AsyncWebServerResponse* res = r->beginResponse_P(200, "text/html", (PGM_P)htmlPROGMEM);
    addNoCache(res);
    r->send(res);
  }

  void sendCsv(AsyncWebServerRequest* r) {
    AsyncResponseStream* res = r->beginResponseStream("text/csv");
    addNoCache(res);
    res->addHeader("Content-Disposition","attachment; filename=\"skyaware-map.csv\"");
    const uint16_t lastSegId = strip.getLastActiveSegmentId();
    for (uint16_t si = 0; si <= lastSegId; si++) {
      Segment& s = strip.getSegment(si);
      for (uint16_t li = 0; li < s.length(); li++) {
        String cell;
        auto segIt = segMap.find((uint8_t)si);
        if (segIt != segMap.end()) {
          auto &inner = segIt->second;
          auto it = inner.find(li);
          if (it != inner.end()) { cell = it->second; cell.trim(); cell.toUpperCase(); }
        }
        if (li > 0) res->print(',');
        res->print(cell);
      }
      res->print("\n");
    }
    r->send(res);
  }

  bool importCsvBody(const String& body, String& err) {
    (void)err;
    uint32_t pos = 0, n = body.length();
    uint16_t rowSeg = 0;
    while (pos <= n) {
      int end = body.indexOf('\n', pos);
      if (end < 0) end = n;
      String line = body.substring(pos, end);
      pos = end + 1;
      line.trim(); // removes \r
      if (line.length() == 0) { rowSeg++; continue; }

      int start = 0; std::vector<String> cells;
      while (start <= line.length()) {
        int c = line.indexOf(',', start);
        if (c < 0) c = line.length();
        String cell = line.substring(start, c); cell.trim();
        cells.push_back(cell);
        start = c + 1;
        if (start > line.length()) break;
      }

      std::map<uint16_t, String> &inner = segMap[(uint8_t)rowSeg];
      for (size_t col = 0; col < cells.size(); col++) {
        String v = cells[col]; v.trim(); v.toUpperCase();
        if (v.length() == 0) {
          auto it = inner.find((uint16_t)col);
          if (it != inner.end()) inner.erase(it);
        } else {
          if (v == "-") v = "SKIP";
          inner[(uint16_t)col] = v;
        }
      }
      rowSeg++;
    }
    return true;
  }

  void handleMeta(AsyncWebServerRequest* r) {
    AsyncResponseStream* res = r->beginResponseStream("application/json");
    addNoCache(res);
    const uint16_t lastSegId = strip.getLastActiveSegmentId();

    res->print('{');
    res->print("\"segments\":[");
    for (uint16_t si = 0; si <= lastSegId; si++) {
      Segment& s = strip.getSegment(si);
      if (si) res->print(',');
      res->print('{');
      res->print("\"id\":");    res->print(si);
      res->print(",\"start\":");res->print(s.start);
      res->print(",\"len\":");  res->print(s.length());
      res->print(",\"leds\":[");
      for (uint16_t li = 0; li < s.length(); li++) {
        if (li) res->print(',');
        uint32_t c = strip.getPixelColor(s.start + li);
        res->print('{');
        res->print("\"i\":"); res->print(li);
        res->print(",\"r\":"); res->print(R(c));
        res->print(",\"g\":"); res->print(G(c));
        res->print(",\"b\":"); res->print(B(c));
        res->print('}');
      }
      res->print("]}");
    }
    res->print("],");

    // map (sparse -> object of objects)
    res->print("\"map\":{");
    bool firstS = true;
    for (auto &segPair : segMap) {
      if (!firstS) res->print(',');
      firstS = false;
      res->print('\"'); res->print((unsigned)segPair.first); res->print("\":{");
      bool firstI = true;
      for (auto &idxPair : segPair.second) {
        if (!firstI) res->print(',');
        firstI = false;
        res->print('\"'); res->print((unsigned)idxPair.first); res->print("\":\"");
        String v = idxPair.second; v.replace("\"",""); v.toUpperCase();
        res->print(v); res->print('\"');
      }
      res->print('}');
    }
    res->print("},");

    res->print("\"own\":"); res->print(ownSegmentsEnabled ? "true" : "false");
    res->print('}');
    r->send(res);
  }

  // ---- Own/Release (no repaint) ----
  static inline void enforceOwnOn() {
    const uint16_t segCount = strip.getLastActiveSegmentId() + 1;
    for (uint16_t i = 0; i < segCount; i++) {
      Segment& s = strip.getSegment(i);
      if (s.mode != FX_MODE_STATIC) s.setMode(FX_MODE_STATIC);
      if (!s.getOption(SEG_OPTION_FREEZE)) s.setOption(SEG_OPTION_FREEZE, true);
    }
  }
  static inline void enforceOwnOff() {
    const uint16_t segCount = strip.getLastActiveSegmentId() + 1;
    for (uint16_t i = 0; i < segCount; i++) {
      Segment& s = strip.getSegment(i);
      if (s.getOption(SEG_OPTION_FREEZE)) s.setOption(SEG_OPTION_FREEZE, false);
    }
  }

  void registerHTTP() {
    // ---- UI ----
    server.on("/skyaware", HTTP_GET, [this](AsyncWebServerRequest* r){ sendHtml(r, SKYAWARE_HTML); });

    // ---- API (clean prefix) ----
    server.on("/skyaware.api/presets", HTTP_GET, [this](AsyncWebServerRequest* r){
      using namespace SkyAwarePreloads;
      DynamicJsonDocument d(1024);
      JsonArray arr = d.to<JsonArray>();
      arr.add("Custom");
      for (uint8_t i = 0; i < PRESET_COUNT; i++) {
        arr.add(String(FPSTR(PRESETS[i].name)));
      }
      String out; serializeJson(arr, out);
      auto* res = r->beginResponse(200, "application/json", out);
      addNoCache(res); r->send(res);
    });

    server.on("/skyaware.api/status", HTTP_GET, [this](AsyncWebServerRequest* r){
      DynamicJsonDocument d(8192);
      d["mapProfile"] = mapProfile;
      JsonObject map = d.createNestedObject("map");
      for (auto &segPair : segMap) {
        char key[6]; snprintf(key, sizeof(key), "%u", (unsigned)segPair.first);
        uint16_t maxIdx = 0; for (auto &kv : segPair.second) if (kv.first > maxIdx) maxIdx = kv.first;
        String line;
        for (uint16_t i=0; i<=maxIdx; i++) {
          if (i) line += ',';
          auto it = segPair.second.find(i);
          if (it != segPair.second.end()) line += it->second;
        }
        map[key] = line;
      }
      String out; serializeJson(d, out);
      auto* res = r->beginResponse(200, "application/json", out);
      addNoCache(res); r->send(res);
    });

    // Apply selected profile:
    // - If mapProfile=Custom, with csv-<segId> fields -> build segMap and persist to /skyaware/map.json.
    // - If mapProfile=Custom, with NO csv-<segId> fields -> just reload segMap from /skyaware/map.json.
    // - If preset, build segMap from PROGMEM and persist only /skyaware/config.json.
    server.on("/skyaware.api/apply", HTTP_POST, [this](AsyncWebServerRequest* r){
      String profile = r->hasArg("mapProfile") ? r->arg("mapProfile") : "";
      profile.trim();
      if (!profile.length()) { r->send(400,"text/plain","missing mapProfile"); return; }

      if (profile.equalsIgnoreCase("Custom")) {
        // collect csv-<segId> fields
        std::map<uint8_t, String> csvRows;
        int n = r->params();
        for (int i=0;i<n;i++) {
          auto* p = r->getParam(i);
          if (!p->isPost()) continue;
          String k = p->name();
          if (k.startsWith("csv-")) {
            uint8_t seg = (uint8_t) strtoul(k.substring(4).c_str(), nullptr, 10);
            csvRows[seg] = p->value();
          }
        }

        // No CSV fields → switch to Custom and reload segMap from persisted map.json
        if (csvRows.empty()) {
          mapProfile = "Custom";

          segMap.clear();
          if (WLED_FS.exists(SKY_MAP_PATH)) {
            File f = WLED_FS.open(SKY_MAP_PATH, "r");
            if (f) {
              DynamicJsonDocument d(8192);
              if (deserializeJson(d, f) == DeserializationError::Ok) {
                JsonObject map = d["map"].as<JsonObject>();
                for (JsonPair p : map) {
                  uint8_t seg = (uint8_t) strtoul(p.key().c_str(), nullptr, 10);
                  if (!p.value().is<const char*>()) continue;
                  String csv = p.value().as<const char*>();
                  std::map<uint16_t, String> inner;
                  uint16_t idx=0; int start=0;
                  while (start <= (int)csv.length()) {
                    int comma = csv.indexOf(',', start);
                    if (comma < 0) comma = csv.length();
                    String ap = csv.substring(start, comma); ap.trim(); ap.toUpperCase();
                    if (ap == "-") ap = "SKIP";
                    if (ap.length()) inner[idx] = ap;
                    idx++; start = comma + 1;
                    if (start > (int)csv.length()) break;
                  }
                  if (!inner.empty()) segMap[seg] = std::move(inner);
                }
              }
              f.close();
            }
          }

          // Persist only profile choice
          { DynamicJsonDocument pd(256); pd["mapProfile"]="Custom";
            File pf=WLED_FS.open(SKY_PROFILE_PATH,"w"); if(pf){ serializeJson(pd,pf); pf.close(); } }

          r->send(200,"text/plain","ok");
          return;
        }

        // CSV fields present → build segMap from table, persist to map.json
        mapProfile = "Custom";
        segMap.clear();
        for (auto &row : csvRows) {
          std::map<uint16_t, String> inner;
          String csv = row.second;
          uint16_t idx=0; int start=0;
          while (start <= (int)csv.length()) {
            int comma = csv.indexOf(',', start);
            if (comma < 0) comma = csv.length();
            String ap = csv.substring(start, comma); ap.trim(); ap.toUpperCase();
            if (ap == "-") ap = "SKIP";
            if (ap.length()) inner[idx] = ap;
            idx++; start = comma + 1;
            if (start > (int)csv.length()) break;
          }
          if (!inner.empty()) segMap[row.first] = std::move(inner);
        }

        // persist files
        if (!WLED_FS.exists(SKY_CFG_DIR)) { WLED_FS.mkdir(SKY_CFG_DIR); }
        // 1) map.json
        {
          DynamicJsonDocument d(4096);
          JsonObject rootObj = d.to<JsonObject>();
          JsonObject map = rootObj.createNestedObject("map");
          for (auto &segPair : segMap) {
            const uint8_t seg = segPair.first;
            const auto &idxMap = segPair.second;
            uint16_t maxIdx = 0; for (const auto &kv : idxMap) if (kv.first > maxIdx) maxIdx = kv.first;
            String csv; for (uint16_t i=0; i<=maxIdx; i++) { if (i) csv += ','; auto it = idxMap.find(i); if (it != idxMap.end()) csv += it->second; }
            char key[6]; snprintf(key, sizeof(key), "%u", (unsigned)seg);
            map[key] = csv;
          }
          File f = WLED_FS.open(SKY_MAP_PATH, "w"); bool ok = false; if (f){ ok = (serializeJson(d,f) > 0); f.close(); }
          if (!ok) { r->send(500,"text/plain","map.json write failed"); return; }
        }
        // 2) config.json
        {
          DynamicJsonDocument pd(256); pd["mapProfile"]="Custom";
          File pf=WLED_FS.open(SKY_PROFILE_PATH,"w"); if(pf){ serializeJson(pd,pf); pf.close(); }
        }

        r->send(200,"text/plain","ok");
        return;
      }

      // preset path
      mapProfile = profile;
      using namespace SkyAwarePreloads;
      segMap.clear();
      bool ok = false;
      for (uint8_t i=0; i<PRESET_COUNT; i++) {
        String pname = String(FPSTR(PRESETS[i].name));
        if (!pname.equalsIgnoreCase(mapProfile)) continue;
        const CsvMap* rows = PRESETS[i].rows;
        for (uint8_t rix = 0; rix < PRESETS[i].count; rix++) {
          uint8_t seg = pgm_read_byte(&rows[rix].segment);
          const char* csvPtr = (const char*) pgm_read_ptr(&rows[rix].csv);
          String csv = String(FPSTR(csvPtr));
          std::map<uint16_t, String> inner;
          uint16_t idx=0; int start=0;
          while (start <= (int)csv.length()) {
            int comma = csv.indexOf(',', start);
            if (comma < 0) comma = csv.length();
            String ap = csv.substring(start, comma); ap.trim(); ap.toUpperCase();
            if (ap.length()) inner[idx] = ap;
            idx++; start = comma + 1;
            if (start > (int)csv.length()) break;
          }
          if (!inner.empty()) segMap[seg] = std::move(inner);
        }
        ok = true; break;
      }
      // persist only profile choice
      { DynamicJsonDocument pd(256); pd["mapProfile"]=mapProfile; File pf=WLED_FS.open(SKY_PROFILE_PATH,"w"); if(pf){ serializeJson(pd,pf); pf.close(); } }
      r->send(ok ? 200 : 404, "text/plain", ok ? "ok" : "preset not found");
    });

    // ---- Stage-0 endpoints ----
    server.on("/json/skyaware", HTTP_GET, [this](AsyncWebServerRequest* r){ handleMeta(r); });

    server.on("/skyaware/own", HTTP_POST, [this](AsyncWebServerRequest* r){
      String v; const int n=r->params();
      for (int i=0;i<n;i++){ auto* p=r->getParam(i); if(p && p->name()=="enable"){ v=p->value(); break; } }
      if (!v.length()) { r->send(400,"text/plain","missing enable"); return; }
      ownSegmentsEnabled = (v != "0");
      if (ownSegmentsEnabled) enforceOwnOn(); else enforceOwnOff();
      r->send(200,"text/plain", ownSegmentsEnabled ? "owned" : "released");
    });

    // CSV export
    server.on("/skyaware/csv", HTTP_GET,  [this](AsyncWebServerRequest* r){ sendCsv(r); });

    // CSV import -> RAM only. Also switch profile to Custom (and persist config.json).
    server.on("/skyaware/csv", HTTP_POST,
      [this](AsyncWebServerRequest* r){
        String *buf = (String*) r->_tempObject;
        if (!buf || buf->length() == 0) {
          r->send(400, "text/plain", "empty body");
          if (buf) { delete buf; r->_tempObject = nullptr; }
          return;
        }
        String err;
        bool ok = importCsvBody(*buf, err);
        delete buf; r->_tempObject = nullptr;
        if (!ok) { r->send(400, "text/plain", err.length()?err:"parse error"); return; }

        // switch to Custom so the UI becomes editable and shows imported map
        mapProfile = "Custom";
        { DynamicJsonDocument pd(256); pd["mapProfile"]="Custom";
          File pf=WLED_FS.open(SKY_PROFILE_PATH,"w"); if(pf){ serializeJson(pd,pf); pf.close(); } }

        r->send(200, "text/plain", "ok");
      },
      /* onUpload */ nullptr,
      [this](AsyncWebServerRequest* r, uint8_t *data, size_t len, size_t index, size_t total){
        String *buf = (String*) r->_tempObject;
        if (!buf) { buf = new String(); if (total > 0) buf->reserve(total); r->_tempObject = buf; }
        buf->concat((const char*)data, len);
      }
    );
  }

  // ---- Hooks ----
  void setup() override {
    if (!WLED_FS.exists(SKY_CFG_DIR)) { WLED_FS.mkdir(SKY_CFG_DIR); }
    // Ensure we always have a profile file to read later
    { DynamicJsonDocument d(256); d["mapProfile"] = mapProfile; File f=WLED_FS.open(SKY_PROFILE_PATH,"w"); if(f){ serializeJson(d,f); f.close(); } }

    if (initialized) return;
    enforceOwnOn();
    registerHTTP();
    initialized = true;
  }

  void loop() override {
    if (!initialized) return;
    if (ownSegmentsEnabled) enforceOwnOn();
  }

  uint16_t getId() override { return USERMOD_ID_UNSPECIFIED; }
};

// Global instance + registration
SkyAwareUsermod skyAware;
REGISTER_USERMOD(skyAware);
