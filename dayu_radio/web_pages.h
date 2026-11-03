// ============================================================================
//  web_pages.h —— 网页资源：HTML/CSS/JS 全部集中在这里
//  说明：不能放在 .ino 里——Arduino builder 会用 ctags 扫描 .ino 的函数定义，
//        HTML 里的 "function xxx(" 会被误判成 C++ 函数并插入原型，把原始字符串截断。
//        放到 .h 里就不参与扫描了。
//  来源：配网页与整体版式移植自「小米闹钟」项目，设置页为收音机重写。
// ============================================================================
#pragma once

// ============================================================
//  网页 · 配网页（未联网 / 从热点进来时首页就是它）
//  与闹钟版的差异：扫描改异步轮询（/scan 可能先回 {"pending":1}），
//  绝不阻塞主循环 —— 收音机在重配网时可能还在播歌。
// ============================================================
const char WIFI_HTML[] = R"HTML(
<!DOCTYPE html><html lang="zh-CN"><head>
<meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1">
<title>Dayu-radio · 连接 WiFi</title>
<style>
:root{--acc:#0aa396}
*{box-sizing:border-box;-webkit-tap-highlight-color:transparent}
body{margin:0;padding:16px 14px 40px;background:#f2f2f7;color:#1c1c1e;
font:16px/1.55 -apple-system,BlinkMacSystemFont,"PingFang SC","Microsoft YaHei",sans-serif}
.wrap{max-width:520px;margin:0 auto}
h1{font-size:22px;margin:6px 6px 2px}
.sub{color:#8e8e93;font-size:13px;margin:0 6px 14px;line-height:1.6}
.card{margin-top:12px;border-radius:16px;background:#fff;box-shadow:0 1px 4px rgba(0,0,0,.06);overflow:hidden}
.net{display:flex;align-items:center;gap:12px;padding:14px 16px;border-bottom:1px solid #f0f0f3;cursor:pointer}
.net:last-child{border-bottom:0}
.net:active{background:#f7f7fa}
.net b{flex:1;font-weight:500;font-size:15px;overflow:hidden;text-overflow:ellipsis;white-space:nowrap}
.bars{color:#a1a1a6;font-size:12px}
.lock{color:#8e8e93;font-size:12px}
.row{display:flex;align-items:center;gap:10px;padding:12px 16px;border-bottom:1px solid #f0f0f3}
.row label{flex:0 0 44px;color:#8e8e93;font-size:14px}
input{flex:1;min-width:0;border:0;background:#f2f2f7;border-radius:10px;padding:10px 12px;font-size:15px;color:inherit}
button{border:0;border-radius:12px;font-size:15px;font-weight:500;background:#f2f2f7;color:#1c1c1e;padding:10px 14px}
button.p{background:var(--acc);color:#fff;display:block;width:calc(100% - 32px);margin:14px 16px;padding:12px}
.msg{margin:14px 6px;font-size:14px;line-height:1.7;color:#8e8e93;word-break:break-all}
.msg.err{color:#d13d00}
.msg.ok{color:#1a8f3c}
.spin{display:inline-block;width:12px;height:12px;border:2px solid #c7c7cc;border-top-color:#0aa396;
border-radius:50%;animation:s .8s linear infinite;vertical-align:-2px;margin-right:6px}
@keyframes s{to{transform:rotate(360deg)}}
</style></head><body><div class="wrap">
<h1>连接 WiFi</h1>
<p class="sub">ESP32 只支持 2.4GHz，5G 网络请在路由器上单独开一个 2.4G 频段</p>
<div class="card" id="list"><div class="net"><b>正在扫描…</b></div></div>
<div class="card">
<div class="row"><label>名称</label><input id="s" placeholder="WiFi 名称" autocomplete="off"></div>
<div class="row"><label>密码</label><input id="p" type="password" placeholder="WiFi 密码"><button onclick="tog()" id="eye">显示</button></div>
<button class="p" onclick="go()">连接</button>
</div>
<p class="msg" id="m"></p>
</div>
<script>
var $=function(i){return document.getElementById(i)};
function tog(){var p=$('p');p.type=(p.type==='password')?'text':'password';$('eye').textContent=(p.type==='password')?'显示':'隐藏'}
function esc(s){return String(s).replace(/[&<>"]/g,function(c){return {'&':'&amp;','<':'&lt;','>':'&gt;','"':'&quot;'}[c]})}
function scan(){
fetch('/scan').then(function(r){return r.json()}).then(function(d){
if(d.pending){setTimeout(scan,400);return}          // 异步扫描还没完成，稍后再来取
if(!d.n.length){$('list').innerHTML='<div class="net"><b>没扫到网络，请确认路由器 2.4G 已开启</b></div>';return}
var h='';
d.n.forEach(function(x){
var lv=['','弱','中','良','强'][x.bars];
h+='<div class="net" data-s="'+esc(x.s)+'" onclick="pick(this)"><b>'+esc(x.s)+'</b>'+
'<span class="bars">'+lv+'</span><span class="lock">'+(x.e?'需密码':'开放')+'</span></div>';
});
$('list').innerHTML=h;
}).catch(function(){$('list').innerHTML='<div class="net"><b>扫描失败，请刷新</b></div>'});
}
function pick(el){$('s').value=el.getAttribute('data-s');$('p').focus()}
function go(){
var s=$('s').value.replace(/^\s+|\s+$/g,'');
if(!s){$('m').className='msg err';$('m').textContent='请先选择或输入 WiFi 名称';return}
$('m').className='msg';
$('m').innerHTML='<span class="spin"></span>正在连接，最多等 20 秒…';
fetch('/connect?ssid='+encodeURIComponent(s)+'&pass='+encodeURIComponent($('p').value))
.then(function(){poll(0)});
}
function poll(n){
fetch('/netstatus').then(function(r){return r.json()}).then(function(d){
if(d.st==1){ if(n<26) setTimeout(function(){poll(n+1)},800); }
else if(d.st==2){
$('m').className='msg ok';
$('m').innerHTML='连接成功：<b>'+esc(d.ssid)+'</b><br>IP 地址：'+d.ip+
'<br><br>请把手机切回家里的 WiFi，再用浏览器打开 <a href="http://'+d.ip+'/">http://'+d.ip+'</a> 进入设置。';
}
else if(d.st==3){ $('m').className='msg err'; $('m').textContent=d.msg; }
else { $('m').className='msg'; $('m').textContent='已提交，正在等待结果…'; }
}).catch(function(){});
}
scan();
</script>
</body></html>
)HTML";

// ============================================================
//  网页 · 设置页（联网后从路由器访问的首页）
//  服务端替换：{{IDX}} {{TOTAL}} {{OPTIONS}} {{VOL}} {{CUSTOM}}
//              {{WIFI}} {{IP}} {{RSSI}} {{FW}}
// ============================================================
const char PAGE_HTML[] = R"HTML(
<!DOCTYPE html><html lang="zh-CN"><head>
<meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1">
<title>Dayu-radio</title>
<style>
:root{--acc:#0aa396}
*{box-sizing:border-box;-webkit-tap-highlight-color:transparent}
body{margin:0;padding:16px 14px 40px;background:#f2f2f7;color:#1c1c1e;
font:16px/1.55 -apple-system,BlinkMacSystemFont,"PingFang SC","Microsoft YaHei",sans-serif}
.wrap{max-width:520px;margin:0 auto}
.now{padding:18px 20px;border-radius:18px;color:#fff;background:linear-gradient(135deg,#25c3b0,#0a8a7e);
box-shadow:0 8px 22px rgba(10,138,126,.28)}
.now .st{font-size:22px;font-weight:600;line-height:1.3;overflow:hidden;text-overflow:ellipsis;white-space:nowrap}
.now .ti{margin-top:4px;font-size:13px;opacity:.9;height:18px;overflow:hidden;text-overflow:ellipsis;white-space:nowrap}
.card{margin-top:14px;padding:4px 16px 8px;border-radius:16px;background:#fff;box-shadow:0 1px 4px rgba(0,0,0,.06)}
h3{margin:14px 0 6px;font-size:13px;font-weight:600;color:#8e8e93;letter-spacing:.6px}
.row{display:flex;align-items:center;gap:12px;padding:11px 0;border-bottom:1px solid #f0f0f3}
.row:last-child{border-bottom:0}
.row>label{flex:0 0 auto;font-size:15px}
input[type=range]{flex:1 1 auto;min-width:0;height:28px;accent-color:var(--acc)}
select{border:0;background:#f2f2f7;border-radius:10px;padding:8px 12px;font-size:15px;
color:inherit;margin-left:auto;max-width:55%}
.val{flex:0 0 40px;text-align:right;font-size:14px;color:#8e8e93;font-variant-numeric:tabular-nums}
.btns{display:flex;gap:10px;flex-wrap:wrap;padding:12px 0 6px}
button{flex:1 1 auto;min-width:100px;padding:11px 14px;border:0;border-radius:12px;
background:#f2f2f7;color:#1c1c1e;font-size:15px;font-weight:500}
button.p{background:var(--acc);color:#fff}
button.dg{background:#ffe9e0;color:#d13d00}
textarea{width:100%;min-height:120px;border:0;background:#f2f2f7;border-radius:10px;padding:10px 12px;
font-size:13px;font-family:ui-monospace,Menlo,monospace;color:inherit;resize:vertical;margin:6px 0 4px}
.tip{margin:18px 6px 0;color:#8e8e93;font-size:13px;line-height:1.8}
.sign{margin:16px 0 0;text-align:center;color:#c1c1c6;font-size:13px;
letter-spacing:2px;font-weight:500}
#toast{position:fixed;left:50%;bottom:26px;transform:translate(-50%,16px);padding:10px 20px;
border-radius:999px;background:#1c1c1e;color:#fff;font-size:14px;opacity:0;transition:.25s;pointer-events:none}
#toast.on{opacity:1;transform:translate(-50%,0)}
</style></head><body><div class="wrap">
<div class="now">
<div class="st" id="st">加载中…</div>
<div class="ti" id="ti">&nbsp;</div>
</div>
<div class="card">
<h3>播放</h3>
<div class="btns">
<button onclick="act('/tuneprev','已切台')">⏮ 上一台</button>
<button onclick="act('/tunenext','已切台')">下一台 ⏭</button>
</div>
<div class="row"><label>选台</label><select id="pick" onchange="act('/tune?i='+this.value,'已切台')">{{OPTIONS}}</select></div>
<div class="row"><label>音量</label><input type="range" id="vol" min="0" max="{{VOLMAX}}" value="{{VOL}}" oninput="volv.textContent=this.value" onchange="savevol()"><span class="val" id="volv">{{VOL}}</span></div>
</div>
<div class="card">
<h3>频谱</h3>
<div class="row"><label>样式</label><select id="spec" onchange="act('/spec?s='+this.value,'已切换')">{{SPECOPTS}}</select></div>
<p style="margin:2px 0 8px;font-size:13px;color:#8e8e93">切换立即生效，重启后保持</p>
</div>
<div class="card">
<h3>自定义电台</h3>
<p style="margin:8px 0 0;font-size:13px;color:#8e8e93">每行一个，格式：<b>名称|URL</b>（竖线分隔）。以 # 开头的行忽略。保存后立即生效（总数 = 内置台 + 自定义台）。</p>
<textarea id="cst" spellcheck="false">{{CUSTOM}}</textarea>
<div class="btns"><button class="p" onclick="saveSt()">保存电台表</button></div>
</div>
<div class="card">
<h3>系统</h3>
<div class="row"><label>WiFi</label><span class="val" style="flex:1;text-align:left;color:#1c1c1e;font-size:13px">{{WIFI}} · {{RSSI}}dBm · {{IP}}</span></div>
<div class="btns">
<button onclick="location.href='/portal'">重新配网</button>
<button onclick="if(confirm('重启设备？'))act('/restart','重启中…')">重启</button>
</div>
<div class="btns">
<button class="dg" onclick="if(confirm('清除保存的 WiFi，设备会重启并进入配网模式？'))act('/resetwifi','已清除，重启中…')">清除 WiFi 配置</button>
</div>
</div>
<p class="tip">BOOT 短按 下一个台 · 音量键调节音量 · BOOT 长按 8 秒 清除配网<br>
固件 {{FW}}</p>
<p class="sign">Made by Dayu</p>
</div>
<div id="toast"></div>
<script>
var $=function(i){return document.getElementById(i)};
function toast(m){var e=$('toast');e.textContent=m;e.classList.add('on');
clearTimeout(e._t);e._t=setTimeout(function(){e.classList.remove('on')},1500)}
function act(u,m){fetch(u).then(function(){toast(m||'完成');setTimeout(poll,600)})
.catch(function(){toast('失败')})}
function savevol(){
fetch('/vol?v='+$('vol').value).then(function(){toast('已保存')}).catch(function(){toast('失败')});
}
function saveSt(){
var d=$('cst').value;
fetch('/stationsave',{method:'POST',
headers:{'Content-Type':'application/x-www-form-urlencoded'},
body:'d='+encodeURIComponent(d)})
.then(function(){toast('已保存，正在切换')}).catch(function(){toast('保存失败')});
}
var lastIdx=-1;
function poll(){
fetch('/api/status').then(function(r){return r.json()}).then(function(s){
$('st').textContent=(s.idx+1)+'/'+s.total+'  '+s.name;
$('ti').textContent=s.playing?(s.title||'\u00a0'):s.status;
if(s.idx!==lastIdx){lastIdx=s.idx;$('pick').value=s.idx}
if(document.activeElement!==$('vol')){$('vol').value=s.vol;$('volv').textContent=s.vol}
}).catch(function(){})
}
poll();setInterval(poll,2000);
</script>
</body></html>
)HTML";
