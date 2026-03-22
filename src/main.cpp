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
#include <Preferences.h>
#include <DNSServer.h>
#include <HTTPClient.h>
#include <WiFiClientSecure.h>
#include <HTTPUpdate.h>
#include <Update.h>

// ─── Network config ───────────────────────────────────────────
// WiFi credentials are stored in NVS (set via AP setup portal).
const char *HOSTNAME = "babyswing"; // → http://babyswing.local
const char *AP_SSID  = "BabySwing-Setup"; // AP mode SSID (open)
// ─────────────────────────────────────────────────────────────

// ─── Firmware version & OTA URLs ─────────────────────────────
// Increment FW_VERSION each release. Commit version.txt with the
// same number to the main branch. Create a GitHub Release and
// upload firmware.bin as an asset named "firmware.bin".
const int   FW_VERSION     = 22;
const char *GITHUB_VER_URL = "https://raw.githubusercontent.com/matteotrachsel/motor-control-swing/main/version.txt";
const char *GITHUB_BIN_URL = "https://github.com/matteotrachsel/motor-control-swing/releases/latest/download/firmware.bin";
// ─── Manual /update page credentials ─────────────────────────
const char *OTA_USER = "admin";
const char *OTA_PASS = "babyswing"; // change to something personal
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

// ─── Run timer ───────────────────────────────────────────────
int  g_timerMins = 0;  // 0 = no timer; >0 = run this many minutes then stop
ulong g_timerEnd  = 0; // millis() when timer fires; 0 = no active timer

// ─── Kick-start (start boost) ─────────────────────────
int  g_kickPct = 0;    // boost % — 0 = off; typical: 60, 80, 100
int  g_kickMs  = 400;  // boost duration ms — typical: 200, 400, 600, 1000
ulong g_kickEnd = 0;   // millis() when kick pulse ends; 0 = not kicking

WebServer       httpSrv(80);
WebSocketsServer wsSrv(81);
Preferences     prefs;
DNSServer       dnsServer;
bool            g_apMode = false; // true = running WiFi config portal

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

void applyMotorRaw(MotorDir dir, int pct)
{
  pct = constrain(pct, 0, 100);
  g_dir = dir;
  g_spd = (dir == MSTOP) ? 0 : pct;
  if (dir == MFWD)      { digitalWrite(PIN_IN1, HIGH); digitalWrite(PIN_IN2, LOW); }
  else if (dir == MREV) { digitalWrite(PIN_IN1, LOW);  digitalWrite(PIN_IN2, HIGH); }
  else                  { digitalWrite(PIN_IN1, LOW);  digitalWrite(PIN_IN2, LOW); }
  ledcWrite(PWM_CH, (dir == MSTOP) ? 0 : map(pct, 0, 100, 0, 255));
  Serial.printf("[Kick] RAW FWD %d%%\n", pct);
}

void hardStop()
{
  g_swinging = false;
  g_timerEnd  = 0;
  g_kickEnd   = 0;
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
  ulong now2 = millis();
  int timerSec = -1;
  if (g_timerEnd != 0 && g_swinging)
    timerSec = (g_timerEnd > now2) ? (int)((g_timerEnd - now2) / 1000) : 0;
  char buf[280];
  snprintf(buf, sizeof(buf),
           "{\"swing\":%s,\"speed\":%d,\"dir\":\"%s\","
           "\"swingSpd\":%d,\"clients\":%d,"
           "\"timerMins\":%d,\"timerSec\":%d,"
           "\"kickPct\":%d,\"kickMs\":%d}",
           g_swinging ? "true" : "false",
           g_spd, dirStr(g_dir),
           g_swingSpd, g_clients,
           g_timerMins, timerSec,
           g_kickPct, g_kickMs);
  return String(buf);
}

void broadcast()
{
  String s = stateJSON();
  wsSrv.broadcastTXT(s);
}

void saveSettings()
{
  prefs.begin("swing", false);
  prefs.putInt("swingSpd",  g_swingSpd);
  prefs.putInt("timerMins", g_timerMins);
  prefs.putInt("kickPct",   g_kickPct);
  prefs.putInt("kickMs",    g_kickMs);
  prefs.end();
  Serial.println("[NVS] settings saved");
}

// ─── WiFi credential NVS helpers ─────────────────────────────

bool loadWifiCreds(String &ssid, String &pass)
{
  prefs.begin("wifi", true);
  ssid = prefs.getString("ssid", "");
  pass = prefs.getString("pass", "");
  prefs.end();
  return ssid.length() > 0;
}

