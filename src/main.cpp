/*
 * ============================================================
 *  ESP32 Baby Swing Controller  —  v2.1
 * ============================================================
 *
 *  Motor drives a linear axis via crank — always spins in one
 *  direction. Swing ON = motor runs at set speed. The crank
 *  converts continuous rotation into back-and-forth swing.
 *
 *  Hardware:
 *    12V Adapter  ──▶  L298N +12V & GND
 *    L298N 5V out ──▶  ESP32 VIN  (5V jumper on L298N)
 *    GPIO 25  ──▶  L298N IN1
 *    GPIO 26  ──▶  L298N IN2
 *    GPIO 27  ──▶  L298N ENA  (PWM speed)
 *    OUT1/OUT2 ──▶  DC Motor
 *    All GNDs connected together!
 *
 *  Access from any device on your WiFi:
 *    http://babyswing.local   (or the IP shown in Serial Monitor)
 *
 *  WebSocket port: 81  (HTTP port: 80)
 *
 * ============================================================
 */

#include <Arduino.h>
#include <WiFi.h>
#include <WebServer.h>
#include <WebSocketsServer.h>
#include <ESPmDNS.h>

// ─── WiFi credentials ────────────────────────────────────────
const char *WIFI_SSID = "Vapporio";
const char *WIFI_PASSWORD = "Ne.k4019d";
const char *HOSTNAME = "babyswing"; // → http://babyswing.local
// ─────────────────────────────────────────────────────────────

// ─── Motor driver pins ───────────────────────────────────────
const int PIN_IN1 = 25;
const int PIN_IN2 = 26;
const int PIN_ENA = 27; // PWM speed

// ─── PWM ─────────────────────────────────────────────────────
const int PWM_CH = 0;
const int PWM_FREQ = 5000;
const int PWM_BITS = 8; // 0–255

// ─── Safety limits ───────────────────────────────────────────
const int MAX_SPEED_PCT = 80;   // hard cap — never exceeded
const long WATCHDOG_MS = 30000; // stop motor if no client this long

// ─── Motor state ─────────────────────────────────────────────
enum MotorDir
{
  MSTOP,
  MFWD,
  MREV
};
int g_spd = 0;
MotorDir g_dir = MSTOP;

// ─── Swing state ─────────────────────────────────────────────
// Motor always runs in one direction — crank handles oscillation.
// Swing ON = motor running; Swing OFF = motor stopped.
bool g_swinging = false;
int g_swingSpd = 40; // % to use when swing starts

// ─── Watchdog ────────────────────────────────────────────────
int g_clients = 0;
ulong g_lastAct = 0;

WebServer httpSrv(80);
WebSocketsServer wsSrv(81);

// ════════════════════════════════════════════════════════════
//  MOTOR
// ════════════════════════════════════════════════════════════

void applyMotor(MotorDir dir, int pct)
{
  pct = constrain(pct, 0, MAX_SPEED_PCT);
  g_dir = dir;
  g_spd = (dir == MSTOP) ? 0 : pct;

  if (dir == MFWD)
  {
    digitalWrite(PIN_IN1, HIGH);
    digitalWrite(PIN_IN2, LOW);
  }
  else if (dir == MREV)
  {
    digitalWrite(PIN_IN1, LOW);
    digitalWrite(PIN_IN2, HIGH);
  }
  else
  {
    digitalWrite(PIN_IN1, LOW);
    digitalWrite(PIN_IN2, LOW);
  }

  ledcWrite(PWM_CH, (dir == MSTOP) ? 0 : map(pct, 0, 100, 0, 255));
  Serial.printf("[Motor] %s %d%%\n",
                dir == MFWD ? "FWD" : dir == MREV ? "REV"
                                                  : "STOP",
                g_spd);
}

void hardStop()
{
  g_swinging = false;
  applyMotor(MSTOP, 0);
}

// ════════════════════════════════════════════════════════════
//  STATE JSON
// ════════════════════════════════════════════════════════════

