#!/usr/bin/env python3
"""Replace secondary screen HTML with Nordic minimal design + add pushbutton."""
import re

PATH = r"c:\Users\chris\Documents\GIT\motor-control-swing\src\main.cpp"

with open(PATH, "r", encoding="utf-8") as f:
    src = f.read()

# helper: regex sub but treat replacement as literal string (no \1, \u etc.)
def literal_sub(pattern, replacement, text, flags=0):
    return re.sub(pattern, lambda m: replacement, text, flags=flags)

def literal_subn(pattern, replacement, text, flags=0):
    count = [0]
    def repl(m):
        count[0] += 1
        return replacement
    result = re.sub(pattern, repl, text, flags=flags)
    return result, count[0]

# ─────────────────────────────────────────────────────────────
# 1.  AP_HTML  (WiFi setup portal)
# ─────────────────────────────────────────────────────────────
NEW_AP = """const char AP_HTML[] PROGMEM = R"rawliteral(<!DOCTYPE html>
<html lang="en">
<head>
<meta charset="UTF-8">
<meta name="viewport" content="width=device-width,initial-scale=1,user-scalable=no">
<meta name="theme-color" content="#F2EEE6">
<title>RockBox \u2013 WiFi Setup</title>
<link rel="preconnect" href="https://fonts.googleapis.com">
<link href="https://fonts.googleapis.com/css2?family=Fraunces:wght@300;400&family=Instrument+Sans:wght@400;500&display=swap" rel="stylesheet">
<style>
*{margin:0;padding:0;box-sizing:border-box}
:root{--paper:#F2EEE6;--sage:#8FA89A;--forest:#2B3A34;--ink:#1C2B26;--mist:#D4CFC6;--err:#C0392B}
html,body{min-height:100%;background:var(--paper);color:var(--ink);font-family:'Instrument Sans',system-ui,sans-serif}
body{display:flex;flex-direction:column;align-items:center;padding:2.5rem 1.2rem;max-width:400px;margin:0 auto;gap:1.6rem}
.wordmark{font-family:'Fraunces',Georgia,serif;font-size:1.6rem;font-weight:300;color:var(--forest);letter-spacing:.02em;align-self:flex-start}
.sub{font-size:.78rem;color:var(--sage);align-self:flex-start;margin-top:-.9rem;letter-spacing:.02em}
.card{width:100%;background:#fff;border:1px solid var(--mist);border-radius:14px;padding:1.4rem;display:flex;flex-direction:column;gap:1rem}
label{font-size:.65rem;text-transform:uppercase;letter-spacing:.1em;color:var(--sage);font-weight:500}
.irow{display:flex;gap:.5rem;align-items:stretch;margin-top:.35rem}
input[type=text],input[type=password]{flex:1;background:var(--paper);border:1px solid var(--mist);border-radius:8px;padding:.7rem .9rem;color:var(--ink);font-size:.88rem;outline:none;font-family:inherit;width:100%}
input:focus{border-color:var(--sage)}
.scan-btn,.show-btn{padding:.7rem .9rem;border:1px solid var(--mist);border-radius:8px;background:var(--paper);color:var(--sage);font-size:.72rem;font-weight:500;cursor:pointer;white-space:nowrap;letter-spacing:.06em;font-family:inherit}
.scan-btn:active,.show-btn:active{background:var(--mist)}
.scan-btn:disabled{opacity:.4;cursor:default}
select#netlist{width:100%;background:var(--paper);border:1px solid var(--mist);border-radius:8px;padding:.5rem .6rem;color:var(--ink);font-size:.82rem;display:none;margin-top:.4rem;font-family:inherit}
.save-btn{width:100%;padding:1rem;border:none;border-radius:10px;background:var(--sage);color:#fff;font-size:.88rem;font-weight:500;letter-spacing:.08em;text-transform:uppercase;cursor:pointer;font-family:inherit}
.save-btn:active{opacity:.85}
.save-btn:disabled{opacity:.4;cursor:default}
#msg{font-size:.78rem;text-align:center;min-height:1.2em;line-height:1.6;color:var(--sage)}
.err{color:var(--err)!important}
</style>
</head>
<body>
<span class="wordmark">RockBox</span>
<span class="sub">One-time WiFi setup &mdash; credentials are stored on the device</span>
<div class="card">
  <div>
    <label>WiFi Network (SSID)</label>
    <div class="irow">
      <input type="text" id="ssid" placeholder="Network name"
        autocomplete="off" autocorrect="off" autocapitalize="none" spellcheck="false">
      <button class="scan-btn" id="scanbtn" onclick="doScan()">Scan</button>
    </div>
    <select id="netlist" size="5"
      onchange="document.getElementById('ssid').value=this.value"></select>
  </div>
  <div>
    <label>Password</label>
    <div class="irow">
      <input type="password" id="pass" placeholder="WiFi password" autocomplete="off">
      <button class="show-btn" id="showbtn" onclick="togglePwd()">Show</button>
    </div>
  </div>
  <div id="msg"></div>
  <button class="save-btn" id="savebtn" onclick="doSave()">Connect &amp; Save</button>
</div>
<script>
function doScan(){
  var btn=document.getElementById('scanbtn');
  btn.textContent='...';btn.disabled=true;
  fetch('/scan').then(function(r){return r.json();}).then(function(nets){
    var sel=document.getElementById('netlist');
    sel.innerHTML='';
    nets.sort(function(a,b){return b.rssi-a.rssi;});
    nets.forEach(function(n){
      var o=document.createElement('option');
      o.value=n.ssid;
      o.textContent=n.ssid+' ('+n.rssi+'dBm'+(n.secure?' \\uD83D\\uDD12':'')+')';
      sel.appendChild(o);
    });
    sel.style.display=nets.length?'block':'none';
    btn.textContent='Scan';btn.disabled=false;
  }).catch(function(){
    btn.textContent='Scan';btn.disabled=false;
    showMsg('Scan failed \\u2014 enter SSID manually','err');
  });
}
function togglePwd(){
  var i=document.getElementById('pass');
  var b=document.getElementById('showbtn');
  if(i.type==='password'){i.type='text';b.textContent='Hide';}
  else{i.type='password';b.textContent='Show';}
}
function showMsg(text,cls){
  var el=document.getElementById('msg');
  el.textContent=text;el.className=cls||'';
}
function doSave(){
  var ssid=document.getElementById('ssid').value.trim();
  var pass=document.getElementById('pass').value;
  if(!ssid){showMsg('Please enter a network name','err');return;}
  if(ssid.length>32){showMsg('SSID too long (max 32 chars)','err');return;}
  if(pass.length>64){showMsg('Password too long (max 64 chars)','err');return;}
  var btn=document.getElementById('savebtn');
  btn.disabled=true;
  showMsg('Saving\\u2026');
  var body='ssid='+encodeURIComponent(ssid)+'&pass='+encodeURIComponent(pass);
  fetch('/save',{method:'POST',
    headers:{'Content-Type':'application/x-www-form-urlencoded'},
    body:body})
  .then(function(){
    showMsg('Saved! Device will restart and connect to \\u201C'+ssid+
      '\\u201D. Reconnect your phone to that network, then open http://rockbox.local');
  }).catch(function(){
    showMsg('Saved! Device is restarting\\u2026');
  });
}
</script>
</body>
</html>
)rawliteral\""""