void saveWifiCreds(const String &ssid, const String &pass)
{
  prefs.begin("wifi", false);
  prefs.putString("ssid", ssid);
  prefs.putString("pass", pass);
  prefs.end();
  Serial.println("[NVS] WiFi credentials saved");
}

void clearWifiCreds()
{
  prefs.begin("wifi", false);
  prefs.remove("ssid");
  prefs.remove("pass");
  prefs.end();
  Serial.println("[NVS] WiFi credentials cleared");
}

// ─── Helper: escape a string for JSON output ─────────────────
static String jsonEsc(const String &s)
{
  String r;
  r.reserve(s.length());
  for (size_t i = 0; i < s.length(); i++) {
    char c = s.charAt(i);
    if      (c == '"')  r += "\\\"";
    else if (c == '\\') r += "\\\\";
    else if (c < 0x20)  r += ' ';
    else                r += c;
  }
  return r;
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
    if (g_timerMins > 0)
      g_timerEnd = millis() + (ulong)g_timerMins * 60000UL;
    else
      g_timerEnd = 0;
    if (g_kickPct > 0 && g_kickPct > g_swingSpd)
    {
      applyMotorRaw(MFWD, g_kickPct);
      g_kickEnd = millis() + (ulong)g_kickMs;
      Serial.printf("[Kick] %d%% for %dms → run %d%%\n", g_kickPct, g_kickMs, g_swingSpd);
    }
    else
    {
      g_kickEnd = 0;
      applyMotor(MFWD, g_swingSpd);
    }
    broadcast();
    Serial.printf("[Swing] START  spd=%d%%  timer=%dmin\n", g_swingSpd, g_timerMins);
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
      saveSettings();
      broadcast();
    }
  }
  else if (cmd == "stop")
  {
    hardStop();
    broadcast();
  }
  else if (cmd == "set_timer")
  {
    String m = jval(p, "minutes");
    if (m.length())
    {
      g_timerMins = constrain(m.toInt(), 0, 240);
      if (g_swinging)
      {
        if (g_timerMins > 0)
          g_timerEnd = millis() + (ulong)g_timerMins * 60000UL;
        else
          g_timerEnd = 0;
      }
      saveSettings();
      broadcast();
      Serial.printf("[Timer] set to %d min\n", g_timerMins);
    }
  }
  else if (cmd == "set_kick")
  {
    String kPct = jval(p, "pct");
    String kMs  = jval(p, "ms");
    if (kPct.length()) g_kickPct = constrain(kPct.toInt(), 0, 100);
    if (kMs.length())  g_kickMs  = constrain(kMs.toInt(), 50, 2000);
    saveSettings();
    broadcast();
    Serial.printf("[Kick] set pct=%d  ms=%d\n", g_kickPct, g_kickMs);
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

/* Timer card */
.tbtns{display:flex;flex-wrap:wrap;gap:.45rem;margin-top:.6rem}
.tbtn{flex:1 1 calc(16.6% - .45rem);min-width:2.8rem;padding:.55rem .2rem;
  border:1px solid var(--bdr);border-radius:7px;background:var(--srf);
  color:var(--dim);font-size:.68rem;font-weight:600;cursor:pointer;
  -webkit-tap-highlight-color:transparent;transition:all .15s;text-align:center}
.tbtn:active{transform:scale(.93)}
.tbtn.ta{border-color:var(--acc);background:#00d4aa14;color:var(--acc)}
.tcd{margin-top:.65rem;font-size:.78rem;font-weight:600;color:var(--acc);
  text-align:center;letter-spacing:.05em;min-height:1.1em}

/* Kick-start card */
.krow{display:flex;flex-wrap:wrap;gap:.45rem;margin-top:.5rem}
.kbtn{flex:1 1 calc(25% - .45rem);min-width:3rem;padding:.55rem .2rem;
  border:1px solid var(--bdr);border-radius:7px;background:var(--srf);
  color:var(--dim);font-size:.68rem;font-weight:600;cursor:pointer;
  -webkit-tap-highlight-color:transparent;transition:all .15s;text-align:center}
.kbtn:active{transform:scale(.93)}
.kbtn.ka{border-color:var(--acc);background:#00d4aa14;color:var(--acc)}
.klbl{font-size:.6rem;color:var(--dim);margin:.5rem 0 .3rem}
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

<div class="card">
  <div class="slab">Timer&nbsp;<b id="tv">Off</b></div>
  <div class="tbtns">
    <button class="tbtn" id="tb0"  onclick="setTimer(0)">Off</button>
    <button class="tbtn" id="tb15" onclick="setTimer(15)">15m</button>
    <button class="tbtn" id="tb30" onclick="setTimer(30)">30m</button>
    <button class="tbtn" id="tb45" onclick="setTimer(45)">45m</button>
    <button class="tbtn" id="tb60" onclick="setTimer(60)">1h</button>
    <button class="tbtn" id="tb90" onclick="setTimer(90)">90m</button>
  </div>
  <div class="tcd" id="tcd"></div>
</div>

<details class="man">
  <summary>Manual control (setup / maintenance)</summary>
  <div class="card" style="margin-top:.8rem">
    <div class="slab">Start boost&nbsp;<b id="kv">Off</b></div>
    <div class="klbl">Boost level</div>
    <div class="krow">
      <button class="kbtn" id="kb0"   onclick="setKick(0,null)">Off</button>
      <button class="kbtn" id="kb60"  onclick="setKick(60,null)">60%</button>
      <button class="kbtn" id="kb80"  onclick="setKick(80,null)">80%</button>
      <button class="kbtn" id="kb100" onclick="setKick(100,null)">100%</button>
    </div>
    <div class="klbl">Duration</div>
    <div class="krow">
      <button class="kbtn" id="kd200"  onclick="setKick(null,200)">200ms</button>
      <button class="kbtn" id="kd400"  onclick="setKick(null,400)">400ms</button>
      <button class="kbtn" id="kd600"  onclick="setKick(null,600)">600ms</button>
      <button class="kbtn" id="kd1000" onclick="setKick(null,1000)">1s</button>
    </div>
  </div>
  <div class="mrow">
    <button class="mb" id="mf" onclick="manual('forward')">&#9650; Fwd</button>
    <button class="mb"         onclick="send({cmd:'stop'})">&#9632; Stop</button>
    <button class="mb" id="mr" onclick="manual('reverse')">&#9660; Rev</button>
  </div>
</details>

<button class="estop" onclick="send({cmd:'stop'})">&#9899; Emergency Stop</button>

<div style="display:flex;justify-content:space-between;width:100%;padding:0 .2rem">
  <a href="/update"     style="font-size:.6rem;color:var(--dim);text-decoration:none">Firmware update</a>
  <a href="/reset-wifi" style="font-size:.6rem;color:var(--dim);text-decoration:none">Reset WiFi</a>
</div>

<script>
var S={swing:false,speed:0,dir:'stop',swingSpd:40,clients:0,timerMins:0,timerSec:-1,kickPct:0,kickMs:400};
var ws,rt,ht,rd=1000,deferredPrompt=null;
var cdInt=null; // countdown interval

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

  // Timer buttons highlight
  var tids=[0,15,30,45,60,90];
  tids.forEach(function(v){
    var el=document.getElementById('tb'+v);
    if(el) el.className='tbtn'+(s.timerMins===v?' ta':'');
  });
  document.getElementById('tv').textContent=s.timerMins>0?(s.timerMins+'m'):'Off';
  // Countdown — sync from server, then tick locally
  if(s.swing&&s.timerSec>=0){
    startCountdown(s.timerSec);
  } else {
    stopCountdown();
  }

  // Kick-start button highlights
  [0,60,80,100].forEach(function(v){
    var el=document.getElementById('kb'+v);
    if(el)el.className='kbtn'+(s.kickPct===v?' ka':'');
  });
  [200,400,600,1000].forEach(function(v){
    var el=document.getElementById('kd'+v);
    if(el)el.className='kbtn'+(s.kickMs===v?' ka':'');
  });
  document.getElementById('kv').textContent=
    s.kickPct>0?(s.kickPct+'% / '+s.kickMs+'ms'):'Off';

  document.getElementById('dot').className='dot ok';
  document.getElementById('ct').textContent=
    s.clients+' device'+(s.clients!==1?'s':'')+' connected';
}

/* ── Timer ── */
function setTimer(m){
  send({cmd:'set_timer',minutes:m});
}
function setKick(pct,ms){
  send({cmd:'set_kick',
        pct:pct!==null?pct:S.kickPct,
        ms:ms!==null?ms:S.kickMs});
}
function fmtSec(s){
  var h=Math.floor(s/3600),m=Math.floor((s%3600)/60),sec=s%60;
  if(h>0) return h+'h '+String(m).padStart(2,'0')+'m '+String(sec).padStart(2,'0')+'s';
  return String(m).padStart(2,'0')+'m '+String(sec).padStart(2,'0')+'s';
}
function startCountdown(secs){
  clearInterval(cdInt);
  var rem=secs;
  function tick(){
    var el=document.getElementById('tcd');
    if(rem<=0){el.textContent='';clearInterval(cdInt);return;}
    el.textContent='⏱ '+fmtSec(rem)+' remaining';
    rem--;
  }
  tick();
  cdInt=setInterval(tick,1000);
}
function stopCountdown(){
  clearInterval(cdInt);
  document.getElementById('tcd').textContent='';
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

// ─── WiFi setup portal HTML (served in AP mode) ───────────
const char AP_HTML[] PROGMEM = R"rawliteral(<!DOCTYPE html>
<html lang="en">
<head>
<meta charset="UTF-8">
<meta name="viewport" content="width=device-width,initial-scale=1,user-scalable=no">
<meta name="theme-color" content="#00d4aa">
<title>Baby Swing – WiFi Setup</title>
<style>
*{margin:0;padding:0;box-sizing:border-box}
:root{--bg:#0a0a0f;--srf:#13131a;--bdr:#1e1e2a;--txt:#e0e0e8;--dim:#6b6b80;
      --acc:#00d4aa;--red:#ff4466}
html,body{min-height:100%;background:var(--bg);color:var(--txt);
  font-family:system-ui,-apple-system,sans-serif}
body{display:flex;flex-direction:column;align-items:center;
  padding:2rem 1rem;max-width:420px;margin:0 auto;gap:1.4rem}
.hdr-t{font-size:1rem;letter-spacing:.12em;text-transform:uppercase;
  color:var(--acc);font-weight:700;align-self:flex-start}
.sub{font-size:.72rem;color:var(--dim);align-self:flex-start;margin-top:-.8rem}
.card{width:100%;background:var(--srf);border:1px solid var(--bdr);
  border-radius:12px;padding:1.2rem;display:flex;flex-direction:column;gap:.9rem}
label{font-size:.68rem;text-transform:uppercase;letter-spacing:.08em;color:var(--dim)}
.irow{display:flex;gap:.5rem;align-items:stretch}
input[type=text],input[type=password]{flex:1;background:var(--bg);
  border:1px solid var(--bdr);border-radius:7px;padding:.65rem .8rem;
  color:var(--txt);font-size:.88rem;outline:none;width:100%}
input:focus{border-color:var(--acc)}
.scan-btn,.show-btn{padding:.65rem .9rem;border:1px solid var(--bdr);
  border-radius:7px;background:var(--bg);color:var(--dim);font-size:.73rem;
  font-weight:700;cursor:pointer;white-space:nowrap;letter-spacing:.05em}
.scan-btn:active,.show-btn:active{background:var(--bdr)}
.scan-btn:disabled{opacity:.45;cursor:default}
select#netlist{width:100%;background:var(--bg);border:1px solid var(--bdr);
  border-radius:7px;padding:.45rem .5rem;color:var(--txt);font-size:.82rem;
  display:none;margin-top:.4rem}
.save-btn{width:100%;padding:1rem;border:none;border-radius:10px;
  background:var(--acc);color:#0a0a0f;font-size:.9rem;font-weight:700;
  letter-spacing:.1em;text-transform:uppercase;cursor:pointer}
.save-btn:active{opacity:.8}
.save-btn:disabled{opacity:.4;cursor:default}
#msg{font-size:.8rem;text-align:center;min-height:1.2em;line-height:1.5}
.ok{color:var(--acc)}.err{color:var(--red)}
</style>
</head>
<body>
<span class="hdr-t">Baby Swing</span>
<span class="sub">One-time WiFi setup &mdash; credentials are stored on the device</span>
<div class="card">
  <div>
    <label>WiFi Network (SSID)</label>
    <div class="irow" style="margin-top:.4rem">
      <input type="text" id="ssid" placeholder="Network name"
        autocomplete="off" autocorrect="off" autocapitalize="none" spellcheck="false">
      <button class="scan-btn" id="scanbtn" onclick="doScan()">Scan</button>
    </div>
    <select id="netlist" size="5"
      onchange="document.getElementById('ssid').value=this.value"></select>
  </div>
  <div>
    <label>Password</label>
    <div class="irow" style="margin-top:.4rem">
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
      o.textContent=n.ssid+' ('+n.rssi+'dBm'+(n.secure?' \uD83D\uDD12':'')+')';
      sel.appendChild(o);
    });
    sel.style.display=nets.length?'block':'none';
    btn.textContent='Scan';btn.disabled=false;
  }).catch(function(){
    btn.textContent='Scan';btn.disabled=false;
    showMsg('Scan failed — enter SSID manually','err');
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
  showMsg('Saving\u2026','ok');
  var body='ssid='+encodeURIComponent(ssid)+'&pass='+encodeURIComponent(pass);
  fetch('/save',{method:'POST',
    headers:{'Content-Type':'application/x-www-form-urlencoded'},
    body:body})
  .then(function(){
    showMsg('Saved! The device will restart and connect to \u201C'+ssid+
      '\u201D.\n\nReconnect your phone to that WiFi network, then open\nhttp://babyswing.local','ok');
  }).catch(function(){
    showMsg('Saved! Device is restarting\u2026','ok');
  });
}
</script>
</body>
</html>
)rawliteral";

