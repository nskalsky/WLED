#pragma once

// Simple, inline UI for /skyaware (no external assets).
// Served from PROGMEM to save RAM.

#if defined(ARDUINO_ARCH_ESP32) || defined(ESP8266)
  #include <pgmspace.h>
#endif

static const char SKY_UI_HTML[] PROGMEM = R"HTML(
<!doctype html>
<meta name=viewport content="width=device-width,initial-scale=1">
<title>SkyAware</title>
<style>
  body{font-family:system-ui,-apple-system,Segoe UI,Roboto,Helvetica,Arial,sans-serif;margin:16px;color:#111}
  .wrap{max-width:980px}
  .card{border:1px solid #e5e5e5;border-radius:12px;padding:14px;background:#fff;box-shadow:0 1px 2px rgba(0,0,0,.04);margin-bottom:14px}
  .row{display:flex;gap:12px;flex-wrap:wrap;align-items:center}
  label{display:inline-flex;align-items:center;gap:.5rem}
  input[type=text],select{padding:.45rem .6rem;border:1px solid #ccc;border-radius:8px;font:inherit}
  button{padding:.48rem .9rem;border:1px solid #bbb;border-radius:10px;background:#fff;cursor:pointer}
  table{border-collapse:collapse;width:100%}
  th,td{border:1px solid #eee;padding:8px;text-align:left;font-size:.95rem}
  .muted{color:#666}.ok{color:#0a0}.err{color:#c00}
  .badge{display:inline-block;padding:6px 10px;border:1px solid #ccc;border-radius:999px;background:#fafafa}
  .catTag{display:inline-block;padding:6px 14px;border-radius:999px;border:1px solid #ccc;background:#f7f7f7;min-width:110px;text-align:center}
  .catTag.VFR{background:#0a0;color:#fff;border-color:#070}
  .catTag.MVFR{background:#08f;color:#fff;border-color:#06c}
  .catTag.IFR{background:#d00;color:#fff;border-color:#a00}
  .catTag.LIFR{background:#c0f;color:#111;border-color:#90c}
  .catTag.UNKNOWN{background:#eee;color:#111;border-color:#ccc}
  .kv{display:grid;grid-template-columns:160px 1fr;gap:6px 10px}
  .hidden{display:none}
  .mt{margin-top:12px}
</style>

<div class=wrap>
  <h2>SkyAware</h2>

  <!-- Status (TOP) -->
  <div class=card>
    <div class=row>
      <div><b>Status</b></div>
      <span id=catTop class="catTag UNKNOWN">UNKNOWN</span>
      <button id=btnRefresh>Fetch Now</button>
      <span id=msg class="muted" style="margin-left:auto"></span>
    </div>
    <div class=row style="align-items:flex-start">
      <div style="flex:1;min-width:260px">
        <div class=kv>
          <div>Next update</div><div id=kvNext>—</div>
          <div>Airport</div><div id=kvAirport>—</div>
          <div>Station</div><div id=kvStation>—</div>
          <div>Obs Time</div><div id=kvTimeZ>—</div>
          <div>Wind</div><div id=kvWind>—</div>
          <div>Visibility</div><div id=kvVis>—</div>
          <div>Clouds</div><div id=kvClouds>—</div>
          <div>Temp/Dew</div><div id=kvTempDew>—</div>
          <div>Altimeter</div><div id=kvAltim>—</div>
        </div>
      </div>
      <div style="flex:1;min-width:260px">
        <div class=kv>
          <div>HTTP</div><div id=kvHttp>—</div>
          <div>DNS</div><div id=kvDns>—</div>
          <div>Timings</div><div id=kvTimes>—</div>
          <div>Wi-Fi</div><div id=kvWifi>—</div>
          <div>Counts</div><div id=kvCounts>—</div>
          <div>Last URL</div><div id=kvUrl style="overflow:auto;white-space:nowrap">—</div>
        </div>
      </div>
    </div>
    <div class=mt><span class=badge>Inspect:
      <a href="/api/skyaware/state" target=_blank>state</a>,
      <a href="/api/skyaware/segments" target=_blank>segments</a>,
      <a href="/api/skyaware/https_test" target=_blank>https_test</a>,
      <a href="/api/skyaware/log" target=_blank>log</a>
    </span></div>
  </div>

  <!-- Mapping / Config (BOTTOM) -->
  <div class=card>
    <div class=row>
      <div><b>Mapping</b></div>
      <label>Segment <select id=segSel></select></label>
      <label><input type=radio name=mode value=SINGLE checked> Single airport</label>
      <label><input type=radio name=mode value=MULTIPLE> Multiple airports</label>
    </div>

    <div id=singleBox class=mt>
      <label>Airport <input id=singleAirport type=text placeholder=KPDX maxlength=8></label>
      <button id=saveSingle>Save</button>
      <span style="margin-left:8px">Category: <span id=catTag class="catTag UNKNOWN">UNKNOWN</span></span>
    </div>

    <div id=multiBox class="mt hidden">
      <div class=muted>Enter airport codes for each LED index. Use '-' to Skip.</div>
      <table class=mt>
        <thead id=ledHead><tr><th style="width:100px">LED #</th><th>Airport</th><th style="width:130px">Cat</th></tr></thead>
        <tbody id=ledTable></tbody>
      </table>
      <div class=mt><button id=saveMulti>Save</button></div>
    </div>
  </div>
</div>

<script>
const $=s=>document.querySelector(s);
const segSel=$('#segSel'), singleBox=$('#singleBox'), multiBox=$('#multiBox'), ledTable=$('#ledTable'), ledHead=$('#ledHead'), msg=$('#msg');

function showMsg(t,ok=true){msg.textContent=t;msg.className=ok?'ok':'err';setTimeout(()=>{msg.textContent='';msg.className='muted';},1800);}
function applyCatTag(el,cat){const x=(cat||'UNKNOWN').toUpperCase();el.textContent=x;el.className='catTag '+x;}

let segments=[], current=null;

// Countdown state
let countdownTimer = null;
let countdownSecs = 0;
function formatHMS(t){
  const s = Math.max(0, Math.floor(t));
  const h = Math.floor(s/3600), m = Math.floor((s%3600)/60), ss = s%60;
  return h>0 ? `${h}:${String(m).padStart(2,'0')}:${String(ss).padStart(2,'0')}`
             : `${m}:${String(ss).padStart(2,'0')}`;
}
function startCountdown(sec){
  if (countdownTimer) { clearInterval(countdownTimer); countdownTimer=null; }
  countdownSecs = Math.max(0, Math.floor(sec||0));
  const tick = () => {
    $('#kvNext').textContent = countdownSecs>0 ? `${formatHMS(countdownSecs)}` : 'now';
    if (countdownSecs<=0) { clearInterval(countdownTimer); countdownTimer=null; return; }
    countdownSecs--;
  };
  tick();
  countdownTimer = setInterval(tick, 1000);
}

async function loadSegments(){
  const j=await fetch('/api/skyaware/segments').then(r=>r.json());
  segments=j.segments||[]; segSel.innerHTML='';
  for(const s of segments){const o=document.createElement('option');o.value=s.index;o.textContent=`${s.index} (len ${s.length})`;segSel.appendChild(o);}
  if(segments.length){segSel.value=segments[0].index;selectSeg();}
}

function getMode(){return document.querySelector('input[name=mode]:checked').value;}
function setMode(m){
  document.querySelector(`input[name=mode][value=${m}]`).checked=true;
  const multi = (m==='MULTIPLE');
  singleBox.classList.toggle('hidden', multi);
  multiBox.classList.toggle('hidden', !multi);
  if (multi && current) {
    const vals=(current.map&&Array.isArray(current.map.leds))?current.map.leds:[];
    buildLedTable(current.length||0, vals, true);
  } else {
    buildLedTable(0, [], false);
  }
}

function selectSeg(){
  const idx=parseInt(segSel.value||0);
  current=segments.find(s=>s.index===idx);
  if(!current){ledTable.innerHTML='';return;}
  const m=(current.map&&current.map.mode)||'SINGLE';
  setMode(m);
  if(m==='SINGLE'){
    $('#singleAirport').value=(current.map&&current.map.airport)||'';
  } else {
    buildLedTable(current.length,(current.map&&current.map.leds)||[],true);
  }
}

function buildLedTable(len,vals,withCat){
  ledHead.innerHTML = withCat
    ? "<tr><th style='width:100px'>LED #</th><th>Airport</th><th style='width:130px'>Cat</th></tr>"
    : "<tr><th style='width:100px'>LED #</th><th>Airport</th></tr>";
  ledTable.innerHTML='';
  for(let i=0;i<len;i++){
    const v=(Array.isArray(vals)&&vals[i])?vals[i]:'-';
    const tr=document.createElement('tr');
    tr.innerHTML= withCat
      ? `<td>${i}</td><td><input type=text data-i='${i}' value='${v}' maxlength=12></td><td class='catCell' data-i='${i}'>-</td>`
      : `<td>${i}</td><td><input type=text data-i='${i}' value='${v}' maxlength=12></td>`;
    ledTable.appendChild(tr);
  }
}

// events
document.addEventListener('change',e=>{if(e.target.name==='mode')setMode(getMode());if(e.target===segSel)selectSeg();});
$('#saveSingle').addEventListener('click',async()=>{
  const seg=segSel.value;
  const ap=($('#singleAirport').value||'').trim().toUpperCase();
  if(!ap){showMsg('Enter airport code',false);return;}
  const body=new URLSearchParams({seg,mode:'SINGLE',airport:ap});
  await fetch('/api/skyaware/map',{method:'POST',headers:{'Content-Type':'application/x-www-form-urlencoded'},body});
  showMsg('Saved.');
  setTimeout(()=>{loadState();loadSegments();},300);
});
$('#saveMulti').addEventListener('click',async()=>{
  const seg=segSel.value;
  const vals=[...ledTable.querySelectorAll('input[data-i]')].map(i=>{let v=(i.value||'-').trim().toUpperCase();if(!v.length)v='-';return v;});
  const body=new URLSearchParams({seg,mode:'MULTIPLE',leds:vals.join(',')});
  await fetch('/api/skyaware/map',{method:'POST',headers:{'Content-Type':'application/x-www-form-urlencoded'},body});
  showMsg('Saved.');
});
$('#btnRefresh').addEventListener('click',async()=>{
  await fetch('/api/skyaware/refresh'); showMsg('Fetch queued.');
  setTimeout(()=>{loadState();},500);
});

// status rendering
function fillSingleTop(s){
  $('#catTop').style.display='';
  applyCatTag($('#catTop'),s.category);
  const m=s.metar||{};
  $('#kvAirport').textContent = s.primary || s.airport || '-';
  $('#kvStation').textContent = m.station||'-';
  $('#kvTimeZ').textContent   = m.timeZ||'-';
  $('#kvWind').textContent    = m.wind||'-';
  $('#kvVis').textContent     = m.vis||'-';
  $('#kvClouds').textContent  = m.clouds||'-';
  $('#kvTempDew').textContent = m.tempDew||'-';
  $('#kvAltim').textContent   = m.altim||'-';
}

function fillMultiTop(s){
  $('#catTop').style.display='none';
  $('#kvAirport').textContent='(multiple)';
  for (const id of ['#kvStation','#kvTimeZ','#kvWind','#kvVis','#kvClouds','#kvTempDew','#kvAltim']) $(id).textContent='—';

  const cats = {};
  (s.airports||[]).forEach(a=>{ if(a && a.id) cats[a.id.toUpperCase()] = (a.good ? a.cat : 'UNKNOWN'); });

  ledTable.querySelectorAll('tr').forEach(tr=>{
    const i = tr.querySelector('input[data-i]');
    const catCell = tr.querySelector('.catCell');
    if (!i || !catCell) return;
    const ap=(i.value||'-').trim().toUpperCase();
    const cat = (ap==='-' ? '' : (cats[ap]||'UNKNOWN'));
    catCell.textContent = cat || '-';
    if (cat) { catCell.className = 'catCell catTag '+cat; } else { catCell.className = 'catCell'; }
  });
}

async function loadState(){
  try{
    const j=await fetch('/api/skyaware/state').then(r=>r.json());
    const s=j.SkyAware||{};
    // diag
    const n=s.net||{};
    $('#kvHttp').textContent=(s.http||'')+(n.ok?' (OK)':'');
    $('#kvDns').textContent=(n.dnsProvider||'-')+(n.dnsFallback?' (fallback)':'');
    $('#kvTimes').textContent=`dns ${n.dnsMs||0}ms • tcp ${n.tcpMs||0}ms • tls ${n.tlsMs||0}ms • http ${n.httpMs||0}ms`;
    $('#kvWifi').textContent=`${(s.ssid||'')} @ ${(s.staIP||'-')} RSSI ${(s.rssi||'')}dBm`;
    $('#kvCounts').textContent=`ok ${s.ok||0} / fail ${s.fail||0} • last ${s.fetchMs||0}ms`;
    $('#kvUrl').textContent=s.url||'-';

    // Countdown: compute using device scheduler timestamps
    const enabled = !!s.enabled;
    if (!enabled) {
      $('#kvNext').textContent = 'disabled';
    } else if (s.pendingFetch) {
      $('#kvNext').textContent = 'queued…';
    } else {
      const nowSec  = Number(s.nowSec||0);
      const nextSec = Number(s.nextAttemptSec||0);
      const delta = Math.max(0, nextSec - nowSec);
      startCountdown(delta);
      if (s.inRetryWindow) $('#kvNext').textContent += (delta>0 ? ' (retry)' : ' (retrying)');
    }

    const mode = (s.mode||'SINGLE').toUpperCase();
    if (mode==='MULTIPLE') fillMultiTop(s); else fillSingleTop(s);

    // If editor is MULTIPLE, re-apply row cats
    const editorMode = getMode();
    if (editorMode==='MULTIPLE' && current) fillMultiTop(s);
  }catch(e){}
}

loadSegments(); loadState();
setInterval(loadState,5000);
</script>
)HTML";
