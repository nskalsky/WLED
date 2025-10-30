// skyaware_html.h - IMPROVED VERSION
#pragma once
#include <pgmspace.h>

static const char SKY_UI_HTML[] PROGMEM = R"HTML(<!DOCTYPE html>
<html lang="en">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>SkyAware</title>
<style>
  :root{
    --bg:#0f1115; --card:#171a21; --muted:#8a93a6; --text:#e8ecf3;
    --hr:#242938; --ok:#2fa84f; --warn:#c9a227; --danger:#d14b4b; --btn:#30364a;
    --vfr:#20c15a; --mvfr:#3a68ff; --ifr:#ff4b4b; --lifr:#ff3fff; --unknown:#d8dee9;
  }
  html,body{background:var(--bg);color:var(--text);font:14px/1.35 system-ui,Segoe UI,Roboto,Helvetica,Arial,sans-serif;margin:0}
  .wrap{max-width:1120px;margin:20px auto;padding:0 12px}
  .row{display:flex;gap:8px;align-items:center}
  .grow{flex:1}
  .card{background:var(--card);border-radius:12px;padding:14px 16px;box-shadow:0 1px 0 rgba(255,255,255,.02) inset}
  .hr{height:1px;background:var(--hr);margin:12px 0}
  .muted{color:var(--muted)}
  .mono{font-family:ui-monospace,SFMono-Regular,Menlo,Consolas,monospace}
  .btn{background:var(--btn);color:var(--text);border:1px solid #3b425a;border-radius:10px;padding:8px 12px;cursor:pointer;transition:all .2s}
  .btn:hover:not(:disabled){background:#3a4157;border-color:#4a5270}
  .btn:disabled{opacity:.6;cursor:not-allowed}
  .btn-ok{background:var(--ok);border-color:#2a8745}
  .btn-ok:hover:not(:disabled){background:#28a047}
  .btn-warn{background:var(--warn);border-color:#a78d23}
  .btn-warn:hover:not(:disabled){background:#b59a25}
  .btn-danger{background:var(--danger);border-color:#b33f3f}
  .btn-danger:hover:not(:disabled){background:#c34545}
  label{display:block;margin:2px 0 6px 0;font-weight:600}
  input[type=text],select{background:#0c0f16;color:var(--text);border:1px solid #3b425a;border-radius:10px;padding:7px 9px;width:100%;transition:border-color .2s}
  input[type=text]:focus,select:focus{outline:none;border-color:#5a6a8a}
  table{width:100%;border-collapse:collapse}
  th,td{padding:6px 8px;border-bottom:1px solid var(--hr);vertical-align:middle}
  th{color:#aab3c7;font-weight:600;text-align:left}
  tr:hover td{background:rgba(255,255,255,.02)}
  .chip{display:inline-block;width:18px;height:18px;border-radius:4px;border:1px solid #0003;margin-right:6px;vertical-align:middle}
  .msg{margin:10px 0 0 0;font-weight:600}
  .pill{padding:2px 8px;border-radius:999px;background:#1f2330;border:1px solid #394057;font-size:12px}
  .pill-large{padding:4px 12px;font-size:13px;font-weight:600}
  .knum{opacity:.9}
  .tight{margin-top:-4px}
  .small{font-size:12px}
  .overflow{max-height:260px;overflow:auto;border:1px solid var(--hr);border-radius:10px}
  .right{justify-content:flex-end}
  
  /* Flight category display */
  .flight-cat-banner{
    background:linear-gradient(135deg, rgba(255,255,255,.03), rgba(255,255,255,.01));
    border-radius:10px;
    padding:12px 16px;
    margin-top:8px;
    border:2px solid var(--hr);
    display:flex;
    align-items:center;
    gap:12px;
  }
  .flight-cat-banner.vfr{border-color:var(--vfr);background:linear-gradient(135deg, rgba(32,193,90,.15), rgba(32,193,90,.05))}
  .flight-cat-banner.mvfr{border-color:var(--mvfr);background:linear-gradient(135deg, rgba(58,104,255,.15), rgba(58,104,255,.05))}
  .flight-cat-banner.ifr{border-color:var(--ifr);background:linear-gradient(135deg, rgba(255,75,75,.15), rgba(255,75,75,.05))}
  .flight-cat-banner.lifr{border-color:var(--lifr);background:linear-gradient(135deg, rgba(255,63,255,.15), rgba(255,63,255,.05))}
  .flight-cat-icon{
    width:48px;
    height:48px;
    border-radius:8px;
    display:flex;
    align-items:center;
    justify-content:center;
    font-weight:700;
    font-size:16px;
    border:2px solid rgba(255,255,255,.1);
  }
  .flight-cat-icon.vfr{background:var(--vfr);color:#fff}
  .flight-cat-icon.mvfr{background:var(--mvfr);color:#fff}
  .flight-cat-icon.ifr{background:var(--ifr);color:#fff}
  .flight-cat-icon.lifr{background:var(--lifr);color:#fff}
  .flight-cat-icon.unknown{background:var(--unknown);color:#333}
  
  /* Mode indicator */
  .mode-indicator{
    display:flex;
    gap:6px;
    padding:10px 14px;
    background:rgba(255,255,255,.02);
    border-radius:8px;
    border:1px solid var(--hr);
  }
  .mode-badge{
    padding:6px 12px;
    border-radius:6px;
    font-weight:600;
    font-size:13px;
    opacity:.4;
    transition:all .2s;
  }
  .mode-badge.active{
    opacity:1;
    background:rgba(58,104,255,.2);
    color:#5a8fff;
    border:1px solid rgba(58,104,255,.4);
  }
  
  /* Unsaved changes warning */
  .unsaved-warning{
    background:rgba(201,162,39,.15);
    border:2px solid var(--warn);
    border-radius:10px;
    padding:10px 14px;
    margin-top:10px;
    display:none;
    align-items:center;
    gap:10px;
  }
  .unsaved-warning.show{display:flex}
  .warning-icon{
    font-size:20px;
    font-weight:700;
    color:var(--warn);
  }
  
  /* Save state indicator */
  .save-status{
    font-size:12px;
    padding:4px 10px;
    border-radius:6px;
    font-weight:600;
    display:none;
  }
  .save-status.saved{
    display:inline-block;
    background:rgba(47,168,79,.15);
    color:var(--ok);
    border:1px solid rgba(47,168,79,.3);
  }
  .save-status.unsaved{
    display:inline-block;
    background:rgba(201,162,39,.15);
    color:var(--warn);
    border:1px solid rgba(201,162,39,.3);
  }
  
  /* Help text */
  .help-text{
    background:rgba(58,104,255,.08);
    border-left:3px solid var(--mvfr);
    padding:10px 12px;
    border-radius:6px;
    margin:8px 0;
    font-size:12px;
    line-height:1.5;
  }
  .help-text strong{color:var(--mvfr)}
  
  /* LED Identification button */
  .btn-id{
    background:rgba(58,104,255,.3);
    border-color:rgba(58,104,255,.5);
    color:#5a8fff;
    font-weight:600;
    padding:6px 10px;
    font-size:12px;
    min-width:50px;
  }
  .btn-id:hover:not(:disabled){
    background:rgba(58,104,255,.5);
    border-color:#5a8fff;
  }
  .btn-id.active{
    background:#3a68ff;
    border-color:#2a5aff;
    color:#fff;
  }
  
  /* LED identification info banner */
  .led-ident-banner{
    background:rgba(58,104,255,.15);
    border:2px solid var(--mvfr);
    border-radius:10px;
    padding:10px 14px;
    margin-top:10px;
    display:none;
    align-items:center;
    gap:12px;
    font-size:13px;
  }
  .led-ident-banner.show{
    display:flex;
  }
  .led-ident-pulse{
    display:inline-block;
    width:12px;
    height:12px;
    background:#3a68ff;
    border-radius:50%;
  }
</style>
</head>
<body>
<div class="wrap">

  <!-- STATUS -->
  <div class="card">
    <div class="row">
      <div class="grow">
        <div class="row tight">
          <div class="pill pill-large">SkyAware</div>
          <div id="p_enabled" class="pill">—</div>
          <span id="saveStatus" class="save-status">● Saved to device</span>
        </div>
        <div class="muted small" style="margin-top:6px">
          <span id="p_url">—</span>
        </div>
        <div class="muted small" id="p_err" style="color:var(--danger)"></div>
      </div>
      <div class="row">
        <button id="btnRefresh" class="btn">🔄 Refresh Now</button>
      </div>
    </div>

    <!-- MODE INDICATOR -->
    <div class="hr"></div>
    <div class="row" style="align-items:flex-start;gap:12px">
      <div class="mode-indicator">
        <div class="mode-badge" id="modeBadgeSingle">
          <span style="font-size:16px">🎯</span> SINGLE
        </div>
        <div class="mode-badge" id="modeBadgeMulti">
          <span style="font-size:16px">🗺️</span> MULTIPLE
        </div>
      </div>
      <div class="grow" id="singleModeInfo" style="display:none">
        <div style="font-size:12px;color:var(--muted);margin-bottom:4px">Primary Airport</div>
        <div style="font-size:16px;font-weight:700;font-family:monospace" id="primaryAirport">—</div>
      </div>
      <div class="grow" id="multiModeInfo" style="display:none">
        <div style="font-size:12px;color:var(--muted);margin-bottom:4px">Mapped Airports</div>
        <div style="font-size:14px;font-weight:600" id="airportCount">—</div>
      </div>
    </div>

    <!-- FLIGHT CATEGORY BANNER (Single Mode Only) -->
    <div id="flightCatBanner" class="flight-cat-banner" style="display:none">
      <div class="flight-cat-icon" id="flightCatIcon">—</div>
      <div class="grow">
        <div style="font-size:11px;color:var(--muted);text-transform:uppercase;letter-spacing:.5px;font-weight:600">Flight Category</div>
        <div style="font-size:18px;font-weight:700;margin:2px 0" id="flightCatText">—</div>
        <div style="font-size:12px;color:var(--muted)" id="flightCatDesc">—</div>
      </div>
      <div style="text-align:right">
        <div style="font-size:11px;color:var(--muted);margin-bottom:4px">METAR</div>
        <div style="font-size:12px;font-family:monospace" id="metarSummary">—</div>
      </div>
    </div>

    <div class="hr"></div>
    <div class="row">
      <div class="grow">
        <div class="row" style="flex-wrap:wrap;gap:12px">
          <div><span class="muted">HTTP:</span> <span class="mono" id="p_http">—</span></div>
          <div><span class="muted">Last OK:</span> <span class="mono" id="p_lastok">—</span>s</div>
          <div><span class="muted">Next:</span> <span class="mono" id="p_eta">—</span>s</div>
          <div><span class="muted">Period:</span> <span class="mono" id="p_period">—</span>s</div>
          <div><span class="muted">Retries:</span> <span class="mono" id="p_retry">—</span></div>
        </div>
        <div class="muted small" style="margin-top:4px">WiFi: <span id="p_wifi">—</span></div>
      </div>
      <div>
        <label style="margin-bottom:8px">Default Station ID</label>
        <div class="row">
          <input type="text" id="inpAirport" class="mono" placeholder="KPDX" style="width:120px">
          <button id="btnSetAp" class="btn btn-ok">Set & Refresh</button>
      </div></div>
    </div>
  </div>

  <!-- MAPPING EDITOR -->
  <div class="card" style="margin-top:12px">
    <div class="row">
      <div class="grow">
        <div style="font-size:16px;font-weight:700;margin-bottom:8px">Segment Configuration</div>
        <div class="row">
          <div><strong>Segment</strong></div>
          <select id="selSeg" style="width:80px"></select>
          <div class="muted">Start:</div><div id="segStart" class="mono knum">—</div>
          <div class="muted">Stop:</div><div id="segStop" class="mono knum">—</div>
          <div class="muted">LEDs:</div><div id="segLen" class="mono knum" style="font-weight:700">—</div>
        </div>
        <div class="row" style="margin-top:8px">
          <div class="muted">Display Mode:</div>
          <select id="selMode" style="width:140px">
            <option value="SINGLE">Single Airport</option>
            <option value="MULTIPLE">Multiple Airports</option>
          </select>
          <span id="segMode" class="pill">—</span>
        </div>
      </div>
      <div class="row right">
        <input id="fileCsv" type="file" accept=".csv,text/csv" style="display:none">
        <button id="btnCsvDown" class="btn">⬇ CSV</button>
        <button id="btnCsvUp" class="btn">⬆ CSV</button>
        <button id="btnClear" class="btn btn-danger">Clear</button>
        <button id="btnSave" class="btn btn-ok" style="font-weight:700">💾 Save to Device</button>
      </div>
    </div>

    <!-- Unsaved changes warning -->
    <div id="unsavedWarning" class="unsaved-warning">
      <div class="warning-icon">⚠</div>
      <div class="grow">
        <strong>Unsaved Changes</strong> — Click "Save to Device" to persist your mapping configuration to WLED.
      </div>
    </div>

    <div class="help-text">
      <strong>Single Mode:</strong> All LEDs in this segment show the same airport.<br>
      <strong>Multiple Mode:</strong> Each LED can show a different airport. Use shorthand like <code>KPDX+5</code> to fill 5 LEDs with KPDX, or list individually.
    </div>

    <div class="hr"></div>
    <div class="muted small" style="margin-bottom:6px">
      <strong>Per-LED Airport Mapping</strong> (one row per LED)
    </div>

    <div class="overflow">
      <table id="tblMap">
        <thead>
          <tr>
            <th style="width:84px">LED Index</th>
            <th style="width:92px">Abs LED</th>
            <th style="width:50px">ID</th>
            <th style="width:200px">Station ID</th>
            <th style="width:160px">Flight Category</th>
            <th>Notes</th>
          </tr>
        </thead>
        <tbody id="mapBody"></tbody>
      </table>
    </div>
  </div>

  <!-- DIAGNOSTICS -->
  <div class="card" style="margin-top:12px">
    <div class="row">
      <div class="grow"><div style="font-size:16px;font-weight:700">Network Diagnostics</div></div>
      <div class="row">
        <button id="btnLoadState" class="btn">Refresh State</button>
        <button id="btnHttpsTest" class="btn">Run HTTPS Test</button>
      </div>
    </div>
    <div class="hr"></div>
    <table>
      <tbody>
        <tr><td class="muted">DNS Provider</td><td class="mono" id="d_dnsProv">—</td></tr>
        <tr><td class="muted">DNS Fallback Used</td><td class="mono" id="d_dnsFb">—</td></tr>
        <tr><td class="muted">Stage</td><td class="mono" id="d_stage">—</td></tr>
        <tr><td class="muted">Detail</td><td class="mono" id="d_detail">—</td></tr>
        <tr><td class="muted">DNS ms</td><td class="mono" id="d_dnsMs">—</td></tr>
        <tr><td class="muted">TCP ms</td><td class="mono" id="d_tcpMs">—</td></tr>
        <tr><td class="muted">TLS ms</td><td class="mono" id="d_tlsMs">—</td></tr>
        <tr><td class="muted">HTTP ms</td><td class="mono" id="d_httpMs">—</td></tr>
        <tr><td class="muted">HTTP Code</td><td class="mono" id="d_http">—</td></tr>
        <tr><td class="muted">Redirect</td><td class="mono" id="d_redirect">—</td></tr>
        <tr><td class="muted">Bytes</td><td class="mono" id="d_bytes">—</td></tr>
        <tr><td class="muted">Last URL</td><td class="mono" id="d_url">—</td></tr>
        <tr><td class="muted">OK</td><td class="mono" id="d_ok">—</td></tr>
      </tbody>
    </table>
    <div class="hr"></div>
    <div class="muted small">Raw /state response</div>
    <pre id="diagRaw" class="mono overflow" style="max-height:320px">—</pre>
  </div>

  <div id="msg" class="msg"></div>
</div>

<script>
/*** Elements ***/
const msg = document.getElementById('msg');
const p_enabled = document.getElementById('p_enabled');
const p_http    = document.getElementById('p_http');
const p_url     = document.getElementById('p_url');
const p_err     = document.getElementById('p_err');
const p_lastok  = document.getElementById('p_lastok');
const p_eta     = document.getElementById('p_eta');
const p_period  = document.getElementById('p_period');
const p_retry   = document.getElementById('p_retry');
const p_wifi    = document.getElementById('p_wifi');
const inpAirport= document.getElementById('inpAirport');
const saveStatus= document.getElementById('saveStatus');
const unsavedWarning = document.getElementById('unsavedWarning');

const modeBadgeSingle = document.getElementById('modeBadgeSingle');
const modeBadgeMulti  = document.getElementById('modeBadgeMulti');
const singleModeInfo  = document.getElementById('singleModeInfo');
const multiModeInfo   = document.getElementById('multiModeInfo');
const primaryAirport  = document.getElementById('primaryAirport');
const airportCount    = document.getElementById('airportCount');
const flightCatBanner = document.getElementById('flightCatBanner');
const flightCatIcon   = document.getElementById('flightCatIcon');
const flightCatText   = document.getElementById('flightCatText');
const flightCatDesc   = document.getElementById('flightCatDesc');
const metarSummary    = document.getElementById('metarSummary');

const selSeg    = document.getElementById('selSeg');
const segStart  = document.getElementById('segStart');
const segStop   = document.getElementById('segStop');
const segLen    = document.getElementById('segLen');
const selMode   = document.getElementById('selMode');
const segMode   = document.getElementById('segMode');
const mapBody   = document.getElementById('mapBody');

const btnCsvDown = document.getElementById('btnCsvDown');
const btnCsvUp   = document.getElementById('btnCsvUp');
const btnClear   = document.getElementById('btnClear');
const fileCsv    = document.getElementById('fileCsv');

const d_dnsProv = document.getElementById('d_dnsProv');
const d_dnsFb   = document.getElementById('d_dnsFb');
const d_stage   = document.getElementById('d_stage');
const d_detail  = document.getElementById('d_detail');
const d_dnsMs   = document.getElementById('d_dnsMs');
const d_tcpMs   = document.getElementById('d_tcpMs');
const d_tlsMs   = document.getElementById('d_tlsMs');
const d_httpMs  = document.getElementById('d_httpMs');
const d_http    = document.getElementById('d_http');
const d_redirect= document.getElementById('d_redirect');
const d_bytes   = document.getElementById('d_bytes');
const d_url     = document.getElementById('d_url');
const d_ok      = document.getElementById('d_ok');
const diagRaw   = document.getElementById('diagRaw');

/*** State ***/
let STATE = {};
let SEGMENTS = [];
let AIRCAT = {};
let tickTimer = null;
let hasUnsavedChanges = false;

/* Per-segment scratch; strictly separated per mode.
   cache[segIdx] = { mode:'SINGLE'|'MULTIPLE',
                     singleAirport:'KXXX[+N]',
                     multi:[... per-LED ...],
                     len,start,stop }
   Persisted to localStorage so MULTIPLE isn't lost by reloads/mode flips.
*/
const cache = {};
const LSK = 'skyaware-cache-v2';

/*** LED IDENTIFICATION STATE & FUNCTIONS ***/

// Global state for LED identification
let ledIdentState = {
  active: false,
  ledIndex: null,
  identifyingSegIdx: null
};

// Function to start LED identification for a specific LED
async function startLedIdentification(ledAbsIndex) {
  try {
    const r = await fetch('/api/skyaware/led/identify?idx=' + ledAbsIndex);
    if (!r.ok) throw new Error('LED identify failed: '+r.status);
    
    ledIdentState.active = true;
    ledIdentState.ledIndex = ledAbsIndex;
    updateLedIdentUI();
    
  } catch (e) {
    console.error('LED identification error:', e);
    setMsg('LED identification failed: '+e.message, 'var(--danger)');
  }
}

// Function to stop LED identification
async function stopLedIdentification() {
  try {
    const r = await fetch('/api/skyaware/led/stop', { method: 'GET' });
    if (!r.ok) throw new Error('LED stop failed: '+r.status);
    
    ledIdentState.active = false;
    ledIdentState.ledIndex = null;
    updateLedIdentUI();
    
  } catch (e) {
    console.error('LED stop error:', e);
  }
}

// Update LED identification UI (button states and banner)
function updateLedIdentUI() {
  const buttons = document.querySelectorAll('.btn-led-id');
  buttons.forEach(btn => {
    const idx = parseInt(btn.getAttribute('data-led-idx'), 10);
    if (ledIdentState.active && idx === ledIdentState.ledIndex) {
      btn.classList.add('active');
      btn.textContent = '🔵 Identifying...';
    } else {
      btn.classList.remove('active');
      btn.textContent = '🆔 ID';
    }
  });
  
  // Update banner if it exists
  const banner = document.querySelector('.led-ident-banner');
  if (banner) {
    if (ledIdentState.active) {
      banner.classList.add('show');
      banner.innerHTML = `
        <div class="led-ident-pulse"></div>
        <div class="grow">
          <strong>LED ${ledIdentState.ledIndex} → IDENT (Cyan)</strong> — Click <strong>Stop ID</strong> to end
        </div>
        <button class="btn btn-id" onclick="stopLedIdentification()" style="min-width:auto">Stop ID</button>
      `;
    } else {
      banner.classList.remove('show');
    }
  }
}

/*** END LED IDENTIFICATION ***/

function loadLS(){
  try{
    const s = localStorage.getItem(LSK);
    if (!s) return;
    const o = JSON.parse(s);
    if (o && typeof o==='object'){
      for (const k of Object.keys(o)) cache[k] = o[k];
    }
  }catch{}
}
function saveLS(){
  try{ localStorage.setItem(LSK, JSON.stringify(cache)); }catch{}
}

/*** Utils ***/
function setMsg(t, color){ msg.textContent=t || ''; msg.style.color = color || ''; }
const upper = s => (s||'').trim().toUpperCase();

function colorForCat(cat){
  switch((cat||'').toUpperCase()){
    case 'VFR': return '#20c15a';
    case 'MVFR':return '#3a68ff';
    case 'IFR': return '#ff4b4b';
    case 'LIFR':return '#ff3fff';
    default:    return '#d8dee9';
  }
}
function catClass(cat){
  return (cat||'unknown').toLowerCase();
}
function catDescription(cat){
  switch((cat||'').toUpperCase()){
    case 'VFR':  return 'Visual Flight Rules - Ceiling ≥3000ft, Visibility ≥5mi';
    case 'MVFR': return 'Marginal VFR - Ceiling 1000-3000ft, Visibility 3-5mi';
    case 'IFR':  return 'Instrument Flight Rules - Ceiling 500-1000ft, Visibility 1-3mi';
    case 'LIFR': return 'Low IFR - Ceiling <500ft, Visibility <1mi';
    default:     return 'Unknown or unavailable';
  }
}
function makeChip(cat){
  if (!cat){ const span=document.createElement('span'); span.textContent='—'; return span; }
  const el = document.createElement('span');
  el.className='chip';
  el.style.background = colorForCat(cat);
  el.title = cat || 'UNKNOWN';
  return el;
}

function markUnsaved(){
  hasUnsavedChanges = true;
  saveStatus.className = 'save-status unsaved';
  saveStatus.textContent = '● Unsaved Changes';
  unsavedWarning.classList.add('show');
}
function markSaved(){
  hasUnsavedChanges = false;
  saveStatus.className = 'save-status saved';
  saveStatus.textContent = '✓ Saved to Device';
  unsavedWarning.classList.remove('show');
}

function expandShorthandToPerLed(tokens, length){
  // tokens: array of user inputs line-by-line
  const out = new Array(length).fill('-');
  let i = 0;
  for (let t=0; t<tokens.length && i<length; t++){
    const raw = (tokens[t]||'').toUpperCase();
    if (!raw){ out[i]='-'; i++; continue; }
    const m = raw.match(/^([A-Z0-9\-_]+)(?:\+(\d+))?$/);
    if (!m){ out[i]='-'; i++; continue; }
    const id = m[1];
    const n  = Math.max(1, parseInt(m[2]||'1',10));
    for(let k=0;k<n && (i+k)<length;k++) out[i+k]=id;
    i += n;
  }
  return out;
}
function collectInputsFromTable(){
  const rows = Array.from(mapBody.querySelectorAll('tr'));
  return rows.map(tr => (tr.querySelector('input')?.value || '').trim().toUpperCase());
}

/*** Fetchers ***/
async function fetchSegments(){
  const r = await fetch('/api/skyaware/segments');
  if (!r.ok) throw new Error('/segments '+r.status);
  const j = await r.json();
  SEGMENTS = (j && j.segments) ? j.segments : [];
}
async function fetchState(){
  const r = await fetch('/api/skyaware/state');
  if (!r.ok) throw new Error('/state '+r.status);
  const j = await r.json();
  STATE = (j && j.SkyAware) ? j.SkyAware : {};
  AIRCAT = {};
  if (STATE.airports && Array.isArray(STATE.airports)){
    STATE.airports.forEach(ap => { AIRCAT[ap.id] = ap.cat; });
  }
}

/*** Cache init (geometry from server; scratch from LS or server map once) ***/
function seedCacheFromServer(){
  SEGMENTS.forEach((seg,idx)=>{
    const len = seg.length|0;
    if (!cache[idx]){
      // initial - NOTE: server returns flat structure with mode/airport/leds at segment level
      const serverMode = seg.mode || 'SINGLE';
      const singleAp   = seg.airport || (STATE.airport||'');
      const multiArr   = [];
      if (serverMode==='MULTIPLE'){
        const arr = Array.isArray(seg.leds) ? seg.leds : [];
        for (let i=0;i<len;i++) multiArr[i] = (i<arr.length)? String(arr[i]).toUpperCase() : '-';
      } else {
        for (let i=0;i<len;i++) multiArr[i] = '-';
      }
      cache[idx] = {mode:serverMode, singleAirport:upper(singleAp||''), multi:multiArr, len, start:seg.start|0, stop:seg.stop|0};
    }else{
      // preserve existing edits; refresh geometry; pad/truncate multi
      cache[idx].len=len; cache[idx].start=seg.start|0; cache[idx].stop=seg.stop|0;
      if (cache[idx].multi.length!==len){
        const old = cache[idx].multi;
        cache[idx].multi = new Array(len).fill('-');
        for (let i=0;i<Math.min(old.length,len);i++) cache[idx].multi[i]=old[i];
      }
      // do NOT overwrite mode/single/multi with server here
    }
  });
}

/*** Rendering ***/
function renderStatus(){
  // Basic status
  p_enabled.textContent = STATE.enabled ? '✓ Enabled' : '○ Disabled';
  p_enabled.style.background = STATE.enabled ? 'rgba(47,168,79,.2)' : 'rgba(138,147,166,.2)';
  p_enabled.style.color = STATE.enabled ? 'var(--ok)' : 'var(--muted)';
  p_enabled.style.border = STATE.enabled ? '1px solid rgba(47,168,79,.4)' : '1px solid var(--hr)';
  
  p_http.textContent    = (STATE.http!=null) ? String(STATE.http) : '—';
  p_url.textContent     = STATE.url || '—';
  p_err.textContent     = STATE.err || '';
  p_lastok.textContent  = (STATE.lastOkSec!=null) ? String(STATE.lastOkSec) : '—';
  p_period.textContent  = (STATE.periodSec!=null) ? String(STATE.periodSec) : '—';
  p_retry.textContent   = (STATE.retryIndex!=null) ? String(STATE.retryIndex) : '—';
  p_wifi.textContent    = `${STATE.ssid||'—'} (${STATE.rssi!=null?STATE.rssi+'dBm':'—'}) • ${STATE.staIP||'—'}`;
  inpAirport.value      = STATE.airport || '';
  
  // Mode badges
  const isSingle = STATE.mode === 'SINGLE';
  modeBadgeSingle.classList.toggle('active', isSingle);
  modeBadgeMulti.classList.toggle('active', !isSingle);
  
  if (isSingle) {
    singleModeInfo.style.display = 'block';
    multiModeInfo.style.display = 'none';
    primaryAirport.textContent = STATE.primary || STATE.airport || '—';
    
    // Flight category banner
    const cat = STATE.category || 'UNKNOWN';
    flightCatBanner.style.display = 'flex';
    flightCatBanner.className = 'flight-cat-banner ' + catClass(cat);
    flightCatIcon.className = 'flight-cat-icon ' + catClass(cat);
    flightCatIcon.textContent = cat.substring(0, 3);
    flightCatText.textContent = cat;
    flightCatDesc.textContent = catDescription(cat);
    
    // METAR summary
    if (STATE.metar && STATE.metar.wind) {
      const parts = [];
      if (STATE.metar.wind) parts.push(STATE.metar.wind);
      if (STATE.metar.vis) parts.push(STATE.metar.vis);
      if (STATE.metar.clouds) parts.push(STATE.metar.clouds.split(' ')[0]);
      metarSummary.textContent = parts.join(' • ') || '—';
    } else {
      metarSummary.textContent = '—';
    }
  } else {
    singleModeInfo.style.display = 'none';
    multiModeInfo.style.display = 'block';
    flightCatBanner.style.display = 'none';
    
    const wanted = STATE.wanted || [];
    const count = wanted.length;
    airportCount.textContent = count === 0 ? 'No airports mapped' : 
                               count === 1 ? '1 airport' :
                               `${count} airports`;
  }
  
  // countdown
  if (tickTimer) { clearInterval(tickTimer); tickTimer=null; }
  const nowSecReported = (STATE.nowSec!=null) ? STATE.nowSec : Math.floor(Date.now()/1000);
  let nowS = nowSecReported;
  let next = STATE.nextAttemptSec||0;
  function tick(){
    const delta = (next - nowS);
    p_eta.textContent = delta>0 ? String(delta) : '0';
    nowS++;
  }
  tick(); tickTimer=setInterval(tick,1000);
}

function fillSegSelector(){
  selSeg.innerHTML='';
  SEGMENTS.forEach((s,idx)=>{
    const opt=document.createElement('option');
    opt.value=String(idx);
    opt.textContent=`Segment ${idx}`;
    selSeg.appendChild(opt);
  });
}

function tdMono(txt){ const td=document.createElement('td'); td.className='mono'; td.textContent=txt; return td; }
function tdMuted(txt){ const td=document.createElement('td'); td.className='muted small'; td.textContent=txt; return td; }

function renderSegment(segIdx){
  const seg = SEGMENTS[segIdx]; 
  const c = cache[segIdx];
  if (!seg || !c){ 
    mapBody.innerHTML='<tr><td colspan="6">No segment data</td></tr>'; 
    return; 
  }
  
  segStart.textContent = String(c.start);
  segStop.textContent  = String(c.stop);
  segLen.textContent   = String(c.len);
  segMode.textContent  = c.mode;
  selMode.value        = c.mode;
  
  mapBody.innerHTML='';
  if (c.mode==='SINGLE'){
    // SINGLE mode: show single row (no ID button)
    const tr=document.createElement('tr');
    const tdIdx = tdMono('All'); 
    tr.appendChild(tdIdx);
    const tdAbs = tdMono(`${c.start}–${c.stop}`); 
    tr.appendChild(tdAbs);
    const tdInp = document.createElement('td');
    const inp = document.createElement('input');
    inp.type='text'; 
    inp.className='mono'; 
    inp.value=c.singleAirport||'';
    inp.style.width='100%';
    inp.oninput = () => markUnsaved();
    tdInp.appendChild(inp);
    tr.appendChild(tdInp);
    const ap = upper(c.singleAirport||STATE.airport||'');
    const tdCat = document.createElement('td');
    tdCat.appendChild(makeChip(AIRCAT[ap]||'UNKNOWN'));
    tdCat.appendChild(document.createTextNode((AIRCAT[ap]||'UNKNOWN')));
    tr.appendChild(tdCat);
    const tdNote = tdMuted('All LEDs same');
    tr.appendChild(tdNote);
    mapBody.appendChild(tr);
  } else {
    // MULTIPLE mode: per-LED with ID buttons
    for (let i=0;i<c.len;i++){
      const tr=document.createElement('tr');
      tr.appendChild(tdMono(String(i)));
      
      // Absolute LED index cell
      const tdAbs = tdMono(String(c.start+i));
      tr.appendChild(tdAbs);
      
      // Add ID button
      const tdBtn = document.createElement('td');
      tdBtn.style.width = '50px';
      tdBtn.style.textAlign = 'center';
      const btnId = document.createElement('button');
      btnId.className = 'btn btn-id btn-led-id';
      btnId.setAttribute('data-led-idx', String(c.start+i));
      btnId.setAttribute('data-seg-idx', String(segIdx));
      btnId.textContent = '🆔 ID';
      btnId.style.width = '100%';
      btnId.onclick = async (e) => {
        e.preventDefault();
        ledIdentState.identifyingSegIdx = segIdx;
        if (ledIdentState.active && ledIdentState.ledIndex === (c.start+i)) {
          await stopLedIdentification();
        } else {
          await stopLedIdentification(); // Stop any current identification
          await startLedIdentification(c.start+i);
        }
      };
      tdBtn.appendChild(btnId);
      tr.appendChild(tdBtn);
      
      // Airport input
      const tdInp=document.createElement('td');
      const inp=document.createElement('input');
      inp.type='text'; 
      inp.className='mono'; 
      inp.value=c.multi[i]||'-';
      inp.style.width='100%';
      inp.oninput = () => markUnsaved();
      tdInp.appendChild(inp);
      tr.appendChild(tdInp);
      
      // Category chip
      const ap = upper(c.multi[i]||'');
      const cat = (ap==='-'||ap==='SKIP'||!ap) ? null : (AIRCAT[ap]||'UNKNOWN');
      const tdCat=document.createElement('td');
      if (cat){
        tdCat.appendChild(makeChip(cat));
        tdCat.appendChild(document.createTextNode(cat));
      } else {
        tdCat.appendChild(document.createTextNode('—'));
      }
      tr.appendChild(tdCat);
      
      // Notes
      const tdNote=tdMuted('');
      tr.appendChild(tdNote);
      mapBody.appendChild(tr);
    }
  }
  
  // Show LED identification banner only in MULTIPLE mode
  const banner = document.querySelector('.led-ident-banner');
  if (c.mode === 'MULTIPLE') {
    if (!banner) {
      const newBanner = document.createElement('div');
      newBanner.className = 'led-ident-banner';
      mapBody.parentElement.parentElement.appendChild(newBanner);
    }
    if (ledIdentState.active && ledIdentState.identifyingSegIdx === segIdx) {
      updateLedIdentUI();
    }
  }
}

function renderDiag(){
  d_dnsProv.textContent = (STATE.net && STATE.net.dnsProvider) || '—';
  d_dnsFb.textContent   = (STATE.net && STATE.net.dnsFallback!=null) ? String(STATE.net.dnsFallback) : '—';
  d_stage.textContent   = (STATE.net && STATE.net.stage) || '—';
  d_detail.textContent  = (STATE.net && STATE.net.detail) || '—';
  d_dnsMs.textContent   = (STATE.net && STATE.net.dnsMs!=null) ? String(STATE.net.dnsMs) : '—';
  d_tcpMs.textContent   = (STATE.net && STATE.net.tcpMs!=null) ? String(STATE.net.tcpMs) : '—';
  d_tlsMs.textContent   = (STATE.net && STATE.net.tlsMs!=null) ? String(STATE.net.tlsMs) : '—';
  d_httpMs.textContent  = (STATE.net && STATE.net.httpMs!=null) ? String(STATE.net.httpMs) : '—';
  d_http.textContent    = (STATE.net && STATE.net.http!=null) ? String(STATE.net.http) : '—';
  d_redirect.textContent= (STATE.net && STATE.net.redirect) || '—';
  d_bytes.textContent   = (STATE.net && STATE.net.bytes!=null) ? String(STATE.net.bytes) : '—';
  d_url.textContent     = STATE.url || '—';
  d_ok.textContent      = (STATE.net && STATE.net.ok!=null) ? String(STATE.net.ok) : '—';
  diagRaw.textContent   = JSON.stringify(STATE, null, 2);
}

/*** Persist edits strictly to the right scratch ***/
function persistEditsToCache(segIdx){
  const seg = SEGMENTS[segIdx]; const c = cache[segIdx]; if(!seg||!c) return;
  const modeNow = selMode.value;
  if (modeNow==='SINGLE'){
    const first = mapBody.querySelector('input');
    const val = upper(first ? first.value : '');
    c.singleAirport = val;                 // update SINGLE scratch
    // do NOT touch c.multi here
  } else {
    const inputs = collectInputsFromTable();
    c.multi = expandShorthandToPerLed(inputs, seg.length); // update MULTIPLE scratch
    // do NOT touch c.singleAirport here
  }
  c.mode = modeNow; // remember last viewed mode
  saveLS();
}

/*** Save to device ***/
async function saveMapping(){
  const segIdx = parseInt(selSeg.value||'0',10);
  const seg = SEGMENTS[segIdx]; const c = cache[segIdx];
  if (!seg || !c){ setMsg('No segment selected', 'var(--danger)'); return; }

  // persist edits locally first
  persistEditsToCache(segIdx);

  const body = new URLSearchParams();
  body.set('seg', String(segIdx));

  if (c.mode==='SINGLE'){
    let ap = c.singleAirport || STATE.airport || '';
    body.set('mode','SINGLE');
    body.set('airport', ap);
  } else {
    const csv = c.multi.join(',');
    body.set('mode','MULTIPLE');
    body.set('leds', csv);
  }

  setMsg('Saving to device...', 'var(--muted)');
  const r = await fetch('/api/skyaware/map', {method:'POST', body});
  if (!r.ok){ setMsg('Save failed: '+r.status, 'var(--danger)'); return; }
  
  markSaved();
  setMsg('✓ Saved to device! Refresh queued.', 'var(--ok)');

  // Refresh server state/segments
  await Promise.all([fetchSegments(), fetchState()]);
  seedCacheFromServer();
  renderSegment(segIdx);
  renderStatus();
  renderDiag();
  
  setTimeout(() => setMsg('', ''), 3000);
}

/*** CSV helpers ***/
function csvFromArray(arr){ return arr.join(','); }
function arrayFromCsv(text){
  return text.split(',').map(s => upper(s.replace(/\r?\n/g,''))).map(s => s||'-');
}
function downloadText(filename, text){
  const blob = new Blob([text], {type:'text/plain'});
  const url = URL.createObjectURL(blob);
  const a = document.createElement('a'); a.href=url; a.download=filename;
  document.body.appendChild(a); a.click(); a.remove();
  setTimeout(()=>URL.revokeObjectURL(url), 0);
}
function downloadCsv(){
  const segIdx = parseInt(selSeg.value||'0',10);
  const seg = SEGMENTS[segIdx]; const c = cache[segIdx];
  if (!seg || !c){ setMsg('No segment selected', 'var(--danger)'); return; }
  persistEditsToCache(segIdx);

  if (c.mode==='SINGLE'){
    // SINGLE: one token shorthand (KPDX+<len>)
    const ap = c.singleAirport || STATE.airport || '';
    const token = ap ? `${ap.includes('+')?ap:(ap+'+'+seg.length)}` : '';
    downloadText(`skyaware-seg${segIdx}-single.csv`, token);
  } else {
    // MULTIPLE: per-LED list
    const arr = c.multi.slice(0, seg.length);
    downloadText(`skyaware-seg${segIdx}-multi.csv`, csvFromArray(arr));
  }
  setMsg('CSV downloaded', 'var(--ok)');
  setTimeout(() => setMsg('', ''), 2000);
}
function uploadCsv(){
  fileCsv.click();
}
fileCsv.addEventListener('change', async ()=>{
  const segIdx = parseInt(selSeg.value||'0',10);
  const seg = SEGMENTS[segIdx]; const c = cache[segIdx];
  if (!seg || !c){ setMsg('No segment selected', 'var(--danger)'); return; }
  const f = fileCsv.files[0]; if (!f) return;
  const text = await f.text();
  const tokens = arrayFromCsv(text).filter(x=>x!=='');
  if (tokens.length===1){
    // single token => interpret as SINGLE shorthand (KPDX or KPDX+N)
    c.singleAirport = tokens[0];
    c.mode = 'SINGLE';
    selMode.value = 'SINGLE';
    markUnsaved();
    setMsg('✓ CSV loaded (SINGLE mode). Click "Save to Device" to apply.', 'var(--warn)');
  } else {
    // multiple tokens => MULTIPLE per-LED
    c.multi = new Array(seg.length).fill('-');
    for (let i=0;i<Math.min(tokens.length, seg.length); i++) c.multi[i]=tokens[i];
    c.mode = 'MULTIPLE';
    selMode.value='MULTIPLE';
    markUnsaved();
    setMsg('✓ CSV loaded (MULTIPLE mode). Click "Save to Device" to apply.', 'var(--warn)');
  }
  saveLS();
  renderSegment(segIdx);
  fileCsv.value='';
});

/*** Clear mapping ***/
function clearMapping(){
  const segIdx = parseInt(selSeg.value||'0',10);
  const seg = SEGMENTS[segIdx]; const c = cache[segIdx];
  if (!seg || !c){ setMsg('No segment selected', 'var(--danger)'); return; }

  if (selMode.value === 'SINGLE'){
    c.singleAirport = '';
    markUnsaved();
    setMsg('Cleared SINGLE mapping. Click "Save to Device" to apply.', 'var(--warn)');
  } else {
    c.multi = new Array(seg.length).fill('-');
    markUnsaved();
    setMsg(`Cleared MULTIPLE mapping (${seg.length} LEDs). Click "Save to Device" to apply.`, 'var(--warn)');
  }
  saveLS();
  renderSegment(segIdx);
}

/*** Actions ***/
async function doRefresh(){ 
  await fetch('/api/skyaware/refresh'); 
  setMsg('✓ Refresh queued', 'var(--ok)');
  setTimeout(() => setMsg('', ''), 2000);
}
async function setAirport(){
  const a = upper(inpAirport.value);
  if (!a){ setMsg('Enter station ID', 'var(--danger)'); return; }
  const url = '/api/skyaware/set?airport='+encodeURIComponent(a);
  const r = await fetch(url);
  if (!r.ok){ setMsg('Set failed: '+r.status, 'var(--danger)'); return; }
  setMsg('✓ Default station set to '+a+', refreshing...', 'var(--ok)');
  await Promise.all([fetchState()]);
  renderStatus(); renderDiag();
  setTimeout(() => setMsg('', ''), 3000);
}
async function loadStateDiag(){
  await fetchState();
  renderStatus();
  renderDiag();
  setMsg('State refreshed', 'var(--ok)');
  setTimeout(() => setMsg('', ''), 2000);
}
async function runHttpsTest(){
  setMsg('Running HTTPS test...', 'var(--muted)');
  const r = await fetch('/api/skyaware/https_test');
  if (!r.ok){ setMsg('HTTPS test failed: '+r.status, 'var(--danger)'); return; }
  const j = await r.json();
  STATE.url = j.url || STATE.url;
  STATE.http = (j.http!=null)? j.http : STATE.http;
  STATE.net = j.net || STATE.net;
  renderDiag();
  diagRaw.textContent = JSON.stringify(j, null, 2);
  setMsg('✓ HTTPS test complete', 'var(--ok)');
  setTimeout(() => setMsg('', ''), 3000);
}

/*** Init ***/
async function init(){
  loadLS();
  try{
    await Promise.all([fetchSegments(), fetchState()]);
    renderStatus();
    fillSegSelector();
    seedCacheFromServer();
    const firstIdx = (selSeg.options.length>0) ? parseInt(selSeg.options[0].value,10) : 0;
    selSeg.value = String(firstIdx);
    renderSegment(firstIdx);
    renderDiag();
    markSaved(); // Start in saved state
    setMsg('');
  }catch(e){
    setMsg('Init failed: '+e.message, 'var(--danger)');
  }
}

/*** Wiring ***/
document.getElementById('btnRefresh').onclick = doRefresh;
document.getElementById('btnSetAp').onclick  = setAirport;
document.getElementById('btnSave').onclick   = saveMapping;
document.getElementById('btnLoadState').onclick = loadStateDiag;
document.getElementById('btnHttpsTest').onclick = runHttpsTest;
btnCsvDown.onclick = downloadCsv;
btnCsvUp.onclick   = uploadCsv;
btnClear.onclick   = clearMapping;

selSeg.onchange = ()=>{
  // persist previous seg edits, then switch
  const prev = parseInt(selSeg.getAttribute('data-prev')||'-1',10);
  if (!isNaN(prev) && prev>=0) persistEditsToCache(prev);
  const idx = parseInt(selSeg.value||'0',10);
  renderSegment(idx);
  selSeg.setAttribute('data-prev', String(idx));
};
selMode.onchange = ()=>{
  // Do NOT mutate the other mode's scratch; just remember the current choice
  const idx = parseInt(selSeg.value||'0',10);
  const c = cache[idx]; 
  if (c){ 
    c.mode = selMode.value; 
    saveLS(); 
    markUnsaved();
  }
  renderSegment(idx);
};

/*** Cleanup on page unload ***/
window.addEventListener('beforeunload', async () => {
  if (ledIdentState.active) {
    await stopLedIdentification();
  }
});

init();
</script>
</body>
</html>
)HTML";