# ESP32 Baby Swing Controller

An ESP32-based motor controller for a baby swing. The motor drives a crank
mechanism that converts continuous rotation into a back-and-forth swing motion.
A built-in Progressive Web App (PWA) lets any phone, tablet, or desktop on the
same WiFi network control the swing — no app store required.

The device advertises itself as **`http://rockbox.local`** on your local network.

---

## Features

- **WebSocket real-time control** — low-latency commands and live state sync
  across all connected devices simultaneously
- **Progressive Web App** — installable on Android and iOS home screens; runs
  full-screen with no browser chrome
- **mDNS hostname** — reach the UI at `http://rockbox.local` without
  memorising an IP address
- **Speed control** — adjustable swing speed via a dial (5–80 %)
- **Hard speed cap** — firmware enforces an 80 % PWM ceiling that cannot be
  overridden remotely
- **Rocking patterns** — choose between Steady, Wave, and Lull to vary the
  motion over time
- **Kick-start** — optional boost pulse on startup to overcome crank stiction
- **Auto-stop timer** — configurable run timer (15, 30, 45, or 60 minutes);
  persisted across device restarts
- **Physical button** — hardware toggle on GPIO 18 starts or stops the swing
  without opening the app
- **Manual motor control** — forward/reverse jog in the setup panel, useful
  during installation or maintenance
- **Emergency stop** — always-visible stop button; also accessible via HTTP GET
  `/stop` as a fallback if WebSocket fails
- **WiFi auto-reconnect** — connection is checked every 10 seconds; the ESP32
  reconnects automatically on link loss
- **WiFi setup portal** — captive portal in AP mode for first-time WiFi
  configuration; no hard-coded credentials needed
- **Over-the-air updates** — automatic check against the GitHub release on boot,
  plus a manual upload page at `/update`
- **Dark / light theme** — toggle from the setup panel
- **Motion history** — 30-minute rolling activity graph in the UI

---

## Hardware

### Bill of Materials

| Component | Notes |
|-----------|-------|
| ESP32 development board | Any standard 38-pin ESP32 DevKit |
| L298N motor driver module | Must have the 5 V regulator jumper installed |
| 12 V DC motor | Sized for your swing's crank load |
| 12 V power supply | Current rating matched to motor stall current |
| Connecting wire | — |
| Momentary push button (optional) | For the physical GPIO 18 toggle |

### Wiring

| ESP32 GPIO | L298N pin | Notes |
|-----------|-----------|-------|
| GPIO 25 | IN1 | Motor direction A |
| GPIO 26 | IN2 | Motor direction B |
| GPIO 27 | ENA | PWM speed signal |
| — | +12 V | From 12 V PSU |
| — | GND | Shared with ESP32 GND |
| VIN (5 V) | 5 V out | L298N onboard regulator powers the ESP32 |
| OUT1 / OUT2 | — | To DC motor terminals |
| GPIO 18 | — | Optional push button to GND (INPUT_PULLUP) |

> All GND pins must be connected together: PSU ground, L298N ground, and ESP32
> ground.

### CAD Files

The `CAD/` folder contains STL and STEP files for 3D-printable mounting
brackets and crank components.

---

## Software Setup

### Prerequisites

