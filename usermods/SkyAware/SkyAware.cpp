// usermods/skyaware/SkyAware.cpp
// SkyAware — Stage 0 (WLED 0.16.0-alpha compatible) + Last-Known Flight Category cache & read-only indicators
//
// WHAT'S NEW (adds without removing your Stage 0 features):
// • In-RAM bounded cache of last-known flight categories per ICAO (with timestamp)
// • HTTP endpoints to set/read that cache (no METAR fetcher yet):
//      POST /skyaware.api/cat  -> icao=KHIO&cat=IFR[&ts=1730850000]   (mirrors /skyaware/cat for b/c)
//      GET  /skyaware.api/cat?icao=KHIO
//      GET  /skyaware.api/cats
// • /skyaware UI shows read-only L/I/M/V pills per LED row (grey if SKIP/no ICAO)
// • **Repaint engine via ID-path**: all pixel writes use the same JSON state path as the UI "ID" button.
//
// -----------------------------------------------------------------------------
// - Owns/releases segments (STATIC + FREEZE). Firmware repaints pixels from category cache.
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
// JSON endpoints live under /skyaware.api/* (and legacy mirrors under /skyaware/* where noted).

#include "wled.h"
#include <pgmspace.h>
#include <map>
#include <vector>
#include <time.h>
#include "preloadmaps.h"   // namespace SkyAwarePreloads { PRESET_COUNT; struct CsvMap{uint8_t segment; const char* csv;}; PRESETS[] }

// ======= helpers to mirror the ID button (JSON state path) ============
static inline uint32_t hexToColor(const String& hex) {
  String s = hex; s.trim(); s.replace("#","");
  while (s.length() < 6) s = "0" + s;
  uint32_t r = strtoul(s.substring(0,2).c_str(), nullptr, 16);
  uint32_t g = strtoul(s.substring(2,4).c_str(), nullptr, 16);
  uint32_t b = strtoul(s.substring(4,6).c_str(), nullptr, 16);
  return RGBW32(r,g,b,0);
}

// Build {"seg":{"id":X,"i":[ idx,"HEX", idx,"HEX", ... ]}} and deserialize via WLED.
static void applySegI_JSON(uint8_t segId, const std::vector<std::pair<uint16_t, String>>& pairs) {
  size_t cap = 512 + pairs.size()*32;
  DynamicJsonDocument d(cap);
  JsonObject root = d.to<JsonObject>();
  JsonObject seg = root.createNestedObject("seg");
  seg["id"] = segId;
  JsonArray i = seg.createNestedArray("i");
  for (auto &p : pairs) { i.add(p.first); i.add(p.second); }
  deserializeState(root, CALL_MODE_DIRECT_CHANGE);
  stateUpdated(CALL_MODE_DIRECT_CHANGE);
}

#ifndef SKY_CFG_DIR
  #define SKY_CFG_DIR "/skyaware"
#endif
#ifndef SKY_MAP_PATH
  #define SKY_MAP_PATH "/skyaware/map.json"
#endif
#ifndef SKY_PROFILE_PATH
  #define SKY_PROFILE_PATH "/skyaware/config.json"
#endif

// ================= Last-known category cache (bounded) ======================
// Categories
enum SkyCat : int8_t { CAT_UNKNOWN=0, CAT_LIFR=1, CAT_IFR=2, CAT_MVFR=3, CAT_VFR=4 };
static inline const char* catToStr(SkyCat c){
  switch(c){ case CAT_LIFR:return "LIFR"; case CAT_IFR:return "IFR"; case CAT_MVFR:return "MVFR"; case CAT_VFR:return "VFR"; default:return "UNKNOWN"; }
}
static inline SkyCat strToCat(const String& s){
  if (s.equalsIgnoreCase("LIFR")) return CAT_LIFR;
  if (s.equalsIgnoreCase("IFR"))  return CAT_IFR;
  if (s.equalsIgnoreCase("MVFR")) return CAT_MVFR;
  if (s.equalsIgnoreCase("VFR"))  return CAT_VFR;
  return CAT_UNKNOWN;
}
static inline uint32_t sa_nowSeconds(){ time_t t=time(nullptr); return (t>100000)?(uint32_t)t:(millis()/1000); }

#ifndef SKY_CAT_CACHE_MAX
  #define SKY_CAT_CACHE_MAX 256
