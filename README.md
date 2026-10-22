# Print-n-Prick

An IoT system that pairs a thermal receipt printer with a hand sanitizer dispenser, controlled remotely through an HTTP backend. Send messages from your phone and they print on thermal paper along with live weather and sensor data. A secondary display module provides a touchscreen GUI, web dashboard, and real-time sensor monitoring.

## HTTP backend

The TZT display polls your web server for commands, audio, and images. You run the included Flask server (or your own) and POST commands to it; the ESP32 polls `/api/commands`, `/api/audio`, and `/api/images`.

**Configure:** In **`include/config_tzt.h`** set **`BACKEND_URL`** to your server (e.g. `"http://192.168.1.100:5000"`). See [BACKEND_HTTP.md](BACKEND_HTTP.md) for setup and usage.

| What you send | What the TZT does |
|---------------|-------------------|
| **Text message** | Prints it (via Main ESP32), saves it to SD (`/data/messages/`), and shows it on the TZT screen. |
| **Audio** | Saves it to SD (`/data/audio/`) and plays it on the TZT. You can send a **URL** (TZT downloads and saves) or a **filename** (play a file already on SD). |
| **Image** | Saves it to SD (`/data/images/`) and shows it on the TZT. You can send a **URL** (TZT downloads and saves) or a **filename** (show a file already on SD). |

**Command types** (POST to your backend; see `raspberrypi/README.md` for the API):

- **`print`** – Text message (`type`, `data`, optional `source`)
- **`download_audio`** – Download audio from URL, save to SD, and play
- **`play_audio`** – Play a file already on SD
- **`download_image`** – Download image from URL, save to SD, and show
- **`show_image`** – Show a file already on SD

Pump/LED and other hardware control use the TZT’s web UI or HTTP API on port 8080.

---

## Architecture

The system is built on **two ESP32 modules** connected over ESP-NOW:

| | Main ESP32 (WROOM-32) | TZT Display ESP32 (CYD) |
|---|---|---|
| **Role** | Hardware controller | Brain / UI / cloud |
| **PlatformIO env** | `esp32-wroom-32` | `tzt-esp32-devkit` |
| **Serial port** | COM3 (CP210x) | COM7 (CH340) |
| **Controls** | Printer, pump, LED, sensors | 2.4" TFT + touch, web server, HTTP backend, SD card, speaker |
| **Network** | WiFi (for ESP-NOW channel sync) | WiFi + HTTP backend + HTTP server (port 8080) |
| **Config file** | `include/config.h` | `include/config_tzt.h` |

```
                 ┌──────────────────┐
  iOS Shortcut ──┤                  │
  Python script ─┤  HTTP backend    │
  Web UI (POST) ─┤  /api/commands   │
                 └────────┬─────────┘
                          │ poll (10-30 s)
                          ▼
              ┌──────────────────────────┐
              │   TZT Display ESP32     │
              │   (CYD 2.4" ILI9341)    │
              │                          │
              │  ● Web server :8080     │
              │  ● HTTP backend client  │
              │  ● LVGL touchscreen GUI │
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

1. User sends message (iOS Shortcut, Python script, or web UI) to the HTTP backend
2. Message is POSTed to the backend (e.g. `/api/commands`)
3. TZT polls the backend every ~10–30 s, fetches new commands
4. TZT sends print command to Main ESP32 over ESP-NOW (chunked if > 199 bytes)
5. Main ESP32 assembles message, fetches weather, reads sensors, prints receipt
6. TZT marks the command as processed (backend may delete or mark it)

### Sensor Data Flow

1. Main ESP32 reads sensors every 2 seconds
2. Sends `SensorDataPacket` to TZT over ESP-NOW every 10 seconds
3. TZT updates the LVGL display and caches data for the web dashboard

---

## Quick Start

### Prerequisites

- [PlatformIO](https://platformio.org/) (CLI or VS Code extension)
- USB data cable for each ESP32
- HTTP backend server (see [BACKEND_HTTP.md](BACKEND_HTTP.md); optional for basic testing)

### 1. Set MAC Addresses

Each board prints its MAC address on boot over serial. Copy them into the config files:

- Main ESP32's MAC goes into TZT's own config, `include/config_tzt.h`, as `MAIN_ESP32_MAC_ADDRESS`
- TZT Display's MAC goes into Main's own config, `include/config.h`, as `TZT_DISPLAY_MAC_ADDRESS`

Format: `{0xAA, 0xBB, 0xCC, 0xDD, 0xEE, 0xFF}`

### 2. Build and Flash

```bash
# Main ESP32 (COM3)
pio run -e esp32-wroom-32 -t upload -t monitor

