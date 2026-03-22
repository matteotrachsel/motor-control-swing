# ESP32 Baby Swing Controller

An ESP32-based motor controller for a baby swing. The motor drives a crank
mechanism that converts continuous rotation into a back-and-forth swing motion.
A built-in Progressive Web App (PWA) lets any phone, tablet, or desktop on the
same WiFi network control the swing — no app store required.

---

## Features

- **WebSocket real-time control** — low-latency commands and live state sync
  across all connected devices simultaneously
- **Progressive Web App** — installable on Android and iOS home screens; runs
  full-screen with no browser chrome
- **mDNS hostname** — reach the UI at `http://babyswing.local` without
  memorising an IP address
- **Speed control** — adjustable swing speed via a slider (10–80 %)
- **Hard speed cap** — firmware enforces an 80 % PWM ceiling; the cap cannot be
  overridden remotely
- **Safety watchdog** — motor stops automatically if no client sends a heartbeat
  for 30 seconds
- **Manual motor control** — hidden panel for forward/reverse jog, useful during
  setup or maintenance
- **Emergency stop** — always-visible stop button; also accessible via HTTP GET
  `/stop` as a fallback if WebSocket fails
- **WiFi auto-reconnect** — connection is checked every 10 seconds; the ESP32
  reconnects automatically on link loss

---

## Hardware

### Bill of Materials

| Component | Notes |
|-----------|-------|
| ESP32 development board | Any standard 38-pin ESP32 DevKit |
| L298N motor driver module | Must have the 5 V regulator jumper installed |
| 12 V DC motor | Sized for your swing's crank load |
| 12 V power supply | Current rating matched to motor stall current |
| Connecting wire | —  |

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

> All GND pins must be connected together: PSU ground, L298N ground, and ESP32
> ground.

---

## Software Setup

### Prerequisites