#endif
struct CatRecord { char icao[5]; SkyCat cat; uint32_t updated; bool inUse; CatRecord(): icao{0}, cat(CAT_UNKNOWN), updated(0), inUse(false){} };
class SkyCatCache {
public:
  SkyCatCache(): _count(0){ for(int i=0;i<SKY_CAT_CACHE_MAX;i++) rec[i]=CatRecord(); }
  void upsert(const char* icao4, SkyCat cat, uint32_t ts){ int i=find(icao4); if(i>=0){ rec[i].cat=cat; rec[i].updated=ts; rec[i].inUse=true; return; } int s=firstFree(); if(s<0) s=evictOldest(); writeAt(s,icao4,cat,ts); if(!rec[s].inUse){rec[s].inUse=true; _count++;} }
  bool get(const char* icao4, SkyCat& out, uint32_t& ts) const { int i=find(icao4); if(i<0) return false; out=rec[i].cat; ts=rec[i].updated; return true; }
  template<class D> void toJson(D& d) const { auto a=d.createNestedArray("airports"); for(int i=0;i<SKY_CAT_CACHE_MAX;i++){ if(!rec[i].inUse) continue; auto o=a.createNestedObject(); o["icao"]=rec[i].icao; o["cat"]=catToStr(rec[i].cat); o["updated"]=rec[i].updated; } }
  uint16_t size() const { return _count; }
private:
  CatRecord rec[SKY_CAT_CACHE_MAX]; uint16_t _count;
  int find(const char* icao4) const { if(!icao4||strlen(icao4)<3) return -1; for(int i=0;i<SKY_CAT_CACHE_MAX;i++){ if(rec[i].inUse && strncmp(rec[i].icao,icao4,4)==0) return i; } return -1; }
  int firstFree() const { for(int i=0;i<SKY_CAT_CACHE_MAX;i++) if(!rec[i].inUse) return i; return -1; }
  int evictOldest(){ int b=-1; uint32_t ts=UINT32_MAX; for(int i=0;i<SKY_CAT_CACHE_MAX;i++){ if(rec[i].inUse && rec[i].updated<ts){ b=i; ts=rec[i].updated; } } if(b>=0) rec[b].inUse=false; return (b>=0)?b:0; }
  void writeAt(int idx,const char* icao4,SkyCat cat,uint32_t ts){ if(idx<0||idx>=SKY_CAT_CACHE_MAX||!icao4) return; strncpy(rec[idx].icao,icao4,4); rec[idx].icao[4]='\0'; rec[idx].cat=cat; rec[idx].updated=ts; }
};

// ================= Color mapping ===========================
static inline uint32_t colorForCat(SkyCat c){
  switch(c){
    case CAT_LIFR: return RGBW32(0xFF,0x3F,0xFF,0x00); // magenta
    case CAT_IFR:  return RGBW32(0xFF,0x4B,0x4B,0x00); // red
    case CAT_MVFR: return RGBW32(0x3A,0x68,0xFF,0x00); // blue
    case CAT_VFR:  return RGBW32(0x20,0xC1,0x5A,0x00); // green
    default:       return RGBW32(0x00,0x00,0x00,0x00); // off
  }
}
static inline uint32_t colorOff(){ return RGBW32(0x00,0x00,0x00,0x00); }