const char *dirStr(MotorDir d)
{
  return d == MFWD ? "forward" : d == MREV ? "reverse"
                                           : "stop";
}

String stateJSON()
{
  char buf[128];
  snprintf(buf, sizeof(buf),
           "{\"swing\":%s,\"speed\":%d,\"dir\":\"%s\","
           "\"swingSpd\":%d,\"clients\":%d}",
           g_swinging ? "true" : "false",
           g_spd, dirStr(g_dir),
           g_swingSpd, g_clients);
  return String(buf);
}

void broadcast()
{
  String s = stateJSON();
  wsSrv.broadcastTXT(s);
}

// ════════════════════════════════════════════════════════════
//  MINI JSON VALUE EXTRACTOR
// ════════════════════════════════════════════════════════════

String jval(const char *json, const char *key)
{
  String s(json);
  String k = String("\"") + key + "\":";
  int i = s.indexOf(k);
  if (i < 0)
    return "";
  i += k.length();
  while (i < (int)s.length() && s[i] == ' ')
    i++;
  bool quoted = (s[i] == '"');
  if (quoted)
  {
    i++;
    int j = s.indexOf('"', i);
    return j < 0 ? "" : s.substring(i, j);
  }
  int j = i;
  while (j < (int)s.length() && s[j] != ',' && s[j] != '}')
    j++;
  return s.substring(i, j);
}

// ════════════════════════════════════════════════════════════
//  WEBSOCKET EVENTS
// ════════════════════════════════════════════════════════════

void onWsEvent(uint8_t num, WStype_t type, uint8_t *payload, size_t length)
{
  if (type == WStype_CONNECTED)
  {
    g_clients++;
    g_lastAct = millis();
    Serial.printf("[WS] +client #%u  total=%d\n", num, g_clients);
    String s = stateJSON();
    wsSrv.sendTXT(num, s);
    broadcast();
    return;
  }
  if (type == WStype_DISCONNECTED)
  {
    g_clients = max(0, g_clients - 1);
    Serial.printf("[WS] -client #%u  total=%d\n", num, g_clients);
    broadcast();
    return;
  }
  if (type != WStype_TEXT)
    return;

  g_lastAct = millis();
  const char *p = (const char *)payload;
  String cmd = jval(p, "cmd");

  if (cmd == "heartbeat")
  {
    // g_lastAct updated above
  }
  else if (cmd == "swing_start")
  {
    String s = jval(p, "speed");
    if (s.length())
      g_swingSpd = constrain(s.toInt(), 5, MAX_SPEED_PCT);
    g_swinging = true;
    applyMotor(MFWD, g_swingSpd); // run forward continuously — crank does the swinging
    broadcast();
    Serial.printf("[Swing] START  spd=%d%%\n", g_swingSpd);
  }
  else if (cmd == "swing_stop")
  {
    hardStop();
    broadcast();
    Serial.println("[Swing] STOP");
  }
  else if (cmd == "set")
  {
    // Manual override (useful for setup / testing direction)
    g_swinging = false;
    String d = jval(p, "dir");
    String s = jval(p, "speed");
    MotorDir dir = MSTOP;
    if (d == "forward")
      dir = MFWD;
    else if (d == "reverse")
      dir = MREV;
    applyMotor(dir, s.length() ? s.toInt() : 0);
    broadcast();
  }
  else if (cmd == "set_speed")
  {
    String s = jval(p, "speed");
    if (s.length())
    {
      g_swingSpd = constrain(s.toInt(), 5, MAX_SPEED_PCT);
      if (g_swinging) applyMotor(MFWD, g_swingSpd);
      broadcast();
    }
  }
  else if (cmd == "stop")
  {
    hardStop();
    broadcast();
  }
}

// ════════════════════════════════════════════════════════════
//  WEB UI  — Progressive Web App (installable on Android/iOS)
// ════════════════════════════════════════════════════════════

