// usermods/skyaware/SkyAware.cpp
// SkyAware — Stage 0 (Owns Segments, UI + Telemetry Only)
//
// This usermod:
//  - Owns/releases segments (STATIC + FREEZE) without repainting colors.
//  - Serves a simple HTML UI at /skyaware with a per-LED table and ID buttons.
//  - Serves /json/skyaware with live per-LED RGBs read from the strip.
//  - Leaves ALL pixel painting to WLED's official /json/state API (hex "i" writes).
//
// Endpoints:
//   GET  /skyaware
//   GET  /json/skyaware
//   POST /skyaware/own?enable=1|0
//
// Clicking an ID button uses the built-in /json/state with the proven form:
//   {"seg":{"id":<seg>,"i":[<idx>,"RRGGBB"]}}
// We do NOT repaint in firmware, so we never fight WLED's renderer.

#include "wled.h"
#include <pgmspace.h>

class SkyAwareUsermod : public Usermod {
public:
  bool initialized = false;
  bool ownSegmentsEnabled = true;

  static const char SKY_UI_HTML[] PROGMEM;

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

  void sendHtml(AsyncWebServerRequest* r, const char* htmlPROGMEM) {
    size_t len = strlen_P((PGM_P)htmlPROGMEM);
    auto* res = r->beginResponse_P(
      200, "text/html",
      reinterpret_cast<const uint8_t*>(htmlPROGMEM),
      len
    );
    res->addHeader("Cache-Control", "no-store");
    r->send(res);
  }

  void handleMeta(AsyncWebServerRequest* r) {
    AsyncResponseStream* res = r->beginResponseStream("application/json");
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
    res->print("\"own\":"); res->print(ownSegmentsEnabled ? "true" : "false");
    res->print('}');
    r->send(res);
  }

  void registerHTTP() {
    server.on("/skyaware", HTTP_GET, [this](AsyncWebServerRequest* r) {
      sendHtml(r, SKY_UI_HTML);
    });

    server.on("/json/skyaware", HTTP_GET, [this](AsyncWebServerRequest* r) {
      handleMeta(r);
    });

    // /skyaware/own?enable=1|0  -- accepts query or body param
    server.on("/skyaware/own", HTTP_POST, [this](AsyncWebServerRequest* r) {
      String v;
      const int n = r->params();
      for (int i = 0; i < n; i++) {
        AsyncWebParameter* p = r->getParam(i);
        if (p && p->name() == "enable") { v = p->value(); break; }
      }
      if (!v.length()) { r->send(400, "text/plain", "missing enable"); return; }

      bool en = (v != "0");
      ownSegmentsEnabled = en;
      if (en) enforceOwnOn();
      else    enforceOwnOff();

      r->send(200, "text/plain", en ? "owned" : "released");
    });
  }

  void setup() override {
    if (initialized) return;
    enforceOwnOn();      // default: own on boot
    registerHTTP();
    initialized = true;
  }

  void loop() override {
    if (!initialized) return;
    if (ownSegmentsEnabled) enforceOwnOn(); // keep sticky, no repaint
  }

  uint16_t getId() override { return USERMOD_ID_UNSPECIFIED; }
};