// ------------------- UI HTML (PROGMEM) -------------------
// Adds a read-only L/I/M/V indicator column. Greyed if SKIP or no ICAO.
static const char SKYAWARE_HTML[] PROGMEM = R"HTML(
<!doctype html>
<html lang="en">
<head>
<meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1">
<title>SkyAware — Stage 0</title>
<style>
:root{--bg:#0f1115;--card:#171a21;--text:#e8ecf3;--muted:#a0a6b6;--btn:#2a3042;--btnh:#353c55;--hr:#242a3a;--cyan:#00ffff;--lifr:#ff3fff;--ifr:#ff4b4b;--mvfr:#3a68ff;--vfr:#20c15a}
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
/* last-known pills */
.pills{display:flex;gap:.35rem}
.pill{padding:.15rem .35rem;border-radius:.45rem;border:1px solid var(--hr);opacity:.45;font-weight:700;font-size:11px}
.pill.active{opacity:1;border-color:#fff}
.pill.lifr{background:var(--lifr)} .pill.ifr{background:var(--ifr)} .pill.mvfr{background:var(--mvfr)} .pill.vfr{background:var(--vfr)}
tr.skip .pills, tr.no-icao .pills { opacity:.35; filter:grayscale(90%); }
.updated{color:var(--muted);font-size:12px}
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
function stripHash(H){ return (H||'').replace(/^#/, '').toUpperCase(); }

let active = null; // {seg, idx, prevHex}
let autoTimer = null;
async function setLedHex(segId, idx, hexNoHash){ await postState({ seg: { id: segId, i: [ idx, hexNoHash ] } }); }
async function restoreActive(){ if (!active) return; try{ await setLedHex(active.seg, active.idx, active.prevHex);} finally{ active=null; if (autoTimer){ clearTimeout(autoTimer); autoTimer=null; } } }
function armAutoClear(){ if (!AUTO_CLEAR_MS) return; if (autoTimer) clearTimeout(autoTimer); autoTimer=setTimeout(async()=>{ try{ await restoreActive(); await refresh(); }catch(e){ showErr(e.message);} }, AUTO_CLEAR_MS); }

function mkPills(){
  const div=document.createElement('div'); div.className='pills';
  const mk=(cls,txt)=>{ const s=document.createElement('span'); s.className='pill '+cls; s.textContent=txt; return s; };
  div.appendChild(mk('lifr','L')); div.appendChild(mk('ifr','I')); div.appendChild(mk('mvfr','M')); div.appendChild(mk('vfr','V'));
  return div;
}

function segTable(seg, mapForSeg, editable){
  const wrap=document.createElement('div'); wrap.className='card';
  const title=document.createElement('div'); title.className='segtitle';
  title.textContent=`Segment ${seg.id} — start ${seg.start}, length ${seg.len}`;
  wrap.appendChild(title);

  const table=document.createElement('table');
  const thead=document.createElement('thead');
  thead.innerHTML='<tr><th style="width:72px">Index</th><th>Color</th><th style="min-width:160px">Airport</th><th>Last Known</th><th class="updated">Updated</th><th style="width:92px">ID</th></tr>';
  table.appendChild(thead);

  const tbody=document.createElement('tbody');
  seg.leds.forEach(L=>{
    const tr=document.createElement('tr'); tr.className='map-row'; tr.dataset.seg=seg.id; tr.dataset.idx=L.i;

    const tdIdx=document.createElement('td'); tdIdx.textContent=L.i;

    const tdCol=document.createElement('td');
    const sw=document.createElement('span'); sw.className='sw';
    const hx=rgb2hex(L.r||0,L.g||0,L.b||0); sw.style.background=hx;
    const label=document.createElement('span'); label.textContent=hx.toUpperCase();
    tdCol.appendChild(sw); tdCol.appendChild(label);

    const tdAirport=document.createElement('td');
    const apKey=String(L.i);
    const input=document.createElement('input'); input.type='text'; input.placeholder= editable ? 'KPDX or SKIP' : 'Preset (read-only)';
    input.value = (mapForSeg && mapForSeg[apKey]) ? mapForSeg[apKey] : '';
    input.readOnly = !editable; if (!editable) { input.title='Preset profile: read-only'; input.style.opacity='0.7'; }
    input.id = `ap-${seg.id}-${L.i}`;
    if (editable) input.oninput=()=>{ input.value = input.value.toUpperCase(); updateRowType(tr,input.value); };
    tdAirport.appendChild(input);

    const tdPills=document.createElement('td'); tdPills.appendChild(mkPills());
    const tdUpd=document.createElement('td'); tdUpd.className='updated';

    const tdBtn=document.createElement('td');
    const btn=document.createElement('button');
    const isActive = active && active.seg===seg.id && active.idx===L.i;
    btn.textContent = isActive ? 'ID (ON)' : 'ID';
    if (isActive) btn.classList.add('badge-on');
    btn.onclick=async()=>{
      try{
        if (tr.classList.contains('skip')||tr.classList.contains('no-icao')) return;
        const currentHex = stripHash(rgb2hex(L.r||0,L.g||0,L.b||0));
        if (active && active.seg===seg.id && active.idx===L.i) { await restoreActive(); }
        else { if (active) await restoreActive(); active={ seg: seg.id, idx: L.i, prevHex: currentHex }; await setLedHex(seg.id,L.i("00FFFF")); armAutoClear(); }
        await refresh();
      }catch(e){ showErr(e.message); }
    };
    tdBtn.appendChild(btn);

    tr.appendChild(tdIdx); tr.appendChild(tdCol); tr.appendChild(tdAirport); tr.appendChild(tdPills); tr.appendChild(tdUpd); tr.appendChild(tdBtn);
    updateRowType(tr, input.value);
    tbody.appendChild(tr);
  });

  table.appendChild(tbody);
  wrap.appendChild(table);
  return wrap;
}

function updateRowType(tr, val){
  const v=(val||'').toUpperCase().trim();
  tr.dataset.icao = (v && v!== 'SKIP' && v!=='-') ? v : '';
  const isSkip = (v==='SKIP'||v==='-');
  tr.dataset.type = isSkip? 'SKIP' : (tr.dataset.icao? 'AIRPORT':'NONE');
  tr.classList.toggle('skip', isSkip);
  tr.classList.toggle('no-icao', !tr.dataset.icao);
}

async function refresh(){
  hideErr();
  const meta = await jget('/json/skyaware');
  lastMeta = meta;

  const ownT = document.getElementById('ownToggle'); if (ownT) ownT.checked = !!meta.own;

  if (active) {
    const s = meta.segments.find(x=>x.id===active.seg);
    const L = s ? s.leds.find(x=>x.i===active.idx) : null;
    if (!L || !(L.r===0 && L.g===255 && L.b===255)) { active=null; if (autoTimer){ clearTimeout(autoTimer); autoTimer=null; } }
  }

  const c=document.getElementById('segs'); if (c) c.innerHTML='';
  const map = meta.map || {};
  const editable = isEditable();
  meta.segments.forEach(seg=>{ const segMap = map[String(seg.id)] || {}; c.appendChild(segTable(seg, segMap, editable)); });

  await refreshCats();
}

function collectCsvFromTable(){
  const out = {}; if (!lastMeta) return out;
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

    const sel = document.getElementById('mapProfile'); sel.innerHTML = '';
    for (const name of presets) { const opt = document.createElement('option'); opt.value = name; opt.textContent = name; sel.appendChild(opt); }
    currentProfileName = status.mapProfile || 'Custom'; sel.value = currentProfileName;

    sel.onchange = async (e) => {
      const val = e.target.value; currentProfileName = val;
      const body = new URLSearchParams(); body.set('mapProfile', val);
      try { const res = await fetch('/skyaware.api/apply', {method:'POST', headers:{'Content-Type':'application/x-www-form-urlencoded'}, body}); if (!res.ok) throw new Error(await res.text()); await refresh(); } catch (err) { showErr('Apply failed: ' + err.message); }
    };

    document.getElementById('applyProfile').onclick = async ()=>{
      try{
        const data = new URLSearchParams(); data.set('mapProfile', currentProfileName);
        if (currentProfileName === 'Custom') { const rows = collectCsvFromTable(); Object.keys(rows).forEach(k => data.set('csv-'+k, rows[k])); }
        const res = await fetch('/skyaware.api/apply', { method:'POST', headers:{'Content-Type':'application/x-www-form-urlencoded'}, body:data });
        if (!res.ok) throw new Error(await res.text());
        await refresh();
      }catch(e){ showErr('Apply failed: ' + e.message); }
    };
  } catch (e) { showErr('Profile UI failed: ' + e.message); }
}

document.getElementById('ownToggle').onchange = async (e) => {
  try {
    const en = e.target.checked;
    await jpost(`/skyaware/own?enable=${en?1:0}`);
    const meta = await jget('/json/skyaware');
    if (en) {
      const segDefs = meta.segments.map(s => ({ id:s.id, fx:0, frz:true, col:[["FFD278"]] }));
      await postState({ on:true, bri:255, seg: segDefs });
      const map = meta.map || {};
      for (const s of meta.segments) {
        const m = map[String(s.id)] || {};
        const iArr = [];
        for (let li=0; li<s.len; li++){
          const v = (m[String(li)]||'').toUpperCase().trim();
          if (v === 'SKIP') { iArr.push(li, "000000"); }
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
    currentProfileName = 'Custom'; const sel = document.getElementById('mapProfile'); if (sel) sel.value = 'Custom';
    await refresh();
  }catch(e){ showErr(e.message); }
  ev.target.value='';
});

document.getElementById('exportCsv').onclick = ()=>{ window.location.href = '/skyaware/csv'; };

async function refreshCats(){
  try{
    let r = await fetch('/skyaware.api/cats'); 
    if (!r.ok) r = await fetch('/skyaware/cats');
    if (!r.ok) return;
    const j = await r.json(); if (!j||!j.ok) return;
    const map = new Map(); (j.airports||[]).forEach(a=>{ if(a&&a.icao) map.set(a.icao.toUpperCase(), a); });
    document.querySelectorAll('tr.map-row').forEach(tr=>{
      const icao = (tr.dataset.icao||'').toUpperCase();
      const rec = map.get(icao);
      const pills = tr.querySelectorAll('.pills .pill'); pills.forEach(p=>p.classList.remove('active'));
      const upd = tr.querySelector('.updated'); if (upd) upd.textContent = rec? String(rec.updated):'';
      const cat = rec? String(rec.cat||'').toUpperCase() : 'UNKNOWN';
      if (cat==='LIFR') tr.querySelector('.pill.lifr')?.classList.add('active');
      else if (cat==='IFR') tr.querySelector('.pill.ifr')?.classList.add('active');
      else if (cat==='MVFR') tr.querySelector('.pill.mvfr')?.classList.add('active');
      else if (cat==='VFR') tr.querySelector('.pill.vfr')?.classList.add('active');
    });
  }catch(e){ /* silent */ }
}

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

  // simple test blinker state (driven via ID-path)
  struct {
    bool active=false;
    uint8_t seg=0;
    uint16_t idx=0;
    String h1, h2;
    uint32_t periodMs=300, lastMs=0;
    uint16_t remaining=0;
    bool phase=false;
  } _blink;

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
                if (comma < 0) comma = csv.length();   // FIX
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
            if (comma < 0) comma = csv.length();   // FIX
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
    if (!bri) bri = 255;
    strip.setBrightness(bri);
    const uint16_t segCount = strip.getLastActiveSegmentId() + 1;
    for (uint16_t i = 0; i < segCount; i++) {
      Segment& s = strip.getSegment(i);
      if (s.mode != FX_MODE_STATIC) s.setMode(FX_MODE_STATIC);
      s.setOption(SEG_OPTION_FREEZE, true);  // stop the renderer from touching pixels
      s.setOption(SEG_OPTION_ON, true);      // visible
      s.setOpacity(255);
    }
  }

  static inline void enforceOwnOff() {
    const uint16_t segCount = strip.getLastActiveSegmentId() + 1;
    for (uint16_t i = 0; i < segCount; i++) {
      Segment& s = strip.getSegment(i);
      if (s.getOption(SEG_OPTION_FREEZE)) s.setOption(SEG_OPTION_FREEZE, false);
    }
  }

  // ---------------- repaint helpers: ID-PATH ----------------
  void repaintIcao(const String& icaoUpper){
    for (auto &segPair : segMap) {
      uint8_t segId = segPair.first;
      auto &inner = segPair.second;

      std::vector<std::pair<uint16_t, String>> updates;
      for (auto &idxPair : inner) {
        const uint16_t li = idxPair.first;
        const String& ap  = idxPair.second;

        if (ap.equalsIgnoreCase("SKIP")) { updates.emplace_back(li, String(F("000000"))); continue; }
        if (!ap.equalsIgnoreCase(icaoUpper)) continue;

        SkyCat cat; uint32_t ts;
        SkyCat c = _catCache.get(ap.c_str(), cat, ts) ? cat : CAT_UNKNOWN;
        uint32_t col = colorForCat(c);
        char hex[7]; snprintf(hex, sizeof(hex), "%02X%02X%02X", R(col), G(col), B(col));
        updates.emplace_back(li, String(hex));
      }
      if (!updates.empty()) applySegI_JSON(segId, updates);
    }
  }

  void repaintAllFromCats(){
    const uint16_t lastSegId = strip.getLastActiveSegmentId();
    for (uint16_t si = 0; si <= lastSegId; si++) {
      auto it = segMap.find((uint8_t)si);
      if (it == segMap.end()) continue;

      std::vector<std::pair<uint16_t, String>> updates;
      for (auto &idxPair : it->second) {
        const uint16_t li = idxPair.first;
        const String& ap  = idxPair.second;

        if (ap.equalsIgnoreCase("SKIP")) { updates.emplace_back(li, String(F("000000"))); continue; }
        SkyCat cat; uint32_t ts;
        SkyCat c = _catCache.get(ap.c_str(), cat, ts) ? cat : CAT_UNKNOWN;
        uint32_t col = colorForCat(c);
        char hex[7]; snprintf(hex, sizeof(hex), "%02X%02X%02X", R(col), G(col), B(col));
        updates.emplace_back(li, String(hex));
      }
      if (!updates.empty()) applySegI_JSON((uint8_t)si, updates);
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
      for (uint8_t i = 0; i < PRESET_COUNT; i++) { arr.add(String(FPSTR(PRESETS[i].name))); }
      String out; serializeJson(arr, out);
      auto* res = r->beginResponse(200, "application/json", out); addNoCache(res); r->send(res);
    });

    server.on("/skyaware.api/status", HTTP_GET, [this](AsyncWebServerRequest* r){
      DynamicJsonDocument d(8192);
      d["mapProfile"] = mapProfile;
      JsonObject map = d.createNestedObject("map");
      for (auto &segPair : segMap) {
        char key[6]; snprintf(key, sizeof(key), "%u", (unsigned)segPair.first);
        uint16_t maxIdx = 0; for (auto &kv : segPair.second) if (kv.first > maxIdx) maxIdx = kv.first;
        String line; for (uint16_t i=0; i<=maxIdx; i++) { if (i) line += ','; auto it = segPair.second.find(i); if (it != segPair.second.end()) line += it->second; }
        map[key] = line;
      }
      String out; serializeJson(d, out);
      auto* res = r->beginResponse(200, "application/json", out); addNoCache(res); r->send(res);
    });

    // Apply profile
    server.on("/skyaware.api/apply", HTTP_POST, [this](AsyncWebServerRequest* r){
      String profile = r->hasArg("mapProfile") ? r->arg("mapProfile") : ""; profile.trim();
      if (!profile.length()) { r->send(400,"text/plain","missing mapProfile"); return; }

      if (profile.equalsIgnoreCase("Custom")) {
        // collect csv-<segId>
        std::map<uint8_t, String> csvRows; int n=r->params();
        for (int i=0;i<n;i++){ auto* p=r->getParam(i); if (!p->isPost()) continue; String k=p->name(); if (k.startsWith("csv-")) { uint8_t seg=(uint8_t) strtoul(k.substring(4).c_str(), nullptr, 10); csvRows[seg]=p->value(); } }

        if (csvRows.empty()) {
          mapProfile = "Custom";
          segMap.clear();
          if (WLED_FS.exists(SKY_MAP_PATH)) { File f=WLED_FS.open(SKY_MAP_PATH, "r"); if (f){ DynamicJsonDocument d(8192); if (deserializeJson(d,f)==DeserializationError::Ok){ JsonObject map=d["map"].as<JsonObject>(); for (JsonPair p:map){ uint8_t seg=(uint8_t)strtoul(p.key().c_str(),nullptr,10); if(!p.value().is<const char*>()) continue; String csv=p.value().as<const char*>(); std::map<uint16_t,String> inner; uint16_t idx=0; int start=0; while(start <= (int)csv.length()){ int comma=csv.indexOf(',',start); if(comma<0) comma=csv.length(); String ap=csv.substring(start,comma); ap.trim(); ap.toUpperCase(); if (ap=="-") ap="SKIP"; if(ap.length()) inner[idx]=ap; idx++; start=comma+1; if(start>(int)csv.length()) break; } if(!inner.empty()) segMap[seg]=std::move(inner);} } f.close(); } }
          { DynamicJsonDocument pd(256); pd["mapProfile"]="Custom"; File pf=WLED_FS.open(SKY_PROFILE_PATH,"w"); if(pf){ serializeJson(pd,pf); pf.close(); } }
          repaintAllFromCats();
          r->send(200,"text/plain","ok"); return;
        }

        mapProfile = "Custom"; segMap.clear();
        for (auto &row : csvRows) {
          std::map<uint16_t, String> inner; String csv=row.second; uint16_t idx=0; int start=0;
          while (start <= (int)csv.length()) { int comma=csv.indexOf(',',start); if (comma<0) comma=csv.length(); String ap=csv.substring(start,comma); ap.trim(); ap.toUpperCase(); if (ap=="-") ap="SKIP"; if (ap.length()) inner[idx]=ap; idx++; start=comma+1; if (start>(int)csv.length()) break; }
          if (!inner.empty()) segMap[row.first]=std::move(inner);
        }
        if (!WLED_FS.exists(SKY_CFG_DIR)) { WLED_FS.mkdir(SKY_CFG_DIR); }
        { DynamicJsonDocument d(4096); JsonObject rootObj=d.to<JsonObject>(); JsonObject map=rootObj.createNestedObject("map"); for (auto &segPair:segMap){ const uint8_t seg=segPair.first; const auto &idxMap=segPair.second; uint16_t maxIdx=0; for (const auto &kv:idxMap) if (kv.first>maxIdx) maxIdx=kv.first; String csv; for (uint16_t i=0;i<=maxIdx;i++){ if(i) csv+=','; auto it=idxMap.find(i); if(it!=idxMap.end()) csv+=it->second; } char key[6]; snprintf(key,sizeof(key),"%u",(unsigned)seg); map[key]=csv; } File f=WLED_FS.open(SKY_MAP_PATH,"w"); bool ok=false; if(f){ ok=(serializeJson(d,f)>0); f.close(); } if(!ok){ r->send(500,"text/plain","map.json write failed"); return; } }
        { DynamicJsonDocument pd(256); pd["mapProfile"]="Custom"; File pf=WLED_FS.open(SKY_PROFILE_PATH,"w"); if(pf){ serializeJson(pd,pf); pf.close(); } }
        repaintAllFromCats();
        r->send(200,"text/plain","ok"); return;
      }

      // preset path
      mapProfile = profile; using namespace SkyAwarePreloads; segMap.clear(); bool ok=false;
      for (uint8_t i=0;i<PRESET_COUNT;i++){ String pname=String(FPSTR(PRESETS[i].name)); if(!pname.equalsIgnoreCase(mapProfile)) continue; const CsvMap* rows=PRESETS[i].rows; for(uint8_t rix=0; rix<PRESETS[i].count; rix++){ uint8_t seg=pgm_read_byte(&rows[rix].segment); const char* csvPtr=(const char*)pgm_read_ptr(&rows[rix].csv); String csv=String(FPSTR(csvPtr)); std::map<uint16_t,String> inner; uint16_t idx=0; int start=0; while(start <= (int)csv.length()){ int comma=csv.indexOf(',',start); if(comma<0) comma=csv.length(); String ap=csv.substring(start,comma); ap.trim(); ap.toUpperCase(); if (ap.length()) inner[idx]=ap; idx++; start=comma+1; if(start>(int)csv.length()) break; } if(!inner.empty()) segMap[seg]=std::move(inner);} ok=true; break; }
      { DynamicJsonDocument pd(256); pd["mapProfile"]=mapProfile; File pf=WLED_FS.open(SKY_PROFILE_PATH,"w"); if(pf){ serializeJson(pd,pf); pf.close(); } }
      repaintAllFromCats();
      r->send(ok ? 200 : 404, "text/plain", ok ? "ok" : "preset not found");
    });

    // ---- Stage-0 meta ----
    server.on("/json/skyaware", HTTP_GET, [this](AsyncWebServerRequest* r){ handleMeta(r); });

    server.on("/skyaware/own", HTTP_POST, [this](AsyncWebServerRequest* r){
      String v; const int n=r->params();
      for (int i=0;i<n;i++){ auto* p=r->getParam(i); if(p && p->name()=="enable"){ v=p->value(); break; } }
      if (!v.length()) { r->send(400,"text/plain","missing enable"); return; }
      ownSegmentsEnabled = (v != "0");
      if (ownSegmentsEnabled) enforceOwnOn(); else enforceOwnOff();
      if (ownSegmentsEnabled) repaintAllFromCats();
      r->send(200,"text/plain", ownSegmentsEnabled ? "owned" : "released");
    });

    // CSV export/import
    server.on("/skyaware/csv", HTTP_GET,  [this](AsyncWebServerRequest* r){ sendCsv(r); });
    server.on("/skyaware/csv", HTTP_POST,
      [this](AsyncWebServerRequest* r){
        String *buf = (String*) r->_tempObject;
        if (!buf || buf->length() == 0) {
          r->send(400, "text/plain", "empty body");
          if (buf) { delete buf; r->_tempObject = nullptr; }
          return;
        }
        String err; bool ok = importCsvBody(*buf, err); delete buf; r->_tempObject = nullptr;
        if (!ok) { r->send(400, "text/plain", err.length()?err:"parse error"); return; }
        mapProfile = "Custom"; { DynamicJsonDocument pd(256); pd["mapProfile"]="Custom"; File pf=WLED_FS.open(SKY_PROFILE_PATH,"w"); if(pf){ serializeJson(pd,pf); pf.close(); } }
        repaintAllFromCats();
        r->send(200, "text/plain", "ok");
      },
      /* onUpload */ nullptr,
      [this](AsyncWebServerRequest* r, uint8_t *data, size_t len, size_t index, size_t total){
        String *buf = (String*) r->_tempObject;
        if (!buf) { buf = new String(); if (total > 0) buf->reserve(total); r->_tempObject = buf; }
        buf->concat((const char*)data, len);
      }
    );

    // ---- NEW: last-known category endpoints (standard) ----
    server.on("/skyaware.api/cat", HTTP_POST, [this](AsyncWebServerRequest* req){
      String icao = req->hasArg("icao") ? req->arg("icao") : "";
      String catS = req->hasArg("cat")  ? req->arg("cat")  : "";
      String tsS  = req->hasArg("ts")   ? req->arg("ts")   : "";
      icao.trim(); catS.trim(); tsS.trim(); icao.toUpperCase();
      if (icao.length()!=4 || catS.length()==0) { req->send(400, "application/json", "{\"ok\":false,\"err\":\"missing icao or cat\"}"); return; }
      SkyCat c = strToCat(catS); uint32_t ts = tsS.length()? (uint32_t)tsS.toInt() : sa_nowSeconds();
      _catCache.upsert(icao.c_str(), c, ts);
      repaintIcao(icao);
      DynamicJsonDocument d(256); d["ok"]=true; d["icao"]=icao; d["cat"]=catToStr(c); d["updated"]=ts; String out; serializeJson(d,out); req->send(200, "application/json", out);
    });
    server.on("/skyaware.api/cat", HTTP_GET, [this](AsyncWebServerRequest* req){
      String icao = req->hasArg("icao") ? req->arg("icao") : ""; icao.trim(); icao.toUpperCase();
      if (icao.length()!=4) { req->send(400, "application/json", "{\"ok\":false,\"err\":\"missing icao\"}"); return; }
      SkyCat c; uint32_t ts; DynamicJsonDocument d(256); d["icao"]=icao;
      if (_catCache.get(icao.c_str(), c, ts)) { d["ok"]=true; d["cat"]=catToStr(c); d["updated"]=ts; }
      else { d["ok"]=false; d["err"]="not_found"; }
      String out; serializeJson(d,out); req->send(200, "application/json", out);
    });
    server.on("/skyaware.api/cats", HTTP_GET, [this](AsyncWebServerRequest* req){
      DynamicJsonDocument d(1024); d["ok"]=true; _catCache.toJson(d); String out; serializeJson(d,out); req->send(200, "application/json", out);
    });

    // ---- Test endpoints: ID-path painting ----
    server.on("/skyaware.api/test/paint", HTTP_GET, [this](AsyncWebServerRequest* req){
      uint8_t seg = req->hasArg("seg") ? (uint8_t) strtoul(req->arg("seg").c_str(), nullptr, 10) : 0;
      uint16_t idx = req->hasArg("idx") ? (uint16_t) strtoul(req->arg("idx").c_str(), nullptr, 10) : 0;
      String hex = req->hasArg("hex") ? req->arg("hex") : "00FFFF";
      const uint16_t lastSegId = strip.getLastActiveSegmentId();
      if (seg > lastSegId) { req->send(400,"application/json","{\"ok\":false,\"err\":\"bad seg\"}"); return; }
      if (idx >= strip.getSegment(seg).length()) { req->send(400,"application/json","{\"ok\":false,\"err\":\"bad idx\"}"); return; }
      applySegI_JSON(seg, {{ idx, hex }});
      req->send(200,"application/json","{\"ok\":true}");
    });

    server.on("/skyaware.api/test/fill", HTTP_GET, [this](AsyncWebServerRequest* req){
      uint8_t seg = req->hasArg("seg") ? (uint8_t) strtoul(req->arg("seg").c_str(), nullptr, 10) : 0;
      uint16_t from = req->hasArg("from") ? (uint16_t) strtoul(req->arg("from").c_str(), nullptr, 10) : 0;
      uint16_t count = req->hasArg("count") ? (uint16_t) strtoul(req->arg("count").c_str(), nullptr, 10) : 1;
      String hex = req->hasArg("hex") ? req->arg("hex") : "00FF00";
      const uint16_t lastSegId = strip.getLastActiveSegmentId();
      if (seg > lastSegId) { req->send(400,"application/json","{\"ok\":false,\"err\":\"bad seg\"}"); return; }
      Segment& s = strip.getSegment(seg);
      if (from >= s.length()) { req->send(400,"application/json","{\"ok\":false,\"err\":\"bad from\"}"); return; }
      uint16_t to = from + count; if (to > s.length()) to = s.length();
      std::vector<std::pair<uint16_t, String>> updates;
      updates.reserve(to - from);
      for (uint16_t i = from; i < to; i++) updates.emplace_back(i, hex);
      applySegI_JSON(seg, updates);
      req->send(200,"application/json","{\"ok\":true}");
    });

    server.on("/skyaware.api/test/blink", HTTP_GET, [this](AsyncWebServerRequest* req){
      uint8_t seg = req->hasArg("seg") ? (uint8_t) strtoul(req->arg("seg").c_str(), nullptr, 10) : 0;
      uint16_t idx = req->hasArg("idx") ? (uint16_t) strtoul(req->arg("idx").c_str(), nullptr, 10) : 0;
      String h1 = req->hasArg("hex1") ? req->arg("hex1") : "FF0000";
      String h2 = req->hasArg("hex2") ? req->arg("hex2") : "0000FF";
      uint32_t ms = req->hasArg("ms")  ? (uint32_t) strtoul(req->arg("ms").c_str(), nullptr, 10) : 300;
      uint16_t n  = req->hasArg("n")   ? (uint16_t) strtoul(req->arg("n").c_str(), nullptr, 10) : 16;
      const uint16_t lastSegId = strip.getLastActiveSegmentId();
      if (seg > lastSegId) { req->send(400,"application/json","{\"ok\":false,\"err\":\"bad seg\"}"); return; }
      if (idx >= strip.getSegment(seg).length()) { req->send(400,"application/json","{\"ok\":false,\"err\":\"bad idx\"}"); return; }
      applySegI_JSON(seg, {{ idx, h2 }}); // prime with second color
      _blink.active = true; _blink.seg=seg; _blink.idx=idx;
      _blink.h1=h1; _blink.h2=h2; _blink.periodMs=ms; _blink.remaining=n;
      _blink.lastMs = millis(); _blink.phase=false;
      req->send(200,"application/json","{\"ok\":true}");
    });

    server.on("/skyaware.api/repaint", HTTP_POST, [this](AsyncWebServerRequest* req){
      repaintAllFromCats();
      req->send(200,"application/json","{\"ok\":true}");
    });
  }

  // ---- Hooks ----
  void setup() override {
    if (!WLED_FS.exists(SKY_CFG_DIR)) { WLED_FS.mkdir(SKY_CFG_DIR); }
    { DynamicJsonDocument d(256); d["mapProfile"] = mapProfile; File f=WLED_FS.open(SKY_PROFILE_PATH,"w"); if(f){ serializeJson(d,f); f.close(); } }
    if (initialized) return;

    if (!bri) bri = 255;
    strip.setBrightness(bri);
    stateUpdated(CALL_MODE_DIRECT_CHANGE);

    enforceOwnOn();
    registerHTTP();
    repaintAllFromCats(); // paint whatever we have
    initialized = true;
  }

  void loop() override {
    if (!initialized) return;
    if (ownSegmentsEnabled) enforceOwnOn();

    // drive test blinker using the same ID path
    if (_blink.active) {
      uint32_t now = millis();
      if (now - _blink.lastMs >= _blink.periodMs) {
        _blink.lastMs = now;
        _blink.phase = !_blink.phase;
        applySegI_JSON(_blink.seg, {{ _blink.idx, _blink.phase ? _blink.h1 : _blink.h2 }});
        if (_blink.remaining > 0) {
          _blink.remaining--;
          if (_blink.remaining == 0) _blink.active = false;
        }
      }
    }
  }

  uint16_t getId() override { return USERMOD_ID_UNSPECIFIED; }

private:
  SkyCatCache _catCache; // bounded last-known categories
};

// Global instance + registration
SkyAwareUsermod skyAware;
REGISTER_USERMOD(skyAware);