const char HTML[] PROGMEM = R"rawliteral(<!DOCTYPE html>
<html lang="en">
<head>
<meta charset="UTF-8">
<meta name="viewport" content="width=device-width,initial-scale=1,user-scalable=no">
<meta name="theme-color" content="#00d4aa">
<meta name="apple-mobile-web-app-capable" content="yes">
<meta name="apple-mobile-web-app-status-bar-style" content="black-translucent">
<meta name="apple-mobile-web-app-title" content="Baby Swing">
<link rel="manifest" href="/manifest.json">
<title>Baby Swing</title>
<style>
*{margin:0;padding:0;box-sizing:border-box}
:root{--bg:#0a0a0f;--srf:#13131a;--bdr:#1e1e2a;--txt:#e0e0e8;--dim:#6b6b80;
      --acc:#00d4aa;--rev:#ffaa00;--red:#ff4466}
html,body{height:100%;background:var(--bg);color:var(--txt);
  font-family:system-ui,-apple-system,sans-serif;overscroll-behavior:none}
body{display:flex;flex-direction:column;align-items:center;
  padding:1.2rem 1rem 2rem;max-width:420px;margin:0 auto;gap:1.1rem}

/* Install banner */
.install{width:100%;background:var(--srf);border:1px solid var(--bdr);
  border-radius:10px;padding:.7rem 1rem;display:flex;align-items:center;
  justify-content:space-between;gap:.8rem;font-size:.72rem;color:var(--dim)}
.install b{color:var(--txt);font-size:.75rem}
.inst-btn{padding:.4rem .8rem;border:1px solid var(--acc);border-radius:6px;
  background:#00d4aa14;color:var(--acc);font-size:.68rem;font-weight:700;
  cursor:pointer;white-space:nowrap;letter-spacing:.06em;
  -webkit-tap-highlight-color:transparent}
.install.hidden{display:none}

/* Header */
.hdr{width:100%;display:flex;align-items:center;justify-content:space-between}
.hdr-t{font-size:1rem;letter-spacing:.12em;text-transform:uppercase;
  color:var(--acc);font-weight:700}
.conn{display:flex;align-items:center;gap:.35rem;font-size:.68rem;color:var(--dim)}
.dot{width:8px;height:8px;border-radius:50%;background:var(--red);
  transition:background .4s,box-shadow .4s}
.dot.ok{background:var(--acc);box-shadow:0 0 6px var(--acc)}

/* Swing button */
.swrap{display:flex;justify-content:center}
.sbtn{width:200px;height:200px;border-radius:50%;border:3px solid var(--bdr);
  background:var(--srf);display:flex;flex-direction:column;align-items:center;
  justify-content:center;cursor:pointer;user-select:none;
  -webkit-tap-highlight-color:transparent;
  transition:border-color .3s,box-shadow .3s;gap:.35rem}
.sbtn:active{transform:scale(.96)}
.sbtn.on{border-color:var(--acc);
  animation:glow 2.5s ease-in-out infinite}
@keyframes glow{
  0%,100%{box-shadow:0 0 0 5px #00d4aa18,0 0 45px #00d4aa28}
  50%    {box-shadow:0 0 0 9px #00d4aa10,0 0 65px #00d4aa3a}}
.s-ico{font-size:3.2rem;line-height:1;color:var(--dim);
  transition:color .3s}
.s-ico.on{color:var(--acc);animation:spin 1.8s linear infinite}
@keyframes spin{from{transform:rotate(0deg)}to{transform:rotate(360deg)}}
.s-st{font-size:.88rem;font-weight:700;color:var(--dim);transition:color .3s}
.s-st.on{color:var(--acc)}
.s-lb{font-size:.6rem;text-transform:uppercase;letter-spacing:.15em;color:var(--dim)}

/* Speed card */
.card{width:100%;background:var(--srf);border:1px solid var(--bdr);
  border-radius:12px;padding:1rem}
.slab{display:flex;justify-content:space-between;font-size:.7rem;
  text-transform:uppercase;letter-spacing:.08em;color:var(--dim);
  margin-bottom:.55rem}
.slab b{color:var(--txt);font-weight:600;letter-spacing:0}
input[type=range]{-webkit-appearance:none;width:100%;height:5px;
  border-radius:3px;background:var(--bdr);outline:none}
input[type=range]::-webkit-slider-thumb{-webkit-appearance:none;width:24px;
  height:24px;border-radius:50%;background:var(--acc);cursor:pointer;
  box-shadow:0 0 8px #00d4aa40}
input[type=range]:disabled{opacity:.35;pointer-events:none}

/* Manual controls */
details.man{width:100%}
details.man summary{font-size:.68rem;text-transform:uppercase;letter-spacing:.1em;
  color:var(--dim);cursor:pointer;padding:.5rem 0;list-style:none;
  display:flex;align-items:center;gap:.5rem;
  -webkit-tap-highlight-color:transparent}
details.man summary::-webkit-details-marker{display:none}
details.man summary::before{content:"\25B6";font-size:.5rem;
  transition:transform .2s;display:inline-block}
details.man[open] summary::before{transform:rotate(90deg)}
.mrow{display:grid;grid-template-columns:1fr 1fr 1fr;gap:.7rem;margin-top:.8rem}
.mb{font-size:.7rem;font-weight:600;letter-spacing:.08em;text-transform:uppercase;
  padding:.85rem .3rem;border:1px solid var(--bdr);border-radius:8px;
  background:var(--srf);color:var(--txt);cursor:pointer;
  -webkit-tap-highlight-color:transparent;transition:all .15s}
.mb:active{transform:scale(.95)}
.mb.af{border-color:var(--acc);background:#00d4aa12;color:var(--acc)}
.mb.ar{border-color:var(--rev);background:#ffaa0012;color:var(--rev)}

/* Emergency stop */
.estop{width:100%;padding:1rem;border:2px solid var(--red);border-radius:10px;
  background:#ff446610;color:var(--red);font-size:.82rem;font-weight:700;
  letter-spacing:.12em;text-transform:uppercase;cursor:pointer;
  -webkit-tap-highlight-color:transparent;transition:all .15s}
.estop:active{background:var(--red);color:#fff}
</style>
</head>
<body>

<!-- Android/iOS install banner -->
<div class="install" id="ibanner">
  <div><b>Install app</b><br>Use offline, no browser bar</div>
  <button class="inst-btn" id="ibtn" onclick="installApp()">Install</button>
</div>

<div class="hdr">
  <span class="hdr-t">Baby Swing</span>
  <div class="conn">
    <div class="dot" id="dot"></div>
    <span id="ct">Connecting&hellip;</span>
  </div>
</div>

<div class="swrap">
  <div class="sbtn" id="sb" onclick="toggleSwing()">
    <div class="s-ico" id="si">&#9711;</div>
    <div class="s-st"  id="ss">Tap to swing</div>
    <div class="s-lb"  id="sl">stopped</div>
  </div>
</div>

<div class="card">
  <div class="slab">Speed&nbsp;<b id="sv">40</b>%</div>
  <input type="range" id="sp" min="10" max="80" value="40"
    oninput="document.getElementById('sv').textContent=this.value;spdInput(this.value)">
</div>

<details class="man">
  <summary>Manual control (setup / maintenance)</summary>
  <div class="mrow">
    <button class="mb" id="mf" onclick="manual('forward')">&#9650; Fwd</button>
    <button class="mb"         onclick="send({cmd:'stop'})">&#9632; Stop</button>
    <button class="mb" id="mr" onclick="manual('reverse')">&#9660; Rev</button>
  </div>
</details>

<button class="estop" onclick="send({cmd:'stop'})">&#9899; Emergency Stop</button>

<script>
var S={swing:false,speed:0,dir:'stop',swingSpd:40,clients:0};
var ws,rt,ht,rd=1000,deferredPrompt=null;

/* ── PWA install ── */
window.addEventListener('beforeinstallprompt',function(e){
  e.preventDefault();
  deferredPrompt=e;
  // Show our install banner with native prompt support
  document.getElementById('ibanner').classList.remove('hidden');
  document.getElementById('ibtn').textContent='Install';
});
window.addEventListener('appinstalled',function(){
  document.getElementById('ibanner').classList.add('hidden');
  deferredPrompt=null;
});
function installApp(){
  if(deferredPrompt){
    deferredPrompt.prompt();
    deferredPrompt.userChoice.then(function(){deferredPrompt=null;});
  } else {
    // Fallback: show manual instructions
    alert('To install:\n\nAndroid Chrome: tap \u22EE (menu) \u2192 "Add to Home screen"\n\niOS Safari: tap \u2191 (share) \u2192 "Add to Home Screen"');
  }
}
// Hide banner if already running as installed PWA
if(window.matchMedia('(display-mode: standalone)').matches||
   window.navigator.standalone===true){
  document.getElementById('ibanner').classList.add('hidden');
}

/* ── WebSocket ── */
function conn(){
  try{ws=new WebSocket('ws://'+location.hostname+':81');}catch(e){sched();return;}
  ws.onopen=function(){
    rd=1000;dot(true);
    clearInterval(ht);
    ht=setInterval(function(){send({cmd:'heartbeat'});},15000);
  };
  ws.onmessage=function(e){try{render(JSON.parse(e.data));}catch(ex){}};
  ws.onclose=ws.onerror=function(){dot(false);clearInterval(ht);sched();};
}
function sched(){
  clearTimeout(rt);
  rt=setTimeout(function(){rd=Math.min(rd*1.5,10000);conn();},rd);
}
function send(o){if(ws&&ws.readyState===1)ws.send(JSON.stringify(o));}
function dot(ok){
  document.getElementById('dot').className='dot'+(ok?' ok':'');
  if(!ok)document.getElementById('ct').textContent='Reconnecting\u2026';
}

/* ── Render state ── */
function render(s){
  S=s;
  var sb=document.getElementById('sb');
  var si=document.getElementById('si');
  var ss=document.getElementById('ss');
  var sp=document.getElementById('sp');

  if(s.swing){
    sb.className='sbtn on';
    si.className='s-ico on'; si.innerHTML='&#9654;'; si.style.fontSize='3.2rem';
    ss.className='s-st on'; ss.textContent='Swinging';
    document.getElementById('sl').textContent='motor running';
    sp.value=s.swingSpd;
    document.getElementById('sv').textContent=s.swingSpd;
  } else {
    sb.className='sbtn';
    si.className='s-ico'; si.innerHTML='&#9711;'; si.style.fontSize='3.2rem';
    ss.className='s-st'; ss.textContent='Tap to swing';
    document.getElementById('sl').textContent='stopped';
    sp.value=s.swingSpd;
    document.getElementById('sv').textContent=s.swingSpd;
  }

  document.getElementById('mf').className=
    'mb'+((!s.swing&&s.dir==='forward')?' af':'');
  document.getElementById('mr').className=
    'mb'+((!s.swing&&s.dir==='reverse')?' ar':'');

  document.getElementById('dot').className='dot ok';
  document.getElementById('ct').textContent=
    s.clients+' device'+(s.clients!==1?'s':'')+' connected';
}

/* ── Controls ── */
function toggleSwing(){
  if(S.swing){
    send({cmd:'swing_stop'});
  } else {
    send({cmd:'swing_start',
          speed:parseInt(document.getElementById('sp').value)});
  }
}
var spdTimer;
function spdInput(v){
  clearTimeout(spdTimer);
  spdTimer=setTimeout(function(){
    if(S.swing) send({cmd:'set_speed',speed:parseInt(v)});
  },120);
}
function manual(dir){
  send({cmd:'set',dir:dir,speed:parseInt(document.getElementById('sp').value)});
}

conn();
</script>
</body>
</html>
)rawliteral";

// ─── Web app manifest ─────────────────────────────────────
const char MANIFEST[] PROGMEM = R"rawliteral({
  "name": "Baby Swing Controller",
  "short_name": "Baby Swing",
  "description": "Control the baby swing from any device on your WiFi",
  "start_url": "/",
  "display": "standalone",
  "orientation": "portrait",
  "background_color": "#0a0a0f",
  "theme_color": "#00d4aa",
  "icons": [
    {"src":"/icon.svg","sizes":"any","type":"image/svg+xml","purpose":"any maskable"}
  ]
})rawliteral";

// ─── Home screen icon (SVG scales to any size) ────────────
const char ICON_SVG[] PROGMEM = R"rawliteral(<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 100 100">
<circle cx="50" cy="50" r="50" fill="#0a0a0f"/>
<circle cx="50" cy="50" r="44" fill="none" stroke="#00d4aa" stroke-width="5"/>
<polygon points="42,32 42,68 72,50" fill="#00d4aa"/>
</svg>)rawliteral";

void handleRoot() { httpSrv.send_P(200, "text/html; charset=utf-8", HTML); }
void handleManifest() { httpSrv.send_P(200, "application/manifest+json", MANIFEST); }
void handleIcon() { httpSrv.send_P(200, "image/svg+xml", ICON_SVG); }

void handleHttpStop()
{ // HTTP fallback stop (works if WebSocket is broken)
  hardStop();
  broadcast();
  httpSrv.send(200, "application/json", stateJSON());
}

// ════════════════════════════════════════════════════════════
//  SETUP
// ════════════════════════════════════════════════════════════

void setup()
{
  Serial.begin(115200);
  Serial.println("\n===========================");
  Serial.println("  Baby Swing Controller v2.1");
  Serial.println("===========================");

  pinMode(PIN_IN1, OUTPUT);
  pinMode(PIN_IN2, OUTPUT);
  ledcSetup(PWM_CH, PWM_FREQ, PWM_BITS);
  ledcAttachPin(PIN_ENA, PWM_CH);
  hardStop();

  // WiFi
  WiFi.setHostname(HOSTNAME);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  Serial.print("WiFi");
  for (int i = 0; WiFi.status() != WL_CONNECTED && i < 40; i++)
  {
    delay(500);
    Serial.print('.');
  }
  if (WiFi.status() != WL_CONNECTED)
  {
    Serial.println("\nFailed! Restarting in 3s...");
    delay(3000);
    ESP.restart();
  }
  Serial.printf("\nConnected!  http://%s\n", WiFi.localIP().toString().c_str());

  if (MDNS.begin(HOSTNAME))
    Serial.printf("mDNS ready: http://%s.local\n", HOSTNAME);

  httpSrv.on("/", handleRoot);
  httpSrv.on("/manifest.json", handleManifest);
  httpSrv.on("/icon.svg", handleIcon);
  httpSrv.on("/stop", handleHttpStop);
  httpSrv.begin();

  wsSrv.begin();
  wsSrv.onEvent(onWsEvent);

  g_lastAct = millis();
  Serial.println("Ready!");
}

// ════════════════════════════════════════════════════════════
//  LOOP
// ════════════════════════════════════════════════════════════

void loop()
{
  static ulong wifiCheck = 0;
  ulong now = millis();

  // WiFi watchdog — non-blocking, checked every 10 s
  if (now - wifiCheck > 10000)
  {
    wifiCheck = now;
    if (WiFi.status() != WL_CONNECTED)
    {
      Serial.println("[WiFi] Lost — reconnecting...");
      WiFi.reconnect();
    }
  }

  httpSrv.handleClient();
  wsSrv.loop();
  now = millis();

  // Safety watchdog: stop motor if no client active for WATCHDOG_MS.
  // PWA sends heartbeat every 15 s, so this only fires when every
  // device has closed the app or lost WiFi.
  if (g_dir != MSTOP && (now - g_lastAct > (ulong)WATCHDOG_MS))
  {
    Serial.println("[SAFETY] Watchdog — no clients — stopping motor");
    hardStop();
  }
  // No swing state machine needed: motor just keeps running in FWD.
  // The crank mechanism converts rotation to back-and-forth swing.
}