- [Visual Studio Code](https://code.visualstudio.com/)
- [PlatformIO IDE extension](https://platformio.org/install/ide?install=vscode)

### Steps

1. **Clone or download** the repository and open the project folder in VS Code.
   PlatformIO will detect `platformio.ini` automatically.

2. **Build and flash** — connect the ESP32 via USB, then click **Upload** in
   the PlatformIO toolbar (or run `pio run --target upload`). PlatformIO
   auto-detects the serial port. To pin a specific port, uncomment the
   `upload_port` line in `platformio.ini`.

3. **Open the Serial Monitor** at 115200 baud to confirm the device boots.

4. **First-time WiFi setup** — on first boot (or after a WiFi reset) the
   device starts in AP mode. See [WiFi Setup](#wifi-setup) below.

> WiFi credentials are stored in NVS (non-volatile storage) and survive
> power cycles. You do not need to hard-code them in `src/main.cpp`.

---

## WiFi Setup

When no WiFi credentials are stored, the device boots into AP (access point)
mode:

1. A network named **`RockBox-Setup`** appears in your WiFi list. Connect
   to it (no password required).
2. A captive portal opens automatically on Android and iOS. If it does not,
   open a browser and go to `http://192.168.4.1`.
3. Tap **Scan** to list nearby networks, select yours, enter the password, and
   tap **Connect & Save**.
4. The device reboots, joins your network, and is accessible at
   `http://rockbox.local`.

To reset WiFi credentials later, open the setup panel (gear icon) in the web
UI and tap **Reset WiFi**, or navigate to `http://rockbox.local/reset-wifi`.

---

## Usage

### Accessing the Web UI

After the ESP32 boots and connects to WiFi, the web interface is available at:

- `http://rockbox.local` — works on most devices without needing the IP
- `http://<ip-address>` — the assigned IP is printed in the Serial Monitor at
  startup

### Installing as a PWA

**Android (Chrome):**
1. Open `http://rockbox.local` in Chrome.
2. Tap the three-dot menu and choose **Add to Home screen** (or tap the
   **Install app** banner that appears at the top of the page).
3. The app opens full-screen from your home screen with no browser bar.

**iOS (Safari):**
1. Open `http://rockbox.local` in Safari.
2. Tap the share button (box with arrow), then **Add to Home Screen**.
3. The app opens full-screen from your home screen.

### Swing Mode

Tap the large circular button to start the swing. The motor runs continuously
in the forward direction; the crank mechanism converts that rotation into
back-and-forth motion. Tap the button again to stop.

The button glows and shows a spinning icon while the motor is running. All
connected devices see the same state in real time.

The physical button on GPIO 18 (if wired) also toggles the swing without
opening the app.

### Speed Dial

The dial sets the motor speed as a percentage (5–80 %). Adjusting it while the
swing is running sends an update immediately (with debouncing). Adjusting it
while the swing is stopped sets the speed for the next start.

### Rocking Patterns

Three patterns are available from the pattern row below the dial:

| Pattern | Behaviour |
|---------|-----------|
| **Steady** | Constant speed — motor runs at exactly the set percentage |
| **Wave** | Sinusoidal variation ±25 % around the target, cycling roughly every 4 seconds |
| **Lull** | Gradual slowdown from full target speed to ~50 % over approximately 5 minutes |

### Auto-stop Timer

Tap a duration button (15, 30, 45, or 60 min) to set a run timer. The
countdown appears in the UI. When the timer elapses the motor stops
automatically. Tap **Off** to disable the timer. The last-set value is
persisted and used for all future sessions.

### Kick-start

Some crank positions require extra torque to get moving. The kick-start feature
fires a brief high-power pulse (60–100 % PWM for 200 ms–1 s) at startup before
dropping to the normal run speed. Configure it in the setup panel (gear icon)
or leave it **Off** if the motor starts reliably without it.

### Manual Control

Open the setup panel (gear icon) and use the **Fwd / Stop / Rev** buttons to
drive the motor directly. This bypasses swing mode and is intended for
installation testing or maintenance. Swing mode is automatically cancelled when
a manual command is sent.

### Emergency Stop

The red **Emergency Stop** button is always visible at the bottom of the
screen. It sends a `stop` command over WebSocket. The HTTP endpoint
`GET /stop` provides the same result if the WebSocket connection is unavailable.

---

## Firmware Updates

### Automatic (on boot)

On every boot the device fetches `version.txt` from the GitHub repository. If
the remote version is newer than the running firmware it downloads
`firmware.bin` from the latest GitHub Release and flashes it automatically,
then reboots.

### Manual upload

Navigate to `http://rockbox.local/update` (credentials: **admin** /
**babyswing** — change `OTA_PASS` in `src/main.cpp` before deploying).

- **Check GitHub for update** — triggers the same automatic check on demand.
- **Upload & Flash** — upload a `.bin` file built with PlatformIO to flash
  immediately.

---

## API / WebSocket Protocol

Connect to `ws://<device-ip>:81`.

### Commands (client → ESP32)

All commands are plain-text strings.

| Command | Arguments | Description |
|---------|-----------|-------------|
| `heartbeat` | — | Keeps the session alive. The PWA sends this automatically every 15 seconds. |
| `swing_start` | `[speed]` (integer, optional) | Starts the swing. If speed is omitted the last-set swing speed is used. Clamped to 5–80 %. |
| `swing_stop` | — | Stops the motor and clears swing mode. |
| `set_speed` | `speed` (integer) | Updates swing speed. Takes effect immediately if running. Clamped to 5–80 %. |
| `set` | `dir` (`forward`/`reverse`/`stop`) `speed` | Manual motor override. Cancels swing mode. Speed clamped to 0–80 %. |
| `set_timer` | `minutes` (integer, 0–240) | Sets the auto-stop timer duration. 0 disables the timer. |
| `set_kick` | `pct` (0–100) `ms` (50–2000) | Configures the kick-start pulse level and duration. |
| `set_pattern` | `pattern` (`wave`/`steady`/`lull`) | Sets the rocking pattern. |
| `stop` | — | Hard stop — halts the motor and cancels swing mode. |

Examples:

```
swing_start 50
set_speed 60
set forward 30
set_timer 30
set_kick 80 400
set_pattern wave
heartbeat
stop
```

### State Broadcast (ESP32 → all clients)

The ESP32 broadcasts the current state to every connected client after any
command and when clients connect or disconnect.

```json
{
  "swing":     false,
  "speed":     0,
  "dir":       "stop",
  "swingSpd":  40,
  "clients":   1,
  "timerMins": 30,
  "timerSec":  1800,
  "kickPct":   80,
  "kickMs":    400,
  "pattern":   "steady",
  "fw":        34
}
```

| Field | Type | Description |
|-------|------|-------------|
| `swing` | boolean | Whether swing mode is active |
| `speed` | integer | Current motor speed in % (0 when stopped) |
| `dir` | string | Current motor direction: `"forward"`, `"reverse"`, or `"stop"` |
| `swingSpd` | integer | Speed percentage used on the next `swing_start` |
| `clients` | integer | Number of connected WebSocket clients |
| `timerMins` | integer | Configured auto-stop duration in minutes (0 = disabled) |
| `timerSec` | integer | Seconds remaining on the active timer; −1 if no timer is running |
| `kickPct` | integer | Kick-start boost level in % (0 = disabled) |
| `kickMs` | integer | Kick-start pulse duration in milliseconds |
| `pattern` | string | Active rocking pattern: `"steady"`, `"wave"`, or `"lull"` |
| `fw` | integer | Running firmware version number |

---

## Safety Features

### Hard Speed Cap

The firmware clamps all speed values to a maximum of **80 %** PWM duty cycle
via `constrain()` in `applyMotor()`. This cap is enforced in firmware and
cannot be raised through any WebSocket command.

### Auto-stop Timer

The run timer is the primary automatic shutoff mechanism. Set a duration in the
UI; when it elapses the motor stops and the timer resets. If no timer is set the
motor runs until manually stopped.

### WiFi Loss

If the WiFi connection drops, the ESP32 attempts to reconnect every 10 seconds
in the main loop. The motor continues running during a dropout and stops only
if the auto-stop timer fires.

If the ESP32 fails to connect to WiFi at boot, it falls back to AP setup mode.

---

## Architecture

The entire stack runs on the ESP32 — there is no separate server.

| Component | Detail |
|-----------|--------|
| HTTP server | Port 80 — serves the HTML page, PWA manifest, SVG icon, `/stop`, `/update`, `/reset-wifi`, and `/ota-github` endpoints |
| WebSocket server | Port 81 — handles all real-time motor commands and state broadcasts |
| mDNS | Advertises `rockbox.local` on the local network |
| NVS (Preferences) | Persists swing speed, timer, kick-start config, and WiFi credentials across power cycles |
| PWA | HTML, CSS, JavaScript, manifest, and icon are all stored in flash as `PROGMEM` strings — no filesystem required |
| OTA engine | Compares running firmware version against `version.txt` on GitHub; downloads and flashes `firmware.bin` if newer |

### PWM Configuration

| Parameter | Value |
|-----------|-------|
| Channel | 0 |
| Frequency | 5 kHz |
| Resolution | 8-bit (0–255) |

### Compile-time Configuration

The following constants near the top of `src/main.cpp` can be changed before
flashing:

| Constant | Default | Description |
|----------|---------|-------------|
| `HOSTNAME` | `"rockbox"` | mDNS hostname (`http://<HOSTNAME>.local`) |
| `FW_VERSION` | integer | Firmware version; increment when creating a release |
| `OTA_USER` | `"admin"` | Username for the `/update` page |
| `OTA_PASS` | `"babyswing"` | Password for the `/update` page — **change this** |
| `AP_SSID` | `"RockBox-Setup"` | SSID broadcast in AP setup mode |

### Runtime Settings (persisted in NVS)

| Setting | Range | Default |
|---------|-------|---------|
| Swing speed | 5–80 % | 40 % |
| Auto-stop timer | 0–240 min | 0 (disabled) |
| Kick-start level | 0–100 % | 0 (disabled) |
| Kick-start duration | 50–2000 ms | 400 ms |
| Rocking pattern | steady / wave / lull | steady |

---

## Automated Builds

A GitHub Actions workflow (`.github/workflows/firmware-release.yml`) builds
`firmware.bin` automatically when a new Git tag is pushed. The resulting binary
is attached to the GitHub Release and served as the OTA target for all devices.

To release a new version:
1. Increment `FW_VERSION` in `src/main.cpp` and update `version.txt` to the
   same number.
2. Commit, tag (`git tag v<N>`), and push the tag.
3. The workflow builds the firmware and creates the release automatically.
