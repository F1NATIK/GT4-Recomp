#include "gt3_phone_view.h"
#include "gt3_race_info.h"

#include "external/stb_image_write.h" // raylib builds its implementation (jpg writer included)

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <unordered_set>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
using socket_t = SOCKET;
static void closeSocket(socket_t s) { closesocket(s); }
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <unistd.h>
using socket_t = int;
static constexpr socket_t INVALID_SOCKET = -1;
static void closeSocket(socket_t s) { close(s); }
#endif

namespace phoneview
{
    namespace
    {
        constexpr int kMaxViewers = 4;

        std::atomic<bool> g_running{false};
        std::atomic<int> g_viewers{0};
        std::atomic<int> g_waitingForOne{0};
        socket_t g_listen = INVALID_SOCKET;
        std::thread g_acceptThread, g_encodeThread;

        // Newest cropped picture (encoder input).
        std::mutex g_rawMutex;
        std::condition_variable g_rawReady;
        std::vector<uint8_t> g_raw;
        int g_rawW = 0, g_rawH = 0;
        bool g_rawNew = false;

        // Newest JPEG (viewers' input).
        std::mutex g_jpegMutex;
        std::condition_variable g_jpegReady;
        std::shared_ptr<const std::vector<uint8_t>> g_jpeg;
        uint64_t g_jpegSerial = 0;

        const char kPage[] =
            "<!doctype html><html><head><meta charset=\"utf-8\">"
            "<meta name=\"viewport\" content=\"width=device-width,initial-scale=1\">"
            "<title>GT3 mirror</title><style>"
            "html,body{margin:0;height:100%;background:#000;overflow:hidden}"
            "#m{position:fixed;inset:0;width:100%;height:100%;object-fit:contain;z-index:1}"
            "#w{position:fixed;inset:0;display:flex;align-items:center;justify-content:center;"
            "color:#888;font:16px system-ui,sans-serif;text-align:center;padding:16px}"
            "#t{position:fixed;right:10px;top:10px;z-index:2;color:#ddd;background:#0008;border:1px solid #555;"
            "border-radius:6px;padding:6px 10px;font:14px system-ui,sans-serif;text-decoration:none}"
            "</style></head><body>"
            "<div id=\"w\">Waiting for the rear-view mirror<br>(it shows in a race, in the bumper view).</div>"
            "<img id=\"m\" src=\"/mirror.mjpg\" alt=\"\">"
            "<a id=\"t\" href=\"/timing\">Timing &amp; map</a>"
            "</body></html>";