# ─────────────────────────────────────────────────────────────
# 2.  OTA_HTML  (manual firmware update page)
# ─────────────────────────────────────────────────────────────
NEW_OTA = """const char OTA_HTML[] PROGMEM = R"rawliteral(<!DOCTYPE html>
<html lang="en">
<head>
<meta charset="UTF-8">
<meta name="viewport" content="width=device-width,initial-scale=1,user-scalable=no">
<meta name="theme-color" content="#F2EEE6">
<title>RockBox \u2013 Firmware Update</title>
<link rel="preconnect" href="https://fonts.googleapis.com">
<link href="https://fonts.googleapis.com/css2?family=Fraunces:wght@300;400&family=Instrument+Sans:wght@400;500&display=swap" rel="stylesheet">
<style>
*{margin:0;padding:0;box-sizing:border-box}
:root{--paper:#F2EEE6;--sage:#8FA89A;--forest:#2B3A34;--ink:#1C2B26;--mist:#D4CFC6;--err:#C0392B}
html,body{min-height:100%;background:var(--paper);color:var(--ink);font-family:'Instrument Sans',system-ui,sans-serif}
body{display:flex;flex-direction:column;align-items:center;padding:2.5rem 1.2rem;max-width:400px;margin:0 auto;gap:1.6rem}
.wordmark{font-family:'Fraunces',Georgia,serif;font-size:1.6rem;font-weight:300;color:var(--forest);letter-spacing:.02em;align-self:flex-start}
.sub{font-size:.78rem;color:var(--sage);align-self:flex-start;margin-top:-.9rem;letter-spacing:.02em}
.card{width:100%;background:#fff;border:1px solid var(--mist);border-radius:14px;padding:1.4rem;display:flex;flex-direction:column;gap:1rem}
.ver{font-size:.78rem;color:var(--sage)}
.ver b{color:var(--ink)}
label.pick{display:flex;flex-direction:column;gap:.4rem;font-size:.65rem;text-transform:uppercase;letter-spacing:.1em;color:var(--sage);font-weight:500;cursor:pointer}
input[type=file]{background:var(--paper);border:1px solid var(--mist);border-radius:8px;padding:.6rem .8rem;color:var(--ink);font-size:.82rem;width:100%;font-family:inherit}
.up-btn{width:100%;padding:1rem;border:none;border-radius:10px;background:var(--sage);color:#fff;font-size:.88rem;font-weight:500;letter-spacing:.08em;text-transform:uppercase;cursor:pointer;font-family:inherit}
.up-btn:active{opacity:.85}
.up-btn:disabled{opacity:.4;cursor:default}
.bar-wrap{width:100%;height:6px;background:var(--mist);border-radius:3px;overflow:hidden;display:none}
.bar{height:100%;width:0%;background:var(--sage);border-radius:3px;transition:width .2s}
#msg{font-size:.78rem;text-align:center;min-height:1.2em;line-height:1.6;color:var(--sage)}
.err{color:var(--err)!important}
a.back{font-size:.7rem;color:var(--sage);text-decoration:none;opacity:.7}
a.back:hover{opacity:1}
</style>
</head>
<body>
<span class="wordmark">RockBox</span>
<span class="sub">Firmware update &mdash; upload a file or check GitHub for a newer version</span>
<div class="card">
  <div class="ver">Current firmware: <b>v__VER__</b></div>
  <button class="up-btn" onclick="window.location='/ota-github'">Check GitHub for update</button>
</div>
<div class="card">
  <label class="pick">
    Upload firmware file (.bin)
    <input type="file" id="bin" accept=".bin">
  </label>
  <div class="bar-wrap" id="bw"><div class="bar" id="bar"></div></div>
  <div id="msg"></div>
  <button class="up-btn" id="upbtn" onclick="doUpload()">Upload &amp; Flash</button>
</div>
<a class="back" href="/">&larr; Back to swing</a>
<script>
function showMsg(t,c){var e=document.getElementById('msg');e.textContent=t;e.className=c||'';}
function doUpload(){
  var f=document.getElementById('bin').files[0];
  if(!f){showMsg('Please select a .bin file','err');return;}
  if(!f.name.endsWith('.bin')){showMsg('File must be a .bin','err');return;}
  var btn=document.getElementById('upbtn');
  btn.disabled=true;
  var bw=document.getElementById('bw');
  var bar=document.getElementById('bar');
  bw.style.display='block';
  showMsg('Uploading\\u2026');
  var fd=new FormData();
  fd.append('firmware',f,f.name);
  var xhr=new XMLHttpRequest();
  xhr.upload.onprogress=function(e){
    if(e.lengthComputable){
      var p=Math.round(e.loaded/e.total*100);
      bar.style.width=p+'%';
      showMsg('Uploading\\u2026 '+p+'%');
    }
  };
  xhr.onload=function(){
    bar.style.width='100%';
    if(xhr.status===200&&xhr.responseText==='OK'){
      showMsg('Flashed \\u2014 device is rebooting\\u2026');
    } else {
      showMsg('Error: '+(xhr.responseText||'Upload failed'),'err');
      btn.disabled=false;
    }
  };
  xhr.onerror=function(){showMsg('Connection lost during upload','err');btn.disabled=false;};
  xhr.open('POST','/update');
  xhr.send(fd);
}
</script>
</body>
</html>
)rawliteral\""""