void handleRoot() { httpSrv.send_P(200, "text/html; charset=utf-8", HTML); }
void handleManifest() { httpSrv.send_P(200, "application/manifest+json", MANIFEST); }
void handleIcon() { httpSrv.send_P(200, "image/svg+xml", ICON_SVG); }

// ════════════════════════════════════════════════════════════
//  AUTO OTA — check GitHub on boot, flash if newer version found
// ════════════════════════════════════════════════════════════

void checkAndApplyOTA()
{
  Serial.printf("[OTA] Checking for update (local v%d)…\n", FW_VERSION);

  WiFiClientSecure verClient;
  verClient.setInsecure(); // skip cert validation — fine for home use
  verClient.setTimeout(5);

  HTTPClient http;
  http.begin(verClient, GITHUB_VER_URL);
  http.setTimeout(5000);
  int code = http.GET();

  if (code != 200) {
    Serial.printf("[OTA] Version check failed (HTTP %d) — skipping\n", code);
    http.end();
    return;
  }

  String body = http.getString();
  http.end();
  body.trim();
  int remoteVer = body.toInt();

  Serial.printf("[OTA] Local v%d  Remote v%d\n", FW_VERSION, remoteVer);

  if (remoteVer <= FW_VERSION) {
    Serial.println("[OTA] Already up to date");
    return;
  }

  Serial.printf("[OTA] Updating to v%d — stopping motor and downloading…\n", remoteVer);
  hardStop(); // safety: stop motor before any flash write

  WiFiClientSecure dlClient;
  dlClient.setInsecure();

  httpUpdate.setLedPin(LED_BUILTIN, LOW); // blink built-in LED during flash
  httpUpdate.rebootOnUpdate(true);        // auto-reboot on success

  t_httpUpdate_return ret = httpUpdate.update(dlClient, GITHUB_BIN_URL);

  // Only reached if update FAILED (success causes immediate reboot)
  switch (ret) {
    case HTTP_UPDATE_FAILED:
      Serial.printf("[OTA] Failed (%d): %s\n",
                    httpUpdate.getLastError(),
                    httpUpdate.getLastErrorString().c_str());
      break;
    case HTTP_UPDATE_NO_UPDATES:
      Serial.println("[OTA] Server reports no update");
      break;
    default:
      break;
  }
  // On failure: continue with existing firmware — swing UI will start normally
}

