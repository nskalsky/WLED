#pragma once
#include <pgmspace.h>

// SkyAware Stage 0 page WITH Map Profile dropdown + Custom CSV bulk editor.
static const char SKYAWARE_HTML[] PROGMEM = R"HTML(
<!doctype html>
<html lang="en">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
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
.badge{display:inline-block;padding:2px 8px;border-radius:999px;border:1px solid #2c3450;background:var(--card);color:#b8c0d9;font-size:12px}
.badge-on{background:#203a20;border-color:#2a5a2a;color:#a9e6a9}
input[type=text], select, textarea{background:#20263a;border:1px solid #2c3450;border-radius:6px;color:var(--text);padding:6px 8px;min-width:160px}
.err{white-space:pre-wrap;background:#241a1a;border:1px solid #4a2d2d;color:#ffd6d6;border-radius:10px;padding:10px;margin-bottom:10px;display:none}
input[type="file"]{display:none}
label.filebtn{padding:6px 10px;border-radius:8px;background:var(--btn);cursor:pointer}
label.filebtn:hover{background:var(--btnh)}
.small{font-size:12px}
.grid{display:grid;gap:8px;grid-template-columns:1fr}
</style>
</head>
<body>
<header><h1>SkyAware — Stage 0 (Owns Segments)</h1></header>
<main>
  <div id="err" class="err"></div>

  <!-- Map Profile selector (Dropdown + Custom CSV bulk editor) -->
  <div class="card">
    <div class="row" style="align-items:flex-start">
      <div>
        <div class="small muted">Map Profile</div>
        <select id="mapProfile"></select>
        <div class="small muted">Choose a precompiled profile or pick <b>Custom</b> to paste CSV per segment (saved to <code>/skyaware/map.json</code>).</div>
      </div>
      <div id="customArea" style="display:none;flex:1;min-width:280px">
        <div class="small muted" style="margin-bottom:6px">Custom CSV (one line per segment; use <b>SKIP</b> or <b>-</b> to leave an LED off)</div>
        <div id="csvContainer" class="grid"></div>
        <div class="row" style="justify-content:flex-start">
          <button id="applyCustom">Apply Custom</button>
        </div>
      </div>
    </div>
  </div>

  <!-- Existing Stage-0 controls -->
  <div class="card">
    <div class="row">
      <div class="muted">
        CSV: row = segment, columns = LED index, value = AIRPORT or SKIP. Airport IDs persist when you Save in Settings → Usermods.<br>
        ID paints one LED <span class="cyan">CYAN</span>. Click again to restore. Only one at a time (auto-clear optional in code).
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

function showErr(msg){ const e=document.getElementById('err'); e.textContent=msg; e.style.display='block'; }
function hideErr(){ const e=document.getElementById('err'); e.style.display='none'; e.textContent=''; }
async function jget(u){ const r=await fetch(u); const ct=r.headers.get('content-type')||''; if(!r.ok) throw new Error(`HTTP ${r.status}`); if(!ct.includes('application/json')){ const t=await r.text(); throw new Error(`Expected JSON, got ${ct||'unknown'}:\n${t.slice(0,200)}`);} return r.json(); }
async function jpost(u){ const r=await fetch(u,{method:'POST'}); if(!r.ok) throw new Error(await r.text()); return r.text(); }
async function postState(payload){ await fetch('/json/state',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify(payload)}); }
function hex(n){ return n.toString(16).padStart(2,'0'); }
function rgb2hex(r,g,b){ return '#'+hex(r)+hex(g)+hex(b); }
function stripHash(H){ return (H||'').replace(/^#/,'').toUpperCase(); }
function debounce(fn,ms){ let t=null; return (...a)=>{ clearTimeout(t); t=setTimeout(()=>fn(...a), ms); }; }

let active = null; // {seg, idx, prevHex}
let autoTimer = null;
async function setLedHex(segId, idx, hexNoHash){ await postState({ seg: { id: segId, i: [ idx, hexNoHash ] } }); }
async function restoreActive(){ if (!active) return; try{ await setLedHex(active.seg, active.idx, active.prevHex);} finally{ active=null; if (autoTimer){ clearTimeout(autoTimer); autoTimer=null; } } }
function armAutoClear(){ if (!AUTO_CLEAR_MS) return; if (autoTimer) clearTimeout(autoTimer); autoTimer=setTimeout(async()=>{ try{ await restoreActive(); await refresh(); }catch(e){ showErr(e.message);} }, AUTO_CLEAR_MS); }

const saveAirportDebounced = debounce(async (seg, idx, airport)=>{
  await fetch('/skyaware/map',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify({seg,idx,airport})});
  // Persist by going to Settings → Usermods → Save
}, 250);

function segTable(seg, mapForSeg){
  const wrap=document.createElement('div'); wrap.className='card';
  const title=document.createElement('div'); title.className='segtitle';
  title.textContent=`Segment ${seg.id} — start ${seg.start}, length ${seg.len}`;
  wrap.appendChild(title);

  const table=document.createElement('table');
  const thead=document.createElement('thead');
  thead.innerHTML='<tr><th style="width:72px">Index</th><th>Color</th><th style="min-width:120px">Airport</th><th style="width:92px">ID</th></tr>';
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
    const input=document.createElement('input'); input.type='text'; input.placeholder='e.g. KPDX or SKIP';
    input.value = (mapForSeg && mapForSeg[apKey]) ? mapForSeg[apKey] : '';
    input.oninput=()=>{ input.value = input.value.toUpperCase(); };
    input.onchange=()=>{ saveAirportDebounced(seg.id, L.i, input.value.toUpperCase().trim()); };
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
  document.getElementById('ownToggle').checked = !!meta.own;

  if (active) {
    const s = meta.segments.find(x=>x.id===active.seg);
    const L = s ? s.leds.find(x=>x.i===active.idx) : null;
    if (!L || !(L.r===0 && L.g===255 && L.b===255)) {
      active=null; if (autoTimer){ clearTimeout(autoTimer); autoTimer=null; }
    }
  }

  const c=document.getElementById('segs'); c.innerHTML='';
  const map = meta.map || {};
  meta.segments.forEach(seg=>{
    const segMap = map[String(seg.id)] || {};
    c.appendChild(segTable(seg, segMap));
  });
}

// ---- Map Profile dropdown + Custom bulk CSV ----
async function fetchJSON(url, opts) {
  const r = await fetch(url, opts);
  if (!r.ok) throw new Error(url + " -> " + r.status);
  return r.json();
}

function ensureCustomInputs(segCount, prefilledMap) {
  const holder = document.getElementById('csvContainer');
  holder.innerHTML = '';
  for (let s=0; s<segCount; s++) {
    const wrap = document.createElement('div');
    const lbl = document.createElement('div'); lbl.className='small muted'; lbl.textContent = 'Segment ' + s;
    const ta = document.createElement('textarea'); ta.id = 'csv-' + s; ta.rows = 2; ta.placeholder = 'KHIO,KPDX,-,KTTD,SKIP,...';
    if (prefilledMap && prefilledMap[String(s)]) ta.value = prefilledMap[String(s)];
    wrap.appendChild(lbl); wrap.appendChild(ta);
    holder.appendChild(wrap);
  }
}

async function loadMapProfileUI() {
  try {
    const [presets, status] = await Promise.all([
      fetchJSON('/skyaware/presets'),
      fetchJSON('/skyaware/status').catch(_ => ({ mapProfile:'Custom', map:{} }))
    ]);

    // Fill dropdown
    const sel = document.getElementById('mapProfile');
    sel.innerHTML = '';
    for (const name of presets) {
      const opt = document.createElement('option'); opt.value = name; opt.textContent = name; sel.appendChild(opt);
    }

    // Select current profile
    const current = status.mapProfile || 'Custom';
    sel.value = current;

    // If Custom, show & prefill textareas from status.map
    const custom = document.getElementById('customArea');
    if (current === 'Custom') {
      custom.style.display = '';
      const csvs = status.map || {};
      const segIds = Object.keys(csvs).map(s=>parseInt(s,10)).sort((a,b)=>a-b);
      const maxSeg = segIds.length ? segIds[segIds.length-1] : 0;
      ensureCustomInputs(maxSeg+1, csvs);
    } else {
      custom.style.display = 'none';
    }

    // Change handler
    sel.onchange = async (e) => {
      const val = e.target.value;
      if (val === 'Custom') {
        document.getElementById('customArea').style.display = '';
        ensureCustomInputs(1, {}); // at least one box
      } else {
        document.getElementById('customArea').style.display = 'none';
        await fetch('/skyaware/apply', {
          method:'POST',
          headers:{'Content-Type':'application/x-www-form-urlencoded'},
          body: 'mapProfile=' + encodeURIComponent(val)
        });
        await refresh();
      }
    };

    document.getElementById('applyCustom').onclick = async () => {
      const data = new URLSearchParams();
      data.set('mapProfile','Custom');
      document.querySelectorAll('[id^="csv-"]').forEach(el => data.set(el.id, el.value));
      const res = await fetch('/skyaware/apply', { method:'POST', headers:{'Content-Type':'application/x-www-form-urlencoded'}, body:data });
      if (!res.ok) showErr(await res.text()); else await refresh();
    };

  } catch (e) {
    showErr('Map Profile UI failed: ' + e.message);
  }
}

// ---- existing handlers ----
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
    await refresh();
  }catch(e){ showErr(e.message); }
  ev.target.value='';
});

document.getElementById('exportCsv').onclick = ()=>{ window.location.href = '/skyaware/csv'; };

// Initial load
refresh().catch(e=>showErr(e.message));
loadMapProfileUI().catch(e=>showErr(e.message));
</script>
</body>
</html>
)HTML";
