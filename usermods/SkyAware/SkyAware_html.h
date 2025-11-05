
#pragma once
#include <pgmspace.h>

static const char SKY_UI_HTML[] PROGMEM = R"HTML(<!DOCTYPE html>
<html lang="en"><meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1">
<title>SkyAware Status</title>
<style>
  :root{--bg:#0f1115;--card:#171a21;--muted:#8a93a6;--text:#e8ecf3;--hr:#242938;--btn:#30364a}
  html,body{background:var(--bg);color:var(--text);font:14px/1.4 system-ui,Segoe UI,Roboto,Helvetica,Arial,sans-serif;margin:0}
  .wrap{max-width:860px;margin:18px auto;padding:0 12px}
  .card{background:var(--card);border-radius:12px;padding:14px 16px;box-shadow:0 1px 0 rgba(255,255,255,.02) inset}
  .row{display:flex;gap:10px;align-items:center;flex-wrap:wrap}
  .pill{padding:4px 10px;border-radius:999px;background:#1e2230;color:#aab3c7;font-weight:600;font-size:12px;border:1px solid #2a3146}
  .btn{background:var(--btn);color:var(--text);border:1px solid #3b425a;border-radius:10px;padding:8px 12px;cursor:pointer}
  .btn:hover{background:#3a4157;border-color:#4a5270}
  .small{font-size:12px;color:var(--muted)}
  input{background:#0c0f16;color:var(--text);border:1px solid #3b425a;border-radius:10px;padding:7px 9px}
</style>
<body>
<div class="wrap">
  <div class="card">
    <div class="row">
      <div class="pill">SkyAware — Status</div>
      <div class="small">Go to <b>WLED → Config → Usermods</b> to edit settings</div>
      <button class="btn" id="btnRefresh">🔄 Query Now</button>
    </div>
    <div style="height:1px;background:var(--hr);margin:12px 0"></div>
    <div class="row">
      <div class="small">Enabled <b id="p_enabled">—</b></div>
      <div class="small">Next update in <b id="p_eta">—</b> s</div>
      <div class="small">Period <b id="p_period">—</b> s</div>
      <div class="small">OK <b id="p_ok">0</b></div>
      <div class="small">FAIL <b id="p_fail">0</b></div>
    </div>
  </div>
  <div class="card" style="margin-top:12px">
    <div class="row">
      <div class="pill">Identify LED</div>
      <input type="number" id="ledIdx" min="0" value="0" style="width:100px">
      <input type="text" id="ledColor" value="00FFFF" style="width:110px" title="RRGGBB hex">
      <input type="number" id="ledMs" min="0" value="30000" style="width:120px" title="Duration ms (0 = latch)">
      <button class="btn" id="btnId">Identify</button>
    </div>
  </div>
</div>
<script>
function q(s){return document.querySelector(s);}
function setEnabled(v){ q('#p_enabled').textContent = v ? 'ON' : 'OFF'; }
async function loadState(){
  try{
    const r=await fetch('/api/skyaware/state'); if(!r.ok) throw 0;
    const j=await r.json();
    setEnabled(!!j.enabled);
    q('#p_period').textContent=j.periodSec??'—';
    q('#p_eta').textContent   =j.nextInSec??'—';
    q('#p_ok').textContent    =j.ok??'0';
    q('#p_fail').textContent  =j.fail??'0';
  }catch(e){}
}
q('#btnRefresh').onclick=async()=>{try{await fetch('/api/skyaware/refresh',{method:'POST'});}catch(e){}}
q('#btnId').onclick=async()=>{
  const idx=+q('#ledIdx').value|0, ms=+q('#ledMs').value|0;
  const hex=(q('#ledColor').value||'00FFFF').replace('#','');
  await fetch(`/api/skyaware/identify?idx=${idx}&ms=${ms}&color=${hex}`);
};
loadState(); setInterval(loadState,1000);
</script>
</body></html>
)HTML";
