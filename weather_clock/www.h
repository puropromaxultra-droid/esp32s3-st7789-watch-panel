// ============================================================
// www.h —— 手机端播放页（编译进 flash，不占 RAM）
//
// 流程：选歌 → 浏览器 decodeAudioData 解码 → OfflineAudioContext 重采样到 31250Hz
//       单声道 → Int16 → 按 32KB 分块 POST /pcm → 板子写 U5 环形缓冲。
//
// ★ 背压靠 TCP 窗口：板子缓冲满时 /pcm 不返回，fetch 就挂住，发送自然降速。
//   所以手机端不需要自己做定时限速，也不用担心把 40 秒缓冲灌爆。
// ============================================================
#pragma once
#include <Arduino.h>

static const char WS_HTML[] PROGMEM = R"HTML(<!DOCTYPE html>
<html lang="zh-CN"><head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1,viewport-fit=cover">
<meta name="theme-color" content="#0b0e12">
<title>CLOCK · MUSIC</title>
<style>
*{box-sizing:border-box;-webkit-tap-highlight-color:transparent}
body{margin:0;padding:18px;background:#0b0e12;color:#e8eef5;
 font:16px/1.55 -apple-system,BlinkMacSystemFont,"Segoe UI",Roboto,sans-serif;
 display:flex;min-height:100vh;align-items:center;justify-content:center}
