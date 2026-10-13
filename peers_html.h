// Shared web list of all tally lights, included by AtemTally.ino.
#pragma once
#include <Arduino.h>

const char PEERS_HTML[] PROGMEM = R"HTML(
<h3>Vse lucke</h3><div id=list>Iscem lucke...</div><div id=edit></div>
<script>
const C=['#666','#2a2','#d22'],T=['','PREVIEW','PROGRAM'];
const esc=s=>String(s).replace(/[&<>"]/g,c=>({'&':'&amp;','<':'&lt;','>':'&gt;','"':'&quot;'}[c]));
async function load(){
 try{
  const L=await (await fetch('/api/peers')).json();
  L.sort((a,b)=>a.cam-b.cam||a.name.localeCompare(b.name));
  let h='';
  for(const p of L) h+=`<div class=card><span class=dot style="background:${p.atem?C[p.tally]:'#555'}"></span>`+
   `<b>${esc(p.name)}</b> &nbsp;CAM ${p.cam} ${esc(p.camName)} <b>${p.atem?T[p.tally]:'ATEM ni povezan'}</b>`+
   `<br><small>${p.ip} &middot; ${p.rssi} dBm &middot; FW ${esc(p.fw)}${p.self?' &middot; ta lucka':''}</small> `+
   `<button onclick="edit('${p.ip}')">Uredi</button></div>`;
  document.getElementById('list').innerHTML=h;
 }catch(e){}
}
async function edit(ip){
 const E=document.getElementById('edit');
 E.innerHTML='Nalagam '+ip+' ...';
 try{
  const s=await (await fetch('http://'+ip+'/api/status')).json();
  const opt=(v,l)=>l.map((t,i)=>`<option value=${i}${i==v?' selected':''}>${t}</option>`).join('');
  E.innerHTML=`<div class=card><h3>Uredi: ${esc(s.name)} (${ip})</h3><form id=f>`+
   `Ime<input name=name value="${esc(s.name)}">Kamera<input name=cam type=number min=1 max=64 value=${s.cam}>`+
   `ATEM IP<input name=atem value="${esc(s.atemIp)}">`+
   `Svetlost<select name=bright>${opt(s.bright,['25 %','50 %','75 %','100 %'])}</select>`+
   `Zasuk<select name=rot>${opt(s.rot,['0','90','180','270'])}</select>`+
   `RGB LED<select name=led>${opt(s.led,['izklop','vklop'])}</select>`+
   `<button>Shrani</button> <button type=button onclick="edit('')">Zapri</button></form></div>`;
  document.getElementById('f').onsubmit=async ev=>{
   ev.preventDefault();
   await fetch('http://'+ip+'/api/set',{method:'POST',body:new URLSearchParams(new FormData(ev.target))});
   E.innerHTML='Shranjeno.'; load();
  };
 }catch(e){E.innerHTML=ip?'Lucka '+ip+' se ne odziva.':'';}
}
load(); setInterval(load,3000);
</script>
)HTML";

