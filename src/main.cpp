/*
 * ============================================================
 *  ESP32 RockBox Controller
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
 *    http://rockbox.local   (or the IP shown in Serial Monitor)
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
const char *HOSTNAME = "rockbox";      // → http://rockbox.local
const char *AP_SSID = "RockBox-Setup"; // AP mode SSID (open)
// ─────────────────────────────────────────────────────────────

// ─── Firmware version & OTA URLs ─────────────────────────────
// Increment FW_VERSION each release. Commit version.txt with the
// same number to the master branch. Create a GitHub Release and
// upload firmware.bin as an asset named "firmware.bin".
const int FW_VERSION = 32;
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

// ─── Physical toggle button ──────────────────────────────────
const int PIN_BTN = 18; // INPUT_PULLUP — connect one pin pair to GND

// ─── PWM ─────────────────────────────────────────────────────
const int PWM_CH = 0;
const int PWM_FREQ = 5000;
const int PWM_BITS = 8; // 0–255

// ─── Safety limits ───────────────────────────────────────────
const int MAX_SPEED_PCT = 100; // hard cap — never exceeded

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
int g_timerMins = 0;  // 0 = no timer; >0 = run this many minutes then stop
ulong g_timerEnd = 0; // millis() when timer fires; 0 = no active timer

// ─── Kick-start (start boost) ─────────────────────────
int g_kickPct = 0;   // boost % — 0 = off; typical: 60, 80, 100
int g_kickMs = 400;  // boost duration ms — typical: 200, 400, 600, 1000
ulong g_kickEnd = 0; // millis() when kick pulse ends; 0 = not kicking

// ─── Rocking pattern ──────────────────────────────────
// "steady" = constant speed
// "wave"   = sinusoidal speed variation (rhythmic)
// "lull"   = gradual slowdown to help baby fall asleep
String g_pattern = "steady";
float  g_wavePhase  = 0.0f;  // advances each pattern tick
float  g_lullFactor = 1.0f;  // 1.0 → 0.5 over ~5 min

WebServer httpSrv(80);
WebSocketsServer wsSrv(81);
Preferences prefs;
DNSServer dnsServer;
bool g_apMode = false; // true = running WiFi config portal

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
  Serial.printf("[Kick] RAW FWD %d%%\n", pct);
}

void hardStop()
{
  g_swinging = false;
  g_timerEnd = 0;
  g_kickEnd = 0;
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
  char buf[320];
  snprintf(buf, sizeof(buf),
           "{\"swing\":%s,\"speed\":%d,\"dir\":\"%s\","
           "\"swingSpd\":%d,\"clients\":%d,"
           "\"timerMins\":%d,\"timerSec\":%d,"
           "\"kickPct\":%d,\"kickMs\":%d,"
           "\"pattern\":\"%s\","
           "\"fw\":%d}",
           g_swinging ? "true" : "false",
           g_spd, dirStr(g_dir),
           g_swingSpd, g_clients,
           g_timerMins, timerSec,
           g_kickPct, g_kickMs,
           g_pattern.c_str(),
           FW_VERSION);
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
  prefs.putInt("swingSpd", g_swingSpd);
  prefs.putInt("timerMins", g_timerMins);
  prefs.putInt("kickPct", g_kickPct);
  prefs.putInt("kickMs", g_kickMs);
  prefs.putString("pattern", g_pattern);
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
  for (size_t i = 0; i < s.length(); i++)
  {
    char c = s.charAt(i);
    if (c == '"')
      r += "\\\"";
    else if (c == '\\')
      r += "\\\\";
    else if (c < 0x20)
      r += ' ';
    else
      r += c;
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
    g_wavePhase  = 0.0f;   // reset pattern state
    g_lullFactor = 1.0f;
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
      if (g_swinging)
        applyMotor(MFWD, g_swingSpd);
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
    String kMs = jval(p, "ms");
    if (kPct.length())
      g_kickPct = constrain(kPct.toInt(), 0, 100);
    if (kMs.length())
      g_kickMs = constrain(kMs.toInt(), 50, 2000);
    saveSettings();
    broadcast();
    Serial.printf("[Kick] set pct=%d  ms=%d\n", g_kickPct, g_kickMs);
  }
  else if (cmd == "set_pattern")
  {
    String pat = jval(p, "pattern");
    if (pat == "wave" || pat == "steady" || pat == "lull")
    {
      g_pattern    = pat;
      g_wavePhase  = 0.0f;
      g_lullFactor = 1.0f;
      saveSettings();
      broadcast();
      Serial.printf("[Pattern] set to %s\n", g_pattern.c_str());
    }
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
<meta name="theme-color" content="#F2EEE6" id="tmeta">
<meta name="apple-mobile-web-app-capable" content="yes">
<meta name="apple-mobile-web-app-status-bar-style" content="default" id="astat">
<meta name="apple-mobile-web-app-title" content="RockBox">
<link rel="manifest" href="/manifest.json">
<title>RockBox</title>
<link rel="preconnect" href="https://fonts.googleapis.com">
<link rel="preconnect" href="https://fonts.gstatic.com" crossorigin>
<link href="https://fonts.googleapis.com/css2?family=Fraunces:opsz,wght@9..144,300;9..144,400&family=Instrument+Sans:wght@400;500;600&family=JetBrains+Mono:wght@400;500&display=swap" rel="stylesheet">
<style>
*{margin:0;padding:0;box-sizing:border-box}
:root{--bg:#F2EEE6;--srf:#FAF8F3;--srf2:#ECE7DC;--line:#DED6C6;--ink:#2B3A34;--ink2:#5C6A63;--ink3:#8A9489;--sage:#8FA89A;--saged:#6B8A7A;--stop:#B84A3B;--stopd:#9A3A2D}
.dk{--bg:#1C211F;--srf:#242A27;--srf2:#2E3532;--line:#3A413D;--ink:#EDE8DC;--ink2:#AAB2AD;--ink3:#7A8580;--sage:#A9C2B2;--saged:#8FA89A;--stop:#D45A4B;--stopd:#B84A3B}
html,body{height:100%;background:var(--bg);color:var(--ink);font-family:'Instrument Sans',system-ui,sans-serif;overscroll-behavior:none}
body{display:flex;flex-direction:column;max-width:420px;margin:0 auto;min-height:100vh;padding-bottom:112px}
#ib{margin:8px 16px 0;background:var(--srf);border:1px solid var(--line);border-radius:14px;padding:12px 16px;display:flex;align-items:center;justify-content:space-between;gap:.8rem;font-size:12px;color:var(--ink2)}
#ib.h{display:none}
.ibtn{padding:6px 12px;border:1px solid var(--saged);border-radius:8px;background:transparent;color:var(--saged);font-size:11px;font-weight:600;cursor:pointer;font-family:'Instrument Sans',sans-serif}
.hdr{padding:10px 16px 0}
.topbar{display:flex;align-items:center;justify-content:space-between;padding:10px 0 8px}
.wm{display:flex;align-items:baseline;gap:6px}
.wm-n{font-family:'Fraunces',serif;font-size:22px;font-weight:400;color:var(--ink);letter-spacing:-.5px}
.wm-v{font-family:'JetBrains Mono',monospace;font-size:9px;color:var(--ink3);letter-spacing:1.5px;text-transform:uppercase}
.dtag{display:flex;align-items:center;gap:6px;padding:4px 10px;border-radius:999px;background:var(--srf);border:1px solid var(--line)}
.ddot{width:6px;height:6px;border-radius:50%;background:var(--ink3);transition:background .3s}
.ddot.on{background:var(--sage)}
.dlbl{font-family:'JetBrains Mono',monospace;font-size:10px;color:var(--ink2);letter-spacing:.8px}
.stbar{display:flex;justify-content:space-between;align-items:center;padding:10px 18px;border-top:1px solid var(--line);border-bottom:1px solid var(--line);margin-top:4px}
.sc{display:flex;align-items:center;gap:6px;font-size:12px;color:var(--ink2)}
.mdot{width:6px;height:6px;border-radius:50%;background:var(--sage)}
.mlbl{letter-spacing:1.2px;text-transform:uppercase;font-size:11px}
.dw{padding:18px 16px 8px;display:flex;justify-content:center;position:relative}
#dsvg{display:block;touch-action:none;user-select:none;cursor:grab}
#dsvg:active{cursor:grabbing}
.dc{position:absolute;inset:0;display:flex;flex-direction:column;align-items:center;justify-content:center;pointer-events:none}
.ds{font-family:'JetBrains Mono',monospace;font-size:11px;color:var(--ink3);letter-spacing:2px;text-transform:uppercase;margin-bottom:4px}
.dv{font-family:'Fraunces',serif;font-size:80px;font-weight:300;color:var(--ink);line-height:1;letter-spacing:-2px}
.du{font-family:'Instrument Sans',sans-serif;font-size:11px;color:var(--ink3);letter-spacing:3px;text-transform:uppercase;margin-top:6px}
.sdb{position:absolute;top:50%;transform:translateY(-50%);width:34px;height:34px;border-radius:50%;background:var(--srf);border:1px solid var(--line);cursor:pointer;display:flex;align-items:center;justify-content:center}
.sdb.L{left:20px}.sdb.R{right:20px}
.pw{padding:0 16px 12px;display:flex;justify-content:center}
.pbtn{padding:7px 16px;border-radius:999px;background:var(--srf);color:var(--ink);border:1px solid var(--line);font-family:'Instrument Sans',sans-serif;font-size:12px;letter-spacing:1.5px;text-transform:uppercase;cursor:pointer;display:flex;align-items:center;gap:8px}
.pi{width:6px;height:6px;background:var(--ink2);border-radius:1px;transition:border-radius .2s,background .2s}
.pi.on{border-radius:50%;background:var(--sage)}
.pnls{padding:4px 16px 0}
.slbl{font-family:'Instrument Sans',sans-serif;font-size:11px;letter-spacing:2.2px;text-transform:uppercase;color:var(--ink3);margin-bottom:10px;padding:0 2px}
.pgrd{display:grid;grid-template-columns:1fr 1fr 1fr;gap:8px;margin-bottom:16px}
.ptbtn{background:var(--bg);border:1px solid var(--line);border-radius:14px;padding:12px 10px;text-align:left;cursor:pointer;transition:all .18s ease}
.ptbtn.on{background:var(--srf);border-color:var(--sage)}
.ptlbl{font-family:'Fraunces',serif;font-size:15px;color:var(--ink);letter-spacing:-.2px;margin-top:8px}
.ptdesc{font-family:'Instrument Sans',sans-serif;font-size:10px;color:var(--ink3);margin-top:2px;letter-spacing:.2px}
.tc{margin-bottom:16px;padding:14px;background:var(--srf);border:1px solid var(--line);border-radius:14px}
.thd{display:flex;justify-content:space-between;align-items:baseline;margin-bottom:10px}
.ttl{font-family:'Instrument Sans',sans-serif;font-size:11px;letter-spacing:2.2px;text-transform:uppercase;color:var(--ink3)}
#tcd2{font-family:'JetBrains Mono',monospace;font-size:13px;color:var(--ink3);transition:color .3s}
#tcd2.on{color:var(--saged)}
.trow{display:flex;gap:6px}
.tbt{flex:1;padding:10px 0;background:var(--bg);color:var(--ink);border:1px solid var(--line);border-radius:10px;font-family:'Instrument Sans',sans-serif;font-size:13px;font-weight:500;letter-spacing:.2px;cursor:pointer;transition:all .15s ease}
.tbt.on{background:var(--ink);color:var(--srf);border-color:var(--ink)}
.tbt.toff{flex:0.6;color:var(--ink3);font-size:12px}
.gc{padding:14px 14px 12px;background:var(--srf);border:1px solid var(--line);border-radius:14px;margin-bottom:16px}
.ghd{display:flex;justify-content:space-between;align-items:baseline;margin-bottom:8px}
.gttl{font-family:'Instrument Sans',sans-serif;font-size:11px;letter-spacing:2.2px;text-transform:uppercase;color:var(--ink3)}
#gavg{font-family:'JetBrains Mono',monospace;font-size:11px;color:var(--ink3)}
.gbrs{height:60px;display:flex;align-items:flex-end;gap:2px;padding:0 2px;border-bottom:1px solid var(--line)}
.gtms{display:flex;justify-content:space-between;font-family:'JetBrains Mono',monospace;font-size:9px;color:var(--ink3);letter-spacing:.5px;margin-top:4px}
.sess{display:flex;justify-content:space-between;font-family:'JetBrains Mono',monospace;font-size:10px;color:var(--ink3);letter-spacing:.8px;padding:0 4px;margin-bottom:16px}
.hdr-r{display:flex;align-items:center;gap:10px}
.gbtn{width:36px;height:36px;border-radius:50%;background:var(--srf);border:1px solid var(--line);color:var(--ink2);cursor:pointer;display:flex;align-items:center;justify-content:center;padding:0;transition:color .15s,background .15s}
.gbtn:active{background:var(--srf2);color:var(--ink)}
.mdl{position:fixed;inset:0;background:rgba(0,0,0,.45);display:none;align-items:flex-end;justify-content:center;z-index:200}
.mdl.on{display:flex;animation:mfade .18s ease}
.mdl-inner{background:var(--bg);width:100%;max-width:420px;max-height:88vh;border-radius:18px 18px 0 0;padding:18px 16px calc(24px + env(safe-area-inset-bottom,0px));overflow-y:auto;animation:mslide .24s ease}
.mdl-hd{display:flex;justify-content:space-between;align-items:center;margin-bottom:14px}
.mdl-ttl{font-family:'Fraunces',serif;font-size:22px;font-weight:400;color:var(--ink);letter-spacing:-.5px}
.mdl-x{width:32px;height:32px;border-radius:50%;background:var(--srf);border:1px solid var(--line);color:var(--ink2);cursor:pointer;font-size:18px;line-height:1;display:flex;align-items:center;justify-content:center}
@keyframes mfade{from{opacity:0}to{opacity:1}}
@keyframes mslide{from{transform:translateY(100%)}to{transform:translateY(0)}}
.kc{margin-top:8px;padding:14px;background:var(--srf);border:1px solid var(--line);border-radius:14px;margin-bottom:8px}
.klbl{font-size:10px;color:var(--ink3);margin:8px 0 6px;letter-spacing:.5px;text-transform:uppercase;font-family:'Instrument Sans',sans-serif}
.klbl:first-child{margin-top:0}
.kr{display:flex;flex-wrap:wrap;gap:6px}
.kbtn{flex:1 1 calc(25% - 6px);min-width:2.5rem;padding:10px 0;border:1px solid var(--line);border-radius:10px;background:var(--bg);color:var(--ink2);font-family:'Instrument Sans',sans-serif;font-size:12px;font-weight:500;cursor:pointer;text-align:center;transition:all .15s}
.kbtn.on{border-color:var(--sage);background:var(--srf);color:var(--saged)}
.mbtns{display:grid;grid-template-columns:1fr 1fr 1fr;gap:8px;margin-top:8px}
.mbt{padding:14px 0;border:1px solid var(--line);border-radius:10px;background:var(--srf);color:var(--ink);font-family:'Instrument Sans',sans-serif;font-size:12px;font-weight:500;cursor:pointer;text-align:center;transition:all .15s}
.mbt.fw{border-color:var(--saged);color:var(--saged)}
.mbt.rv{border-color:#C9B79C;color:#8B6F55}
.fwrow{display:flex;justify-content:space-between;align-items:center;padding:0 4px;margin-bottom:12px}
a.fwlk{font-size:11px;color:var(--ink3);text-decoration:none;font-family:'JetBrains Mono',monospace;letter-spacing:.5px}
.thmbtn{padding:4px 10px;border:1px solid var(--line);border-radius:999px;background:transparent;color:var(--ink3);font-family:'Instrument Sans',sans-serif;font-size:11px;cursor:pointer;letter-spacing:.5px}
.ewrap{position:fixed;bottom:0;left:50%;transform:translateX(-50%);width:100%;max-width:420px;padding:12px 14px 34px;background:linear-gradient(to top,var(--bg) 60%,transparent);pointer-events:none;z-index:100}
#estop{width:100%;padding:18px 0;background:var(--stop);color:#fff;border:none;border-radius:14px;cursor:pointer;pointer-events:auto;display:flex;align-items:center;justify-content:center;gap:12px;font-family:'Instrument Sans',sans-serif;transition:all .15s ease;box-shadow:0 8px 24px rgba(180,74,59,.25),inset 0 1px 0 rgba(255,255,255,.15)}
#estop.cf{background:var(--stopd)}
.eico{width:24px;height:24px;border-radius:6px;background:rgba(255,255,255,.18);display:flex;align-items:center;justify-content:center}
.elbl{font-size:14px;font-weight:600;letter-spacing:3px;text-transform:uppercase}
</style>
</head>
<body>

<div id="ib" class="h">
  <div><strong style="color:var(--ink);font-size:13px">Install RockBox</strong><br>Add to home screen</div>
  <button class="ibtn" onclick="installApp()">Install</button>
</div>

<div class="hdr">
  <div class="topbar">
    <div class="wm">
      <span class="wm-n">RockBox</span>
      <span class="wm-v" id="vbadge">v2</span>
    </div>
    <div class="hdr-r">
      <span class="ddot" id="ddot"></span>
      <button class="gbtn" onclick="openSetup()" aria-label="Setup">
        <svg width="18" height="18" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="1.6" stroke-linecap="round" stroke-linejoin="round"><circle cx="12" cy="12" r="3"/><path d="M19.4 15a1.65 1.65 0 0 0 .33 1.82l.06.06a2 2 0 1 1-2.83 2.83l-.06-.06a1.65 1.65 0 0 0-1.82-.33 1.65 1.65 0 0 0-1 1.51V21a2 2 0 1 1-4 0v-.09A1.65 1.65 0 0 0 9 19.4a1.65 1.65 0 0 0-1.82.33l-.06.06a2 2 0 1 1-2.83-2.83l.06-.06a1.65 1.65 0 0 0 .33-1.82 1.65 1.65 0 0 0-1.51-1H3a2 2 0 1 1 0-4h.09A1.65 1.65 0 0 0 4.6 9a1.65 1.65 0 0 0-.33-1.82l-.06-.06a2 2 0 1 1 2.83-2.83l.06.06a1.65 1.65 0 0 0 1.82.33H9a1.65 1.65 0 0 0 1-1.51V3a2 2 0 1 1 4 0v.09a1.65 1.65 0 0 0 1 1.51 1.65 1.65 0 0 0 1.82-.33l.06-.06a2 2 0 1 1 2.83 2.83l-.06.06a1.65 1.65 0 0 0-.33 1.82V9a1.65 1.65 0 0 0 1.51 1H21a2 2 0 1 1 0 4h-.09a1.65 1.65 0 0 0-1.51 1z"/></svg>
      </button>
    </div>
  </div>
</div>

<div class="stbar">
  <div class="sc">
    <svg width="14" height="14" viewBox="0 0 24 24" fill="none" stroke="var(--saged)" stroke-width="1.6" stroke-linecap="round">
      <path d="M4 10a12 12 0 0 1 16 0"/><path d="M7 13.5a7.5 7.5 0 0 1 10 0"/>
      <path d="M10 17a3 3 0 0 1 4 0"/><circle cx="12" cy="19.5" r=".5" fill="var(--saged)" stroke="none"/>
    </svg>
    <span id="wifist">Connecting</span>
  </div>
  <div class="sc">
    <span class="mdot"></span>
    <span class="mlbl">Mains</span>
  </div>
</div>

<div class="dw">
  <svg id="dsvg" width="240" height="240" viewBox="0 0 240 240">
    <path id="dtrack" fill="none" stroke="var(--line)" stroke-width="2" stroke-linecap="round"/>
    <path id="darc"  fill="none" stroke="var(--sage)" stroke-width="2" stroke-linecap="round"/>
    <g id="dticks"></g>
    <circle id="dknob"  r="10" fill="var(--srf)" stroke="var(--sage)" stroke-width="2"/>
    <circle id="dknob2" r="3"  fill="var(--saged)"/>
  </svg>
  <div class="dc">
    <div class="ds" id="dstate">Paused</div>
    <div class="dv" id="dnum">4.0</div>
    <div class="du">Level &middot; of 10</div>
  </div>
  <button class="sdb L" onclick="adjustDial(-0.5)">
    <svg width="14" height="14" viewBox="0 0 24 24" fill="none" stroke="var(--ink2)" stroke-width="1.8" stroke-linecap="round"><path d="M5 12h14"/></svg>
  </button>
  <button class="sdb R" onclick="adjustDial(0.5)">
    <svg width="14" height="14" viewBox="0 0 24 24" fill="none" stroke="var(--ink2)" stroke-width="1.8" stroke-linecap="round"><path d="M12 5v14M5 12h14"/></svg>
  </button>
</div>

<div class="pw">
  <button class="pbtn" onclick="toggleSwing()">
    <span class="pi" id="pi"></span>
    <span id="pbtnlbl">Resume Rocking</span>
  </button>
</div>

<div class="pnls">

  <div class="slbl">Pattern</div>
  <div class="pgrd">
    <button class="ptbtn on" data-pat="wave" onclick="setPattern('wave')">
      <svg viewBox="0 0 40 20" width="100%" height="22" style="display:block">
        <path data-stroke="1" d="M2 10 Q 8 2, 14 10 T 26 10 T 38 10" stroke="var(--saged)" stroke-width="1.4" fill="none" stroke-linecap="round"/>
      </svg>
      <div class="ptlbl">Wave</div><div class="ptdesc">Rhythmic variation</div>
    </button>
    <button class="ptbtn" data-pat="steady" onclick="setPattern('steady')">
      <svg viewBox="0 0 40 20" width="100%" height="22" style="display:block">
        <path data-stroke="1" d="M2 10 L 38 10" stroke="var(--ink3)" stroke-width="1.4" fill="none" stroke-linecap="round"/>
      </svg>
      <div class="ptlbl">Steady</div><div class="ptdesc">Constant speed</div>
    </button>
    <button class="ptbtn" data-pat="lull" onclick="setPattern('lull')">
      <svg viewBox="0 0 40 20" width="100%" height="22" style="display:block">
        <path data-stroke="1" d="M2 4 Q 10 4, 12 10 T 22 13 T 38 14" stroke="var(--ink3)" stroke-width="1.4" fill="none" stroke-linecap="round"/>
      </svg>
      <div class="ptlbl">Lull</div><div class="ptdesc">Gradual slowdown</div>
    </button>
  </div>

  <div class="tc">
    <div class="thd">
      <div class="ttl">Auto-stop Timer</div>
      <div id="tcd2">&#8212;:&#8212;</div>
    </div>
    <div class="trow">
      <button class="tbt" id="tb15" onclick="setTimer(15)">15<span style="opacity:.6;margin-left:2px;font-size:11px">m</span></button>
      <button class="tbt" id="tb30" onclick="setTimer(30)">30<span style="opacity:.6;margin-left:2px;font-size:11px">m</span></button>
      <button class="tbt" id="tb45" onclick="setTimer(45)">45<span style="opacity:.6;margin-left:2px;font-size:11px">m</span></button>
      <button class="tbt" id="tb60" onclick="setTimer(60)">60<span style="opacity:.6;margin-left:2px;font-size:11px">m</span></button>
      <button class="tbt toff" id="tb0" onclick="setTimer(0)">Off</button>
    </div>
  </div>

  <div class="gc">
    <div class="ghd">
      <div class="gttl">Motion &middot; Last 30 min</div>
      <div id="gavg">avg 0.0</div>
    </div>
    <div class="gbrs" id="gbrs"></div>
    <div class="gtms"><span>&#8211;30m</span><span>&#8211;20m</span><span>&#8211;10m</span><span>now</span></div>
  </div>

  <div class="sess">
    <span id="sessuptime">UPTIME 00:00:00</span>
    <span id="sesspacket"></span>
  </div>

</div>

<div id="smod" class="mdl" onclick="if(event.target===this)closeSetup()">
  <div class="mdl-inner">
    <div class="mdl-hd">
      <span class="mdl-ttl">Setup</span>
      <button class="mdl-x" onclick="closeSetup()">&times;</button>
    </div>
    <div class="kc">
      <div class="klbl" style="margin-top:0">Boost level</div>
      <div class="kr">
        <button class="kbtn" id="kb0"    onclick="setKick(0,null)">Off</button>
        <button class="kbtn" id="kb60"   onclick="setKick(60,null)">60%</button>
        <button class="kbtn" id="kb80"   onclick="setKick(80,null)">80%</button>
        <button class="kbtn" id="kb100"  onclick="setKick(100,null)">100%</button>
      </div>
      <div class="klbl">Duration</div>
      <div class="kr">
        <button class="kbtn" id="kd200"  onclick="setKick(null,200)">200ms</button>
        <button class="kbtn" id="kd400"  onclick="setKick(null,400)">400ms</button>
        <button class="kbtn" id="kd600"  onclick="setKick(null,600)">600ms</button>
        <button class="kbtn" id="kd1000" onclick="setKick(null,1000)">1s</button>
      </div>
    </div>
    <div class="mbtns">
      <button class="mbt fw" id="mf" onclick="manual('forward')">&#9650; Fwd</button>
      <button class="mbt"         onclick="send({cmd:'stop'})">&#9632; Stop</button>
      <button class="mbt rv" id="mr" onclick="manual('reverse')">&#9660; Rev</button>
    </div>
    <div class="fwrow">
      <a href="/update"     class="fwlk">Firmware update</a>
      <button class="thmbtn" onclick="toggleTheme()">&#9788; Theme</button>
      <a href="/reset-wifi" class="fwlk">Reset WiFi</a>
    </div>
  </div>
</div>

<div class="ewrap">
  <button id="estop" onclick="eStop()">
    <div class="eico">
      <svg width="12" height="12" viewBox="0 0 24 24" fill="#fff"><rect x="6" y="6" width="12" height="12" rx="1.5"/></svg>
    </div>
    <span class="elbl" id="eslbl">Emergency Stop</span>
  </button>
</div>

<script>
var S={swing:false,speed:0,dir:'stop',swingSpd:40,clients:0,timerMins:0,timerSec:-1,kickPct:0,kickMs:400,pattern:'steady',fw:0};
var dialVal=4.0,running=false;
var motion=[],uptimeSec=0,cdInt=null;
var ws,rt,ht,rd=1000,deferredPrompt=null;

for(var _i=0;_i<60;_i++){
  var _b=2+Math.sin(_i/8)*1.5+Math.cos(_i/5)*1.2;
  motion.push(Math.max(0,Math.min(8,_b+(_i>40?1.8:0))));
}

// Dial
var CX=120,CY=120,R_T=98,R_O=116,SD=-220,SWEEP=260;
function d2p(deg,r){var rad=(deg-90)*Math.PI/180;return[CX+Math.cos(rad)*r,CY+Math.sin(rad)*r];}
function mkArc(r,a0,a1){var p0=d2p(a0,r),p1=d2p(a1,r);return 'M '+p0[0]+' '+p0[1]+' A '+r+' '+r+' 0 '+((a1-a0)>180?1:0)+' 1 '+p1[0]+' '+p1[1];}

function initDial(){
  document.getElementById('dtrack').setAttribute('d',mkArc(R_T,SD,SD+SWEEP));
  var g=document.getElementById('dticks');g.innerHTML='';
  for(var i=0;i<=10;i++){
    var a=SD+(i/10)*SWEEP,maj=(i%2===0);
    var inn=d2p(a,R_O-2),out=d2p(a,R_O-(maj?12:7));
    var ln=document.createElementNS('http://www.w3.org/2000/svg','line');
    ['x1','y1','x2','y2'].forEach(function(k,j){ln.setAttribute(k,[inn[0],inn[1],out[0],out[1]][j]);});
    ln.setAttribute('stroke-width',maj?'1.4':'1');ln.setAttribute('stroke-linecap','round');
    ln.id='tick'+i;g.appendChild(ln);
  }
  updateDial();
}

function updateDial(){
  var angle=SD+(dialVal/10)*SWEEP;
  var darc=document.getElementById('darc');
  if(dialVal>0){darc.setAttribute('d',mkArc(R_T,SD,angle));darc.style.display='';}
  else darc.style.display='none';
  var kp=d2p(angle,R_T);
  ['dknob','dknob2'].forEach(function(id){
    document.getElementById(id).setAttribute('cx',kp[0]);
    document.getElementById(id).setAttribute('cy',kp[1]);
  });
  for(var i=0;i<=10;i++){
    var t=document.getElementById('tick'+i);
    if(t)t.setAttribute('stroke',(i<=dialVal*10/10)?'var(--sage)':'var(--line)');
  }
  document.getElementById('dnum').textContent=dialVal.toFixed(1);
  document.getElementById('dstate').textContent=running?'Rocking':'Paused';
  var pi=document.getElementById('pi'),lbl=document.getElementById('pbtnlbl');
  if(running){pi.classList.add('on');lbl.textContent='Pause Rocking';}
  else{pi.classList.remove('on');lbl.textContent='Resume Rocking';}
}

var dragging=false,dsvg=document.getElementById('dsvg');
function onDialPt(e){
  var rect=dsvg.getBoundingClientRect();
  var px=(e.touches?e.touches[0].clientX:e.clientX)-(rect.left+rect.width/2);
  var py=(e.touches?e.touches[0].clientY:e.clientY)-(rect.top+rect.height/2);
  var deg=Math.atan2(py,px)*180/Math.PI+90;
  if(deg>180)deg-=360;
  dialVal=Math.round(Math.max(0,Math.min(SWEEP,deg-SD))/SWEEP*100)/10;
  updateDial();scheduleSpdSend();
}
dsvg.addEventListener('mousedown',function(e){dragging=true;onDialPt(e);});
dsvg.addEventListener('touchstart',function(e){e.preventDefault();dragging=true;onDialPt(e);},{passive:false});
window.addEventListener('mousemove',function(e){if(dragging)onDialPt(e);});
window.addEventListener('mouseup',function(){dragging=false;});
window.addEventListener('touchmove',function(e){if(dragging){e.preventDefault();onDialPt(e);}},{passive:false});
window.addEventListener('touchend',function(){dragging=false;});

function adjustDial(d){dialVal=Math.max(0,Math.min(10,parseFloat((dialVal+d).toFixed(1))));updateDial();scheduleSpdSend();}
var spdTmr;
function scheduleSpdSend(){clearTimeout(spdTmr);spdTmr=setTimeout(function(){if(running)send({cmd:'set_speed',speed:Math.round(dialVal*10)});},120);}
initDial();

// Pattern
function setPattern(p){
  send({cmd:'set_pattern',pattern:p});
  updatePatternUI(p);
}
function updatePatternUI(p){
  document.querySelectorAll('.ptbtn').forEach(function(b){
    var on=b.dataset.pat===p;
    b.className='ptbtn'+(on?' on':'');
    var path=b.querySelector('[data-stroke]');
    if(path)path.setAttribute('stroke',on?'var(--saged)':'var(--ink3)');
  });
}

// Motion graph
function addMotionPt(v){motion.push(v);if(motion.length>60)motion.shift();renderGraph();}
function renderGraph(){
  var bars=document.getElementById('gbrs');bars.innerHTML='';
  var sum=0,cnt=0;
  motion.forEach(function(v,i){
    if(v>0){sum+=v;cnt++;}
    var b=document.createElement('div');
    b.style.flex='1';b.style.height=Math.max(4,(v/10)*100)+'%';
    b.style.background=v>0?'var(--sage)':'var(--line)';
    b.style.opacity=v>0?(0.4+(i/motion.length)*0.6).toString():'0.3';
    b.style.borderRadius='1px';bars.appendChild(b);
  });
  document.getElementById('gavg').textContent='avg '+(cnt>0?(sum/cnt).toFixed(1):'0.0');
}
renderGraph();

// Timer
function setTimer(m){send({cmd:'set_timer',minutes:m});}
function fmtSec(s){return String(Math.floor(s/60)).padStart(2,'0')+':'+String(s%60).padStart(2,'0');}
function startCountdown(secs){
  clearInterval(cdInt);var rem=secs;
  var el=document.getElementById('tcd2');el.classList.add('on');
  function tick(){if(rem<=0){el.textContent='\u2014:\u2014';el.classList.remove('on');clearInterval(cdInt);return;}el.textContent=fmtSec(rem--);}
  tick();cdInt=setInterval(tick,1000);
}
function stopCountdown(){clearInterval(cdInt);var el=document.getElementById('tcd2');el.textContent='\u2014:\u2014';el.classList.remove('on');}

// Kick
function setKick(pct,ms){send({cmd:'set_kick',pct:pct!==null?pct:S.kickPct,ms:ms!==null?ms:S.kickMs});}

// Manual
function manual(dir){send({cmd:'set',dir:dir,speed:Math.round(dialVal*10)});}

// Swing
function toggleSwing(){
  if(running)send({cmd:'swing_stop'});
  else send({cmd:'swing_start',speed:Math.max(10,Math.round(dialVal*10))});
}

// E-stop — single-tap, fires immediately
function eStop(){
  send({cmd:'stop'});running=false;dialVal=0;updateDial();
  var el=document.getElementById('estop');
  el.classList.add('cf');
  setTimeout(function(){el.classList.remove('cf');},500);
}

// Render
function render(s){
  S=s;running=!!s.swing;
  dialVal=Math.max(0,Math.min(10,Math.round(s.swingSpd)/10));
  updateDial();
  if(s.pattern)updatePatternUI(s.pattern);
  [0,15,30,45,60].forEach(function(v){var el=document.getElementById('tb'+v);if(el)el.className=(v===0?'tbt toff':'tbt')+(s.timerMins===v?' on':'');});
  if(s.swing&&s.timerSec>=0)startCountdown(s.timerSec);
  else{stopCountdown();if(s.timerMins>0)document.getElementById('tcd2').textContent=s.timerMins+':00';}
  [0,60,80,100].forEach(function(v){var el=document.getElementById('kb'+v);if(el)el.className='kbtn'+(s.kickPct===v?' on':'');});
  [200,400,600,1000].forEach(function(v){var el=document.getElementById('kd'+v);if(el)el.className='kbtn'+(s.kickMs===v?' on':'');});
  document.getElementById('mf').className='mbt fw'+((!s.swing&&s.dir==='forward')?' on':'');
  document.getElementById('mr').className='mbt rv'+((!s.swing&&s.dir==='reverse')?' on':'');
  document.getElementById('ddot').className='ddot on';
  document.getElementById('wifist').textContent=s.clients+' device'+(s.clients!==1?'s':'')+' connected';
  if(s.fw)document.getElementById('vbadge').textContent='v'+s.fw;
  addMotionPt(running?dialVal:0);
}

// Uptime
setInterval(function(){
  uptimeSec++;
  var h=Math.floor(uptimeSec/3600),m=Math.floor((uptimeSec%3600)/60),sc=uptimeSec%60;
  document.getElementById('sessuptime').textContent='UPTIME '+String(h).padStart(2,'0')+':'+String(m).padStart(2,'0')+':'+String(sc).padStart(2,'0');
},1000);

// Setup modal
function openSetup(){document.getElementById('smod').classList.add('on');}
function closeSetup(){document.getElementById('smod').classList.remove('on');}

// Theme
var darkMode=false;
function toggleTheme(){
  darkMode=!darkMode;
  document.body.classList.toggle('dk',darkMode);
  document.getElementById('tmeta').setAttribute('content',darkMode?'#1C211F':'#F2EEE6');
  document.getElementById('astat').setAttribute('content',darkMode?'black-translucent':'default');
}

// PWA install
window.addEventListener('beforeinstallprompt',function(e){e.preventDefault();deferredPrompt=e;document.getElementById('ib').classList.remove('h');});
window.addEventListener('appinstalled',function(){document.getElementById('ib').classList.add('h');deferredPrompt=null;});
function installApp(){
  if(deferredPrompt){deferredPrompt.prompt();deferredPrompt.userChoice.then(function(){deferredPrompt=null;});}
  else alert('Android Chrome: tap \u22EE \u2192 "Add to Home screen"\n\niOS Safari: tap \u2191 \u2192 "Add to Home Screen"');
}
if(window.matchMedia('(display-mode:standalone)').matches||window.navigator.standalone===true)document.getElementById('ib').classList.add('h');

// WebSocket
function conn(){
  try{ws=new WebSocket('ws://'+location.hostname+':81');}catch(e){sched();return;}
  ws.onopen=function(){rd=1000;document.getElementById('ddot').className='ddot on';clearInterval(ht);ht=setInterval(function(){send({cmd:'heartbeat'});},15000);};
  ws.onmessage=function(e){try{render(JSON.parse(e.data));}catch(ex){}};
  ws.onclose=ws.onerror=function(){document.getElementById('ddot').className='ddot';clearInterval(ht);sched();};
}
function sched(){clearTimeout(rt);rt=setTimeout(function(){rd=Math.min(rd*1.5,10000);conn();},rd);}
function send(o){if(ws&&ws.readyState===1)ws.send(JSON.stringify(o));}
conn();
</script>
</body>
</html>
)rawliteral";

// ─── Web app manifest ─────────────────────────────────────
const char MANIFEST[] PROGMEM = R"rawliteral({
  "name": "RockBox",
  "short_name": "RockBox",
  "description": "Control your baby rocker from anywhere in the house",
  "start_url": "/",
  "display": "standalone",
  "orientation": "portrait",
  "background_color": "#F2EEE6",
  "theme_color": "#F2EEE6",
  "icons": [
    {"src":"/icon.svg","sizes":"any","type":"image/svg+xml","purpose":"any maskable"}
  ]
})rawliteral";

// ─── Home screen icon (SVG scales to any size) ────────────
const char ICON_SVG[] PROGMEM = R"rawliteral(<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 100 100">
<circle cx="50" cy="50" r="50" fill="#F2EEE6"/>
<circle cx="50" cy="50" r="32" fill="none" stroke="#8FA89A" stroke-width="4"/>
<circle cx="50" cy="50" r="8" fill="#6B8A7A"/>
<line x1="50" y1="18" x2="50" y2="14" stroke="#8FA89A" stroke-width="3" stroke-linecap="round"/>
<line x1="50" y1="82" x2="50" y2="86" stroke="#8FA89A" stroke-width="3" stroke-linecap="round"/>
<line x1="18" y1="50" x2="14" y2="50" stroke="#8FA89A" stroke-width="3" stroke-linecap="round"/>
<line x1="82" y1="50" x2="86" y2="50" stroke="#8FA89A" stroke-width="3" stroke-linecap="round"/>
</svg>)rawliteral";

// ─── WiFi setup portal HTML (served in AP mode) ───────────
const char AP_HTML[] PROGMEM = R"rawliteral(<!DOCTYPE html>
<html lang="en">
<head>
<meta charset="UTF-8">
<meta name="viewport" content="width=device-width,initial-scale=1,user-scalable=no">
<meta name="theme-color" content="#F2EEE6">
<title>RockBox – WiFi Setup</title>
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
      o.textContent=n.ssid+' ('+n.rssi+'dBm'+(n.secure?' \uD83D\uDD12':'')+')';
      sel.appendChild(o);
    });
    sel.style.display=nets.length?'block':'none';
    btn.textContent='Scan';btn.disabled=false;
  }).catch(function(){
    btn.textContent='Scan';btn.disabled=false;
    showMsg('Scan failed \u2014 enter SSID manually','err');
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
  showMsg('Saving\u2026');
  var body='ssid='+encodeURIComponent(ssid)+'&pass='+encodeURIComponent(pass);
  fetch('/save',{method:'POST',
    headers:{'Content-Type':'application/x-www-form-urlencoded'},
    body:body})
  .then(function(){
    showMsg('Saved! Device will restart and connect to \u201C'+ssid+
      '\u201D. Reconnect your phone to that network, then open http://rockbox.local');
  }).catch(function(){
    showMsg('Saved! Device is restarting\u2026');
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
  Serial.printf("[OTA] URL: %s\n", GITHUB_VER_URL);

  WiFiClientSecure verClient;
  verClient.setInsecure(); // skip cert validation — fine for home use
  verClient.setTimeout(15);

  HTTPClient http;
  http.begin(verClient, GITHUB_VER_URL);
  http.setTimeout(15000);
  http.setFollowRedirects(HTTPC_FORCE_FOLLOW_REDIRECTS);
  int code = http.GET();

  Serial.printf("[OTA] HTTP response: %d\n", code);
  if (code != 200)
  {
    Serial.printf("[OTA] Version check failed (HTTP %d) — skipping\n", code);
    Serial.printf("[OTA] Error string: %s\n", http.errorToString(code).c_str());
    http.end();
    return;
  }

  String body = http.getString();
  http.end();
  body.trim();
  int remoteVer = body.toInt();

  Serial.printf("[OTA] Local v%d  Remote v%d\n", FW_VERSION, remoteVer);

  if (remoteVer <= FW_VERSION)
  {
    Serial.println("[OTA] Already up to date");
    return;
  }

  Serial.printf("[OTA] Updating to v%d — stopping motor and downloading…\n", remoteVer);
  hardStop(); // safety: stop motor before any flash write

  WiFiClientSecure dlClient;
  dlClient.setInsecure();
  dlClient.setTimeout(120); // 120 s socket timeout

  HTTPClient dlHttp;
  dlHttp.begin(dlClient, GITHUB_BIN_URL);
  dlHttp.setFollowRedirects(HTTPC_FORCE_FOLLOW_REDIRECTS);
  dlHttp.setTimeout(30000);

  int dlCode = dlHttp.GET();
  Serial.printf("[OTA] Binary HTTP: %d\n", dlCode);
  if (dlCode != 200)
  {
    Serial.printf("[OTA] Download failed (HTTP %d) — skipping\n", dlCode);
    dlHttp.end();
    return;
  }

  int binSize = dlHttp.getSize();
  Serial.printf("[OTA] Binary size: %d bytes\n", binSize);

  if (!Update.begin(binSize > 0 ? binSize : UPDATE_SIZE_UNKNOWN))
  {
    Serial.printf("[OTA] Update.begin failed: %s\n", Update.errorString());
    dlHttp.end();
    return;
  }

  WiFiClient *stream = dlHttp.getStreamPtr();
  uint8_t *buf = (uint8_t *)malloc(4096);
  if (!buf)
  {
    Serial.println("[OTA] malloc failed — aborting");
    dlHttp.end();
    return;
  }
  int written = 0;

  while (written < binSize)
  {
    int toRead = min(4096, binSize - written);
    int got = stream->readBytes(buf, toRead); // blocks up to dlClient timeout
    if (got <= 0)
    {
      Serial.printf("[OTA] Stream ended early at %d bytes\n", written);
      break;
    }
    Update.write(buf, got);
    written += got;
    if (written % 65536 == 0)
      Serial.printf("[OTA] %d / %d bytes\n", written, binSize);
  }

  free(buf);
  dlHttp.end();
  Serial.printf("[OTA] Written %d bytes total\n", written);

  if (Update.end(true))
  {
    Serial.println("[OTA] Flash OK — rebooting");
    ESP.restart();
  }
  else
  {
    Serial.printf("[OTA] Flash failed: %s\n", Update.errorString());
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
<meta name="theme-color" content="#F2EEE6">
<title>RockBox – Firmware Update</title>
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
  showMsg('Uploading\u2026');
  var fd=new FormData();
  fd.append('firmware',f,f.name);
  var xhr=new XMLHttpRequest();
  xhr.upload.onprogress=function(e){
    if(e.lengthComputable){
      var p=Math.round(e.loaded/e.total*100);
      bar.style.width=p+'%';
      showMsg('Uploading\u2026 '+p+'%');
    }
  };
  xhr.onload=function(){
    bar.style.width='100%';
    if(xhr.status===200&&xhr.responseText==='OK'){
      showMsg('Flashed \u2014 device is rebooting\u2026');
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
  if (!httpSrv.authenticate(OTA_USER, OTA_PASS))
  {
    return httpSrv.requestAuthentication();
  }
  // Inject current firmware version into the HTML
  String html = String(OTA_HTML);
  html.replace("__VER__", String(FW_VERSION));
  httpSrv.send(200, "text/html; charset=utf-8", html);
}

void handleOTAUploadDone()
{
  if (!httpSrv.authenticate(OTA_USER, OTA_PASS))
  {
    return httpSrv.requestAuthentication();
  }
  if (Update.hasError())
  {
    String err = "FAIL: " + String(Update.errorString());
    httpSrv.send(500, "text/plain", err);
    Serial.printf("[OTA] Manual upload failed: %s\n", Update.errorString());
  }
  else
  {
    httpSrv.send(200, "text/plain", "OK");
    Serial.println("[OTA] Manual upload complete — rebooting");
    delay(500);
    ESP.restart();
  }
}

void handleOTAUploadData()
{
  if (!httpSrv.authenticate(OTA_USER, OTA_PASS))
  {
    return httpSrv.requestAuthentication();
  }
  HTTPUpload &upload = httpSrv.upload();
  if (upload.status == UPLOAD_FILE_START)
  {
    Serial.printf("[OTA] Manual upload start: %s\n", upload.filename.c_str());
    hardStop(); // stop motor before touching flash
    if (!Update.begin(UPDATE_SIZE_UNKNOWN))
    {
      Update.printError(Serial);
    }
  }
  else if (upload.status == UPLOAD_FILE_WRITE)
  {
    if (Update.write(upload.buf, upload.currentSize) != upload.currentSize)
    {
      Update.printError(Serial);
    }
  }
  else if (upload.status == UPLOAD_FILE_END)
  {
    if (Update.end(true))
    {
      Serial.printf("[OTA] Manual upload done: %u bytes\n", upload.totalSize);
    }
    else
    {
      Update.printError(Serial);
    }
  }
}

void handleOTAGithub()
{
  if (!httpSrv.authenticate(OTA_USER, OTA_PASS))
  {
    return httpSrv.requestAuthentication();
  }
  // Send page first — if update found the device reboots, if not the JS redirects back.
  String html = F("<!DOCTYPE html><html><head><meta charset='UTF-8'>"
                  "<meta name='viewport' content='width=device-width,initial-scale=1'>"
                  "<meta name='theme-color' content='#F2EEE6'>"
                  "<title>RockBox \u2013 OTA Check</title>"
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
                  "</body></html>");
  httpSrv.send(200, "text/html; charset=utf-8", html);
  delay(100); // ensure response is flushed before blocking download
  checkAndApplyOTA();
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
  for (int i = 0; i < n; i++)
  {
    if (i)
      json += ",";
    json += "{\"ssid\":\"" + jsonEsc(WiFi.SSID(i)) + "\","
                                                     "\"rssi\":" +
            WiFi.RSSI(i) + ","
                           "\"secure\":" +
            (WiFi.encryptionType(i) != WIFI_AUTH_OPEN ? "true" : "false") + "}";
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

  if (ssid.length() == 0 || ssid.length() > 32 || pass.length() > 64)
  {
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
  httpSrv.on("/", HTTP_GET, handleAPRoot);
  httpSrv.on("/scan", HTTP_GET, handleAPScan);
  httpSrv.on("/save", HTTP_POST, handleAPSave);
  // Catch-all: redirect to setup page (makes captive portal work on iOS/Android)
  httpSrv.onNotFound([]()
                     {
    httpSrv.sendHeader("Location", "http://192.168.4.1/");
    httpSrv.send(302, "text/plain", ""); });
  httpSrv.begin();

  Serial.printf("[AP] Started portal: %s\n", AP_SSID);
  Serial.printf("[AP] Connect to WiFi '%s' then open http://192.168.4.1\n", AP_SSID);
  Serial.printf("[AP] (or http://%s)\n", WiFi.softAPIP().toString().c_str());
}

// ─── Start: normal swing mode ────────────────────────────────
void startNormalMode()
{
  Serial.printf("[WiFi] Connected!  http://%s\n", WiFi.localIP().toString().c_str());

  if (MDNS.begin(HOSTNAME))
    Serial.printf("[mDNS] http://%s.local\n", HOSTNAME);

  httpSrv.on("/", handleRoot);
  httpSrv.on("/manifest.json", handleManifest);
  httpSrv.on("/icon.svg", handleIcon);
  httpSrv.on("/stop", handleHttpStop);
  httpSrv.on("/reset-wifi", handleResetWifi);
  httpSrv.on("/update", HTTP_GET, handleOTAPage);
  httpSrv.on("/update", HTTP_POST, handleOTAUploadDone, handleOTAUploadData);
  httpSrv.on("/ota-github", HTTP_GET, handleOTAGithub);
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
  Serial.printf("  RockBox Controller v%d\n", FW_VERSION);
  Serial.println("===========================");

  // Hardware init — motor stopped from the very start
  pinMode(PIN_IN1, OUTPUT);
  pinMode(PIN_IN2, OUTPUT);
  ledcSetup(PWM_CH, PWM_FREQ, PWM_BITS);
  ledcAttachPin(PIN_ENA, PWM_CH);
  hardStop();
  pinMode(PIN_BTN, INPUT_PULLUP);

  // Load persisted swing settings from NVS
  prefs.begin("swing", true);
  g_swingSpd  = prefs.getInt("swingSpd", g_swingSpd);
  g_timerMins = prefs.getInt("timerMins", g_timerMins);
  g_kickPct   = prefs.getInt("kickPct", g_kickPct);
  g_kickMs    = prefs.getInt("kickMs", g_kickMs);
  g_pattern   = prefs.getString("pattern", g_pattern);
  prefs.end();
  Serial.printf("[NVS] swingSpd=%d  timerMins=%d  kickPct=%d  kickMs=%d\n",
                g_swingSpd, g_timerMins, g_kickPct, g_kickMs);

  // ── WiFi ──────────────────────────────────────────────────
  String ssid, pass;
  bool hasCreds = loadWifiCreds(ssid, pass);

  if (!hasCreds)
  {
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
  while (WiFi.status() != WL_CONNECTED && millis() - t0 < CONNECT_TIMEOUT_MS)
  {
    delay(500);
    Serial.print('.');
  }
  Serial.println();

  if (WiFi.status() != WL_CONNECTED)
  {
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
  if (g_apMode)
  {
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

  // Physical button: debounced toggle (50 ms) — starts or stops swing
  static bool  btnLast    = HIGH;
  static ulong btnDebTime = 0;
  bool btnNow = digitalRead(PIN_BTN);
  if (btnNow != btnLast) btnDebTime = now;
  if (now - btnDebTime > 50 && btnNow == LOW && btnLast != LOW)
  {
    // Falling edge confirmed after debounce
    if (g_swinging)
    {
      hardStop();
      Serial.println("[BTN] Stop");
    }
    else
    {
      g_swinging   = true;
      g_wavePhase  = 0.0f;
      g_lullFactor = 1.0f;
      g_timerEnd   = g_timerMins > 0 ? now + (ulong)g_timerMins * 60000UL : 0UL;
      applyMotor(MFWD, g_swingSpd);
      Serial.printf("[BTN] Start spd=%d pattern=%s\n", g_swingSpd, g_pattern.c_str());
    }
    broadcast();
  }
  btnLast = btnNow;

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

  // Pattern tick — runs every 500 ms while swinging (not during kick)
  static ulong patternTick = 0;
  if (g_swinging && g_kickEnd == 0 && now - patternTick >= 500)
  {
    patternTick = now;
    if (g_pattern == "wave")
    {
      // Sinusoidal variation: speed oscillates ±25% around target, period ~4 s
      g_wavePhase += 0.785f; // π/4 per tick → 8 ticks = 4 s full cycle
      float factor = 0.75f + 0.25f * sinf(g_wavePhase);
      int spd = constrain((int)(g_swingSpd * factor), 5, MAX_SPEED_PCT);
      applyMotor(MFWD, spd);
    }
    else if (g_pattern == "lull")
    {
      // Gradual decay: full speed → 50% over ~5 min (600 ticks × 500 ms)
      g_lullFactor -= 0.000833f;
      if (g_lullFactor < 0.5f) g_lullFactor = 0.5f;
      int spd = constrain((int)(g_swingSpd * g_lullFactor), 5, MAX_SPEED_PCT);
      applyMotor(MFWD, spd);
    }
    // steady: no speed change needed
  }
}