# ─────────────────────────────────────────────────────────────
# 3.  OTA GitHub check spinner  (inside handleOTAGithub)
# ─────────────────────────────────────────────────────────────
NEW_OTA_CHECK = """  String html = F("<!DOCTYPE html><html><head><meta charset='UTF-8'>"
                  "<meta name='viewport' content='width=device-width,initial-scale=1'>"
                  "<meta name='theme-color' content='#F2EEE6'>"
                  "<title>RockBox \\u2013 OTA Check</title>"
                  "<link rel='preconnect' href='https://fonts.googleapis.com'>"
                  "<link href='https://fonts.googleapis.com/css2?family=Fraunces:wght@300&family=Instrument+Sans:wght@400&display=swap' rel='stylesheet'>"
                  "<style>*{margin:0;padding:0;box-sizing:border-box}"
                  "body{background:#F2EEE6;color:#1C2B26;font-family:'Instrument Sans',system-ui,sans-serif;"
                  "display:flex;flex-direction:column;align-items:center;justify-content:center;"
                  "min-height:100vh;gap:1.2rem;padding:2rem}"
                  "h1{font-family:'Fraunces',Georgia,serif;font-size:1.4rem;font-weight:300;color:#2B3A34}"
                  "p{font-size:.82rem;color:#8FA89A;text-align:center;line-height:1.6}"
                  ".spin{width:32px;height:32px;border:2px solid #D4CFC6;"
                  "border-top-color:#8FA89A;border-radius:50%;"
                  "animation:spin .9s linear infinite}"
                  "@keyframes spin{to{transform:rotate(360deg)}}"
                  "</style></head><body>"
                  "<div class='spin'></div>"
                  "<h1>Checking for update</h1>"
                  "<p>Comparing with GitHub&hellip;<br>"
                  "Device will reboot automatically if a newer version is found.</p>"
                  "<script>setTimeout(function(){window.location='/update';},30000);</script>"
                  "</body></html>");"""