# TZT Display (COM7)
pio run -e tzt-esp32-devkit -t upload -t monitor
```

### 3. Connect to WiFi

On first boot each board creates a WiFi access point for configuration:

| Board | AP SSID | AP Password | Portal URL |
|---|---|---|---|
| Main ESP32 | `Print-n-Prick` | `your-portal-password` | `http://192.168.4.1` |
| TZT Display | `TZT_Display` | `your-portal-password` | `http://192.168.4.1` |

Connect to each AP, open the portal, and enter your home WiFi credentials. The boards save them and reconnect automatically on future boots.

### 4. Access the Web Dashboard

Once TZT is on your WiFi, find its IP in the serial monitor and open:

```
http://<TZT-IP>:8080
```

Password: `your-web-password` (set via `WEB_PASSWORD` in `secrets.h`)

---

## Hardware

### Main ESP32 (WROOM-32)

#### Pin Map

| GPIO | Function | Direction | Component |
|------|----------|-----------|-----------|
| 2 | Onboard LED | Output | Status indicator |
| 16 | Serial RX2 | Input | Thermal printer RX |
| 17 | Serial TX2 | Output | Thermal printer TX |
| 26 | Pump control | Output | IRF520 MOSFET gate |
| 27 | LED PWM | Output | 12 V LED via MOSFET |
| 32 | IR sensor | Input | Motion detection |
| 34 | Moisture sensor | ADC Input | Capacitive moisture probe |
| 35 | Light sensor | ADC Input | LM393 analog output |

> GPIO 34 and 35 are input-only pins (no internal pull-ups). GPIO 16/17 are used for the thermal printer's Serial2 UART.

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

The touchscreen GUI has a bottom navigation bar with four tabs, plus a persistent status bar (time/IP/WiFi SSID):

1. **Home** -- latest printed message, quick status, and a button into message history (SD card)
2. **Sensors** -- live moisture, sanitizer, LED, and pump status from the Main ESP32 (read-only)
3. **Media** -- Photo/Audio sub-tab toggle: browse and play received images/audio, reminders, groceries, and todos
4. **Settings** -- WiFi SSID display, volume control, and device configuration

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

- Password: `your-web-password` (set via `WEB_PASSWORD` in `secrets.h`)
- Session tokens valid for 1 hour (cookie-based)

### Sections

- **Sensor Status** -- Moisture, sanitizer level, light, IR, LED brightness, pump state (read-only; live data from the Main ESP32 over ESP-NOW)
- **Reminders** -- Schedule messages to print at a future time (presets: 1 min to 1 week)
- **Grocery List** -- Add items, print the list, clear all
- **Todo List** -- Add items, print the list, clear all
- **Hardware Diagnostics** -- Read sensors, send a test message through the full print pipeline (SD save -> ESP-NOW -> Main prints)

> Pump/LED testing and hardware settings (duration, cooldown, calibration, auto-dispense, auto-brightness) live only on the **Main ESP32's test server** (`http://<Main-ESP32-IP>:8081`) -- see [Hardware Testing](#hardware-testing) below. They used to be duplicated here and on the TZT, which let the two boards fight over settings on every TZT boot; now there's a single place that owns them.

### API Endpoints

