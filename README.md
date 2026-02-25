# Print-n-Prick

An IoT system that pairs a thermal receipt printer with a hand sanitizer dispenser, controlled remotely through Firebase. Send messages from your phone and they print on thermal paper along with live weather and sensor data. A secondary display module provides a touchscreen GUI, web dashboard, and real-time sensor monitoring.

## Firebase: URL and rules

### 1. Use your own Firebase project (change the URL)

The TZT and the test script both need your **Firebase Realtime Database URL**.

**In firmware (TZT):**  
Edit **`include/config_tzt.h`** and set:

```c
#define FIREBASE_DATABASE_URL "https://YOUR-PROJECT-ID-default-rtdb.firebaseio.com"
```

Replace `YOUR-PROJECT-ID` with your project ID (e.g. from Firebase Console → Project settings → Your apps → Realtime Database URL).

**In the Python test script:**  
Edit **`scripts/test_tzt_display.py`** and set:

```python
DEFAULT_FIREBASE_URL = "https://YOUR-PROJECT-ID-default-rtdb.firebaseio.com"
```

Or pass it when you run:

```bash
python scripts/test_tzt_display.py --firebase-url "https://YOUR-PROJECT-ID-default-rtdb.firebaseio.com"
```

**Where to get the URL:**  
Firebase Console → [Realtime Database](https://console.firebase.google.com/) → select your project → the URL is at the top (e.g. `https://myproject-default-rtdb.firebaseio.com`). Use the **default** database (not a named one) unless you changed it.

---

### 2. Database rules (TZT only uses `/commands` now)

The TZT now only reads **`/commands`** from Firebase (and deletes each command after use). You can use this minimal rule set:

```json
{
  "rules": {
    "commands": {
      ".read": true,
      ".write": true
    }
  }
}
```

In Firebase Console: **Realtime Database** → **Rules** → paste the above → **Publish**.

If you still have other apps or the web UI reading/writing other paths, keep those rules too; the important part is that **`commands`** has read and write so the TZT can poll and your script/shortcuts can push.

---

Publish the rules, then retry the TZT; "Unauthorized" should stop if the URL and network are correct.

---

## What Firebase is used for (only this)

Firebase is **only** used to send content to the TZT. All other data (settings, groceries, todos, reminders) is stored on the TZT’s **SD card**, not in Firebase.

| What you send | What the TZT does |
|---------------|-------------------|
| **Text message** | Prints it (via Main ESP32), saves it to SD (`/data/messages/`), and shows it on the TZT screen. |
| **Audio** | Saves it to SD (`/data/audio/`) and plays it on the TZT. You can send a **URL** (TZT downloads and saves) or a **filename** (play a file already on SD). |
| **Image** | Saves it to SD (`/data/images/`) and shows it on the TZT. You can send a **URL** (TZT downloads and saves) or a **filename** (show a file already on SD). |

**How to send:** Write a command object under Firebase `/commands/<unique_id>.json`. The TZT polls `/commands.json` every ~10–30 s, runs each command, then deletes it.

**Command types:**

- **`print`** – Text message  
  - `type`: `"print"`  
  - `data`: message text (string)  
  - `source`: optional, e.g. `"shortcut"` for full receipt style  

- **`download_audio`** – Download audio from URL, save to SD, and play  
  - `type`: `"download_audio"`  
  - `url`: full URL of the audio file  
  - `name`: filename to save (e.g. `alarm.mp3`)  

- **`play_audio`** – Play a file already on SD  
  - `type`: `"play_audio"`  
  - `data`: filename in `/data/audio/` (e.g. `beep.mp3`)  

- **`download_image`** – Download image from URL, save to SD, and show  
  - `type`: `"download_image"`  
  - `url`: full URL of the image  
  - `name`: filename to save (e.g. `photo.jpg`)  

- **`show_image`** – Show a file already on SD  
  - `type`: `"show_image"`  
  - `data`: filename in `/data/images/` (e.g. `logo.jpg`)  

Pump/LED and other hardware control are **not** done via Firebase; use the TZT’s web UI or HTTP API on port 8080.

---

## Architecture

The system is built on **two ESP32 modules** connected over ESP-NOW:

| | Main ESP32 (WROOM-32) | TZT Display ESP32 (CYD) |
|---|---|---|
| **Role** | Hardware controller | Brain / UI / cloud |
| **PlatformIO env** | `esp32-wroom-32` | `tzt-esp32-devkit` |
| **Serial port** | COM4 | COM3 |
| **Controls** | Printer, pump, LED, sensors | 2.4" TFT + touch, web server, Firebase, SD card, speaker |
| **Network** | WiFi (for ESP-NOW channel sync) | WiFi + Firebase + HTTP server (port 8080) |
| **Config file** | `include/config.h` | `include/config_tzt.h` |

```
                 ┌──────────────────┐
  iOS Shortcut ──┤                  │
  Python script ─┤  Firebase RTDB   │
  Web UI (POST) ─┤  /commands.json  │
                 └────────┬─────────┘
                          │ poll (10-30 s)
                          ▼
              ┌──────────────────────────┐
              │   TZT Display ESP32      │
              │   (CYD 2.4" ILI9341)     │
              │                          │
              │  ● Web server :8080      │
              │  ● Firebase client       │
              │  ● LVGL touchscreen GUI  │
              │  ● Reminder scheduler    │
              │  ● Grocery / Todo lists  │
              │  ● SD card (FAT32)       │
              │  ● Speaker (GPIO26 DAC)  │
              └────────────┬─────────────┘
                           │ ESP-NOW
                           ▼
              ┌──────────────────────────┐
              │   Main ESP32 (WROOM-32)  │
              │                          │
              │  ● Thermal printer       │
              │  ● Sanitizer pump        │
              │  ● 12 V LED (PWM)        │
              │  ● IR motion sensor      │
              │  ● Moisture sensor       │
              │  ● Light sensor (LM393)  │
              │  ● Sends sensor data     │
              │    back via ESP-NOW      │
              └──────────────────────────┘
```

### Message Flow

1. User sends message (iOS Shortcut, Python script, or web UI)
2. Message is written to Firebase `/commands/{id}.json` with `"processed": false`
3. TZT polls Firebase, detects unprocessed command
4. TZT sends print command to Main ESP32 over ESP-NOW (chunked if > 199 bytes)
5. Main ESP32 assembles message, fetches weather, reads sensors, prints receipt
6. TZT deletes the command from Firebase

### Sensor Data Flow

1. Main ESP32 reads sensors every 2 seconds
2. Sends `SensorDataPacket` to TZT over ESP-NOW every 10 seconds
3. TZT updates the LVGL display and caches data for the web dashboard

---

## Quick Start

### Prerequisites

- [PlatformIO](https://platformio.org/) (CLI or VS Code extension)
- USB data cable for each ESP32
- Firebase Realtime Database (free tier is sufficient)

### 1. Configure Firebase

See [Firebase Setup](#firebase-setup) below. You need the database URL in both config files.

### 2. Set MAC Addresses

Each board prints its MAC address on boot over serial. Copy them into the config files:

- Main ESP32's MAC goes into `include/config_tzt.h` as `MAIN_ESP32_MAC_ADDRESS`
- TZT Display's MAC goes into `include/config.h` as `TZT_DISPLAY_MAC_ADDRESS`

Format: `{0xAA, 0xBB, 0xCC, 0xDD, 0xEE, 0xFF}`

### 3. Build and Flash

```bash
# Main ESP32 (COM4)
pio run -e esp32-wroom-32 -t upload -t monitor

# TZT Display (COM3)
pio run -e tzt-esp32-devkit -t upload -t monitor
```

### 4. Connect to WiFi

On first boot each board creates a WiFi access point for configuration:

| Board | AP SSID | AP Password | Portal URL |
|---|---|---|---|
| Main ESP32 | `Print-n-Prick` | `08202022` | `http://192.168.4.1` |
| TZT Display | `TZT_Display` | `08202022` | `http://192.168.4.1` |

Connect to each AP, open the portal, and enter your home WiFi credentials. The boards save them and reconnect automatically on future boots.

### 5. Access the Web Dashboard

Once TZT is on your WiFi, find its IP in the serial monitor and open:

```
http://<TZT-IP>:8080
```

Password: `0820`

---

## Firebase Setup

### 1. Create a Firebase Project

1. Go to [Firebase Console](https://console.firebase.google.com/)
2. Click **Add project**, give it a name, and create it
3. In the sidebar, go to **Build > Realtime Database**
4. Click **Create Database**, choose a region, and start in **test mode**

### 2. Copy the Database URL

It looks like this:

```
https://your-project-id-default-rtdb.firebaseio.com
```

Paste it into both config files:

- `include/config.h` line: `#define FIREBASE_DATABASE_URL "https://..."`
- `include/config_tzt.h` line: `#define FIREBASE_DATABASE_URL "https://..."`

### 3. Set Security Rules

In **Realtime Database > Rules**, paste and publish:

```json
{
  "rules": {
    ".read": true,
    ".write": true
  }
}
```

> These rules allow public access. This is fine for personal/home use. For a public deployment, add Firebase Authentication and restrict rules.

### 4. Database Structure

The TZT Display manages all Firebase data. No manual setup is needed beyond creating the database; the firmware creates nodes automatically.

```
your-project-default-rtdb/
├── commands/                   # Print and dispense commands (created by clients, deleted after processing)
│   └── {pushId}.json           #   { type, data, source, processed }
├── config.json                 # Device settings (LED brightness, pump duration, auto-dispense, etc.)
├── reminders/                  # Scheduled reminders
│   └── {id}.json               #   { id, message, scheduledTime, createdTime, printed, active }
├── groceries.json              # Grocery list (JSON array of strings)
└── todos.json                  # Todo list (JSON array of strings)
```

### 5. Firebase Free Tier Limits

| Resource | Limit | Typical Usage |
|---|---|---|
| Storage | 1 GB | Well under 1 MB |
| Downloads | 10 GB/month | Minimal |
| Simultaneous connections | 100 | 1-2 |

The firmware includes rate limiting (60 requests/minute max) and adaptive polling (30 s idle, 10 s when active) to stay well within limits.

---

## Hardware

### Main ESP32 (WROOM-32)

#### Pin Map

| GPIO | Function | Direction | Component |
|------|----------|-----------|-----------|
| 2 | Onboard LED | Output | Status indicator |
| 4 | Serial TX2 | Output | Thermal printer TX |
| 26 | Pump control | Output | IRF520 MOSFET gate |
| 27 | LED PWM | Output | 12 V LED via MOSFET |
| 32 | IR sensor | Input | Motion detection |
| 33 | Serial RX2 | Input | Thermal printer RX |
| 34 | Moisture sensor | ADC Input | Capacitive moisture probe |
| 35 | Light sensor | ADC Input | LM393 analog output |

> GPIO 34 and 35 are input-only pins (no internal pull-ups). GPIO 4 and 33 replaced GPIO 16/17 to avoid flash-memory boot conflicts.

#### Components

- **Thermal Printer** -- QR204, 9 V, 9600 baud, Serial2
- **Sanitizer Pump** -- CJWP08 micro diaphragm, 3.3 V, via IRF520 MOSFET
- **12 V LED** -- 5 W, PWM dimming via IRF520 MOSFET, software ramp (1 V/s)
- **IR Sensor** -- Digital output, active LOW = detected
- **Moisture Sensor** -- Analog 0-4095 mapped to 0-100%
- **Light Sensor** -- LM393, analog output, potentiometer for sensitivity tuning

### TZT Display ESP32 (CYD -- Cheap Yellow Display)

The TZT module is an [ESP32 Cheap Yellow Display](https://github.com/witnessmenow/ESP32-Cheap-Yellow-Display) -- a single board with an integrated 2.4" ILI9341 TFT, XPT2046 resistive touch controller, micro SD card slot, speaker amplifier, and RGB LED.

#### Pin Map (Fixed by PCB)

**Display (HSPI):**

| GPIO | Function |
|------|----------|
| 2 | TFT DC (data/command) |
| 12 | TFT MISO |
| 13 | TFT MOSI |
| 14 | TFT SCLK |
| 15 | TFT CS |
| 27 | TFT backlight |

**Touch (shared HSPI):**

| GPIO | Function |
|------|----------|
| 33 | XPT2046 CS |
| 25 | XPT2046 CLK |
| 32 | XPT2046 MOSI |
| 36 | XPT2046 IRQ |
| 39 | XPT2046 MISO |

**SD Card (VSPI):**

| GPIO | Function |
|------|----------|
| 5 | SD CS |
| 18 | SD SCK |
| 19 | SD MISO |
| 23 | SD MOSI |

**Peripherals:**

| GPIO | Function |
|------|----------|
| 4 | RGB LED -- Red |
| 16 | RGB LED -- Green |
| 17 | RGB LED -- Blue |
| 26 | Speaker (DAC channel 2, onboard amp) |
| 34 | LDR (light-dependent resistor) |

**Connectors:**

| Label | Type | Pins | Use |
|-------|------|------|-----|
| P1 | 4-pin 1.25 mm JST | VIN, TX, RX, GND | Serial |
| P3 | 4-pin 1.25 mm JST | GND, IO35, IO22, IO21 | GPIO |
| P4 | 2-pin 1.25 mm JST | Speaker +/- | Speaker output |
| CN1 | 4-pin 1.25 mm JST | GND, IO22, IO27, 3.3V | GPIO / I2C |

> Reference: [CYD Pin Documentation](https://github.com/witnessmenow/ESP32-Cheap-Yellow-Display/blob/main/PINS.md)

#### LVGL Screens

The touchscreen GUI has three screens, navigated with Prev/Next buttons:

1. **Message** -- shows the last printed message (scrollable text area)
2. **Sensors** -- live moisture, sanitizer, LED, and pump status from the Main ESP32
3. **Sketch Board** -- freehand drawing canvas with 10 colors

---

## ESP-NOW Protocol

The two boards communicate over ESP-NOW, a low-latency peer-to-peer protocol that works alongside WiFi.

### Packet Types

| Type | ID | Direction | Purpose |
|------|----|-----------|---------|
| `SENSOR_DATA` | 0x01 | Main -> TZT | Sensor readings (moisture, light, IR, LED, pump) |
| `COMMAND` | 0x02 | TZT -> Main | Hardware commands (print, pump, LED, settings) |
| `ACK` | 0x03 | Both | Acknowledgment with sequence number |
| `STATUS_REQUEST` | 0x04 | TZT -> Main | Request immediate sensor data |
| `CHUNK_ACK` | 0x05 | Main -> TZT | Acknowledge receipt of a print chunk |

### Command Types

| Command | ID | Description |
|---------|----|-------------|
| `CMD_PRINT` | 0x01 | Print message (up to 200 bytes) |
| `CMD_DISPENSE_START` | 0x02 | Start pump |
| `CMD_DISPENSE_STOP` | 0x03 | Stop pump |
| `CMD_RESET_SANITIZER` | 0x04 | Reset sanitizer level to 100% |
| `CMD_SET_LED_BRIGHTNESS` | 0x05 | Set LED (param1 = 0-255) |
| `CMD_SET_AUTO_BRIGHTNESS` | 0x07 | Toggle auto-brightness (param1 = 0/1) |
| `CMD_SET_AUTO_DISPENSE` | 0x0D | Toggle auto-dispense (param1 = 0/1) |
| `CMD_PRINT_CHUNK_START` | 0x10 | Begin chunked print (param2 = total chunks) |
| `CMD_PRINT_CHUNK` | 0x0F | One chunk of a long message (param1 = index) |

### Chunked Printing

Messages longer than 199 bytes are split into chunks:

1. TZT sends `CMD_PRINT_CHUNK_START` with the total chunk count
2. TZT sends the first `CMD_PRINT_CHUNK`
3. Main acknowledges with `CHUNK_ACK`
4. TZT sends the next chunk; repeat until done
5. Main assembles the full message and prints

Timeout: 15 seconds per chunk. Retry: 2 seconds. Max payload: ~25 KB (128 chunks x 199 bytes).

### Reliability

- XOR checksums on every packet
- Sequence numbers for ACK matching
- Automatic retry on send failure (2-3 attempts)
- Collision avoidance: 700 ms quiet period after receiving sensor data before sending commands
- Duplicate detection: Main deduplicates repeated settings commands within 1 second

---

## Web Dashboard

The TZT Display hosts a web server on port 8080 with a mobile-optimized dashboard.

### Authentication

- Password: `0820` (configured in `config_tzt.h` as `WEB_PASSWORD`)
- Session tokens valid for 1 hour (cookie-based)

### Sections

- **Sensor Status** -- Moisture, sanitizer level, light, IR, LED brightness, pump state
- **Reminders** -- Schedule messages to print at a future time (presets: 1 min to 1 week)
- **Grocery List** -- Add items, print the list, clear all
- **Todo List** -- Add items, print the list, clear all
- **Hardware Controls** -- Test pump, test printer, LED brightness slider, auto-dispense toggle

### API Endpoints

| Method | Path | Description |
|--------|------|-------------|
| GET | `/api/status` | Sensor readings |
| GET | `/api/health` | Uptime, memory, WiFi info |
| POST | `/api/reset-sanitizer` | Reset sanitizer to 100% |
| GET/POST | `/api/reminders` | List or add reminders |
| DELETE | `/api/reminders/{id}` | Delete a reminder |
| GET/POST | `/api/groceries` | List or add grocery items |
| DELETE | `/api/groceries` | Clear grocery list |
| POST | `/api/groceries/print` | Print grocery list |
| GET/POST | `/api/todos` | List or add todo items |
| DELETE | `/api/todos` | Clear todo list |
| POST | `/api/todos/print` | Print todo list |
| POST | `/api/test/pump` | Test pump |
| POST | `/api/test/printer` | Test printer |
| POST | `/api/test/led12v` | Test 12 V LED |
| POST | `/api/led/calibrate` | Calibrate light sensor |
| POST | `/api/pump/duration` | Set pump duration |
| POST | `/api/pump/cooldown` | Set pump cooldown |
| POST | `/api/pump/auto-dispense` | Toggle auto-dispense |

---

## Remote Messaging

### iOS Shortcuts

Create a shortcut with these four actions:

1. **Ask for Input** -- Question: "Your message:", type: Text, allow multiple lines
2. **Text** -- Paste the JSON template below, replacing the placeholder with the Provided Input variable:

```json
{
  "type": "print",
  "data": "[Provided Input]",
  "processed": false
}
```

3. **Get Contents of URL**
   - URL: `https://<YOUR-FIREBASE-URL>/commands.json`
   - Method: POST
   - Headers: `Content-Type: application/json`
   - Request Body: **File** (select the Text from step 2)

4. **Show Notification** -- "Message sent!"

> Use `"source": "web"` in the JSON to print the message alone (no header/weather). Omit `source` or use `"source": "shortcut"` for a full receipt with weather and sensor data.

### Python Script

```bash
python send_message.py "Your message here"
```

### Firebase Console (Manual)

In the Realtime Database, add a child under `/commands/` with:

```json
{
  "type": "print",
  "data": "Hello from Firebase!",
  "processed": false
}
```

### Print Formats

**Full receipt** (Shortcut / no source):

```
================================
     SMIT'S MESSAGE
================================
Date: Feb 17, 2026 02:30 PM

Your message text here

--------------------------------
Today's Weather:
  Partly Cloudy, 52 F

Moisture: 45.2%  Sanitizer: 78%
================================
```

**Message-only** (Web UI / `"source": "web"`):

```
Your message text here
```

---

## Hardware Testing

### Main ESP32 Test Suite

Type `test` in the serial monitor (115200 baud) to enter the interactive test menu:

| Key | Test |
|-----|------|
| 1 | Pump (1 s run) |
| 2 | LED (full brightness, 2 s) |
| 3 | LED ramp up/down |
| 4 | IR sensor (5 s monitor) |
| 5 | Moisture sensor (5 s) |
| 6 | Light sensor (continuous) |
| 7 | Thermal printer |
| 8 | All sensors (10 s) |
| 9 | Run all tests |

Type `exit` to return to normal operation.

### TZT Display -- SD Card and Speaker Test

A standalone test sketch lives in the `test/` directory:

```bash
cd test
pio run -e tzt-sd-speaker -t upload -t monitor
```

| Key | Test |
|-----|------|
| 1 | SD card -- detect, read, write, append, speed test |
| 2 | Speaker -- beep, ascending tones, frequency sweep, melody |
| 3 | Both |

### TZT Display -- Touch Calibration

```bash
cd test
pio run -e tzt-touch-cal -t upload -t monitor
```

Six-point affine calibration with outlier rejection. Outputs `TOUCH_AX` through `TOUCH_CY` coefficients to paste into `config_tzt.h`.

---

## Project Structure

```
Prick_n_Print/
├── include/
│   ├── config.h              # Main ESP32 configuration (pins, timing, WiFi, ESP-NOW)
│   ├── config_tzt.h          # TZT Display configuration (display, WiFi, Firebase, ESP-NOW)
│   ├── User_Setup.h          # TFT_eSPI pin definitions (copied into library at build time)
│   ├── lv_conf.h             # LVGL configuration
│   ├── HardwareAbstraction.h # Hardware abstraction layer
│   ├── HardwareTest.h        # Test suite header
│   ├── PrinterService.h      # Thermal printer service
│   ├── FirebaseService.h     # Firebase HTTP client
│   ├── ReminderService.h     # Reminder scheduler
│   └── Logger.h              # Serial logging
├── src/
│   ├── main.cpp              # Main ESP32 entry point
│   ├── HardwareAbstraction.cpp
│   ├── HardwareTest.cpp      # Interactive hardware test suite
│   ├── PrinterService.cpp
│   ├── Logger.cpp
│   └── tzt-display/
│       └── main.cpp          # TZT Display entry point (web server, Firebase, LVGL, ESP-NOW)
├── data/
│   └── index.html            # Standalone message-sending page (writes to Firebase directly)
├── scripts/
│   └── copy_tft_user_setup.py  # Pre-build script: copies User_Setup.h into TFT_eSPI library
├── test/
│   ├── platformio.ini        # Test environments (tzt-sd-speaker, tzt-touch-cal)
│   └── src/
│       ├── sd_speaker_test.cpp   # SD card + speaker hardware test
│       └── main.cpp              # Touch calibration tool
└── platformio.ini            # Main build configuration (esp32-wroom-32, tzt-esp32-devkit)
```

### Build Environments

| Environment | Board | Source Filter | Purpose |
|---|---|---|---|
| `esp32-wroom-32` | ESP32 Dev | `+<*> -<tzt-display/>` | Main ESP32 firmware |
| `tzt-esp32-devkit` | ESP32 Dev | `-<*> +<tzt-display/>` | TZT Display firmware |
| `esp32-wroom-32-debug` | ESP32 Dev | same as main | Debug build with verbose logging |

---

## Configuration Reference

### `include/config.h` (Main ESP32)

| Define | Default | Description |
|--------|---------|-------------|
| `SANITIZER_PUMP_PIN` | 26 | Pump MOSFET gate GPIO |
| `THERMAL_TX_PIN` | 4 | Printer TX GPIO |
| `THERMAL_RX_PIN` | 33 | Printer RX GPIO |
| `IR_SENSOR_PIN` | 32 | IR motion sensor GPIO |
| `MOISTURE_SENSOR_PIN` | 34 | Moisture sensor ADC GPIO |
| `LIGHT_SENSOR_PIN` | 35 | Light sensor ADC GPIO |
| `LED_PWM_PIN` | 27 | 12 V LED MOSFET GPIO |
| `AP_SSID` | `"Print-n-Prick"` | WiFi AP name (first boot) |
| `AP_PASSWORD` | `"08202022"` | WiFi AP password |
| `TZT_DISPLAY_MAC_ADDRESS` | -- | TZT board's MAC (from serial) |
| `ESP_NOW_CHANNEL` | 6 | Fallback ESP-NOW channel |
| `ESP_NOW_SEND_INTERVAL` | 10000 | Sensor broadcast interval (ms) |
| `DISPENSE_COOLDOWN_MS` | 3000 | Min time between pump activations |
| `MAX_DISPENSE_DURATION_MS` | 2000 | Max pump run time |

### `include/config_tzt.h` (TZT Display)

| Define | Default | Description |
|--------|---------|-------------|
| `TZT_SCREEN_WIDTH` | 320 | Display width (landscape) |
| `TZT_SCREEN_HEIGHT` | 240 | Display height (landscape) |
| `TZT_TOUCH_CS` | 33 | Touch controller CS GPIO |
| `TZT_TFT_BL_PIN` | 27 | Backlight GPIO |
| `TZT_AP_SSID` | `"TZT_Display"` | WiFi AP name (first boot) |
| `TZT_AP_PASSWORD` | `"08202022"` | WiFi AP password |
| `MAIN_ESP32_MAC_ADDRESS` | -- | Main board's MAC (from serial) |
| `FIREBASE_DATABASE_URL` | -- | Firebase Realtime Database URL |
| `WEB_PASSWORD` | `"0820"` | Web dashboard login password |
| `ESP_NOW_CHANNEL` | 6 | Fallback ESP-NOW channel |
| `WEATHER_API_KEY` | -- | OpenWeatherMap API key |
| `WEATHER_LATITUDE` | -- | Location latitude |
| `WEATHER_LONGITUDE` | -- | Location longitude |
| `NTP_SERVER` | `"pool.ntp.org"` | Time server |
| `GMT_OFFSET_SEC` | -21600 | UTC offset (CST = -6 h) |
| `DAYLIGHT_OFFSET_SEC` | 3600 | DST offset (+1 h) |

---

## Troubleshooting

### ESP32 not detected (no COM port)

1. Use a **data-capable** USB cable (not charge-only)
2. Install the correct USB-serial driver:
   - CP210x: [Silicon Labs](https://www.silabs.com/developers/usb-to-uart-bridge-vcp-drivers)
   - CH340: [WCH](https://www.wch.cn/downloads/CH341SER_EXE.html)
3. Hold **BOOT**, press **RESET**, release BOOT, then upload
4. Run `pio device list` to check port; update `upload_port` in `platformio.ini`

### WiFi AP not appearing

1. Check serial monitor for boot messages
2. Force config portal: set `FORCE_WIFI_CONFIG_PORTAL` to `1` in the board's config file, flash, configure WiFi, then set it back to `0`

### Web UI controls do nothing (pump, printer, LED)

This means ESP-NOW between TZT and Main is not working:

1. Both boards must be on the **same WiFi SSID** and channel
2. Verify MAC addresses match: Main has TZT's MAC in `config.h`, TZT has Main's MAC in `config_tzt.h`
3. Check serial output for "ESP-NOW init OK" on both boards
4. If channel mismatches, set `ESP_NOW_CHANNEL` to your router's channel in both config files

### Messages not printing

1. Verify Firebase URL is correct in `config_tzt.h`
2. Check TZT serial monitor for Firebase polling activity
3. Ensure Firebase security rules allow read/write
4. TZT polls every 10-30 seconds; wait at least 30 seconds

### Firebase 401/403 errors

1. Go to Firebase Console > Realtime Database > Rules
2. Set `.read` and `.write` to `true`
3. Click Publish

---

## Power

### Power Rails

| Rail | Voltage | Load | Current |
|------|---------|------|---------|
| 3.3 V | 3.3 V | Pump + moisture/light sensors | ~520 mA peak |
| 5 V | 5 V | Main ESP32 + IR sensor + TZT display | ~620 mA peak |
| 9 V | 9 V | Thermal printer | ~1 A printing |
| 12 V | 12 V | 5 W LED | ~417 mA |

**Total peak: ~19 W output / ~24 W input (80% efficiency)**

A **30 W USB-C power supply** is recommended. A 20 W supply is insufficient when the printer, LED, and pump are active simultaneously.

### LM2596 Multi-Channel PSU

Provides 3.3 V, 5 V, 9 V, and 12 V from the USB-C input. Each rail has two screw terminals.