// ════════════════════════════════════════════════════════════
//  MANUAL OTA — web upload page at /update
// ════════════════════════════════════════════════════════════

const char OTA_HTML[] PROGMEM = R"rawliteral(<!DOCTYPE html>
<html lang="en">
<head>
<meta charset="UTF-8">
<meta name="viewport" content="width=device-width,initial-scale=1,user-scalable=no">
<meta name="theme-color" content="#00d4aa">
<title>Baby Swing – Firmware Update</title>
<style>
*{margin:0;padding:0;box-sizing:border-box}
:root{--bg:#0a0a0f;--srf:#13131a;--bdr:#1e1e2a;--txt:#e0e0e8;--dim:#6b6b80;
      --acc:#00d4aa;--red:#ff4466}
html,body{min-height:100%;background:var(--bg);color:var(--txt);
  font-family:system-ui,-apple-system,sans-serif}
body{display:flex;flex-direction:column;align-items:center;
  padding:2rem 1rem;max-width:420px;margin:0 auto;gap:1.4rem}
.hdr-t{font-size:1rem;letter-spacing:.12em;text-transform:uppercase;
  color:var(--acc);font-weight:700;align-self:flex-start}
.sub{font-size:.72rem;color:var(--dim);align-self:flex-start;margin-top:-.8rem}
.card{width:100%;background:var(--srf);border:1px solid var(--bdr);
  border-radius:12px;padding:1.2rem;display:flex;flex-direction:column;gap:.9rem}
.ver{font-size:.75rem;color:var(--dim)}
.ver b{color:var(--txt)}
label.pick{display:flex;flex-direction:column;gap:.4rem;
  font-size:.68rem;text-transform:uppercase;letter-spacing:.08em;color:var(--dim);
  cursor:pointer}
input[type=file]{background:var(--bg);border:1px solid var(--bdr);
  border-radius:7px;padding:.6rem .8rem;color:var(--txt);font-size:.82rem;width:100%}
.up-btn{width:100%;padding:1rem;border:none;border-radius:10px;
  background:var(--acc);color:#0a0a0f;font-size:.9rem;font-weight:700;
  letter-spacing:.1em;text-transform:uppercase;cursor:pointer}
.up-btn:active{opacity:.8}
.up-btn:disabled{opacity:.4;cursor:default}
.bar-wrap{width:100%;height:10px;background:var(--bdr);border-radius:5px;
  overflow:hidden;display:none}
.bar{height:100%;width:0%;background:var(--acc);border-radius:5px;
  transition:width .2s}
#msg{font-size:.8rem;text-align:center;min-height:1.2em;line-height:1.5}
.ok{color:var(--acc)}.err{color:var(--red)}
a.back{font-size:.65rem;color:var(--dim);text-decoration:none}
a.back:hover{color:var(--txt)}
</style>
</head>
<body>
<span class="hdr-t">Baby Swing</span>
<span class="sub">Manual firmware update &mdash; auto-update runs on every boot</span>
<div class="card">
  <div class="ver">Current firmware: <b>v__VER__</b></div>
  <label class="pick">
    Select firmware file (.bin)
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
  showMsg('Uploading\u2026','ok');
  var fd=new FormData();
  fd.append('firmware',f,f.name);
  var xhr=new XMLHttpRequest();
  xhr.upload.onprogress=function(e){
    if(e.lengthComputable){
      var p=Math.round(e.loaded/e.total*100);
      bar.style.width=p+'%';
      showMsg('Uploading\u2026 '+p+'%','ok');
    }
  };
  xhr.onload=function(){
    bar.style.width='100%';
    if(xhr.status===200&&xhr.responseText==='OK'){
      showMsg('Flashed successfully \u2014 device is rebooting\u2026','ok');
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
)rawliteral";

void handleOTAPage()
{
  if (!httpSrv.authenticate(OTA_USER, OTA_PASS)) {
    return httpSrv.requestAuthentication();
  }
  // Inject current firmware version into the HTML
  String html = String(OTA_HTML);
  html.replace("__VER__", String(FW_VERSION));
  httpSrv.send(200, "text/html; charset=utf-8", html);
}

void handleOTAUploadDone()
{
  if (!httpSrv.authenticate(OTA_USER, OTA_PASS)) {
    return httpSrv.requestAuthentication();
  }
  if (Update.hasError()) {
    String err = "FAIL: " + String(Update.errorString());
    httpSrv.send(500, "text/plain", err);
    Serial.printf("[OTA] Manual upload failed: %s\n", Update.errorString());
  } else {
    httpSrv.send(200, "text/plain", "OK");
    Serial.println("[OTA] Manual upload complete — rebooting");
    delay(500);
    ESP.restart();
  }
}

void handleOTAUploadData()
{
  if (!httpSrv.authenticate(OTA_USER, OTA_PASS)) {
    return httpSrv.requestAuthentication();
  }
  HTTPUpload &upload = httpSrv.upload();
  if (upload.status == UPLOAD_FILE_START) {
    Serial.printf("[OTA] Manual upload start: %s\n", upload.filename.c_str());
    hardStop(); // stop motor before touching flash
    if (!Update.begin(UPDATE_SIZE_UNKNOWN)) {
      Update.printError(Serial);
    }
  } else if (upload.status == UPLOAD_FILE_WRITE) {
    if (Update.write(upload.buf, upload.currentSize) != upload.currentSize) {
      Update.printError(Serial);
    }
  } else if (upload.status == UPLOAD_FILE_END) {
    if (Update.end(true)) {
      Serial.printf("[OTA] Manual upload done: %u bytes\n", upload.totalSize);
    } else {
      Update.printError(Serial);
    }
  }
}

void handleHttpStop()
{ // HTTP fallback stop (works if WebSocket is broken)
  hardStop();
  broadcast();
  httpSrv.send(200, "application/json", stateJSON());
}

// ─── /reset-wifi  (normal mode) ──────────────────────────────
// Clears stored WiFi credentials and reboots into AP setup mode.
void handleResetWifi()
{
  clearWifiCreds();
  httpSrv.send(200, "text/html; charset=utf-8",
    "<html><head><meta charset='UTF-8'>"
    "<meta name='viewport' content='width=device-width,initial-scale=1'>"
    "<meta name='theme-color' content='#00d4aa'>"
    "<style>body{background:#0a0a0f;color:#e0e0e8;"
    "font-family:system-ui,sans-serif;text-align:center;padding:2rem}"
    "h2{color:#00d4aa}p{margin-top:1rem;color:#6b6b80;font-size:.85rem}</style></head>"
    "<body><h2>WiFi Reset</h2>"
    "<p>Credentials cleared. Device is restarting into setup mode.</p>"
    "<p>Connect to WiFi <b>BabySwing-Setup</b> to reconfigure.</p>"
    "</body></html>");
  delay(1500);
  ESP.restart();
}

// ─── AP mode handlers ────────────────────────────────────────

void handleAPRoot()
{
  httpSrv.send_P(200, "text/html; charset=utf-8", AP_HTML);
}

void handleAPScan()
{
  // Scan for nearby networks (blocking ~2-3 s)
  WiFi.mode(WIFI_AP_STA); // need STA side active to scan
  int n = WiFi.scanNetworks(false, false);
  String json = "[";
  for (int i = 0; i < n; i++) {
    if (i) json += ",";
    json += "{\"ssid\":\"" + jsonEsc(WiFi.SSID(i)) + "\","
            "\"rssi\":"   + WiFi.RSSI(i) + ","
            "\"secure\":"+ (WiFi.encryptionType(i) != WIFI_AUTH_OPEN ? "true" : "false") + "}";
  }
  json += "]";
  WiFi.scanDelete();
  WiFi.mode(WIFI_AP); // restore AP-only
  httpSrv.send(200, "application/json", json);
}

void handleAPSave()
{
  String ssid = httpSrv.arg("ssid");
  String pass = httpSrv.arg("pass");
  ssid.trim();

  if (ssid.length() == 0 || ssid.length() > 32 || pass.length() > 64) {
    httpSrv.send(400, "text/plain", "Invalid input");
    return;
  }

  saveWifiCreds(ssid, pass);
  httpSrv.send(200, "text/plain", "OK");
  Serial.printf("[AP] Credentials saved for SSID: %s — restarting\n", ssid.c_str());
  delay(800); // let HTTP response reach the browser
  ESP.restart();
}

// ════════════════════════════════════════════════════════════
//  SETUP
// ════════════════════════════════════════════════════════════

// ─── Start: AP config portal ─────────────────────────────────
void startAPMode()
{
  g_apMode = true;
  WiFi.disconnect(true);
  delay(100);
  WiFi.mode(WIFI_AP);
  WiFi.softAP(AP_SSID); // open network — no password required

  // Captive portal: redirect all DNS queries to our IP
  dnsServer.setErrorReplyCode(DNSReplyCode::NoError);
  dnsServer.start(53, "*", WiFi.softAPIP());

  // Register AP routes
  httpSrv.on("/",     HTTP_GET,  handleAPRoot);
  httpSrv.on("/scan", HTTP_GET,  handleAPScan);
  httpSrv.on("/save", HTTP_POST, handleAPSave);
  // Catch-all: redirect to setup page (makes captive portal work on iOS/Android)
  httpSrv.onNotFound([]() {
    httpSrv.sendHeader("Location", "http://192.168.4.1/");
    httpSrv.send(302, "text/plain", "");
  });
  httpSrv.begin();

  Serial.println("[AP] Started portal: BabySwing-Setup");
  Serial.printf("[AP] Connect to WiFi '%s' then open http://192.168.4.1\n", AP_SSID);
  Serial.printf("[AP] (or http://%s)\n", WiFi.softAPIP().toString().c_str());
}

// ─── Start: normal swing mode ────────────────────────────────
void startNormalMode()
{
  Serial.printf("[WiFi] Connected!  http://%s\n", WiFi.localIP().toString().c_str());

  // Check GitHub for a newer firmware version; flash and reboot if found.
  // On any network error the function returns immediately and boot continues.
  checkAndApplyOTA();

  if (MDNS.begin(HOSTNAME))
    Serial.printf("[mDNS] http://%s.local\n", HOSTNAME);

  httpSrv.on("/",              handleRoot);
  httpSrv.on("/manifest.json", handleManifest);
  httpSrv.on("/icon.svg",      handleIcon);
  httpSrv.on("/stop",          handleHttpStop);
  httpSrv.on("/reset-wifi",    handleResetWifi);
  httpSrv.on("/update", HTTP_GET,  handleOTAPage);
  httpSrv.on("/update", HTTP_POST, handleOTAUploadDone, handleOTAUploadData);
  httpSrv.begin();

  wsSrv.begin();
  wsSrv.onEvent(onWsEvent);

  g_lastAct = millis();
  Serial.println("[Normal] Ready!");
}

// ─── Arduino setup ───────────────────────────────────────────
void setup()
{
  Serial.begin(115200);
  Serial.println("\n===========================");
  Serial.println("  Baby Swing Controller v2.1");
  Serial.println("===========================");

  // Hardware init — motor stopped from the very start
  pinMode(PIN_IN1, OUTPUT);
  pinMode(PIN_IN2, OUTPUT);
  ledcSetup(PWM_CH, PWM_FREQ, PWM_BITS);
  ledcAttachPin(PIN_ENA, PWM_CH);
  hardStop();

  // Load persisted swing settings from NVS
  prefs.begin("swing", true);
  g_swingSpd  = prefs.getInt("swingSpd",  g_swingSpd);
  g_timerMins = prefs.getInt("timerMins", g_timerMins);
  g_kickPct   = prefs.getInt("kickPct",   g_kickPct);
  g_kickMs    = prefs.getInt("kickMs",    g_kickMs);
  prefs.end();
  Serial.printf("[NVS] swingSpd=%d  timerMins=%d  kickPct=%d  kickMs=%d\n",
                g_swingSpd, g_timerMins, g_kickPct, g_kickMs);

  // ── WiFi ──────────────────────────────────────────────────
  String ssid, pass;
  bool hasCreds = loadWifiCreds(ssid, pass);

  if (!hasCreds) {
    Serial.println("[WiFi] No credentials stored → starting AP setup portal");
    startAPMode();
    return;
  }

  // Attempt to connect with stored credentials
  WiFi.mode(WIFI_STA);
  WiFi.setHostname(HOSTNAME);
  WiFi.begin(ssid.c_str(), pass.c_str());
  Serial.printf("[WiFi] Connecting to \"%s\"", ssid.c_str());

  const int CONNECT_TIMEOUT_MS = 15000; // 15 s
  ulong t0 = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - t0 < CONNECT_TIMEOUT_MS) {
    delay(500);
    Serial.print('.');
  }
  Serial.println();

  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("[WiFi] Connection failed → starting AP setup portal");
    startAPMode();
    return;
  }

  startNormalMode();
}

// ════════════════════════════════════════════════════════════
//  LOOP
// ════════════════════════════════════════════════════════════

void loop()
{
  // ── AP mode: only serve the config portal ─────────────────
  if (g_apMode) {
    dnsServer.processNextRequest();
    httpSrv.handleClient();
    return;
  }

  // ── Normal mode ────────────────────────────────────────────
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

  // Run-timer: stop motor when the user-set duration elapses.
  // Motor keeps running after phone disconnects until timer fires.
  if (g_swinging && g_timerEnd != 0 && now >= g_timerEnd)
  {
    Serial.println("[Timer] Duration elapsed — stopping motor");
    hardStop();
    broadcast();
  }
  // Kick-start: drop from boost PWM to run speed when pulse window ends.
  if (g_kickEnd != 0 && now >= g_kickEnd)
  {
    g_kickEnd = 0;
    if (g_swinging)
    {
      applyMotor(MFWD, g_swingSpd);
      Serial.printf("[Kick] done — running at %d%%\n", g_swingSpd);
    }
  }
  // No swing state machine needed: motor just keeps running in FWD.
  // The crank mechanism converts rotation to back-and-forth swing.
}