// ---------------- HTML UI ----------------
const char SkyAwareUsermod::SKY_UI_HTML[] PROGMEM = R"HTML(<!doctype html>
<html lang="en"><head>
<meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1">
<title>SkyAware — Stage 0</title>
<style>
:root{--bg:#0f1115;--card:#171a21;--text:#e8ecf3;--muted:#a0a6b6;--btn:#2a3042;--btnh:#353c55;--hr:#242a3a;--cyan:#00ffff}
*{box-sizing:border-box}body{margin:0;background:var(--bg);color:var(--text);font:14px/1.45 system-ui,Segoe UI,Roboto,Helvetica,Arial,sans-serif}
header{padding:14px 18px;border-bottom:1px solid var(--hr)}h1{margin:0;font-size:18px}
main{padding:16px 18px}
.card{background:var(--card);border:1px solid #262b3c;border-radius:14px;box-shadow:0 4px 16px rgba(0,0,0,.25);padding:14px;margin:10px 0}
.row{display:flex;gap:12px;align-items:center;justify-content:space-between;margin-bottom:8px}
.muted{color:var(--muted)}.cyan{color:var(--cyan)}
button{all:unset;background:var(--btn);color:var(--text);padding:6px 10px;border-radius:8px;cursor:pointer}
button:hover{background:var(--btnh)}
.segtitle{font-weight:600;margin-bottom:8px}
table{width:100%;border-collapse:collapse}
th,td{padding:8px;border-bottom:1px solid #262b3c;text-align:left}
.sw{display:inline-block;width:16px;height:16px;border-radius:4px;border:1px solid #2c3450;margin-right:8px;vertical-align:middle}
.badge{display:inline-block;padding:2px 8px;border-radius:999px;border:1px solid #2c3450;background:#20263a;color:#b8c0d9;font-size:12px}
input[type=text]{background:#20263a;border:1px solid #2c3450;border-radius:6px;color:var(--text);padding:4px 6px;min-width:90px}
.err{white-space:pre-wrap;background:#241a1a;border:1px solid #4a2d2d;color:#ffd6d6;border-radius:10px;padding:10px;margin-bottom:10px;display:none}
.ctrls{display:flex;gap:12px;align-items:center}
.badge-on{background:#203a20;border-color:#2a5a2a;color:#a9e6a9}
</style>
</head><body>
<header><h1>SkyAware — Stage 0 (Owns Segments)</h1></header>
<main>
  <div id="err" class="err"></div>
  <div class="card">
    <div class="row">
      <div class="muted">ID paints one LED <span class="cyan">CYAN</span>. Click the same ID again to restore. Only one LED can be ID’d at a time. Auto-clears after 10s.</div>
      <div class="ctrls">
        <label><input id="ownToggle" type="checkbox" checked /> Own segments</label>
        <button id="clear">Clear Identify</button>
      </div>
    </div>
  </div>
  <div id="segs"></div>
</main>
<script>
const AUTO_CLEAR_MS = 10000; // 10s; set 0 to disable

function showErr(msg){ const e=document.getElementById('err'); e.textContent=msg; e.style.display='block'; }
function hideErr(){ const e=document.getElementById('err'); e.style.display='none'; e.textContent=''; }
async function jget(u){ const r=await fetch(u); const ct=r.headers.get('content-type')||''; if(!r.ok) throw new Error(`HTTP ${r.status}`); if(!ct.includes('application/json')){ const t=await r.text(); throw new Error(`Expected JSON, got ${ct||'unknown'}:\\n${t.slice(0,200)}`);} return r.json(); }
async function jpost(u){ const r=await fetch(u,{method:'POST'}); if(!r.ok) throw new Error(await r.text()); return r.text(); }
function hex(n){ return n.toString(16).padStart(2,'0'); }
function rgb2hex(r,g,b){ return '#'+hex(r)+hex(g)+hex(b); }
function stripHash(H){ return (H||'').replace(/^#/,'').toUpperCase(); }

// Track the currently-ID’d LED and its previous color
let active = null;          // { seg, idx, prevHex }
let autoTimer = null;

async function setLedHex(segId, idx, hexNoHash){
  await fetch('/json/state', {
    method:'POST',
    headers:{'Content-Type':'application/json'},
    body: JSON.stringify({ seg: { id: segId, i: [ idx, hexNoHash ] } })
  });
}

async function restoreActive(){
  if (!active) return;
  try {
    await setLedHex(active.seg, active.idx, active.prevHex);
  } finally {
    active = null;
    if (autoTimer){ clearTimeout(autoTimer); autoTimer = null; }
  }
}

function armAutoClear(){
  if (!AUTO_CLEAR_MS) return;
  if (autoTimer) clearTimeout(autoTimer);
  autoTimer = setTimeout(async ()=>{
    try { await restoreActive(); await refresh(); } catch(e){ showErr(e.message); }
  }, AUTO_CLEAR_MS);
}

function segTable(seg){
  const wrap=document.createElement('div');wrap.className='card';
  const title=document.createElement('div');title.className='segtitle';
  title.textContent=`Segment ${seg.id} — start ${seg.start}, length ${seg.len}`;
  wrap.appendChild(title);

  const badges=document.createElement('div');
  badges.innerHTML = `<span class="badge">static</span> <span class="badge">frozen</span>`;
  badges.style.marginBottom='8px'; wrap.appendChild(badges);

  const table=document.createElement('table');
  const thead=document.createElement('thead');
  thead.innerHTML='<tr><th style="width:72px">Index</th><th>Color</th><th style="width:72px">ID</th></tr>';
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

    const tdBtn=document.createElement('td');
    const btn=document.createElement('button');
    const isActive = active && active.seg===seg.id && active.idx===L.i;
    btn.textContent = isActive ? 'ID (ON)' : 'ID';
    if (isActive) btn.classList.add('badge-on');

    btn.onclick=async()=>{
      try{
        // Current color from table row
        const currentHex = stripHash(rgb2hex(L.r||0,L.g||0,L.b||0));

        if (active && active.seg===seg.id && active.idx===L.i) {
          // Toggle off: restore previous color
          await restoreActive();
        } else {
          // Switching: restore previous active first
          if (active) await restoreActive();

          // Remember previous color and apply CYAN
          active = { seg: seg.id, idx: L.i, prevHex: currentHex };
          await setLedHex(seg.id, L.i, "00FFFF");
          armAutoClear();
        }
        await refresh();
      }catch(e){ showErr(e.message); }
    };
    tdBtn.appendChild(btn);

    tr.appendChild(tdIdx); tr.appendChild(tdCol); tr.appendChild(tdBtn);
    tbody.appendChild(tr);
  });

  table.appendChild(tbody);
  wrap.appendChild(table);
  return wrap;
}

async function refresh(){
  hideErr();
  const meta = await jget('/json/skyaware');
  document.getElementById('ownToggle').checked = !!meta.own;

  // If the active LED no longer appears cyan (e.g., changed elsewhere), forget it
  if (active) {
    // find that LED in metadata and see if it's still cyan-ish (exact "00FFFF" not guaranteed)
    const seg = meta.segments.find(s => s.id===active.seg);
    const L = seg ? seg.leds.find(x => x.i===active.idx) : null;
    if (!L || !(L.r===0 && L.g===255 && L.b===255)) {
      active = null;
      if (autoTimer){ clearTimeout(autoTimer); autoTimer = null; }
    }
  }

  const c=document.getElementById('segs'); c.innerHTML='';
  meta.segments.forEach(seg => c.appendChild(segTable(seg)));
}

document.getElementById('ownToggle').onchange = async (e) => {
  try {
    const en = e.target.checked;
    await jpost(`/skyaware/own?enable=${en?1:0}`);
    if (en) {
      await fetch('/json/state', {
        method:'POST', headers:{'Content-Type':'application/json'},
        body: JSON.stringify({ on:true, bri:255, seg:[{ id:0, fx:0, frz:true }] })
      });
    } else {
      await fetch('/json/state', {
        method:'POST', headers:{'Content-Type':'application/json'},
        body: JSON.stringify({ seg:[{ id:0, frz:false }] })
      });
    }
    await refresh();
  } catch (err) { showErr(err.message); e.target.checked = !e.target.checked; }
};

document.getElementById('clear').onclick = async () => {
  try{
    if (active) { await restoreActive(); await refresh(); }
  }catch(e){ showErr(e.message); }
};

refresh().catch(e=>showErr(e.message));
</script>
</body></html>
)HTML";

// Global instance + registration
SkyAwareUsermod skyAware;
REGISTER_USERMOD(skyAware);