        // Live timing and track map: polls /race.json (10 per second) and
        // /track.json (when its point count changes).
        const char kTimingPage[] = R"HTML(<!doctype html><html><head><meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1"><title>GT3 timing</title><style>
html,body{margin:0;height:100%;background:#0b0d10;color:#e8e8e8;font:15px system-ui,sans-serif;overflow:hidden}
#wrap{display:flex;height:100%}#map{flex:1 1 55%;min-width:0}#side{flex:1 1 45%;overflow:auto;padding:8px}
@media (orientation:portrait){#wrap{flex-direction:column}#map{flex:1 1 50%}#side{flex:1 1 50%}}
canvas{width:100%;height:100%;display:block}h1{font-size:18px;margin:4px 0 8px}
table{border-collapse:collapse;width:100%;table-layout:fixed;font-size:14px}td,th{padding:5px 4px;text-align:right;white-space:nowrap;overflow:hidden;text-overflow:ellipsis}
th{color:#999;font-weight:500;font-size:12px}td:nth-child(2),th:nth-child(2){text-align:left;width:auto}
th:nth-child(1){width:2.2em}th:nth-child(3){width:2.4em}th:nth-child(4),th:nth-child(5){width:5.2em}th:nth-child(6),th:nth-child(7){width:4.4em}
@media (max-width:560px){table{font-size:12.5px}th:nth-child(5),td:nth-child(5){display:none}}
tr.you td{background:#253a5a;font-weight:600}.muted{color:#888}a{color:#9cf}
</style></head><body><div id="wrap"><div id="map"><canvas id="c"></canvas></div><div id="side">
<h1><span id="clock">--:--.---</span> <span class="muted" id="state">waiting for a race</span> <a style="float:right" href="/">Mirror</a></h1>
<table><thead><tr><th>Pos</th><th>Car</th><th>Lap</th><th>Lap time</th><th>Last lap</th><th>Gap</th><th>Interval</th></tr></thead>
<tbody id="rows"></tbody></table><p class="muted" style="font-size:12px">Gaps: time behind the leader / the car ahead at the same distance.
The map draws itself from where the cars have been.</p></div></div>
<script>
const colors=['#ffd24a','#4ad2ff','#ff6a6a','#7dff7a','#d18cff','#ffa24a','#ff7ad9','#aaaaaa'];
let track=[],trackN=-1,last=null;
function t(ms){if(ms==null||ms<0)return'';const m=Math.floor(ms/60000),s=((ms%60000)/1000).toFixed(3);return m+"'"+(s<10?'0':'')+s;}
function g(ms){if(ms==null||ms<0)return'';return'+'+(ms/1000).toFixed(1)+'s';}
function shortName(c){const n=c.name||('Car '+(c.i+1));return innerWidth<600&&c.name?n.replace(/^\S+\s+/,''):n;}
function esc(s){return String(s).replace(/[&<>"]/g,c=>({'&':'&amp;','<':'&lt;','>':'&gt;','"':'&quot;'}[c]));}
function draw(){const cv=document.getElementById('c'),r=cv.getBoundingClientRect(),dpr=window.devicePixelRatio||1;
cv.width=r.width*dpr;cv.height=r.height*dpr;const x=cv.getContext('2d');x.scale(dpr,dpr);x.clearRect(0,0,r.width,r.height);
const pts=track.slice();if(last)for(const c of last.cars)pts.push([c.x,c.z]);if(!pts.length)return;
let x0=1e9,x1=-1e9,z0=1e9,z1=-1e9;for(const p of pts){x0=Math.min(x0,p[0]);x1=Math.max(x1,p[0]);z0=Math.min(z0,p[1]);z1=Math.max(z1,p[1]);}
const pad=22,s=Math.min((r.width-2*pad)/Math.max(x1-x0,1),(r.height-2*pad)/Math.max(z1-z0,1));
const ox=(r.width-(x1-x0)*s)/2,oz=(r.height-(z1-z0)*s)/2,P=p=>[ox+(p[0]-x0)*s,oz+(p[1]-z0)*s];
x.fillStyle='#3a414c';for(const p of track){const q=P(p);x.fillRect(q[0]-1.5,q[1]-1.5,3,3);}
if(!last)return;for(const c of last.cars.slice().sort((a,b)=>b.pos-a.pos)){const q=P([c.x,c.z]);
x.beginPath();x.arc(q[0],q[1],c.you?8:6,0,7);x.fillStyle=c.you?'#fff':colors[c.i%8];x.fill();
x.fillStyle='#000';x.font='bold 9px system-ui';x.textAlign='center';x.textBaseline='middle';x.fillText(c.pos,q[0],q[1]+0.5);}}
function render(j){last=j.active?j:null;document.getElementById('state').textContent=j.active?'':'waiting for a race';
document.getElementById('clock').textContent=j.active?t(j.clock):'--:--.---';const rows=document.getElementById('rows');
if(!j.active){rows.innerHTML='';draw();return;}
rows.innerHTML=j.cars.slice().sort((a,b)=>a.pos-b.pos).map(c=>'<tr class="'+(c.you?'you':'')+'"><td>'+c.pos+'</td><td><span style="color:'+
(c.you?'#fff':colors[c.i%8])+'">&#9679;</span> '+esc(shortName(c))+(c.you?' <span class="muted">(you)</span>':'')+'</td><td>'+c.lap+'</td><td>'+t(c.cur)+'</td><td>'+(c.last>0?t(c.last):'')+
'</td><td>'+(c.pos==1?'<b style="color:#ffb830">LEAD</b>':g(c.gap))+'</td><td>'+(c.pos==1?'':g(c.int))+'</td></tr>').join('');draw();}
async function poll(){try{const j=await(await fetch('/race.json',{cache:'no-store'})).json();
if(j.track!==trackN){trackN=j.track;track=(await(await fetch('/track.json',{cache:'no-store'})).json()).points;}render(j);}catch(e){}
setTimeout(poll,100);}window.addEventListener('resize',draw);poll();
</script></body></html>)HTML";

        // The dashboard (/): the panels the launcher's Phone page chose, from
        // /layout.json (re-read every 1.5 s, so changes show while playing),
        // with /race.json 12 times a second.
        const char kDashPage[] = R"HTML(<!doctype html><html><head><meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1,viewport-fit=cover"><title>GT3</title><style>
html,body{margin:0;height:100%;background:#0b0d10;color:#e8e8e8;font:15px system-ui,sans-serif;overflow:hidden}
#dash{display:flex;height:100%;gap:6px;padding:6px;box-sizing:border-box}
.col{display:flex;flex-direction:column;gap:6px;flex:1 1 0;min-width:0;min-height:0}
.panel{position:relative;background:#14181e;border-radius:8px;overflow:hidden;min-height:0}
.panel canvas{position:absolute;inset:0;width:100%;height:100%}.panel img{position:absolute;inset:0;width:100%;height:100%;object-fit:contain}
.wait{position:absolute;inset:0;display:flex;align-items:center;justify-content:center;color:#666;font-size:13px;text-align:center;padding:8px}
.tbl{position:absolute;inset:0;overflow:auto;padding:4px 6px}
table{border-collapse:collapse;width:100%;table-layout:fixed;font-size:14px}td,th{padding:4px 3px;text-align:right;white-space:nowrap;overflow:hidden;text-overflow:ellipsis}
th{color:#999;font-weight:500;font-size:12px}td.c,th.c{text-align:left;width:auto}
th.p{width:2em}th.l{width:2.3em}th.t{width:5.2em}th.g{width:4.2em}table.narrow .w{display:none}table.narrow{font-size:13px}
.panel.mirror{flex:0 0 auto;align-self:center;aspect-ratio:3.6}
tr.you td{background:#253a5a;font-weight:600}.muted{color:#888}
.pos{position:absolute;inset:0;display:flex;flex-direction:column;align-items:center;justify-content:center;text-align:center}
.big{font-weight:800;line-height:1;color:#ffb830}.lab{position:absolute;left:8px;top:5px;font-size:11px;color:#667;letter-spacing:.05em;text-transform:uppercase}
</style></head><body><div id="dash"></div><script>
const NAMES=['mirror','position','timing','map','radar'],LABELS=['Mirror','Position','Timing','Map','Radar'];
const colors=['#ffd24a','#4ad2ff','#ff6a6a','#7dff7a','#d18cff','#ffa24a','#ff7ad9','#aaaaaa'];
let layout=null,layoutV=-1,cols=0,last=null,track=[],edges=[],trackN=-1,trackAt=0,best={},race=0,mapKey='';const mapLayer=document.createElement('canvas');
function t(ms){if(ms==null||ms<0)return'';const m=Math.floor(ms/60000),s=((ms%60000)/1000).toFixed(3);return m+"'"+(s<10?'0':'')+s;}
function g(ms){if(ms==null||ms<0)return'';return'+'+(ms/1000).toFixed(1)+'s';}
function esc(s){return String(s).replace(/[&<>"]/g,c=>({'&':'&amp;','<':'&lt;','>':'&gt;','"':'&quot;'}[c]));}
function shortName(c,w){const n=c.name||('Car '+(c.i+1));return w<420&&c.name?n.replace(/^\S+\s+/,''):n;}
function ord(n){const s=['th','st','nd','rd'],v=n%100;return n+(s[(v-20)%10]||s[v]||s[0]);}
function panelHtml(p){switch(p){
case 0:return'<div class="wait">Rear-view mirror<br>(in races, bumper view)</div><img src="/mirror.mjpg" alt="">';
case 1:return'<div class="pos"><div id="posBig" class="big">-</div><div id="posOf" class="muted"></div><div id="posLap" style="margin-top:.4em"></div><div id="posTimes" class="muted" style="font-size:.85em"></div></div>';
case 2:return'<div class="tbl"><table><thead><tr><th class="p">Pos</th><th class="c">Car</th><th class="l">Lap</th><th class="t">Lap time</th><th class="t w">Last lap</th><th class="g">Gap</th><th class="g w">Int.</th></tr></thead><tbody id="rows"></tbody></table></div>';
case 3:return'<canvas id="cmap"></canvas>';case 4:return'<canvas id="crad"></canvas>';}return'';}
function build(){const d=document.getElementById('dash');d.innerHTML='';
const list=layout.order.filter(p=>layout.size[p]>0);cols=layout.arrange==1?1:layout.arrange==2?2:(innerWidth>innerHeight?2:1);if(list.length<2)cols=1;
const ce=[];for(let i=0;i<cols;i++){const c=document.createElement('div');c.className='col';d.appendChild(c);ce.push(c);}
const total=list.reduce((a,p)=>a+layout.size[p],0);let acc=0;
for(const p of list){const ci=(cols==2&&acc>=total/2-0.01)?1:0;acc+=layout.size[p];const el=document.createElement('div');el.className='panel';
if(p==0){el.classList.add('mirror');el.style.width=[0,60,80,100][layout.size[p]]+'%';}else el.style.flex=layout.size[p]+' 1 0';
el.innerHTML=panelHtml(p)+(p>=3?'<div class="lab">'+LABELS[p]+'</div>':'');ce[ci].appendChild(el);}
if(cols==2&&!ce[1].children.length&&ce[0].children.length>1)ce[1].appendChild(ce[0].lastChild);
if(!list.length)d.innerHTML='<div class="wait">No panels chosen: tick some on the launcher\'s Phone page.</div>';render(last);}
function canvasCtx(id){const cv=document.getElementById(id);if(!cv)return null;const r=cv.getBoundingClientRect(),dpr=window.devicePixelRatio||1;
cv.width=Math.max(1,r.width*dpr);cv.height=Math.max(1,r.height*dpr);const x=cv.getContext('2d');x.scale(dpr,dpr);x.clearRect(0,0,r.width,r.height);return[x,r.width,r.height];}
function drawMap(){const c=canvasCtx('cmap');if(!c)return;const[x,W,H]=c;
const drawn=edges.length||layout.fallback!==0;if(!drawn){x.fillStyle='#667';x.font='12px system-ui';x.textAlign='center';x.fillText(last?'Waiting for the course map':'',W/2,H/2);}const pts=edges.length?[].concat(...edges):(drawn?track.slice():[]);if(last&&!edges.length)for(const k of last.cars)pts.push([k.x,k.z]);if(!pts.length)return;
let x0=1e9,x1=-1e9,z0=1e9,z1=-1e9;for(const p of pts){x0=Math.min(x0,p[0]);x1=Math.max(x1,p[0]);z0=Math.min(z0,p[1]);z1=Math.max(z1,p[1]);}
const pad=16,s=Math.min((W-2*pad)/Math.max(x1-x0,1),(H-2*pad)/Math.max(z1-z0,1));
const ox=(W-(x1-x0)*s)/2,oz=(H-(z1-z0)*s)/2,P=p=>[ox+(p[0]-x0)*s,oz+(p[1]-z0)*s];
const key=[drawn?track.length:-1,edges.length,W,H,x0,x1,z0,z1].join(),dpr=window.devicePixelRatio||1;
if(mapKey!==key){mapKey=key;mapLayer.width=Math.max(1,W*dpr);mapLayer.height=Math.max(1,H*dpr);const m=mapLayer.getContext('2d');m.scale(dpr,dpr);
if(edges.length){m.beginPath();for(const e of edges){e.forEach((p,i)=>{const q=P(p);i?m.lineTo(q[0],q[1]):m.moveTo(q[0],q[1]);});m.closePath();}m.fillStyle='#2e3541';m.fill('evenodd');m.strokeStyle='#58627a';m.lineWidth=1.2;m.stroke();}else if(drawn){m.fillStyle='#3a414c';for(const p of track){const q=P(p);m.fillRect(q[0]-1.5,q[1]-1.5,3,3);}}}
x.drawImage(mapLayer,0,0,W,H);
if(!last)return;for(const k of last.cars.slice().sort((a,b)=>b.pos-a.pos)){const q=P([k.x,k.z]);
x.beginPath();x.arc(q[0],q[1],k.you?8:6,0,7);x.fillStyle=k.you?'#fff':colors[k.i%8];x.fill();
x.fillStyle='#000';x.font='bold 9px system-ui';x.textAlign='center';x.textBaseline='middle';x.fillText(k.pos,q[0],q[1]+0.5);}}
function drawRadar(){const c=canvasCtx('crad');if(!c)return;const[x,W,H]=c;const cx=W/2,cy=H/2,range=layout.radar||30,k=Math.min(W,H)/2/range;
x.strokeStyle='#2a3038';x.lineWidth=1;const step=range<=40?10:(range<=80?20:25);for(let m=step;m<range;m+=step){x.beginPath();x.arc(cx,cy,m*k,0,7);x.stroke();}
x.beginPath();x.moveTo(cx,0);x.lineTo(cx,H);x.moveTo(0,cy);x.lineTo(W,cy);x.stroke();
const cw=Math.max(5,1.8*k),cl=Math.max(10,4.4*k);x.fillStyle='#78dcff';x.fillRect(cx-cw/2,cy-cl/2,cw,cl);
if(!last)return;const me=last.cars.find(q=>q.you);if(!me)return;
const fx=-Math.sin(me.yaw),fz=-Math.cos(me.yaw),rx=Math.cos(me.yaw),rz=-Math.sin(me.yaw);
for(const q of last.cars){if(q.you)continue;const dx=q.x-me.x,dz=q.z-me.z,al=dx*fx+dz*fz,sd=dx*rx+dz*rz;
if(Math.abs(al)>range*1.5||Math.abs(sd)>range*1.5)continue;const ofx=-Math.sin(q.yaw),ofz=-Math.cos(q.yaw),hd=Math.atan2(ofx*rx+ofz*rz,ofx*fx+ofz*fz);
const near=Math.abs(al)<4.9&&Math.abs(sd)<4,px=cx+sd*k,py=cy-al*k;x.save();x.translate(px,py);x.rotate(hd);
x.fillStyle=near?'#ff8c00':colors[q.i%8];x.fillRect(-cw/2,-cl/2,cw,cl);x.restore();
x.fillStyle='#000';x.font='bold '+Math.round(Math.max(9,Math.min(cw*0.9,16)))+'px system-ui';x.textAlign='center';x.textBaseline='middle';x.fillText(q.pos,px,py+0.5);}
x.fillStyle='#667';x.font='11px system-ui';x.textAlign='right';x.textBaseline='bottom';x.fillText(range+' m',W-6,H-4);}
function render(j){const on=j&&j.active;const rows=document.getElementById('rows');
const mi=document.querySelector('.mirror img');if(mi&&mi.naturalWidth>0)mi.parentElement.style.aspectRatio=mi.naturalWidth/mi.naturalHeight;
if(rows){const tb=rows.parentElement,w=tb.getBoundingClientRect().width;tb.classList.toggle('narrow',w<520);rows.innerHTML=!on?'':j.cars.slice().sort((a,b)=>a.pos-b.pos).map(c=>'<tr class="'+(c.you?'you':'')+'"><td>'+c.pos+'</td><td class="c"><span style="color:'+
(c.you?'#fff':colors[c.i%8])+'">&#9679;</span> '+esc(shortName(c,w))+'</td><td>'+c.lap+'</td><td>'+t(c.cur)+'</td><td class="w">'+(c.last>0?t(c.last):'')+
'</td><td>'+(c.pos==1?'<b style="color:#ffb830">LEAD</b>':g(c.gap))+'</td><td class="w">'+(c.pos==1?'':g(c.int))+'</td></tr>').join('');}
const pb=document.getElementById('posBig');if(pb){const box=pb.parentElement.getBoundingClientRect();pb.parentElement.style.fontSize=Math.max(12,Math.min(box.height*0.1,box.width*0.08))+'px';
pb.style.fontSize=Math.max(24,Math.min(box.height*0.42,box.width*0.3))+'px';const me=on?j.cars.find(c=>c.you):null;
pb.textContent=me?ord(me.pos):'-';document.getElementById('posOf').textContent=on?'of '+j.cars.length:'waiting for a race';
document.getElementById('posLap').textContent=me?'Lap '+me.lap+(j.laps>0?'/'+j.laps:'')+'  '+t(me.cur):'';
document.getElementById('posTimes').textContent=me?('Last '+(me.last>0?t(me.last):'-')+'   Best '+(best[me.i]?t(best[me.i]):'-')):'';}
drawMap();drawRadar();}
async function poll(){try{const j=await(await fetch('/race.json',{cache:'no-store'})).json();
if(j.active){if(j.clock<race)best={};race=j.clock;for(const c of j.cars)if(c.last>0&&(!best[c.i]||c.last<best[c.i]))best[c.i]=c.last;}
if(j.track!==trackN&&(j.track<trackN||performance.now()-trackAt>2000)){trackN=j.track;trackAt=performance.now();const tj=await(await fetch('/track.json',{cache:'no-store'})).json();track=tj.points;edges=(tj.edges||[]).map(e=>{const a=[];for(let i=0;i+1<e.length;i+=2)a.push([e[i],e[i+1]]);return a;});}last=j.active?j:null;if(layout)render(last);}catch(e){}
setTimeout(poll,80);}
async function pollLayout(){try{const l=await(await fetch('/layout.json',{cache:'no-store'})).json();if(l.v!==layoutV){layoutV=l.v;layout=l;build();}}catch(e){}setTimeout(pollLayout,1500);}
window.addEventListener('resize',()=>{if(!layout)return;const want=layout.arrange==1?1:layout.arrange==2?2:(innerWidth>innerHeight?2:1);if(want!==cols)build();else render(last);});
pollLayout();poll();
</script></body></html>)HTML";

        // The dashboard's layout (gt3_phone_view.h), and its version for the page.
        std::mutex g_layoutMutex;
        int g_order[5] = {0, 1, 2, 3, 4}, g_sizes[5] = {3, 1, 2, 2, 2}, g_arrange = 0, g_radarRange = 30, g_mapFallback = 1;
        uint32_t g_layoutVersion = 1;

        // order: 5 digits (panel + 1, each once); sizes: 5 digits (size + 1).
        bool parseOrder(int value, int out[5])
        {
            bool seen[5] = {};
            for (int i = 4; i >= 0; --i)
            {
                const int d = value % 10 - 1;
                value /= 10;
                if (d < 0 || d > 4 || seen[d])
                    return false;
                seen[d] = true;
                out[i] = d;
            }
            return value == 0;
        }
        bool parseSizes(int value, int out[5])
        {
            for (int i = 4; i >= 0; --i)
            {
                const int d = value % 10 - 1;
                value /= 10;
                if (d < 0 || d > 3)
                    return false;
                out[i] = d;
            }
            return value == 0;
        }
        void setLayout(int order, int sizes, int arrange, int radarRange, int mapFallback)
        {
            std::lock_guard<std::mutex> lock(g_layoutMutex);
            int o[5], s[5];
            bool changed = false;
            if (order >= 0 && parseOrder(order, o) && std::memcmp(o, g_order, sizeof(o)) != 0)
            {
                std::memcpy(g_order, o, sizeof(o));
                changed = true;
            }
            if (sizes >= 0 && parseSizes(sizes, s) && std::memcmp(s, g_sizes, sizeof(s)) != 0)
            {
                std::memcpy(g_sizes, s, sizeof(s));
                changed = true;
            }
            if (arrange >= 0 && arrange <= 2 && arrange != g_arrange)
            {
                g_arrange = arrange;
                changed = true;
            }
            if (radarRange >= 10 && radarRange <= 200 && radarRange != g_radarRange)
            {
                g_radarRange = radarRange;
                changed = true;
            }
            if ((mapFallback == 0 || mapFallback == 1) && mapFallback != g_mapFallback)
            {
                g_mapFallback = mapFallback;
                changed = true;
            }
            if (changed)
                ++g_layoutVersion;
        }
        int envInt(const char *name)
        {
            const char *v = std::getenv(name);
            return (v && *v) ? std::atoi(v) : -1;
        }
        std::string layoutJson()
        {
            std::lock_guard<std::mutex> lock(g_layoutMutex);
            char buf[200];
            std::snprintf(buf, sizeof(buf),
                          "{\"v\":%u,\"order\":[%d,%d,%d,%d,%d],\"size\":[%d,%d,%d,%d,%d],\"arrange\":%d,\"radar\":%d,\"fallback\":%d}",
                          g_layoutVersion, g_order[0], g_order[1], g_order[2], g_order[3], g_order[4], g_sizes[0], g_sizes[1], g_sizes[2],
                          g_sizes[3], g_sizes[4], g_arrange, g_radarRange, g_mapFallback);
            return buf;
        }

        // Race sampling (20 per second, for the phone's radar): the track drawn from the cars' own
        // paths (one point per 4 m cell) and each car's distance / time
        // trace, for time gaps at equal distance.
        const uint8_t *g_rdram = nullptr;
        std::thread g_sampleThread;
        std::mutex g_raceMutex;
        gt3race::Snapshot g_snapshot;
        bool g_snapshotValid = false;
        std::vector<std::pair<float, float>> g_track;
        std::unordered_set<uint64_t> g_trackCells;
        // The course's two edges from GT3's own data (gt3race::findCourseEdges),
        // searched for every 2 s of a race until found (at most 30 times):
        // the phone's map shows the whole track from the start.
        std::vector<std::vector<float>> g_edges;
        uint32_t g_edgeSerial = 0;
        int g_edgeTries = 0; // sampler thread only; -1 found for this race
        std::chrono::steady_clock::time_point g_edgeNext{};
        struct TracePoint
        {
            int lap;
            float progress;
            uint32_t clock;
        };
        std::vector<std::vector<TracePoint>> g_traces; // per car, in time order
        uint32_t g_trackRace = 0, g_lastClock = 0;
        float g_lapLength = 1.0f; // the largest progress seen (a lap, once any car has done one)

        // Race distance: laps done times the lap plus the progress into this
        // one (on the grid, lap 0, just below zero).
        double raceDistance(int lap, float progress) { return (lap - 1) * static_cast<double>(g_lapLength) + progress; }

        // Clock time when car `k` first reached race distance `distance` (-1: not yet).
        double timeAt(size_t k, double distance)
        {
            if (k >= g_traces.size())
                return -1.0;
            const auto &tr = g_traces[k];
            for (size_t i = 0; i < tr.size(); ++i)
            {
                const double d = raceDistance(tr[i].lap, tr[i].progress);
                if (d < distance)
                    continue;
                if (i == 0)
                    return tr[0].clock;
                const double d0 = raceDistance(tr[i - 1].lap, tr[i - 1].progress);
                const double span = d - d0;
                return span > 0.0 ? tr[i - 1].clock + (tr[i].clock - static_cast<double>(tr[i - 1].clock)) * (distance - d0) / span : tr[i].clock;
            }
            return -1.0;
        }

        void sampleLoop()
        {
            while (g_running.load())
            {
                gt3race::Snapshot s;
                const bool ok = g_rdram && gt3race::read(g_rdram, s);
                std::vector<std::vector<float>> edges;
                bool edgesFound = false;
                if (ok)
                {
                    if (s.race != g_trackRace || s.clockMs + 1000u < g_lastClock)
                    {
                        g_edgeTries = 0;
                        g_edgeNext = {};
                    }
                    const auto now = std::chrono::steady_clock::now();
                    if (g_edgeTries >= 0 && g_edgeTries < 30 && now >= g_edgeNext)
                    {
                        ++g_edgeTries;
                        g_edgeNext = now + std::chrono::seconds(2);
                        const auto t0 = std::chrono::steady_clock::now();
                        edgesFound = gt3race::findCourseEdges(g_rdram, s, edges);
                        if (edgesFound)
                        {
                            std::cerr << "[phone-view] course edges from GT3's data: " << edges[0].size() / 2 << " and " << edges[1].size() / 2
                                      << " points (search "
                                      << std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0).count()
                                      << " ms, try " << g_edgeTries << ")" << std::endl;
                            g_edgeTries = -1;
                        }
                    }
                }
                {
                    std::lock_guard<std::mutex> lock(g_raceMutex);
                    g_snapshotValid = ok;
                    if (ok)
                    {
                        if (s.race != g_trackRace || s.clockMs + 1000u < g_lastClock)
                        {
                            // A new race (or a restart): new map and traces.
                            g_track.clear();
                            g_trackCells.clear();
                            g_traces.clear();
                            g_trackRace = s.race;
                            g_lapLength = 1.0f;
                            if (!g_edges.empty())
                            {
                                g_edges.clear();
                                ++g_edgeSerial;
                            }
                        }
                        if (edgesFound)
                        {
                            g_edges = std::move(edges);
                            ++g_edgeSerial;
                        }
                        for (const gt3race::Car &c : s.cars)
                            if (std::isfinite(c.progress) && c.progress < 1e5f)
                                g_lapLength = std::max(g_lapLength, c.progress + 1.0f);
                        g_lastClock = s.clockMs;
                        g_traces.resize(s.cars.size());
                        for (const gt3race::Car &c : s.cars)
                        {
                            if (!std::isfinite(c.x) || !std::isfinite(c.z) || std::fabs(c.x) > 1e5f || std::fabs(c.z) > 1e5f)
                                continue;
                            const uint64_t cell = (static_cast<uint64_t>(static_cast<uint32_t>(static_cast<int32_t>(std::floor(c.x / 4.0f)))) << 32) |
                                                  static_cast<uint32_t>(static_cast<int32_t>(std::floor(c.z / 4.0f)));
                            if (g_track.size() < 20000u && g_trackCells.insert(cell).second)
                                g_track.emplace_back(c.x, c.z);
                            auto &tr = g_traces[static_cast<size_t>(c.index)];
                            if (tr.empty() || raceDistance(c.lap, c.progress) > raceDistance(tr.back().lap, tr.back().progress) + 0.5)
                                tr.push_back(TracePoint{c.lap, c.progress, s.clockMs});
                        }
                        g_snapshot = std::move(s);
                    }
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(50));
            }
        }

        // What /track.json holds, for the pages to re-fetch on a change: the
        // driven points' count, or (edges found) a serial.
        size_t trackVersion() { return g_edges.empty() ? g_track.size() : 1000000u + g_edgeSerial; }

        std::string raceJson()
        {
            std::lock_guard<std::mutex> lock(g_raceMutex);
            if (!g_snapshotValid)
                return "{\"active\":false,\"track\":" + std::to_string(trackVersion()) + "}";
            const gt3race::Snapshot &s = g_snapshot;
            const gt3race::Car *leader = nullptr;
            for (const gt3race::Car &c : s.cars)
                if (!leader || c.position < leader->position)
                    leader = &c;
            std::string out = "{\"active\":true,\"clock\":" + std::to_string(s.clockMs) + ",\"laps\":" + std::to_string(s.totalLaps) +
                              ",\"track\":" + std::to_string(trackVersion()) + ",\"cars\":[";
            char buf[400];
            for (size_t i = 0; i < s.cars.size(); ++i)
            {
                const gt3race::Car &c = s.cars[i];
                // Gap: now minus when the leader (interval: the car one place
                // ahead) was at this car's distance.
                const gt3race::Car *ahead = nullptr;
                for (const gt3race::Car &o : s.cars)
                    if (o.position == c.position - 1)
                        ahead = &o;
                const double distance = raceDistance(c.lap, c.progress);
                const double atLeader = leader ? timeAt(static_cast<size_t>(leader->index), distance) : -1.0;
                const double atAhead = ahead ? timeAt(static_cast<size_t>(ahead->index), distance) : -1.0;
                const long long gap = atLeader >= 0.0 ? static_cast<long long>(s.clockMs - atLeader) : -1;
                const long long interval = atAhead >= 0.0 ? static_cast<long long>(s.clockMs - atAhead) : -1;
                const long long current = s.clockMs >= c.lapStartMs ? static_cast<long long>(s.clockMs - c.lapStartMs) : -1;
                const long long lastLap = c.lastLapMs > 0u && c.lastLapMs < 359999999u ? static_cast<long long>(c.lastLapMs) : -1;
                std::snprintf(buf, sizeof(buf),
                              "%s{\"i\":%d,\"you\":%d,\"pos\":%d,\"lap\":%d,\"cur\":%lld,\"last\":%lld,\"x\":%.2f,\"z\":%.2f,\"yaw\":%.4f,\"d\":%.1f,\"gap\":%lld,\"int\":%lld,\"name\":\"",
                              i ? "," : "", c.index, c.player ? 1 : 0, c.position, std::max(c.lap, 1), current, lastLap, c.x, c.z, std::isfinite(c.yaw) ? c.yaw : 0.0f, distance, gap,
                              interval);
                out += buf;
                for (const char ch : c.name)
                    if (ch == '"' || ch == '\\')
                        out += '\\', out += ch;
                    else if (ch >= 0x20 && ch < 0x7f)
                        out += ch;
                out += "\"}";
            }
            return out + "]}";
        }

        std::string trackJson()
        {
            std::lock_guard<std::mutex> lock(g_raceMutex);
            std::string out = "{\"points\":[";
            char buf[48];
            for (size_t i = 0; i < g_track.size(); ++i)
            {
                std::snprintf(buf, sizeof(buf), "%s[%.1f,%.1f]", i ? "," : "", g_track[i].first, g_track[i].second);
                out += buf;
            }
            out += "],\"edges\":[";
            for (size_t e = 0; e < g_edges.size(); ++e)
            {
                out += e ? ",[" : "[";
                for (size_t i = 0; i < g_edges[e].size(); ++i)
                {
                    std::snprintf(buf, sizeof(buf), "%s%.1f", i ? "," : "", g_edges[e][i]);
                    out += buf;
                }
                out += "]";
            }
            return out + "]}";
        }

        bool sendAll(socket_t s, const char *data, size_t size)
        {
            while (size > 0)
            {
                const int n = send(s, data, static_cast<int>(std::min<size_t>(size, 1 << 20)), 0);
                if (n <= 0)
                    return false;
                data += n;
                size -= static_cast<size_t>(n);
            }
            return true;
        }

        bool sendText(socket_t s, const std::string &text) { return sendAll(s, text.data(), text.size()); }

        // The newest JPEG after `serial`, waiting up to `ms`.
        std::shared_ptr<const std::vector<uint8_t>> nextJpeg(uint64_t &serial, int ms)
        {
            std::unique_lock<std::mutex> lock(g_jpegMutex);
            g_jpegReady.wait_for(lock, std::chrono::milliseconds(ms), [&] { return g_jpegSerial != serial || !g_running.load(); });
            if (g_jpegSerial == serial || !g_jpeg)
                return nullptr;
            serial = g_jpegSerial;
            return g_jpeg;
        }

        void serveStream(socket_t s)
        {
            if (!sendText(s, "HTTP/1.1 200 OK\r\nContent-Type: multipart/x-mixed-replace; boundary=gt3frame\r\n"
                             "Cache-Control: no-cache, no-store\r\nConnection: close\r\n\r\n"))
                return;
            ++g_viewers;
            uint64_t serial = 0;
            while (g_running.load())
            {
                const auto jpeg = nextJpeg(serial, 1000);
                if (!jpeg)
                    continue;
                const std::string head = "--gt3frame\r\nContent-Type: image/jpeg\r\nContent-Length: " + std::to_string(jpeg->size()) + "\r\n\r\n";
                if (!sendText(s, head) || !sendAll(s, reinterpret_cast<const char *>(jpeg->data()), jpeg->size()) || !sendText(s, "\r\n"))
                    break;
            }
            --g_viewers;
        }

        void serveOne(socket_t s)
        {
            ++g_waitingForOne;
            uint64_t serial;
            {
                std::lock_guard<std::mutex> lock(g_jpegMutex);
                serial = g_jpegSerial;
            }
            auto jpeg = nextJpeg(serial, 1500);
            --g_waitingForOne;
            if (!jpeg)
            {
                std::lock_guard<std::mutex> lock(g_jpegMutex);
                jpeg = g_jpeg; // an older picture, if any
            }
            if (!jpeg)
            {
                sendText(s, "HTTP/1.1 503 Service Unavailable\r\nContent-Type: text/plain\r\nConnection: close\r\n\r\nNo mirror picture yet.\n");
                return;
            }
            if (sendText(s, "HTTP/1.1 200 OK\r\nContent-Type: image/jpeg\r\nCache-Control: no-cache\r\nContent-Length: " +
                                std::to_string(jpeg->size()) + "\r\nConnection: close\r\n\r\n"))
                sendAll(s, reinterpret_cast<const char *>(jpeg->data()), jpeg->size());
        }

        void serveClient(socket_t s)
        {
            // The request line: "GET <path> HTTP/1.x" (headers read and ignored).
            std::string request;
            char buf[1024];
            while (request.find("\r\n\r\n") == std::string::npos && request.size() < 8192)
            {
                const int n = recv(s, buf, sizeof(buf), 0);
                if (n <= 0)
                    break;
                request.append(buf, static_cast<size_t>(n));
            }
            std::string path;
            if (request.compare(0, 4, "GET ") == 0)
                path = request.substr(4, request.find(' ', 4) - 4);
            const size_t query = path.find('?');
            if (query != std::string::npos)
                path.resize(query);
            if (path == "/" || path == "/index.html")
                sendText(s, "HTTP/1.1 200 OK\r\nContent-Type: text/html; charset=utf-8\r\nCache-Control: no-cache\r\nContent-Length: " +
                                std::to_string(sizeof(kDashPage) - 1) + "\r\nConnection: close\r\n\r\n" + kDashPage);
            else if (path == "/mirror")
                sendText(s, "HTTP/1.1 200 OK\r\nContent-Type: text/html; charset=utf-8\r\nCache-Control: no-cache\r\nContent-Length: " +
                                std::to_string(sizeof(kPage) - 1) + "\r\nConnection: close\r\n\r\n" + kPage);
            else if (path == "/timing")
                sendText(s, "HTTP/1.1 200 OK\r\nContent-Type: text/html; charset=utf-8\r\nCache-Control: no-cache\r\nContent-Length: " +
                                std::to_string(sizeof(kTimingPage) - 1) + "\r\nConnection: close\r\n\r\n" + kTimingPage);
            else if (path == "/race.json" || path == "/track.json" || path == "/layout.json")
            {
                const std::string body = path == "/race.json" ? raceJson() : (path == "/track.json" ? trackJson() : layoutJson());
                sendText(s, "HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nCache-Control: no-cache, no-store\r\nContent-Length: " +
                                std::to_string(body.size()) + "\r\nConnection: close\r\n\r\n" + body);
            }
            else if (path == "/mirror.mjpg")
                serveStream(s);
            else if (path == "/mirror.jpg")
                serveOne(s);
            else
                sendText(s, "HTTP/1.1 404 Not Found\r\nContent-Type: text/plain\r\nConnection: close\r\n\r\nNot found.\n");
            closeSocket(s);
        }

        void acceptLoop()
        {
            static std::atomic<int> clients{0};
            while (g_running.load())
            {
                sockaddr_in from{};
#if defined(_WIN32)
                int len = sizeof(from);
#else
                socklen_t len = sizeof(from);
#endif
                const socket_t s = accept(g_listen, reinterpret_cast<sockaddr *>(&from), &len);
                if (s == INVALID_SOCKET)
                {
                    if (!g_running.load())
                        break;
                    std::this_thread::sleep_for(std::chrono::milliseconds(50));
                    continue;
                }
                if (clients.load() >= kMaxViewers + 2)
                {
                    closeSocket(s);
                    continue;
                }
                const int one = 1;
                setsockopt(s, IPPROTO_TCP, TCP_NODELAY, reinterpret_cast<const char *>(&one), sizeof(one));
#if defined(_WIN32)
                const DWORD timeout = 5000;
#else
                const timeval timeout{5, 0};
#endif
                setsockopt(s, SOL_SOCKET, SO_SNDTIMEO, reinterpret_cast<const char *>(&timeout), sizeof(timeout));
                setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char *>(&timeout), sizeof(timeout));
                ++clients;
                std::thread([s] {
                    serveClient(s);
                    --clients;
                }).detach();
            }
        }

        void appendBytes(void *context, void *data, int size)
        {
            auto *out = static_cast<std::vector<uint8_t> *>(context);
            out->insert(out->end(), static_cast<const uint8_t *>(data), static_cast<const uint8_t *>(data) + size);
        }

        void encodeLoop()
        {
            static const int quality = [] {
                const char *v = std::getenv("GT3_PHONE_VIEW_QUALITY");
                // 92: above 90 stb keeps full-resolution colour (no 4:2:0).
                return v && *v ? std::clamp(std::atoi(v), 10, 100) : 92;
            }();
            std::vector<uint8_t> raw;
            while (g_running.load())
            {
                int w = 0, h = 0;
                {
                    std::unique_lock<std::mutex> lock(g_rawMutex);
                    g_rawReady.wait(lock, [] { return g_rawNew || !g_running.load(); });
                    if (!g_running.load())
                        break;
                    raw.swap(g_raw);
                    w = g_rawW;
                    h = g_rawH;
                    g_rawNew = false;
                }
                auto jpeg = std::make_shared<std::vector<uint8_t>>();
                jpeg->reserve(static_cast<size_t>(w) * h / 4);
                if (!stbi_write_jpg_to_func(&appendBytes, jpeg.get(), w, h, 4, raw.data(), quality))
                    continue;
                {
                    std::lock_guard<std::mutex> lock(g_jpegMutex);
                    g_jpeg = std::move(jpeg);
                    ++g_jpegSerial;
                }
                g_jpegReady.notify_all();
            }
        }
    }

    bool start(const uint8_t *rdram)
    {
        g_rdram = rdram;
        const char *v = std::getenv("GT3_PHONE_VIEW");
        if (!v || !*v || std::strcmp(v, "0") == 0 || g_running.load())
            return g_running.load();
        int port = std::atoi(v);
        if (port == 1)
            port = 8093;
        if (port < 1024 || port > 65535)
        {
            std::cerr << "[phone-view] GT3_PHONE_VIEW=" << v << ": expected 1 or a port 1024-65535" << std::endl;
            return false;
        }
#if defined(_WIN32)
        WSADATA wsa;
        if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0)
            return false;
#endif
        g_listen = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        if (g_listen == INVALID_SOCKET)
            return false;
        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_addr.s_addr = htonl(INADDR_ANY);
        addr.sin_port = htons(static_cast<uint16_t>(port));
        if (bind(g_listen, reinterpret_cast<const sockaddr *>(&addr), sizeof(addr)) != 0 || listen(g_listen, 8) != 0)
        {
            std::cerr << "[phone-view] cannot listen on port " << port << " (in use?)" << std::endl;
            closeSocket(g_listen);
            g_listen = INVALID_SOCKET;
            return false;
        }
        setLayout(envInt("GT3_PHONE_ORDER"), envInt("GT3_PHONE_SIZES"), envInt("GT3_PHONE_ARRANGE"), envInt("GT3_PHONE_RADAR_RANGE"),
                  envInt("GT3_PHONE_MAP_FALLBACK"));
        g_running.store(true);
        g_acceptThread = std::thread(acceptLoop);
        g_encodeThread = std::thread(encodeLoop);
        g_sampleThread = std::thread(sampleLoop);
        std::cerr << "[phone-view] dashboard at http://<this PC's address>:" << port
                  << "/ (panels from the launcher's Phone page); mirror alone at /mirror (MJPEG /mirror.mjpg), timing and map at /timing" << std::endl;
        return true;
    }

    void stop()
    {
        if (!g_running.exchange(false))
            return;
        closeSocket(g_listen);
        g_listen = INVALID_SOCKET;
        g_rawReady.notify_all();
        g_jpegReady.notify_all();
        if (g_acceptThread.joinable())
            g_acceptThread.join();
        if (g_encodeThread.joinable())
            g_encodeThread.join();
        if (g_sampleThread.joinable())
            g_sampleThread.join();
    }

    void applyLive(int order, int sizes, int arrange, int radarRange, int mapFallback)
    {
        setLayout(order, sizes, arrange, radarRange, mapFallback);
    }

    bool wanted() { return g_running.load(std::memory_order_relaxed) && (g_viewers.load() > 0 || g_waitingForOne.load() > 0); }

    bool mirrorPanel()
    {
        if (!g_running.load(std::memory_order_relaxed))
            return false;
        std::lock_guard<std::mutex> lock(g_layoutMutex);
        return g_sizes[0] > 0;
    }

    int fps()
    {
        static const int value = [] {
            const char *v = std::getenv("GT3_PHONE_VIEW_FPS");
            return v && *v ? std::clamp(std::atoi(v), 1, 60) : 30;
        }();
        return value;
    }

    void submit(const uint8_t *rgba, int width, int height)
    {
        if (!g_running.load() || width <= 0 || height <= 0)
            return;
        {
            std::lock_guard<std::mutex> lock(g_rawMutex);
            g_raw.assign(rgba, rgba + static_cast<size_t>(width) * height * 4);
            g_rawW = width;
            g_rawH = height;
            g_rawNew = true;
        }
        g_rawReady.notify_one();
    }
}