- [Visual Studio Code](https://code.visualstudio.com/)
- [PlatformIO IDE extension](https://platformio.org/install/ide?install=vscode)

### Steps

1. **Clone or download** the repository and open the project folder in VS Code.
   PlatformIO will detect `platformio.ini` automatically.

2. **Set your WiFi credentials** — open `src/main.cpp` and edit the two lines
   near the top:

   ```cpp
   const char *WIFI_SSID     = "YourNetworkName";
   const char *WIFI_PASSWORD = "YourPassword";
   ```

3. **Build and flash** — connect the ESP32 via USB, then click
   **Upload** in the PlatformIO toolbar (or run `pio run --target upload`).
   PlatformIO auto-detects the serial port. To pin a specific port, uncomment
   the `upload_port` line in `platformio.ini`.

4. **Open the Serial Monitor** at 115200 baud to confirm the ESP32 connects to
   WiFi and to read its assigned IP address.

---

## Usage

### Accessing the Web UI

After the ESP32 boots and connects to WiFi, the web interface is available at:

- `http://babyswing.local` — works on most devices without needing the IP
- `http://<ip-address>` — the assigned IP is printed in the Serial Monitor at
  startup

### Installing as a PWA

**Android (Chrome):**
1. Open `http://babyswing.local` in Chrome.
2. Tap the three-dot menu and choose **Add to Home screen** (or tap the
   **Install app** banner that appears at the top of the page).
3. The app opens full-screen from your home screen with no browser bar.

**iOS (Safari):**
1. Open `http://babyswing.local` in Safari.
2. Tap the share button (box with arrow), then **Add to Home Screen**.
3. The app opens full-screen from your home screen.

### Swing Mode

Tap the large circular button to start the swing. The motor runs continuously
in the forward direction; the crank mechanism converts that rotation into
back-and-forth motion. Tap the button again to stop.

The button glows and shows a spinning icon while the motor is running. All
connected devices see the same state in real time.

### Speed Slider

The slider sets the motor speed as a percentage (10–80 %). Moving the slider
while the swing is running sends an update immediately (with a 120 ms debounce).
Moving it while the swing is stopped sets the speed that will be used when the
swing next starts.

### Manual Control

Expand the **Manual control** section at the bottom of the page. Three buttons
let you drive the motor forward, reverse, or stop it directly. This bypasses
swing mode and is intended for installation testing or maintenance. Swing mode
is automatically cancelled when a manual command is sent.

### Emergency Stop

The red **Emergency Stop** button is always visible. It sends a `stop` command
over WebSocket. The HTTP endpoint `GET /stop` provides the same result if the
WebSocket connection is unavailable.

---

## API / WebSocket Protocol

Connect to `ws://<device-ip>:81`.

### Commands (client → ESP32)

All commands are JSON objects with a `"cmd"` field.

| `cmd` | Additional fields | Description |
|-------|-------------------|-------------|
| `heartbeat` | — | Resets the safety watchdog timer. The PWA sends this automatically every 15 seconds. |
| `swing_start` | `speed` (integer, optional) | Starts the swing at the given speed percentage. If `speed` is omitted the last-set swing speed is used. Clamped to 5–80 %. |
| `swing_stop` | — | Stops the motor and clears swing mode. |
| `set` | `dir` (`"forward"`, `"reverse"`, or `"stop"`), `speed` (integer) | Manual motor override. Cancels swing mode. Speed is clamped to 0–80 %. |
| `set_speed` | `speed` (integer) | Updates the swing speed. If the swing is currently running, the new speed takes effect immediately. Clamped to 5–80 %. |
| `stop` | — | Hard stop — halts the motor and cancels swing mode. |

Examples:

```json
{"cmd":"swing_start","speed":50}
{"cmd":"set_speed","speed":60}
{"cmd":"set","dir":"reverse","speed":30}
{"cmd":"heartbeat"}
{"cmd":"stop"}
```

### State Broadcast (ESP32 → all clients)

The ESP32 broadcasts the current state to every connected client after any
command and when clients connect or disconnect.

```json
{
  "swing":    false,
  "speed":    0,
  "dir":      "stop",
  "swingSpd": 40,
  "clients":  1
}
```

| Field | Type | Description |
|-------|------|-------------|
| `swing` | boolean | Whether swing mode is active |
| `speed` | integer | Current motor speed in % (0 when stopped) |
| `dir` | string | Current motor direction: `"forward"`, `"reverse"`, or `"stop"` |
| `swingSpd` | integer | Speed percentage that will be used on the next `swing_start` |
| `clients` | integer | Number of currently connected WebSocket clients |

---

## Safety Features

### Watchdog Timer

If the motor is running and no WebSocket client sends any message (including a
heartbeat) for **30 seconds**, the firmware stops the motor automatically. The
PWA sends a heartbeat every 15 seconds while it is open, so the watchdog only
fires when every connected device has closed the app or lost WiFi.

### Hard Speed Cap

The firmware clamps all speed values to a maximum of **80 %** PWM duty cycle
via `constrain()` in `applyMotor()`. This cap is enforced in firmware and
cannot be raised through any WebSocket command.

### WiFi Loss

If the WiFi connection drops, the ESP32 attempts to reconnect every 10 seconds
in the main loop. The motor continues running during a brief dropout but the
watchdog will stop it after 30 seconds if no client reconnects.

If the ESP32 fails to connect to WiFi at boot, it restarts automatically after
3 seconds and tries again.

---

## Architecture

The entire stack runs on the ESP32 — there is no separate server.

| Component | Detail |
|-----------|--------|
| HTTP server | Port 80 — serves the HTML page, PWA manifest, SVG icon, and `/stop` endpoint |
| WebSocket server | Port 81 — handles all real-time motor commands and state broadcasts |
| mDNS | Advertises `babyswing.local` on the local network |
| PWA | The HTML, CSS, JavaScript, manifest, and icon are all stored in flash as `PROGMEM` strings — no filesystem required |

The HTML page connects back to the ESP32's own IP on port 81 using
`ws://` WebSocket. All state changes broadcast to every connected client so
multiple phones always show the same view.