#app{width:100%;max-width:430px}
h1{font-size:13px;letter-spacing:.3em;color:#5ad1e6;margin:0 0 18px;font-weight:700}
.card{background:#131920;border:1px solid #1e2731;border-radius:20px;padding:20px}
.ttl{font-size:18px;font-weight:650;margin:0 0 3px;word-break:break-all;line-height:1.35}
.sub{color:#77899c;font-size:12px;margin:0 0 16px;word-break:break-all}
.pg{height:6px;background:#1b232d;border-radius:3px;overflow:hidden}
.pg>i{display:block;height:100%;width:0;background:linear-gradient(90deg,#2f8feb,#5ad1e6);
 border-radius:3px;transition:width .25s}
.meta{display:flex;justify-content:space-between;font-size:11px;color:#65788b;margin-top:7px;
 font-variant-numeric:tabular-nums}
label.file{display:block;padding:15px;margin-top:16px;border:1.5px dashed #2a3644;
 border-radius:14px;text-align:center;color:#8b9dae;cursor:pointer;font-size:14px}
label.file:active{background:#161d25}
input[type=file]{display:none}
.row{display:flex;gap:9px;margin-top:14px}
button{flex:1;padding:13px 0;border:0;border-radius:13px;background:#1f6feb;color:#fff;
 font-size:15px;font-weight:650;font-family:inherit;cursor:pointer;transition:opacity .15s}
button:active{opacity:.7}
button:disabled{opacity:.35}
button.gh{background:#1a222c;color:#bccbd8}
.vol{display:flex;align-items:center;gap:12px;margin-top:16px;font-size:12px;color:#65788b}
.vol input{flex:1;accent-color:#5ad1e6}
#log{font-size:12px;color:#7d8f9f;margin-top:14px;min-height:17px}
#log.err{color:#ff7a6b}
</style></head><body>
<div id="app">
<h1>CLOCK · MUSIC</h1>
<div class="card">
  <p class="ttl" id="ttl">还没有选歌</p>
  <p class="sub" id="sub">支持 mp3 / m4a / flac / wav / ogg</p>
  <div class="pg"><i id="bar"></i></div>
  <div class="meta"><span id="t1">00:00</span><span id="t2">--:--</span></div>
  <label class="file" for="f" id="lb">点这里选一首歌</label>
  <input type="file" id="f" accept="audio/*">
  <div class="row">
    <button id="bPlay">播放</button>
    <button id="bPause" class="gh">暂停</button>
    <button id="bStop" class="gh">停止</button>
  </div>
  <div class="vol"><span>音量</span><input type="range" id="vol" min="0" max="100" value="55"><span id="vnum">55</span></div>
  <div id="log">就绪 · v4 · 31.25kHz</div>
</div>
</div>
<script>
var $=function(s){return document.querySelector(s)};
var SR=31250, VER=4, pcm=null, fname='', off=0, running=false, paused=false, total=0;

function fmt(sec){
  sec=Math.max(0,Math.floor(sec));
  return ('0'+Math.floor(sec/60)).slice(-2)+':'+('0'+(sec%60)).slice(-2);
}
function log(m,err){var e=$('#log');e.textContent=m;e.className=err?'err':'';}

$('#f').addEventListener('change', async function(e){
  var f=e.target.files[0]; if(!f) return;
  fname=f.name; $('#lb').textContent=f.name; $('#ttl').textContent=f.name;
  pcm=null; off=0; total=0; setBtns();
  try{
    log('读取文件…');
    var ab=await f.arrayBuffer();
    log('解码中…');
    var AC=window.AudioContext||window.webkitAudioContext;
    var ac=new AC();
    var abuf=await ac.decodeAudioData(ab);
    ac.close();
    var n=Math.max(1,Math.round(abuf.duration*SR));
    log('重采样到 '+(SR/1000)+'kHz（'+abuf.duration.toFixed(1)+' 秒）…');
    var oc=new OfflineAudioContext(1,n,SR);
    var src=oc.createBufferSource();
    src.buffer=abuf; src.connect(oc.destination); src.start();
    var out=await oc.startRendering();
    var ch=out.getChannelData(0);
    // ★ 有些手机会悄悄把 OfflineAudioContext 的采样率钳到 44.1k/48k（不报错）。
    //   那样推上去的就不是 SR 点的音频 ⇒ 板上按 31250 放会变慢、变调。
    //   这里当场校验，钳住了就自己用线性插值重采一遍。
    if (Math.abs(out.sampleRate-SR)>1){
      log('浏览器钳到 '+out.sampleRate+'Hz，本地重采样…');
      var ratio=out.sampleRate/SR, m=Math.max(1,Math.floor(ch.length/ratio));
      var r=new Float32Array(m);
      for(var k=0;k<m;k++){
        var x=k*ratio, i0=Math.floor(x), f=x-i0;
        var a=ch[i0]||0, b=ch[i0+1]!==undefined?ch[i0+1]:a;
        r[k]=a+(b-a)*f;
      }
      ch=r;
    }
    pcm=new Int16Array(ch.length);
    for(var i=0;i<ch.length;i++){
      var v=ch[i]; v=v<-1?-1:(v>1?1:v);
      pcm[i]=v<0?v*32768:v*32767;
    }
    total=pcm.byteLength;
    $('#t2').textContent=fmt(ch.length/SR);
    $('#sub').textContent=(total/1048576).toFixed(1)+' MB · '+(SR/1000)+'kHz 单声道 · v'+VER;
    log('就绪，点「播放」推给时钟');
    setBtns();
  }catch(err){
    log('失败：'+(err&&err.message||err),1);
  }
});

function setBtns(){
  $('#bPlay').disabled = !pcm || running;
  $('#bPause').disabled = !running;
  $('#bStop').disabled = !running;
}

async function push(){
  running=true; paused=false; setBtns();
  $('#log').textContent='连接时钟…';
  try{
    await fetch('/start?len='+total+'&name='+encodeURIComponent(fname));
  }catch(e){ log('连不上时钟',1); running=false; setBtns(); return; }

  $('#log').textContent='推流中…';
  // 32KB 一块：每块 = 0.52 秒音频。块越大，每块的建连/请求开销摊得越薄
  // （手机端每个 POST 都是新 TCP 连接，16KB 时开销能吃掉三成带宽）。
  var CH=32768, slow=0;
  while(off<total && running){
    if(paused){ await new Promise(r=>setTimeout(r,150)); continue; }
    var end=Math.min(off+CH,total);
    var body=new Uint8Array(pcm.buffer,pcm.byteOffset+off,end-off);
    var t0=Date.now();
    try{
      await fetch('/pcm',{method:'POST',body:body});
    }catch(e){ log('推送中断（时钟可能掉线）',1); break; }
    if(!running) break;
    var dt=Date.now()-t0;
    if(dt>900){ slow=1; $('#log').textContent='缓冲中…'; }
    else if(slow){ slow=0; $('#log').textContent='推流中…'; }
    off=end;
    $('#bar').style.width=(off/total*100)+'%';
    $('#t1').textContent=fmt(off/SR);
    await new Promise(r=>setTimeout(r,0));
  }
  if(off>=total){
    try{ await fetch('/eos'); }catch(e){}
    log('发送完毕，时钟还在放最后几十秒');
  }
  running=false; setBtns();
}

$('#bPlay').onclick=function(){ if(pcm&&!running) push(); };
$('#bPause').onclick=async function(){
  paused=!paused;
  $('#bPause').textContent=paused?'继续':'暂停';
  $('#log').textContent=paused?'已暂停（时钟缓冲放完就停）':'继续推流';
  try{ await fetch('/ctl?c='+(paused?'pause':'resume')); }catch(e){}
};
$('#bStop').onclick=async function(){
  running=false; off=0; paused=false; $('#bPause').textContent='暂停';
  $('#bar').style.width='0'; $('#t1').textContent='00:00';
  try{ await fetch('/ctl?c=stop'); }catch(e){}
  log('已停止'); setBtns();
};
$('#vol').oninput=function(){
  var v=this.value; $('#vnum').textContent=v;
  fetch('/ctl?c=vol&v='+v).catch(function(){});
};
setBtns();
</script></body></html>)HTML";