| Method | Path | Description |
|--------|------|-------------|
| GET | `/api/login` | Shortcut-friendly login (session cookie) |
| GET | `/api/status` | Sensor readings |
| POST | `/api/reset-sanitizer` | Reset sanitizer to 100% |
| POST | `/api/test/send-message` | Send a test message through the full print pipeline |
| POST | `/api/test/play-audio` | Play an audio file already on SD |
| POST | `/api/test/show-image` | Show an image file already on SD |
| GET/POST | `/api/reminders` | List or add reminders |
| DELETE | `/api/reminders/{id}` | Delete a reminder |
| GET/POST | `/api/groceries` | List or add grocery items |
| DELETE | `/api/groceries` | Clear grocery list |
| POST | `/api/groceries/print` | Print grocery list |
| GET/POST | `/api/todos` | List or add todo items |
| DELETE | `/api/todos` | Clear todo list |
| POST | `/api/todos/print` | Print todo list |

Pump/LED/printer hardware control, tests, and settings are not on this server -- see [Hardware Testing](#hardware-testing) for the Main ESP32's own test server.

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
   - URL: `http://<YOUR-BACKEND-URL>/api/commands` (e.g. `http://192.168.1.100:5000/api/commands`)
   - Method: POST
   - Headers: `Content-Type: application/json`
   - Request Body: **File** (select the Text from step 2)

4. **Show Notification** -- "Message sent!"

> Use `"source": "web"` in the JSON to print the message alone (no header/weather). Omit `source` or use `"source": "shortcut"` for a full receipt with weather and sensor data.

### Python Script

```bash
python scripts/post_to_backend.py --url http://YOUR_SERVER_IP:5000 --message "Your message here"
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

### Main ESP32 Test Server

The Main ESP32 runs an HTTP test server on **port 8081** (`http://<Main-ESP32-IP>:8081`) -- open it in a browser for a bench-test panel, or hit the routes directly with curl. This is the **only** place hardware is tested or reconfigured; the TZT dashboard's old duplicate controls (and its boot-time settings push over ESP-NOW) were removed so the two boards can no longer fight over pump/LED settings.

| Route | Description |
|-------|-------------|
| GET `/` | Bench-test web panel |
| GET `/status` | Live hardware status (JSON) |
| GET `/sensors` | Read moisture/IR/light now |
| GET `/printer/test` | Printer test pattern |
| GET `/printer/raw` | Raw test line |
| GET `/printer/receipt` | Sample receipt |
| GET `/printer/feed?lines=N` | Feed N lines |
| GET `/pump?ms=N` | Run pump for N ms (max 5000) |
| GET `/led?level=N&ms=N` | Set LED level (0-255) for N ms |
| GET `/settings` | Get pump duration/cooldown, auto-dispense, auto-brightness |
| GET `/settings/pump-duration?ms=N` | Set max pump dispense duration |
| GET `/settings/pump-cooldown?ms=N` | Set pump cooldown between dispenses |
| GET `/settings/auto-dispense?enabled=0\|1` | Toggle IR-sensor-triggers-pump |
| GET `/settings/auto-brightness?enabled=0\|1` | Toggle LED-follows-light-sensor |
| GET `/settings/light-calibration?point=min\|max` | Record current light reading as the dark/bright calibration point |

### TZT Display -- Touch Calibration

The current `TOUCH_AX`-`TOUCH_CY` coefficients in `config_tzt.h` were captured from a one-off calibration sketch that is no longer part of this repo. If the touchscreen needs recalibrating on different hardware, write a small sketch using `TFT_eSPI`'s built-in `calibrateTouch()` and paste the resulting coefficients into `config_tzt.h`.

---

## Project Structure

```
Prick_n_Print/
├── include/
│   ├── config.h              # Main ESP32 configuration (pins, timing, WiFi, ESP-NOW)
│   ├── config_tzt.h          # TZT Display configuration (display, WiFi, HTTP backend, ESP-NOW)
│   ├── User_Setup.h          # TFT_eSPI pin definitions (copied into library at build time)
│   ├── lv_conf.h             # LVGL configuration
│   ├── HardwareAbstraction.h # Hardware abstraction layer
│   ├── TestServer.h          # Bench-test HTTP server (port 8081) - hardware tests & settings
│   ├── PrinterService.h      # Thermal printer service
│   ├── HttpBackendService.h  # HTTP backend client
│   ├── ReminderService.h     # Reminder scheduler
│   └── Logger.h              # Serial logging
├── src/
│   ├── main.cpp              # Main ESP32 entry point
│   ├── HardwareAbstraction.cpp
│   ├── TestServer.cpp        # Bench-test HTTP server (port 8081) - hardware tests & settings
│   ├── PrinterService.cpp
│   ├── Logger.cpp
│   └── tzt-display/
│       └── main.cpp          # TZT Display entry point (web server, HTTP backend, LVGL, ESP-NOW)
├── raspberrypi/
│   ├── app.py                # Flask backend (commands, audio, images, reminders)
│   ├── setup.sh              # Pi install script (Tailscale, nginx, ufw, systemd)
│   └── tests/                # pytest suite for app.py
├── scripts/
│   ├── copy_tft_user_setup.py  # Pre-build script: copies User_Setup.h into TFT_eSPI library
│   ├── ota_upload_auth.py      # OTA upload auth helper (esp32-*-ota envs)
│   ├── post_to_backend.py      # CLI: send a message/image/audio to the backend
│   └── test_backend.py         # CLI: smoke-test a running backend
└── platformio.ini            # Build configuration (esp32-wroom-32[-debug|-ota], tzt-esp32-devkit[-ota], tzt-esp32-lvgl)
```

### Build Environments

| Environment | Board | Port | Purpose |
|---|---|---|---|
| `esp32-wroom-32` | ESP32 Dev | COM3 | Main ESP32 firmware (release) |
| `esp32-wroom-32-debug` | ESP32 Dev | COM3 | Main ESP32, verbose logging (`CORE_DEBUG_LEVEL=5`) |
| `esp32-wroom-32-ota` | ESP32 Dev | `Print-n-Prick.local` | Main ESP32, OTA upload over WiFi |
| `tzt-esp32-devkit` | ESP32 Dev | COM7 | TZT Display firmware (release) |
| `tzt-esp32-lvgl` | ESP32 Dev | auto-detect | Same TZT firmware as `tzt-esp32-devkit`, no fixed port |
| `tzt-esp32-devkit-ota` | ESP32 Dev | `TZT-Display.local` | TZT Display, OTA upload over WiFi |

Main uses `build_src_filter = +<*> -<tzt-display/>`; TZT uses `-<*> +<tzt-display/>` (plus the shared `HttpBackendService`/`ReminderService`/`Logger`/`OTAUpdateService` files). OTA uploads require `OTA_PASSWORD` in `secrets.h` and the board already on WiFi from a prior USB flash.

---

## Configuration Reference

### `include/config.h` (Main ESP32)

| Define | Default | Description |
|--------|---------|-------------|
| `SANITIZER_PUMP_PIN` | 26 | Pump MOSFET gate GPIO |
| `THERMAL_TX_PIN` | 17 | Printer TX GPIO |
| `THERMAL_RX_PIN` | 16 | Printer RX GPIO |
| `IR_SENSOR_PIN` | 32 | IR motion sensor GPIO |
| `MOISTURE_SENSOR_PIN` | 34 | Moisture sensor ADC GPIO |
| `LIGHT_SENSOR_PIN` | 35 | Light sensor ADC GPIO |
| `LED_PWM_PIN` | 27 | 12 V LED MOSFET GPIO |
| `AP_SSID` | `"Print-n-Prick"` | WiFi AP name (first boot) |
| `AP_PASSWORD` | `"your-portal-password"` | WiFi AP password |
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
| `TZT_AP_PASSWORD` | `"your-portal-password"` | WiFi AP password |
| `MAIN_ESP32_MAC_ADDRESS` | -- | Main board's MAC (from serial) |
| `BACKEND_URL` | `"http://192.168.1.100:5000"` | HTTP backend server URL (no trailing slash) |
| `WEB_PASSWORD` | `"your-web-password"` | Web dashboard login password |
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

1. Verify `BACKEND_URL` is correct in `config_tzt.h` and the backend server is running
2. Check TZT serial monitor for backend polling activity
3. TZT polls every 10-30 seconds; wait at least 30 seconds
4. Use `scripts/post_to_backend.py` or the server API to send commands (see BACKEND_HTTP.md)

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