# ─────────────────────────────────────────────────────────────
# 4.  Reset-WiFi confirmation page
# ─────────────────────────────────────────────────────────────
NEW_RESET = """  clearWifiCreds();
  httpSrv.send(200, "text/html; charset=utf-8",
               "<html><head><meta charset='UTF-8'>"
               "<meta name='viewport' content='width=device-width,initial-scale=1'>"
               "<meta name='theme-color' content='#F2EEE6'>"
               "<link rel='preconnect' href='https://fonts.googleapis.com'>"
               "<link href='https://fonts.googleapis.com/css2?family=Fraunces:wght@300;400&family=Instrument+Sans:wght@400&display=swap' rel='stylesheet'>"
               "<style>*{margin:0;padding:0;box-sizing:border-box}"
               "body{background:#F2EEE6;color:#1C2B26;font-family:'Instrument Sans',system-ui,sans-serif;"
               "display:flex;flex-direction:column;align-items:center;justify-content:center;"
               "min-height:100vh;gap:1rem;padding:2rem;text-align:center}"
               "h2{font-family:'Fraunces',Georgia,serif;font-size:1.5rem;font-weight:300;color:#2B3A34}"
               "p{font-size:.82rem;color:#8FA89A;line-height:1.6;max-width:280px}"
               "b{color:#1C2B26;font-weight:500}"
               "</style></head>"
               "<body><h2>WiFi Reset</h2>"
               "<p>Credentials cleared. Device is restarting into setup mode.</p>"
               "<p>Connect to WiFi <b>RockBox-Setup</b> to reconfigure.</p>"
               "</body></html>");"""

# ─────────────────────────────────────────────────────────────
# Apply replacements
# ─────────────────────────────────────────────────────────────

ap_pat = re.compile(
    r'const char AP_HTML\[\] PROGMEM = R"rawliteral\(.*?\)rawliteral";',
    re.DOTALL
)
src, n = literal_subn(ap_pat, NEW_AP, src)
print(f"AP_HTML replacements: {n}")

ota_pat = re.compile(
    r'const char OTA_HTML\[\] PROGMEM = R"rawliteral\(.*?\)rawliteral";',
    re.DOTALL
)
src, n = literal_subn(ota_pat, NEW_OTA, src)
print(f"OTA_HTML replacements: {n}")

ota_check_pat = re.compile(
    r'  String html = F\("<!DOCTYPE html>.*?</body></html>"\);',
    re.DOTALL
)
src, n = literal_subn(ota_check_pat, NEW_OTA_CHECK, src)
print(f"OTA check spinner replacements: {n}")

reset_pat = re.compile(
    r'  clearWifiCreds\(\);\s*httpSrv\.send\(200.*?"</body></html>"\);',
    re.DOTALL
)
src, n = literal_subn(reset_pat, NEW_RESET, src)
print(f"Reset-WiFi page replacements: {n}")

with open(PATH, "w", encoding="utf-8") as f:
    f.write(src)

print("Done.")
