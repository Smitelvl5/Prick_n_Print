# Print-n-Prick - IoT Message Dispenser System

An integrated IoT system combining a thermal printer and hand sanitizer dispenser with remote messaging capabilities. The system enables remote message delivery through Firebase, printing messages on thermal receipts along with environmental data including weather conditions, moisture sensor readings, and sanitizer level information.

## Quick Overview

**Two ESP32 Architecture:**
- **Main ESP32** (`esp32-wroom-32`): Controls all hardware (printer, pump, sensors), processes Firebase commands, provides API endpoints
- **TZT Display ESP32** (`tzt-esp32-lvgl`): Hosts web interface, displays sensor data, proxies API requests to Main ESP32

**Key Features:**
- 📱 Remote message delivery via iOS Shortcuts or Python script
- 🖨️ Thermal printer with formatted receipts (weather, sensors, timestamps)
- ⏰ Automated reminder system with scheduling
- 🛒 Grocery list management with print functionality
- 🧴 Hand sanitizer dispenser with IR sensor detection
- 📊 Real-time sensor monitoring (moisture, light, sanitizer level)
- 🌐 Mobile-optimized web interface
- 📡 ESP-NOW peer-to-peer communication between modules
- ☁️ Firebase Realtime Database for cloud sync

## Table of Contents

- [Web Server Interface](#web-server-interface)
- [Remote Message Delivery](#remote-message-delivery)
- [Print Output Formats](#print-output-formats)
- [WiFi and Access Credentials](#wifi-and-access-credentials)
- [Quick Start Guide](#quick-start-guide)
- [API Documentation](#api-documentation)
- [Firebase Configuration](#firebase-configuration)
- [System Operation](#system-operation)
- [System Requirements](#system-requirements)
- [Hardware Components](#hardware-components)
- [Power Calculations](#power-calculations)
- [Display Integration](#display-integration-35-tft-lcd-ili9486)
- [System Architecture](#system-architecture)
- [Troubleshooting](#troubleshooting)
- [Usage Tips](#usage-tips)

---

## System Architecture

The system consists of **two ESP32 modules** working together to provide a complete IoT solution:

### ESP32 Module Overview

#### 1. Main ESP32 Module (`esp32-wroom-32`)
**Primary Role:** Hardware controller and Firebase command processor

**Responsibilities:**
- **Hardware Control:**
  - Thermal printer control (Serial2 communication)
  - Sanitizer pump control (MOSFET via GPIO 26)
  - 12V LED brightness control (PWM via GPIO 27)
  - Sensor reading (moisture, light, IR motion detection)
  - GPIO pin management for all peripherals

- **ESP-NOW Communication:**
  - Sends real-time sensor data to TZT Display every 2 seconds
  - Receives commands from TZT Display via ESP-NOW (print, dispense, LED, tests)
  - Maintains peer connection with TZT Display module

- **System Services:**
  - OTA update support

**Key Features:**
- Direct hardware control of all peripherals
- ESP-NOW peer-to-peer communication with TZT (no Firebase or HTTP server on Main)

#### 2. TZT ESP32 LVGL Display (`tzt-esp32-lvgl`)
**Primary Role:** Web interface host and display controller

**Responsibilities:**
- **Web Server:**
  - Hosts web-based management interface on port 8080
  - Serves HTML/CSS/JavaScript for mobile-optimized UI
  - Handles user authentication (password: `0820`)
  - Proxies all API requests to Main ESP32 module

- **Display Management:**
  - Controls 3.5" TFT LCD display (ILI9486 controller)
  - LVGL graphics library for UI rendering
  - Real-time sensor data visualization
  - Touch screen support (if available)

- **ESP-NOW Communication:**
  - Receives sensor data from Main ESP32 every 2 seconds
  - Can send commands to Main ESP32 via ESP-NOW
  - Requests sensor updates when needed
  - Maintains peer connection with Main ESP32 module

- **API Proxy:**
  - All `/api/*` requests are forwarded to Main ESP32
  - Maintains authentication tokens
  - Provides seamless user experience
  - Handles CORS and error responses

**Key Features:**
- Mobile-optimized web interface
- Real-time sensor data display
- ESP-NOW peer-to-peer communication
- Authentication and session management
- Hardware test interface

### System Communication Flow

```
┌─────────────────────────────────────────────────────────────────┐
│                         USER INTERACTIONS                        │
└─────────────────────────────────────────────────────────────────┘
                              │
                              │
        ┌─────────────────────┴─────────────────────┐
        │                                           │
        ▼                                           ▼
┌───────────────┐                          ┌───────────────┐
│  iOS Shortcut │                          │  Web Browser  │
│  Python Script│                          │  (Mobile/PC)  │
└───────┬───────┘                          └───────┬───────┘
        │                                           │
        │ POST to Firebase                          │ HTTP Request
        │                                           │
        ▼                                           ▼
┌──────────────────────────────────────────────────────────────┐
│                    FIREBASE REALTIME DATABASE                  │
│  ┌──────────────┐  ┌──────────────┐  ┌──────────────┐       │
│  │  commands    │  │  reminders   │  │  groceries   │       │
│  │    .json     │  │    .json     │  │    .json     │       │
│  └──────────────┘  └──────────────┘  └──────────────┘       │
└──────────────────────────────────────────────────────────────┘
        │                                           │
        │ Poll every 30s                            │ HTTP Response
        │                                           │
        ▼                                           ▼
┌─────────────────────────────────────────────────────────────────┐
│                    MAIN ESP32 MODULE                            │
│  ┌──────────────────────────────────────────────────────────┐  │
│  │  Firebase Service: Polls commands, updates status        │  │
│  │  Printer Service: Prints messages, receipts, lists       │  │
│  │  Hardware Abstraction: Controls all GPIO and peripherals │  │
│  │  Reminder Service: Schedules and executes reminders      │  │
│  │  Request Queue: Non-blocking async operations            │  │
│  └──────────────────────────────────────────────────────────┘  │
│                                                                  │
│  Hardware Control:                                              │
│  • Thermal Printer (Serial2)                                    │
│  • Sanitizer Pump (GPIO 26)                                     │
│  • 12V LED (GPIO 27 PWM)                                        │
│  • Moisture Sensor (GPIO 34 ADC)                                │
│  • Light Sensor (GPIO 35 ADC)                                   │
│  • IR Motion Sensor (GPIO 32)                                   │
└─────────────────────────────────────────────────────────────────┘
        │                                           │
        │ ESP-NOW (every 1s)                        │ HTTP Proxy
        │ Sensor Data                                │ API Requests
        │                                           │
        ▼                                           ▼
┌─────────────────────────────────────────────────────────────────┐
│                  TZT ESP32 LVGL DISPLAY                          │
│  ┌──────────────────────────────────────────────────────────┐  │
│  │  Web Server: Serves HTML interface on port 8080         │  │
│  │  LVGL Display: Renders UI on 3.5" TFT screen             │  │
│  │  ESP-NOW Service: Receives sensor data, sends commands    │  │
│  │  API Proxy: Forwards requests to Main ESP32              │  │
│  └──────────────────────────────────────────────────────────┘  │
│                                                                  │
│  Display:                                                       │
│  • 3.5" TFT LCD (ILI9486)                                       │
│  • Touch Screen (optional)                                      │
│  • Real-time sensor visualization                              │
└─────────────────────────────────────────────────────────────────┘
```

### Complete System Flow

#### Flow 1: Remote Message Delivery (iOS Shortcut / Python Script)

1. **User sends message** via iOS Shortcut or Python script
2. **Message posted to Firebase** at `/commands/{commandId}.json`
3. **TZT Display polls Firebase** (or receives push) and detects new command
4. **TZT sends print command** to Main ESP32 via ESP-NOW
5. **Main ESP32** receives command and Printer Service executes print
6. **Receipt printed** with message, weather, sensor data
7. **TZT marks command as processed** in Firebase (and/or Main acknowledges)

#### Flow 2: Web Interface Access

1. **User opens browser** and navigates to TZT Display IP (e.g., `http://192.168.1.249:8080`)
2. **TZT Display serves login page** (password: `0820`)
3. **User authenticates** and receives session token
4. **TZT Display serves main interface** HTML/CSS/JavaScript
5. **User interacts with interface** (view status, add reminders, manage groceries)
6. **JavaScript makes API calls** to TZT Display endpoints
7. **TZT Display proxies requests** to Main ESP32 via HTTP
8. **Main ESP32 processes request** and returns response
9. **TZT Display forwards response** to browser
10. **Interface updates** with new data

#### Flow 3: Real-Time Sensor Data Display

1. **Main ESP32 reads sensors** (moisture, light, IR, sanitizer level)
2. **Sensor data updated** in ESP-NOW service
3. **Main ESP32 sends sensor data** to TZT Display via ESP-NOW every 2 seconds
4. **TZT Display receives data** via ESP-NOW callback
5. **TZT Display updates display** with new sensor values
6. **Web interface polls** `/api/status` endpoint (auto-refresh every 30s)
7. **TZT Display proxies status request** to Main ESP32
8. **Main ESP32 returns current sensor readings**
9. **Web interface updates** sensor displays

#### Flow 4: Reminder System

1. **User creates reminder** via web interface
2. **TZT Display proxies POST** to Main ESP32 `/api/reminders`
3. **Main ESP32 saves reminder** to Firebase `/reminders.json`
4. **Main ESP32 checks reminders** every 60 seconds
5. **When scheduled time arrives**, reminder triggers
6. **Printer Service prints** reminder message
7. **Reminder marked as printed** in Firebase
8. **Web interface shows** updated reminder status

#### Flow 5: ESP-NOW Command Flow (TZT Display → Main ESP32)

1. **User interacts with TZT Display** (touch screen or web interface)
2. **TZT Display sends command** via ESP-NOW to Main ESP32
3. **Main ESP32 receives command** via ESP-NOW callback
4. **Command processed** (e.g., start pump, change LED brightness)
5. **Hardware responds** to command
6. **Main ESP32 sends acknowledgment** back via ESP-NOW (optional)

### Building and Uploading

**Main ESP32 Module:**
```bash
# Build firmware
pio run -e esp32-wroom-32

# Upload to device
pio run -e esp32-wroom-32 --target upload

# Monitor serial output
pio device monitor -e esp32-wroom-32
```

**TZT ESP32 LVGL Display:**
```bash
# Build firmware
pio run -e tzt-esp32-lvgl

# Upload to device
pio run -e tzt-esp32-lvgl --target upload

# Monitor serial output
pio device monitor -e tzt-esp32-lvgl
```

**Note:** Each ESP32 requires separate firmware upload. The build system automatically excludes the appropriate source files for each environment.

## Web Server Interface

The **TZT ESP32 LVGL Display** hosts the web-based management interface. The display module proxies all API requests to the main ESP32 module which controls the hardware.

**Architecture Note:**
- **TZT Display** serves the HTML/CSS/JavaScript web interface
- **TZT Display** proxies all `/api/*` requests to **Main ESP32**
- **Main ESP32** processes API requests and controls hardware
- **Main ESP32** returns responses back through **TZT Display** to the browser
- This separation allows the TZT Display to focus on UI while Main ESP32 handles hardware

### Accessing the Web Interface

**Option 1: Connected to Your WiFi Network (Recommended)**

1. **TZT Display connects to your WiFi** - On first boot, TZT Display creates access point `TZT_Display` (password: `08202022`)
2. **Configure WiFi** - Connect to `TZT_Display` AP, open browser to `192.168.4.1`, and configure your WiFi credentials
3. **Obtain IP Address** - Check serial monitor output for: `TZT Display Web Server started on http://[YOUR-IP]:8080`
   - Example: `TZT Display Web Server started on http://192.168.1.249:8080`
4. **Configure Main ESP32 IP** - Edit `include/config_tzt.h` and set `MAIN_MODULE_IP` to your main ESP32's IP address
5. **Open Browser** - Navigate to the TZT Display IP address (e.g., `http://192.168.1.249:8080`)
6. **Authentication** - Enter password: `0820`

**Option 2: Direct Access Point Mode**

1. **Connect to TZT Display AP** - Connect to WiFi network `TZT_Display` (password: `08202022`)
2. **Open Browser** - Navigate to `http://192.168.4.1:8080`
3. **Authentication** - Enter password: `0820`

**Note:** 
- The TZT Display IP address is **dynamic** when connected to your WiFi. Always check the serial monitor for the current IP address.
- The web server runs on **port 8080** on the TZT Display.
- The TZT Display proxies all API calls to the main ESP32 module (configured in `include/config_tzt.h`).
- The main ESP32 module only provides API endpoints (no web interface).

**Mobile Support** - Interface is optimized for mobile devices (iOS and Android)

### Web Interface Features

#### System Status Dashboard
- **🟣 Moisture Sensor** - Real-time moisture percentage display
- **Sanitizer Level** - Current sanitizer level percentage
- **Reset Sanitizer** - Reset sanitizer level to 100% after refilling

#### Reminders Management
- **Schedule Reminders** - Configure messages to print automatically at specified times
- **Add Reminders** - Create reminders with custom messages and date/time (preset options: 1 min, 30 mins, 1 hour, 12 hours, 1 day, 1 week)
- **View Reminders** - Display all pending and completed reminders
- **Edit Reminders** - Modify existing reminder configurations
- **Delete Reminders** - Remove reminders before execution

#### Grocery List Management
- **Grocery List** - Maintain a list of grocery items
- **Add Items** - Add items to the grocery list
- **Print List** - Print formatted grocery list to thermal printer
- **Clear List** - Remove all items from the grocery list

### Mobile Optimization
- **Cross-Platform Support** - Compatible with iOS and Android devices
- **Touch Interface** - Optimized for touch input
- **Auto-Refresh** - Automatic status updates every 30 seconds
- **Persistent Storage** - All data synchronized with Firebase
- **Offline Capability** - Interface remains functional when Firebase connectivity is unavailable

---

## Remote Message Delivery

The system supports remote message delivery from any location with internet connectivity.

### Method 1: iOS Shortcuts (iOS Devices)

Send messages directly from iOS devices using the built-in Shortcuts application without requiring additional software.

#### Step-by-Step Setup

**1. Open Shortcuts App**
- Pre-installed on all iPhones (blue icon with white squares)
- If you can't find it, download from App Store

**2. Create New Shortcut**
- Tap the **"+"** button (top right)
- Tap **"Add Action"**

**3. Add Actions (In Order):**

**Action 1: Ask for Input**
- Search: **"Ask for Input"**
- Configure:
  - **Question:** `💌 Your message to her:`
  - **Input Type:** Text
  - **Allow Multiple Lines:** ON

**Action 2: Create JSON Text**
- Search: **"Text"**
- Type this exactly:
```json
{
  "type": "print",
  "data": "MESSAGE_HERE",
  "processed": false
}
```
- Tap on `"MESSAGE_HERE"` and delete it
- Tap the blue **"Provided Input"** variable (from Action 1) to insert it
- Result should show: `"data": "[Variable: Provided Input]"`

**Action 3: Send to Firebase**
- Search: **"Get Contents of URL"**
- Configure:
  - **URL:** `https://printerpot-d96f8-default-rtdb.firebaseio.com/commands.json`
  - **Method:** **POST**
  - Tap **"Show More"** to expand options
  - **Headers:** Tap **"Add Field"**
    - **Key:** `Content-Type`
    - **Value:** `application/json`
  - **Request Body:** Select **"File"** (Important: NOT JSON or Text!)
  - **Body:** Select the **"Text"** variable from Action 2

**Action 4: Show Success Message**
- Search: **"Show Notification"**
- Configure:
  - **Title:** `💌 Sent!`
  - **Body:** `Your message is printing now ❤️`

**4. Customize Your Shortcut**
- Name it: **"💕 Send Love Note"**
- Choose an icon (Heart or Envelope)
- Add to Home Screen for one-tap access

**5. Usage:**
1. Execute the shortcut from the home screen or widget
2. Enter the message text
3. Tap **Done**
4. The message will be queued for printing on the device

#### Additional Configuration Options
- **Home Screen Widget:** Long-press home screen → **"+"** → **Shortcuts** → Add widget
- **Apple Watch Integration:** Shortcut automatically appears on Apple Watch
- **Siri Integration:** Use voice command *"Hey Siri, [Shortcut Name]"* for hands-free operation

---

### Method 2: Python Script (Desktop/Server)

#### Setup
```bash
python send_message.py "Your message here"
```

#### Features
- Send messages with integrated weather data, moisture sensor readings, and sanitizer level
- Command-line interface for quick message delivery
- Weather data displayed in Fahrenheit
- Includes moisture sensor percentage
- Includes sanitizer level information

#### Usage
```bash
# Interactive mode (prompts for message input)
python send_message.py

# Command line mode (message provided as argument)
python send_message.py "Your message text"
```

---

## Print Output Formats

### Message Receipt Format
When a message is sent via iOS Shortcut or Python script, the thermal printer outputs:

```
================================
SMIT'S MESSAGE
================================
Date: Dec 25, 2024 02:30 PM

[Your romantic message here]

--------------------------------
Today's Weather:
  Sunny, 72°F

Moisture: 45.2%  Sanitizer: 78.5%
================================
```

### Reminder Receipt Format
When a scheduled reminder executes, the output includes:
```
================================
REMINDER
================================
Set on: Dec 25, 2024 10:00 AM

[Her reminder message here]

================================
```

### Grocery List Format
When printing the grocery list:
```
================================
GROCERY LIST
================================
Date: Dec 25, 2024 02:30 PM

1. Milk
2. Bread
3. Eggs
4. Apples

================================
```

---

## WiFi and Access Credentials

### ESP32 Access Point (Initial Setup)
- **SSID:** `Print-n-Prick`
- **Password:** `08202022`
- **Purpose:** Used for initial WiFi configuration when ESP32 can't connect to your network
- **Note:** No username required - just SSID and password

### Web Interface Authentication
- **Password:** `0820`
- **Username:** Not required (password-only authentication)
- **Purpose:** Access to the web management interface

### Your WiFi Network Configuration
- **Method:** Configured through WiFiManager portal
- **Process:** 
  1. Connect to ESP32 AP (`Print-n-Prick`)
  2. Open browser to `192.168.4.1` (captive portal may open automatically)
  3. Enter your WiFi network SSID and password
  4. ESP32 saves credentials and connects automatically on future boots
- **Note:** Your WiFi credentials are stored on the ESP32 and not hardcoded in the firmware

### Changing Credentials

**To change ESP32 Access Point password:**
Edit `include/config.h`:
```cpp
#define AP_SSID "Print-n-Prick"        // Change AP name here
#define AP_PASSWORD "08202022"         // Change AP password here
```

**To change Web Interface password:**
Edit `include/config.h`:
```cpp
#define WEB_PASSWORD "0820"            // Change web password here
```

**To reset WiFi network credentials:**
- Hold the BOOT button on ESP32 for 10+ seconds during boot, OR
- Delete the WiFi credentials from ESP32 flash memory using esptool

---

## Quick Start Guide

### 1. Upload Firmware
```bash
pio run --target upload
```

### 2. Access Web Interface
- Connect to WiFi network `Print-n-Prick` (password: `08202022`)
- Navigate to `http://192.168.4.1` in a web browser
- Enter authentication password: `0820`
- Web interface provides access to:
  - System status monitoring (moisture sensor, sanitizer level)
  - Reminder scheduling and management
  - Grocery list management
  - Sanitizer level reset functionality

### 3. Send Messages
```bash
python send_message.py "Your message text"
```

Alternatively, use iOS Shortcuts for one-tap message delivery from iOS devices.

---

## API Documentation

### API Architecture

**Important:** The TZT Display module hosts the web interface and **proxies all API requests** to the Main ESP32 module. This means:

- **Web Interface:** Served by TZT Display on port 8080
- **API Endpoints:** Proxied from TZT Display to Main ESP32
- **Hardware Control:** All hardware operations executed by Main ESP32
- **Authentication:** Handled by TZT Display, token passed to Main ESP32

### Base URL

**For Web Interface Access:**
- `http://[TZT-Display-IP]:8080` - Access web interface (served by TZT Display)
- `http://192.168.4.1:8080` - Access point mode (if WiFi not configured)

**For Direct API Access (Advanced):**
- `http://[Main-ESP32-IP]:8080` - Direct access to Main ESP32 API (bypasses TZT Display)
- `http://[TZT-Display-IP]:8080/api/*` - Proxied access via TZT Display (recommended)

### Authentication
- **Web Interface Password:** `0820` (configured in `config_tzt.h`)
- **Session Tokens:** Valid for 1 hour after login
- **Cookie-based:** Authentication token stored in browser cookie
- **Token Passing:** TZT Display forwards authentication to Main ESP32

### Rate Limiting
- **Default Limit:** 60 requests per minute per IP address
- **Rate Limit Response:** HTTP 429 Too Many Requests

### API Endpoints

#### GET `/api/status`
Get current sensor readings and sanitizer level.

**Response:**
```json
{
  "moisture": "45.3",
  "sanitizer": "78.5"
}
```

#### POST `/api/reset-sanitizer`
Reset sanitizer level to 100% (use when refilling).

**Response:**
```json
{
  "success": true
}
```

#### GET `/api/reminders`
Get all reminders.

**Response:**
```json
[
  {
    "id": "1234567890",
    "message": "Don't forget to smile! 😊",
    "scheduledTime": 1638360000,
    "printed": false
  }
]
```

#### POST `/api/reminders`
Create a new reminder.

**Request Body:**
```json
{
  "message": "Good morning beautiful! ☀️",
  "scheduledTime": 1638360000
}
```

**Response:**
```json
{
  "success": true
}
```

#### DELETE `/api/reminders/{id}`
Delete a specific reminder.

#### GET `/api/groceries`
Get all grocery items.

**Response:**
```json
["Milk", "Bread", "Eggs", "Apples"]
```

#### POST `/api/groceries`
Add an item to the grocery list.

**Request Body:**
```json
{
  "item": "Bananas"
}
```

#### DELETE `/api/groceries`
Clear all grocery items.

#### POST `/api/groceries/print`
Print the grocery list to thermal printer.

#### GET `/api/health`
System health check endpoint.

**Response:**
```json
{
  "healthy": true,
  "firmware": "2.0.0",
  "uptime": 3600000,
  "wifi": {
    "connected": true,
    "ip": "192.168.1.248",
    "rssi": -45
  },
  "memory": {
    "freeHeap": 234567,
    "usagePercent": 27
  }
}
```

---

## Firebase Configuration

### Database URL
`https://printerpot-d96f8-default-rtdb.firebaseio.com`

### Firebase Security Rules
Update Firebase Console → Realtime Database → Rules:

```json
{
  "rules": {
    ".read": true,
    ".write": true
  }
}
```

**Security Note:** These rules permit public access. Suitable for personal use; implement authentication for production deployments.

### Firebase Quotas (Free Tier)
- **Data Storage:** 1 GB ✅
- **Downloads:** 10 GB/month ✅
- **Uploads:** 10 GB/month ✅
- **Connections:** 100 simultaneous ✅
- **Operations:** Unlimited* ✅

*May throttle excessive requests (queue system prevents this)

### Database Structure
```
printerpot-d96f8-default-rtdb/
├── config.json (settings: ledBrightness, autoDispense, autoBrightness, pumpDurationTenths, pumpCooldownTenths, checksPerDay)
├── commands/ (print/water commands)
├── reminders/ (scheduled messages)
├── groceries.json (grocery list)
└── todos.json (todo list)
```
Settings in `config` are loaded on TZT boot so the device and Main ESP32 remember LED, toggles, and pump duration/cooldown. Sensor data is not stored in Firebase.

### Firebase Command Interface

Commands can be sent through Firebase Realtime Database at `/commands/{commandId}`.

#### Command Structure
```json
{
  "type": "command_type",
  "data": "command_data",
  "processed": false
}
```

#### Available Commands

**Print Message**

Optional `source` tag controls how the message is printed:
- **`source: "web"`** – Message only (no title, date, weather). Use from index.html or any web UI.
- **`source: "shortcut"`** or omit – Full receipt (title, date, weather, dividers). Use from iOS Shortcut or scripts.

```json
{
  "type": "print",
  "data": "Your romantic message here",
  "source": "web",
  "processed": false
}
```

Full receipt (e.g. iOS Shortcut; omit `source` or use `"shortcut"`):
```json
{
  "type": "print",
  "data": "Your romantic message here",
  "processed": false
}
```

**Start Sanitizer Dispense**
```json
{
  "type": "dispense_start",
  "data": "",
  "processed": false
}
```

**Stop Sanitizer Dispense**
```json
{
  "type": "dispense_stop",
  "data": "",
  "processed": false
}
```

**Get Weather**
```json
{
  "type": "weather",
  "data": "",
  "processed": false
}
```

---

## System Operation

### Message Delivery Workflow

**Step-by-step process for remote message delivery:**

1. **Message Submission** (User Device)
   - User sends message via iOS Shortcut, Python script, or Firebase console
   - Message posted to Firebase at `/commands/{commandId}.json`
   - Command structure: `{"type": "print", "data": "message text", "processed": false}`

2. **Firebase Polling** (TZT Display)
   - TZT Display polls Firebase (e.g. every 30 seconds) for new commands
   - Checks `/commands.json` for unprocessed commands
   - Uses FirebaseService with rate limiting

3. **Command Forwarding** (TZT → Main via ESP-NOW)
   - TZT detects new `print` command and sends CMD_PRINT to Main ESP32 via ESP-NOW
   - Main receives command and validates

4. **Execution** (Main ESP32)
   - Print command executed by PrinterService (weather/sensors may be fetched by TZT or Main depending on implementation)

5. **Weather Data Fetching** (TZT or Main)
   - Fetches current weather from OpenWeatherMap API
   - Uses configured latitude/longitude coordinates
   - Formats temperature in Fahrenheit, 12-hour time format

6. **Sensor Reading** (Main ESP32)
   - Reads moisture sensor (GPIO 34, ADC)
   - Reads sanitizer level (calculated from usage)
   - Reads light sensor (GPIO 35, ADC) - optional for display

7. **Receipt Generation** (Main ESP32 → Thermal Printer)
   - PrinterService formats receipt with:
     - Header: "SMIT'S MESSAGE"
     - Date and time (12-hour format with AM/PM)
     - Custom message text
     - Weather information (condition, temperature)
     - Sensor data (moisture percentage, sanitizer level)
   - Thermal printer outputs formatted receipt via Serial2

8. **Command Cleanup** (TZT → Firebase)
   - Command marked as processed in Firebase
   - Command deleted from `/commands/{commandId}.json`
   - Prevents duplicate processing

**Timing:** Messages typically print within 30-60 seconds of submission (depending on polling cycle)

### Reminder System

**Automated reminder scheduling and execution:**

1. **Reminder Creation** (TZT Display → Main ESP32)
   - User accesses web interface on TZT Display (e.g., `http://192.168.1.249:8080`)
   - User navigates to Reminders tab
   - User enters message and selects scheduled time (preset options: 1 min, 30 mins, 1 hour, 12 hours, 1 day, 1 week)

2. **API Request** (TZT Display → Main ESP32)
   - TZT Display proxies POST request to Main ESP32 `/api/reminders`
   - Request includes: `{"message": "...", "scheduledTime": timestamp}`

3. **Data Persistence** (Main ESP32 → Firebase)
   - Main ESP32 saves reminder to Firebase `/reminders.json`
   - Reminder persists across system reboots
   - Each reminder has unique ID, message, scheduled time, and printed status

4. **Reminder Monitoring** (Main ESP32)
   - Main ESP32 checks scheduled reminders every 60 seconds
   - Compares current time with scheduled times
   - Identifies reminders that are due and not yet printed

5. **Automatic Execution** (Main ESP32)
   - When scheduled time is reached, reminder triggers automatically
   - PrinterService prints reminder message
   - Reminder marked as `printed: true` in Firebase

6. **Receipt Content:**
   - Header: "REMINDER"
   - Reminder creation timestamp
   - Reminder message text (standard size, centered)
   - No additional environmental data (simpler format)

7. **Status Updates** (Main ESP32 → TZT Display)
   - Web interface auto-refreshes every 30 seconds
   - Shows updated reminder status (pending/completed)
   - User can view, edit, or delete reminders

### Grocery List Management

**Shared grocery list with print functionality:**

1. **List Access** (TZT Display)
   - User navigates to Groceries tab in web interface
   - Interface displays current grocery list items

2. **Item Addition** (TZT Display → Main ESP32 → Firebase)
   - User enters item name and clicks "Add Item"
   - TZT Display proxies POST to Main ESP32 `/api/groceries`
   - Main ESP32 adds item to Firebase `/groceries.json`
   - List persists across system reboots

3. **Item Management** (TZT Display → Main ESP32)
   - User can delete individual items via DELETE `/api/groceries/{itemId}`
   - User can clear entire list via DELETE `/api/groceries`
   - All changes synced to Firebase immediately

4. **Print Functionality** (TZT Display → Main ESP32 → Printer)
   - User clicks "Print List" button
   - TZT Display proxies POST to Main ESP32 `/api/groceries/print`
   - Main ESP32 formats grocery list:
     - Header: "GROCERY LIST"
     - Date and time stamp
     - Numbered list of items
   - Thermal printer outputs formatted list

5. **Data Synchronization** (Main ESP32)
   - Main ESP32 loads groceries from Firebase every 5 minutes
   - Ensures web interface and Firebase stay in sync
   - Handles concurrent modifications gracefully

### Hand Sanitizer Dispensing System

**Automatic sanitizer dispensing with safety features:**

1. **IR Sensor Detection** (Main ESP32)
   - Infrared motion sensor (GPIO 32) detects hand placement
   - Sensor provides digital HIGH signal when hand detected
   - Continuous monitoring in main loop

2. **Automatic Dispensing** (Main ESP32)
   - When IR sensor detects hand, pump activation can be triggered
   - Pump controlled via MOSFET (GPIO 26)
   - Pump runs for configured duration (max 2 seconds)

3. **Cooldown Protection** (Main ESP32)
   - 3-second cooldown period between dispenses
   - Prevents continuous operation and pump damage
   - Timer tracked in HardwareAbstraction class

4. **Safety Limit** (Main ESP32)
   - Maximum dispense duration: 2 seconds
   - Prevents excessive sanitizer dispensing
   - Hardware protection against pump overrun

5. **Level Monitoring** (Main ESP32)
   - System tracks sanitizer usage (estimated)
   - Reports sanitizer level percentage (0-100%)
   - User can reset level to 100% after refilling via web interface

6. **Status Updates** (Main ESP32 → TZT Display)
   - Sanitizer level sent to TZT Display via ESP-NOW every 2 seconds
   - Web interface displays current sanitizer level
   - User can reset level via "Reset Sanitizer" button

### ESP-NOW Real-Time Communication

**Peer-to-peer communication between ESP32 modules:**

1. **Sensor Data Transmission** (Main ESP32 → TZT Display)
   - Main ESP32 sends sensor data every 2 seconds via ESP-NOW
   - Data packet includes:
     - Moisture percentage
     - Sanitizer level
     - IR sensor state
     - Light sensor reading
     - LED brightness
     - Pump status
     - Timestamp and sequence number
   - Uses ESP-NOW protocol (low-latency, peer-to-peer)

2. **Display Updates** (TZT Display)
   - TZT Display receives sensor data via ESP-NOW callback
   - Updates LVGL display with real-time values
   - Updates web interface via JavaScript polling (every 30s)

3. **Command Transmission** (TZT Display → Main ESP32)
   - TZT Display can send commands to Main ESP32 via ESP-NOW
   - Commands include: print, dispense, LED control, etc.
   - Main ESP32 processes commands and controls hardware
   - Lower latency than HTTP proxy for time-sensitive operations

4. **Connection Management**
   - ESP-NOW peer connection established during initialization
   - MAC addresses configured in `config.h` and `config_tzt.h`
   - Automatic reconnection on communication failure
   - Channel synchronization (both modules use same WiFi channel)

---

## System Requirements

- **ESP32** microcontroller with WiFi connectivity
- **Thermal printer** (optional component)
- **Hand sanitizer pump** and **infrared sensor**
- **Python 3.7+** for control scripts (optional)
- **Firebase** account (free tier sufficient)

---

## Hardware Components

> **Device Color Legend:**
> - 🔵 **ESP32** - Blue
> - 🟢 **Thermal Printer** - Green  
> - 🟠 **Pump** - Orange
> - 🟡 **LED** - Yellow
> - 🟣 **Sensors** - Purple
> - ⚫ **MOSFET** - Gray
> - 🔷 **LM2596** - Teal
> - 🖥️ **Display** - Cyan

### Main Controller
- **ESP-WROOM-32 Development Expansion Board** with screw terminals
- **ESP32 ESP-WROOM-32 Module** (5V) - plugs into expansion board

### Printing System
- **Embedded Thermal Printer QR204** Receipt Ticket Printers (9V)

### Sensors
- **Infrared (IR) Motion Sensor Module** (5V) - hand detection
- **Moisture Sensor** (Analog) - connected to GPIO 34
- **LM393 Light Sensor Module** (Analog) - connected to GPIO 35

### Dispensing System
- **CJWP08 Micro M20 Diaphragm Water Pump** (3.3V) - hand sanitizer dispensing
- **IRF520 MOSFET Driver Module** - pump control (handles higher current loads)

### Lighting System
- **12V 5W LED** - ambient lighting
- **MOSFET Driver Module** - LED PWM control

### Display System
- **3.5" 480×320 TFT LCD Module Screen Display** (ILI9486 Controller) - SPI interface, 5V power

### Power Management
- **LM2596 Multi Channel Switching Power Supply Module** (3.3V/5V/9V/12V/ADJ Adjustable)
- **20W USB-C Wall Power Supply** (input power source)
- **Type-C USB Jack 3.1 Type-C 2Pin Female** (power input connector)

### ESP32 Pin Constraints and Boot Considerations

**⚠️ Important:** Some GPIO pins on ESP32 have special functions and can cause boot issues if not handled properly.

#### Pins to Avoid or Use with Caution:

**Strapping Pins (Must be in correct state during boot):**
- **GPIO 0**: Must be HIGH for normal boot (LOW = flash/programming mode)
- **GPIO 2**: Must be LOW during boot
- **GPIO 5**: Must be HIGH during boot
- **GPIO 12**: Must be LOW during boot
- **GPIO 15**: Must be HIGH during boot

**Flash Memory Interface (Can interfere with boot):**
- **GPIO 16**: Connected to flash memory - ⚠️ **Can cause boot issues if used for Serial/UART**
- **GPIO 17**: Connected to flash memory - ⚠️ **Can cause boot issues if used for Serial/UART**

**Flash/PSRAM Pins (Reserved for internal use):**
- **GPIO 6-11**: Reserved for flash/PSRAM - **DO NOT USE**

**Input-Only Pins (No output capability):**
- **GPIO 34, 35, 36, 39**: Input-only pins - Good for analog sensors, cannot be used for outputs

**USB Serial (Used for programming/debugging):**
- **GPIO 1 (TX0)**: USB Serial TX
- **GPIO 3 (RX0)**: USB Serial RX

#### Safe Pins for General Use:
- **GPIO 4, 13, 14, 18, 19, 21, 22, 23, 25, 26, 27, 32, 33**: Safe for general I/O
- **GPIO 0**: Can be used but requires HIGH state during boot (currently used for Touch IRQ with proper handling)
- **GPIO 4, 33**: Now used for Serial2 (UART2) to avoid boot issues with GPIO 16/17

### Pin Connections

Pins are organized by component type for easier reference and wiring.

#### Display System (TFT LCD + SD Card + Touch Screen)

**Shared SPI Bus (GPIO 23, 13, 14):**
- **GPIO 23**: SPI MISO - Shared by LCD, SD card, and Touch screen
  - LCD_D0 / SD_DO / Touch MISO
- **GPIO 13**: SPI MOSI - Shared by LCD, SD card, and Touch screen
  - LCD_D1 / SD_DI / Touch MOSI
- **GPIO 14**: SPI SCK - Shared by LCD, SD card, and Touch screen
  - SD_SCK / Touch SCK

**LCD Display Control:**
- **GPIO 22**: LCD_CS (LCD Chip Select)
- **GPIO 18**: LCD_RS (Data/Command Select - DC pin)
- **GPIO 19**: LCD_RST (Reset)

**Touch Screen Control:**
- **GPIO 25**: Touch CS (Touch Controller Chip Select)
- **GPIO 0**: Touch IRQ (Touch Interrupt - optional but recommended) - moved from GPIO 4 to free it for Serial2 TX

**SD Card:**
- Uses shared SPI bus (GPIO 23, 13, 14)
- **SD_SS**: May need separate CS pin if not sharing MISO properly

**Notes:** 
- Display, SD card, and touch screen all share the same SPI bus (GPIO 23/13/14). Use different chip select pins (LCD_CS, SD_SS, Touch CS) to communicate with each device separately.
- **SD_SS pin**: Check your SD card wiring - may need separate CS pin if not sharing MISO.
- **Touch screen requires GPIO 25 (CS) and GPIO 0 (IRQ)** for proper operation.

#### Sensors

**Digital Sensors:**
- **GPIO 32**: IR Sensor (Infrared Motion Sensor Module) - Digital input with pull-up, 5V

**Analog Sensors (Input-Only Pins):**
- **GPIO 34**: Moisture Sensor - Analog input, input-only pin (3.3V ADC)
- **GPIO 35**: Light Sensor (LM393 Light Sensor Module) - Analog input, input-only pin (3.3V ADC)

**Note:** GPIO 34 and 35 are input-only pins (no output capability), which is perfect for analog sensors.

#### MOSFET Control (Pump and LED)

- **GPIO 26**: Pump Control - IRF520 MOSFET Driver Module Gate (pump control signal)
  - Digital output, controls sanitizer pump via MOSFET module
- **GPIO 27**: LED PWM Control - 12V LED PWM control via MOSFET
  - PWM output (0-255 levels), controls 12V LED brightness via MOSFET module

**Note:** Both MOSFETs use 3.3V logic signals from ESP32 to control higher voltage/current loads.

#### Serial Communication (Thermal Printer)

✅ **Current Configuration (Safe for Boot):**
- **GPIO 4**: Thermal Printer TX (TX2 - ESP32 sends data to printer) ✅ **Safe - no boot issues**
- **GPIO 33**: Thermal Printer RX (RX2 - ESP32 receives data from printer) ✅ **Safe - no boot issues**

**Previous Configuration (Problematic - Now Fixed):**
- ~~GPIO 16/17~~ - These pins were moved because they can interfere with flash memory during boot

**Note:** 
- Serial2 is used with inverted logic enabled.
- TX2 (ESP32 TX pin on GPIO 4, connects to Printer RX)
- RX2 (ESP32 RX pin on GPIO 33, connects to Printer TX)
- Touch IRQ was moved from GPIO 4 to GPIO 0 to free up GPIO 4 for Serial2 TX

#### Status Indicators

- **GPIO 2**: Built-in LED (status indicator, BOARD_LED_PIN) - Digital output, 3.3V internal

### Complete GPIO Pin Assignment Table

All GPIO pins listed in numerical order for quick reference:

| GPIO Pin | Function | Type | Signal Voltage | Device Voltage | Component | Notes |
|----------|----------|------|----------------|---------------|-----------|-------|
| **GPIO 0** | Touch IRQ | Digital Input | 3.3V (logic) | 5V (Display VCC) | Touch Screen | Must be HIGH during boot (strapping pin) |
| **GPIO 4** | Serial TX2 | Serial Output | 3.3V | 9V (Printer VCC) | Thermal Printer | ✅ Safe for boot |
| **GPIO 13** | SPI MOSI | SPI Output | 3.3V | 5V (Display VCC) | TFT LCD / SD Card / Touch (Shared) | Shared SPI bus |
| **GPIO 14** | SPI SCK | SPI Clock | 3.3V | 5V (Display VCC) | TFT LCD / SD Card / Touch (Shared) | Shared SPI bus |
| **GPIO 18** | LCD RS | Digital Output | 3.3V | 5V (Display VCC) | TFT LCD | Data/Command select |
| **GPIO 19** | LCD RST | Digital Output | 3.3V | 5V (Display VCC) | TFT LCD | Reset pin |
| **GPIO 2** | Built-in LED | Digital Output | 3.3V | 3.3V (internal) | Status LED | On-board LED (BOARD_LED_PIN) |
| **GPIO 22** | LCD CS | SPI Chip Select | 3.3V | 5V (Display VCC) | TFT LCD | Chip select |
| **GPIO 23** | SPI MISO | SPI Input | 3.3V | 5V (Display VCC) | TFT LCD / SD Card / Touch (Shared) | Shared SPI bus |
| **GPIO 25** | Touch CS | SPI Chip Select | 3.3V | 5V (Display VCC) | Touch Screen | Touch controller CS |
| **GPIO 26** | Pump Control | Digital Output | 3.3V | 3.3V (MOSFET VIN) | MOSFET Gate (Pump) | Sanitizer pump control |
| **GPIO 27** | LED PWM | PWM Output | 3.3V | 12V (LED load) | 12V LED via MOSFET | Brightness control (0-255) |
| **GPIO 32** | IR Sensor | Digital Input | 3.3V (logic) | 5V (Sensor VCC) | IR Motion Sensor | Hand detection |
| **GPIO 33** | Serial RX2 | Serial Input | 3.3V | 9V (Printer VCC) | Thermal Printer | ✅ Safe for boot |
| **GPIO 34** | Moisture Sensor | Analog Input | 3.3V (ADC) | 3.3V | Moisture Sensor | Input-only pin |
| **GPIO 35** | Light Sensor | Analog Input | 3.3V (ADC) | 3.3V | LM393 Light Sensor | Input-only pin |

**Unused/Reserved Pins:**
- **GPIO 1, 3**: USB Serial (TX0, RX0) - Reserved for programming/debugging
- **GPIO 2, 5, 12, 15**: Strapping pins - Set to safe states, not used
- **GPIO 6-11**: Flash/PSRAM - Reserved for internal use, DO NOT USE
- **GPIO 16, 17**: Flash memory interface - Previously used for Serial2, moved to avoid boot issues
- **GPIO 20, 24, 28-31, 36-39**: Available for future use (some are input-only)

### GPIO & Voltage Summary Table (By Component Type)

Pins are organized by component type for easier reference.

#### Display System

| GPIO Pin | Function | Type | Signal Voltage | Device Voltage | Component |
|----------|----------|------|----------------|---------------|------------|
| **GPIO 23** | SPI MISO | SPI Input | 3.3V | 5V (Display VCC) | TFT LCD / SD Card / Touch (Shared) |
| **GPIO 13** | SPI MOSI | SPI Output | 3.3V | 5V (Display VCC) | TFT LCD / SD Card / Touch (Shared) |
| **GPIO 14** | SPI SCK | SPI Clock | 3.3V | 5V (Display VCC) | TFT LCD / SD Card / Touch (Shared) |
| **GPIO 22** | LCD CS | SPI Chip Select | 3.3V | 5V (Display VCC) | TFT LCD |
| **GPIO 18** | LCD RS | Digital Output | 3.3V | 5V (Display VCC) | TFT LCD |
| **GPIO 19** | LCD RST | Reset | Digital Output | 3.3V | 5V (Display VCC) | TFT LCD |
| **GPIO 25** | Touch CS | SPI Chip Select | 3.3V | 5V (Display VCC) | Touch Screen |
| **GPIO 0** | Touch IRQ | Digital Input | 3.3V (logic) | 5V (Display VCC) | Touch Screen |

#### Sensors

| GPIO Pin | Function | Type | Signal Voltage | Device Voltage | Component |
|----------|----------|------|----------------|---------------|------------|
| **GPIO 32** | IR Sensor | Digital Input | 3.3V (logic) | 5V (Sensor VCC) | IR Motion Sensor |
| **GPIO 34** | Moisture Sensor | Analog Input | 3.3V (ADC) | 3.3V | Moisture Sensor |
| **GPIO 35** | Light Sensor | Analog Input | 3.3V (ADC) | 3.3V | LM393 Light Sensor |

#### MOSFET Control

| GPIO Pin | Function | Type | Signal Voltage | Device Voltage | Component |
|----------|----------|------|----------------|---------------|------------|
| **GPIO 26** | Pump Control | Digital Output | 3.3V | 3.3V (MOSFET VIN) | MOSFET Gate (Pump) |
| **GPIO 27** | LED PWM | PWM Output | 3.3V | 12V (LED load) | 12V LED via MOSFET |

#### Serial Communication

| GPIO Pin | Function | Type | Signal Voltage | Device Voltage | Component | Status |
|----------|----------|------|----------------|---------------|------------|--------|
| **GPIO 4** | Serial TX2 | Serial Output | 3.3V | 9V (Printer VCC) | Thermal Printer | ✅ **Safe for boot** |
| **GPIO 33** | Serial RX2 | Serial Input | 3.3V | 9V (Printer VCC) | Thermal Printer | ✅ **Safe for boot** |

**Note:** GPIO 16/17 were previously used but moved to GPIO 4/33 to avoid flash memory boot issues. Touch IRQ was moved from GPIO 4 to GPIO 0 to free up GPIO 4 for Serial2 TX.

#### Status Indicators

| GPIO Pin | Function | Type | Signal Voltage | Device Voltage | Component |
|----------|----------|------|----------------|---------------|------------|
| **GPIO 2** | LED Control | Digital Output | 3.3V | 3.3V (internal) | Built-in LED (BOARD_LED_PIN) |

### Power Rail Summary

| Power Rail | Voltage | Connection 1 | Connection 2 | Total Current |
|------------|---------|--------------|--------------|---------------|
| **3.3V** | 3.3V | Pump (~300-500mA) | Moisture + Light Sensors (~10-20mA) | ~310-520mA |
| **5V** | 5V | ESP32 (~200-500mA) | IR Sensor (~10-20mA) + Display (~100mA) | ~310-620mA |
| **9V** | 9V | Thermal Printer (~50-100mA idle, ~500-1000mA printing) | *Available* | ~50-1000mA |
| **12V** | 12V | 12V LED (~417mA) | *Available* | ~417mA |

**Key Points:**
- All ESP32 GPIO signals are **3.3V logic** (regardless of device operating voltage)
- Devices operate at their specified voltages (3.3V, 5V, 9V, 12V) but communicate via 3.3V logic
- **Signal Voltage** = ESP32 GPIO output/input voltage (always 3.3V)
- **Device Voltage** = Component's power supply voltage

### MOSFET Module Wiring Guide

The system uses **2 IRF520 MOSFET Driver Modules** to control high-current devices (pump and LED).

#### Wiring Steps

**MOSFET Module #1 (Pump) - Correct Wiring:**
1. Connect **VIN** pin → **3.3V Rail** (module power supply - supports 3.3V or 5V)
2. Connect **GND** pin → **Power Supply GND** (common ground)
3. Connect **Signal** pin → **ESP32 GPIO 26** (control signal - 3.3V)
4. Connect **GND** pin → **ESP32 GND** (also connect to common ground)
5. Connect **V+** pin → **Pump positive (+) terminal**
6. Connect **V-** pin → **Pump negative (-) terminal**

**Note:** 
- The pump is powered through the MOSFET module's V+ and V- terminals, not directly from the power supply
- Module supports 3.3V input voltage, so 3.3V rail is correct
- The Load LED on the module should turn ON when GPIO 26 goes HIGH

**MOSFET Module #2 (LED) - Correct Wiring:**
1. Connect **VIN** pin → **5V Rail** (module power supply - use 5V for better logic compatibility, or 3.3V works too)
2. Connect **GND** pin → **Power Supply GND** (common ground)
3. Connect **Signal** pin → **ESP32 GPIO 27** (PWM control signal - 3.3V)
4. Connect **GND** pin → **ESP32 GND** (also connect to common ground)
5. Connect **V+** pin → **12V Rail** ⚠️ **IMPORTANT: This is the power source for the LED load!**
6. Connect **V-** pin → **LED negative (-) terminal**
7. Connect **LED positive (+)** → **12V Rail** (same as V+ - LED positive connects to same 12V rail as V+)

**Note:**
- **V+ MUST be connected to 12V rail** - this provides power to the load side of the MOSFET
- The LED positive and V+ both connect to the 12V rail
- When Signal goes HIGH, the MOSFET switches and connects V+ to V-, completing the circuit
- If V+ and V- show 0V, check that V+ is connected to 12V rail

#### Important Notes

- **MOSFET Module Specifications (HCMODU0083):**
  - **Input Voltage:** 3.3V or 5V (supports both!)
  - **Max Load Current:** <5A
  - **Output Load Voltage:** 0-24V
  - **Applications:** LED lights, DC motors, miniature pumps, solenoid valves
  - **LED Indicator:** Provides visual indication when load is being switched
- **IRF520 MOSFET Technical Details:**
  - **Logic/IO Voltage (Vgs):** Up to 10V (maximum gate-source voltage)
  - **Input Logic Threshold:** 2-4V (minimum voltage to turn on MOSFET)
  - **Supply Voltage (Vds):** Up to 100V
  - **Output Current:** 9.2A continuous, 33A peak
  - **RDS(on):** 0.27Ω typical (on-resistance)
  - **Package:** TO-220
  - **Note:** ESP32's 3.3V GPIO is above the 2-4V threshold, so it will work, though may not fully saturate. The HCMODU0083 module handles this with internal circuitry.
- **MOSFET modules have built-in protection** - The IRF520 modules include pull-down resistors and protection circuits
- **✅ 3.3V Compatibility** - The HCMODU0083 module supports 3.3V input voltage, so connecting VIN to 3.3V rail is correct and will work with ESP32's 3.3V GPIO signals
- **Module LED Indicators (HCMODU0083):**
  - **VCC/VIN LED** - Power indicator (should ALWAYS be ON when VIN is connected - this is normal!)
  - **Load LED** - Switching indicator (should turn ON when GPIO signal goes HIGH and MOSFET switches)
  - **Important:** The VCC/VIN LED staying on is CORRECT - it just means the module has power
  - **What to watch:** The Load LED should turn ON/OFF when you toggle the pump
   - **Troubleshooting:** If Load LED doesn't turn ON when GPIO goes HIGH:
    1. **Signal pin not connected** - Verify GPIO 26 is connected to Signal terminal
    2. **GND not connected** - Verify both module GND pins are connected to common ground
    3. **VIN not connected** - Verify VIN is connected to 3.3V rail (or 5V if preferred)
    4. **Load not connected** - Verify pump V+ and V- are connected to module V+ and V- terminals
    5. **Check Serial Monitor** - Look for "GPIO 26 state after write: HIGH" message
    6. **Test with multimeter** - Measure voltage on Signal pin when pump test is ON (should be ~3.3V)
- **PWM on GPIO 27** - The LED MOSFET receives PWM signals for brightness control (0-255 levels)
- **Digital on GPIO 26** - The pump MOSFET receives simple ON/OFF signals
- **Common Ground** - All GND connections must be connected together (power supply, ESP32, MOSFETs, and all components)
- **High-side vs Low-side switching** - These MOSFETs switch the **negative/ground** side of the load (low-side switching), which is common for N-channel MOSFETs

#### Troubleshooting: Load LED Not Turning ON / Pump Not Running

**For HCMODU0083 Module (supports 3.3V input):**

1. **Check Load LED** - Does the Load LED on the MOSFET module turn ON when you press the pump test button?
   - **YES (Load LED ON):** MOSFET is switching, but pump wiring might be wrong
   - **NO (Load LED OFF):** MOSFET isn't switching - check wiring below

2. **Verify Wiring (HCMODU0083 Configuration):**
   ```
   MOSFET Module Connections:
   - VIN → 3.3V Rail (or 5V - module supports both)
   - GND → Power Supply GND (common ground)
   - Signal → ESP32 GPIO 26
   - GND → ESP32 GND (also common)
   - V+ → Pump positive (+) terminal
   - V- → Pump negative (-) terminal
   
   Pump Connections:
   - Pump positive (+) → MOSFET Module V+
   - Pump negative (-) → MOSFET Module V-
   ```

3. **Check Serial Monitor:**
   - Look for: "Setting GPIO 26 to HIGH"
   - Look for: "GPIO 26 state after write: HIGH"
   - If GPIO shows HIGH but Load LED doesn't turn on, check Signal pin connection

4. **Test with Multimeter:**
   - Measure voltage on GPIO 26 when pump test is ON (should be ~3.3V)
   - Measure voltage on MOSFET Gate terminal (should match GPIO 26)
   - Measure voltage on MOSFET Drain when ON (should be close to 0V if MOSFET is conducting)

---

## Power Calculations

### Power Requirements by Rail

> **Note:** Each rail has 2 connection spots. Low-current sensors can share a connection.

#### 3.3V Rail (2 connections)
- **Connection 1:** Pump (CJWP08): ~300-500mA when running
- **Connection 2:** Moisture Sensor + Light Sensor (LM393): ~5-10mA + ~5-10mA = ~10-20mA (shared connection)
- **Total**: ~310-520mA (worst case ~0.52A)
- **Power**: 0.52A × 3.3V = **1.7W**

#### 5V Rail (2 connections)
- **Connection 1:** ESP32: ~200-500mA (spikes during WiFi transmission)
- **Connection 2:** IR Sensor Module + TFT LCD Display: ~10-20mA + ~100mA = ~110-120mA (shared connection)
- **Total**: ~310-620mA (worst case ~0.62A)
- **Power**: 0.62A × 5V = **3.1W**

#### 9V Rail (Custom) (2 connections)
- **Connection 1:** Thermal Printer (QR204): ~50-100mA idle, ~500-1000mA when printing
- **Connection 2:** *Available*
- **Total**: ~50-100mA idle, ~500-1000mA when printing
- **Power**: 1A × 9V = **9W** (peak during printing)

#### 12V Rail (2 connections)
- **Connection 1:** 12V 5W LED: ~417mA (5W ÷ 12V = 0.417A)
- **Connection 2:** *Available*
- **Total**: ~417mA
- **Power**: 0.417A × 12V = **5W**

### Total Power Consumption
- **Combined Output Power:** ~19.3W (peak: printer printing + LED on + pump running + display active)
- **With 80% Efficiency:** ~24.1W input required
- **Recommended USB-C Supply:** 30W minimum (20W insufficient with display)

### Power Supply Specifications
- **Input:** 5V USB-C (20W = 5V @ 4A, or 30W = 5V @ 6A recommended)
- **LM2596 Module:** Typically rated for 2-3A per channel
- **Available Input Current (20W):** 4A
- **Required Input Current:** ~4.82A (24.1W ÷ 5V)
- **Headroom (20W):** Negative margin - **30W supply required**

### Notes
- **Each rail has 2 connection spots** - components must be distributed accordingly
- Low-current sensors (Moisture Sensor, Light Sensor) share one connection on the 3.3V rail
- The 5V rail connection 2 is shared by IR Sensor Module and TFT LCD Display
- The 9V rail connection 1 is dedicated to the thermal printer (connection 2 available)
- The 12V rail connection 1 is dedicated to the LED (connection 2 available)
- Peak power consumption occurs when printer is printing, LED is active, pump is running, and display is active simultaneously
- **20W USB-C charger is insufficient with display** - 30W required for reliable operation
- ESP32 operates from 5V rail (expansion board handles voltage regulation internally)

---

## Display Integration: 3.5" TFT LCD (ILI9486)

### Display Specifications

- **Model:** 3.5" 480×320 TFT LCD Module
- **Controller:** ILI9486
- **Resolution:** 480×320 pixels
- **Interface:** 4-wire SPI
- **Power:** 5V (VCC), 3.3V logic (TTL)
- **Power Consumption:** ~80-150mA (typical ~100mA)
- **Display Colors:** RGB 65K color
- **AliExpress Item:** [3256808804486054](https://www.aliexpress.us/item/3256808804486054.html)

### Display Module Pinout

| Pin Name | Description | GPIO Connection | Interface Type |
|----------|-------------|-----------------|---------------|
| **GND** | Power source ground (multiple pins) | GND (common ground) | Power |
| **5V** | Power positive (5V) | 5V Rail (Connection 2) | Power |
| **3V3** | Power source positive (3.3V) | *Not used (using 5V)* | Power (optional) |
| **LCD_RST** | Module reset signal (low level reset) | GPIO 19 | LCD Control |
| **LCD_CS** | Module chip selection signal (low level enable) | GPIO 22 | LCD Control |
| **LCD_RS** | Command/data selection signal (low=command, high=data) | GPIO 18 | LCD Control |
| **LCD_WR** | Module write control signal | GPIO 23 (parallel) or N/A (SPI) | LCD Control (Parallel) |
| **LCD_RD** | Module read control signal | GPIO 25 (parallel) or N/A (SPI) | LCD Control (Parallel) |
| **LCD_D0** | LCD data bit 0 / SPI MISO | GPIO 23 | LCD Data (SPI/Parallel) |
| **LCD_D1** | LCD data bit 1 / SPI MOSI | GPIO 13 | LCD Data (SPI/Parallel) |
| **SD_SCK** | SD card clock (SPI clock) | GPIO 14 | SD Card SPI |
| **SD_DO** | SD card data out (MISO) | GPIO 23 (shared with LCD_D0) | SD Card SPI |
| **SD_DI** | SD card data in (MOSI) | GPIO 13 (shared with LCD_D1) | SD Card SPI |
| **SD_SS** | SD card chip select | GPIO 23 (or separate pin if needed) | SD Card SPI |
| **F_CS** | Flash memory chip select | GPIO 0 (Touch IRQ) or available GPIO | Flash Memory |

**Pin Summary:**
- **Power Pins:** GND (multiple), 5V, 3V3
- **LCD Control Pins:** LCD_RST, LCD_CS, LCD_RS, LCD_WR, LCD_RD
- **LCD Data Pins:** LCD_D0, LCD_D1 (used for SPI or parallel data)
- **SD Card Pins:** SD_SCK, SD_DO, SD_DI, SD_SS
- **Flash Memory:** F_CS (onboard flash chip select)

**Note:** 
- The module supports both **SPI** and **8-bit parallel** interfaces for the LCD
- LCD_D0 and LCD_D1 serve dual purpose: SPI data lines or parallel data bits
- SD card shares SPI bus with display (SD_DO/SD_DI use same pins as LCD_D0/LCD_D1)
- F_CS is for onboard flash memory (if present)
- LCD_WR and LCD_RD are only used in parallel interface mode

### GPIO Pin Assignments for Display

#### Option 1: SPI Interface with SD Card and Touch Screen (Recommended - Uses 9 pins)

**Shared SPI Bus (GPIO 23/13/14):**
- **GPIO 23**: MISO (Master In Slave Out) - shared by LCD, SD card, and touch screen
- **GPIO 13**: MOSI (Master Out Slave In) - shared by LCD, SD card, and touch screen
- **GPIO 14**: SCK (SPI Clock) - shared by LCD, SD card, and touch screen

**LCD Display:**
- **GPIO 22**: LCD_CS (LCD Chip Select)
- **GPIO 18**: LCD_RS (Data/Command - low=command, high=data)
- **GPIO 19**: LCD_RST (Reset - low level reset)

**SD Card:**
- **GPIO 23**: SD_SS (SD Card Chip Select) ⚠️ *Note: May need separate CS pin*

**Touch Screen:**
- **GPIO 25**: Touch CS (Touch Controller Chip Select)
- **GPIO 0**: Touch IRQ (Touch Interrupt - optional but recommended for efficient operation)

**Total Pins Required:** 9 pins (3 shared SPI + 3 LCD control + 1 SD CS + 2 Touch control)
**Advantages:** Fewer pins, simpler wiring, standard interface, SD card support, touch screen support, all devices share SPI bus efficiently

#### Option 2: 8-Bit Parallel Interface (Uses 13+ pins)

**Control Pins:**
- **GPIO 22**: LCD_CS (Chip Select)
- **GPIO 18**: LCD_RS (Command/Data Select)
- **GPIO 19**: LCD_RST (Reset)
- **GPIO 23**: LCD_WR (Write Control)
- **GPIO 25**: LCD_RD (Read Control)

**8-Bit Data Bus:**
- **GPIO 26**: LCD_D0 (Data bit 0)
- **GPIO 27**: LCD_D1 (Data bit 1)
- **GPIO 32**: LCD_D2 (Data bit 2) ⚠️ *Note: Currently used for IR sensor*
- **GPIO 33**: LCD_D3 (Data bit 3)
- **GPIO 25**: LCD_D4 (Data bit 4) ⚠️ *Note: Conflicts with LCD_RD*
- **GPIO 26**: LCD_D5 (Data bit 5) ⚠️ *Note: Conflicts with LCD_D0*
- **GPIO 27**: LCD_D6 (Data bit 6) ⚠️ *Note: Conflicts with LCD_D1*
- **GPIO 14**: LCD_D7 (Data bit 7) ⚠️ *Note: Conflicts with SPI SCK*

**Total Pins Required:** 13 pins (5 control + 8 data)
**Advantages:** Faster data transfer, better for high refresh rates
**Disadvantages:** More pins required, conflicts with existing GPIO assignments

**⚠️ Important:** The parallel interface conflicts with existing GPIO pins (GPIO 32 for IR sensor, and pin sharing issues). **SPI interface is recommended** to avoid conflicts.

### Power Connection

**5V Rail (Connection 2)**
- **5V Rail:** Shares with IR sensor (~10-20mA used by IR sensor, ~500mA available)
- **Display Power Consumption:** ~100mA typical
- **Total on 5V Rail Connection 2:** ~110-120mA (IR sensor + display)
- **Compatibility:** ✅ **Yes** - Fits perfectly on 5V rail connection 2

### Wiring Diagram

#### SPI Interface Wiring (Recommended)

```
3.5" TFT LCD (ILI9486) - SPI Mode:
   Power:
   5V ──────> 5V Rail (Connection 2, shared with IR sensor)
   GND ─────> GND (common ground)
   
   LCD Control:
   LCD_CS ──> GPIO 22 (Chip Select)
   LCD_RS ──> GPIO 18 (Data/Command)
   LCD_RST ─> GPIO 19 (Reset)
   
   LCD SPI (shared with SD card):
   LCD_D0 ──> GPIO 23 (MISO - ESP32 receives)
   LCD_D1 ──> GPIO 13 (MOSI - ESP32 sends)
   SD_SCK ──> GPIO 14 (Clock - shared with SD card)
   
   SD Card SPI:
   SD_DO ───> GPIO 23 (MISO - shared with LCD_D0)
   SD_DI ───> GPIO 13 (MOSI - shared with LCD_D1)
   SD_SCK ──> GPIO 14 (Clock - shared with LCD)
   SD_SS ───> GPIO 23 (SD Card Chip Select) ⚠️ Note: May need separate CS pin
   
   Touch Screen SPI:
   Touch MISO ─> GPIO 23 (shared with LCD_D0 and SD_DO)
   Touch MOSI ─> GPIO 13 (shared with LCD_D1 and SD_DI)
   Touch SCK ──> GPIO 14 (shared with LCD and SD card)
   Touch CS ───> GPIO 25 (Touch Controller Chip Select)
   Touch IRQ ──> GPIO 0 (Touch Interrupt - optional but recommended)
   
   LED ──────> 5V (backlight, optional - can share VCC)
```

**Notes:** 
- Display, SD card, and touch screen all share the same SPI bus (GPIO 23/13/14). Use different chip select pins (LCD_CS, SD_SS, Touch CS) to communicate with each device separately.
- **SD_SS pin**: May need a separate CS pin if SD card doesn't share MISO properly.
- **Touch screen uses GPIO 25 (CS) and GPIO 0 (IRQ)** for proper operation.

#### 8-Bit Parallel Interface Wiring (Alternative)

```
3.5" TFT LCD (ILI9486) - Parallel Mode:
   Power:
   5V ──────> 5V Rail (Connection 2)
   GND ─────> GND (common ground)
   
   LCD Control:
   LCD_RST ─> GPIO 19 (Reset)
   LCD_CS ──> GPIO 22 (Chip Select)
   LCD_RS ──> GPIO 18 (Command/Data)
   LCD_WR ──> GPIO 23 (Write Control)
   LCD_RD ──> GPIO 25 (Read Control)
   
   Data Bus:
   Pin 9 (LCD_D0) ──> GPIO 26
   Pin 10 (LCD_D1) ─> GPIO 27
   Pin 11 (LCD_D2) ─> GPIO 32 ⚠️ (conflicts with IR sensor)
   Pin 12 (LCD_D3) ─> GPIO 33
   Pin 13 (LCD_D4) ─> GPIO 25 ⚠️ (conflicts with LCD_RD)
   Pin 14 (LCD_D5) ─> GPIO 26 ⚠️ (conflicts with LCD_D0)
   Pin 15 (LCD_D6) ─> GPIO 27 ⚠️ (conflicts with LCD_D1)
   Pin 16 (LCD_D7) ─> GPIO 14 ⚠️ (conflicts with SPI SCK)
   
   LED ──────> 5V (backlight, optional)
```

**⚠️ Note:** Parallel interface requires reassigning GPIO pins and may conflict with existing sensors. **SPI interface is strongly recommended.**

### Complete GPIO Pin Assignment Summary (Display + SD Card + Touch Screen)

| GPIO Pin | Function | Module Pin | Device | Shared? |
|----------|----------|------------|--------|---------|
| **GPIO 23** | SPI MISO | LCD_D0 / SD_DO / Touch MISO | All (SPI Bus) | ✅ Yes - All devices |
| **GPIO 13** | SPI MOSI | LCD_D1 / SD_DI / Touch MOSI | All (SPI Bus) | ✅ Yes - All devices |
| **GPIO 14** | SPI SCK | SD_SCK / Touch SCK | All (SPI Bus) | ✅ Yes - All devices |
| **GPIO 22** | LCD CS | LCD_CS | LCD Display | ❌ No |
| **GPIO 18** | LCD RS | LCD_RS | LCD Display | ❌ No |
| **GPIO 19** | LCD RST | LCD_RST | LCD Display | ❌ No |
| **GPIO 23** | SD CS | SD_SS | SD Card | ❌ No ⚠️ *May need separate pin* |
| **GPIO 25** | Touch CS | Touch CS | Touch Screen | ❌ No |
| **GPIO 0** | Touch IRQ | Touch IRQ | Touch Screen | ❌ No |

**Total GPIO Pins Used:** 9 pins

**Quick Reference:**
- **Shared SPI Bus (3 pins):** GPIO 23, 13, 14
- **LCD Control (3 pins):** GPIO 22, 18, 19
- **SD Card (1 pin):** GPIO 23 (or separate CS pin if needed)
- **Touch Screen (2 pins):** GPIO 25, 4

**All Other System Pins (Organized by Component Type):**

**Sensors:**
- GPIO 32: IR Sensor
- GPIO 34: Moisture Sensor
- GPIO 35: Light Sensor

**MOSFET Control:**
- GPIO 26: Pump MOSFET
- GPIO 27: LED PWM

**Serial Communication:**
- GPIO 16: Printer RX2
- GPIO 17: Printer TX2

**Status Indicators:**
- GPIO 2: Built-in LED (BOARD_LED_PIN)

### Library Requirements

**Recommended Libraries:**
- **TFT_eSPI** - Optimized for ESP32, supports ILI9486
- **Adafruit_ILI9486** - Alternative option

**Installation:**
```cpp
// In platformio.ini, add:
lib_deps = 
    bodmer/TFT_eSPI@^2.5.0
```

### Power Supply Considerations

- **With Current 20W Supply:**
  - Adding 3.5" display: +0.5W (5V @ 100mA)
  - **Total:** ~18.8W output (~23.5W input)
  - ⚠️ **May exceed 20W supply capacity** - 30W recommended

- **With 30W Supply:**
  - ✅ **Plenty of headroom** for 3.5" display addition
  - Recommended before adding display
  - Additional ~6-7W available for other components

### Integration Notes

- **SPI Bus:** Uses HSPI (Hardware SPI) on ESP32 for optimal performance
- **Backlight:** Can be controlled via PWM on available GPIO if needed
- **Touch Support:** If display includes touch, additional pins required (see Touch Screen section below)
- **Refresh Rate:** ILI9486 supports up to 60fps for smooth animations

---

## Future Expansion: Touch Screen

### Touch Screen Compatibility Analysis

**Yes, there is room for a touch screen!** Here's the detailed breakdown:

#### GPIO Pins Available for Touch Screen

**SPI-Based Touch Screen (Recommended):**
- **Display Interface:** GPIO 23 (MISO), GPIO 13 (MOSI), GPIO 14 (SCK), GPIO 22 (CS)
- **Display Control:** GPIO 18 (DC), GPIO 19 (RST) - or use other available pins
- **Touch Controller:** GPIO 25, 26, 27 (for touch interrupt/CS)
- **Total Pins Needed:** ~7-9 pins (display + touch controller)

**I2C-Based Touch Screen (Alternative):**
- **Display Interface:** GPIO 21 (SDA), GPIO 22 (SCL)
- **Touch Controller:** Same I2C bus or separate GPIO for interrupt
- **Total Pins Needed:** ~3-4 pins (simpler but less common)

#### Power Requirements

**Touch Screen Power Options:**

1. **3.3V Rail - Connection 2 (Shared with Sensors)**
   - **Current Available:** ~10-20mA used by sensors, ~500mA remaining capacity
   - **Touch Screen Power:** 4.0" ST7796S needs ~90mA
   - **Compatibility:** ✅ **Yes** - Can share connection with sensors (total ~100-110mA)
   - **Recommendation:** Use 3.3V touch screen, share connection with existing sensors
   - **Power Headroom:** ~400mA remaining (plenty of margin)

2. **5V Rail - Connection 2 (Shared with IR Sensor)**
   - **Current Available:** ~10-20mA used by IR sensor, ~500mA remaining capacity
   - **Touch Screen Power:** 4.0" ST7796S needs ~90mA (if using 5V version)
   - **Compatibility:** ✅ **Yes** - Can share connection with IR sensor (total ~100-110mA)
   - **Recommendation:** Use 5V touch screen, share connection with IR sensor
   - **Power Headroom:** ~400mA remaining (plenty of margin)

3. **9V Rail - Connection 2 (Dedicated)**
   - **Current Available:** Up to ~1A (9W) when printer is idle
   - **Compatibility:** ⚠️ **Possible** - Only if touch screen accepts 9V (uncommon, would need regulator)

4. **12V Rail - Connection 2 (Dedicated)**
   - **Current Available:** Up to ~1.5A (18W)
   - **Compatibility:** ⚠️ **Possible** - Only if touch screen accepts 12V (uncommon, would need regulator)

#### Recommended Touch Screen Options

**Recommended Models:**

1. **3.5" SPI TFT LCD (ILI9486)**
   - **Screen Size:** 3.5 inch
   - **Resolution:** 480×320 pixels
   - **Display Colors:** RGB 65K color
   - **Driver IC:** ILI9486
   - **Interface:** 4-wire SPI
   - **Power:** 5V (VCC), 3.3V logic (TTL)
   - **Power Consumption:** ~80-150mA (typical ~100mA)
   - **GPIO Pins Needed:** 6 pins (SPI + control)
   - **Compatibility:** ✅ **Excellent** - Perfect fit for this system
   - **AliExpress Item:** [3256808804486054](https://www.aliexpress.us/item/3256808804486054.html)
   - **Power Connection:** 5V rail connection 2 (shared with IR sensor)
   - **Note:** Touch version available if needed (requires additional pins)

2. **4.0" SPI TFT Touch Screen (ST7796S/ILI9486) - MSP4021**
   - **Screen Size:** 4.0 inch
   - **Resolution:** 480×320 pixels (HD)
   - **Display Colors:** RGB 65K color
   - **Driver IC:** ST7796S/ILI9486
   - **Interface:** 4-wire SPI
   - **Power:** 3.3V~5V (VCC), 3.3V logic (TTL)
   - **Power Consumption:** ~90mA
   - **GPIO Pins Needed:** ~7-9 pins (SPI + control + touch)
   - **Compatibility:** ✅ **Excellent** - Perfect fit for this system
   - **Features:**
     - Optional touch function (resistive/capacitive)
     - SD card slot for expansion
     - Military grade process standard
     - Operating temperature: -20℃~70℃
   - **Power Connection:** Can share 3.3V or 5V rail connection 2
   - **Note:** Model MSP4020 available without touch screen

**Alternative Options:**

3. **2.4" TFT Touch Screen (ILI9341 + XPT2046)**
   - **Interface:** SPI
   - **Power:** 3.3V or 5V, ~80-150mA
   - **GPIO Pins:** 7-9 pins
   - **Compatibility:** ✅ **Excellent** - Smaller size option

4. **3.2" TFT Touch Screen (ILI9341 + XPT2046)**
   - **Interface:** SPI
   - **Power:** 3.3V or 5V, ~100-200mA
   - **GPIO Pins:** 7-9 pins
   - **Compatibility:** ✅ **Good** - Medium size option

#### Power Supply Considerations

- **With Current 20W Supply:**
  - Adding 4.0" touch screen: +0.3W (3.3V @ 90mA) or +0.45W (5V @ 90mA)
  - **Total:** ~18.6-18.75W output (~23.25-23.4W input)
  - ⚠️ **May exceed 20W supply capacity** - 30W recommended

- **With 30W Supply:**
  - ✅ **Plenty of headroom** for 4.0" touch screen addition
  - Recommended before adding touch screen
  - Additional ~6-7W available for other components

#### Pin Assignment Example (SPI Touch Screen)

```
Display SPI:
- GPIO 23: MISO
- GPIO 13: MOSI
- GPIO 14: SCK
- GPIO 22: CS (Display)

Display Control:
- GPIO 18: DC (Data/Command)
- GPIO 19: RST (Reset)

Touch Controller:
- GPIO 25: Touch CS
- GPIO 0: Touch IRQ (optional)
- GPIO 27: Touch MOSI/MISO (if separate SPI)
```

#### Summary

✅ **4.0" Touch Screen (ST7796S/ILI9486) is Fully Compatible:**
- ✅ Sufficient GPIO pins available (7-9 pins for 4-wire SPI interface)
- ✅ Power consumption (~90mA) fits perfectly on 3.3V or 5V rail (can share connections)
- ✅ 4-wire SPI interface matches available GPIO pins (GPIO 23/13/14/22)
- ✅ 3.3V~5V power range compatible with existing power rails
- ✅ 480×320 resolution provides excellent display quality
- ⚠️ **Recommendation:** Upgrade to 30W power supply for optimal reliability
- ✅ **Best Option:** 4.0" ST7796S touch screen (MSP4021) on 3.3V or 5V rail connection 2

---

## System Architecture

The system follows a modular, service-oriented architecture:

```
┌─────────────────────────────────────────────────────────┐
│                    Application Layer                     │
│              (main.cpp - orchestration)                  │
└─────────────────────────────────────────────────────────┘
                            │
┌─────────────────────────────────────────────────────────┐
│                    Service Layer                         │
│  ┌──────────────┐  ┌──────────────┐  ┌──────────────┐  │
│  │  Firebase    │  │   Reminder   │  │    Printer   │  │
│  │   Service    │  │   Service    │  │   Service    │  │
│  └──────────────┘  └──────────────┘  └──────────────┘  │
│  ┌──────────────┐  ┌──────────────┐  ┌──────────────┐  │
│  │  OTA Update  │  │ API Security │  │    Health    │  │
│  │   Service    │  │              │  │   Monitor    │  │
│  └──────────────┘  └──────────────┘  └──────────────┘  │
└─────────────────────────────────────────────────────────┘
                            │
┌─────────────────────────────────────────────────────────┐
│              Hardware Abstraction Layer                  │
│         (HardwareAbstraction class)                      │
└─────────────────────────────────────────────────────────┘
                            │
┌─────────────────────────────────────────────────────────┐
│                   Hardware Layer                         │
│  (ESP32, Sensors, Printer, Pump, etc.)                  │
└─────────────────────────────────────────────────────────┘
```

### Request Queue System
The system implements an asynchronous request queue to eliminate HTTP blocking:
- **Instant Web Responses** - No waiting for Firebase operations
- **Non-Blocking Operation** - Web server remains responsive during background operations
- **Rate Limit Protection** - Controlled Firebase access with 2-second intervals
- **Automatic Retry** - Graceful failure handling with automatic retry mechanism

---

## Troubleshooting

### ESP32 Not Found / Cannot Upload / WiFi AP "Print-n-Prick" Not Visible

**If the ESP32 is not found (no COM port) when uploading:**
1. **USB cable** – Use a data-capable USB cable (some charge-only cables have no data lines).
2. **Drivers** – Install the USB‑serial driver for your board:
   - **CP210x** (common on many ESP32‑WROOM): [Silicon Labs CP210x](https://www.silabs.com/developers/usb-to-uart-bridge-vcp-drivers)
   - **CH340/CH341**: [CH340 driver (e.g. wch.cn)](https://www.wch.cn/downloads/CH341SER_EXE.html) or search “CH340 driver” for your OS.
3. **Boot mode** – Hold the **BOOT** button, press **RESET** (or plug in USB), then release BOOT. Run `pio run -e esp32-wroom-32 --target upload` while or right after doing this.
4. **Port** – Run `pio device list` to see serial ports. If the ESP32 appears (e.g. `COM3` or `/dev/ttyUSB0`), set `upload_port = COM3` (or the correct port) in `platformio.ini` under `[env:esp32-wroom-32]` and try again.
5. **Slower upload** – If upload fails with timeouts, try `upload_speed = 115200` in `platformio.ini`.

**If the WiFi AP "Print-n-Prick" does not appear** (you need it to run WiFi Manager and set your WiFi):
1. **WiFi runs first** – Firmware now starts WiFi Manager before hardware (printer, sensors). The AP should appear even if peripherals fail or are unplugged.
2. **Force config portal** – In `include/config.h`, set `#define FORCE_WIFI_CONFIG_PORTAL 1`, rebuild and upload. On every boot the ESP32 will clear saved WiFi and open the config AP. Connect to **Print-n-Prick** (password: `08202022`), go to `192.168.4.1`, enter your WiFi, then set `FORCE_WIFI_CONFIG_PORTAL` back to `0` and reflash.
3. **AP credentials** – AP SSID: `Print-n-Prick`, password: `08202022`. Config page: `http://192.168.4.1`.

### iOS Shortcut Issues

**"Invalid JSON" Error:**
- Make sure Request Body is set to **"File"** (not JSON or Text)
- Verify the Text variable contains proper JSON format
- Check that Content-Type header is exactly `application/json`

**Message Not Printing:**
- Verify Main ESP32 and TZT Display are online and on same WiFi
- Check Firebase connection on TZT (TZT polls Firebase; forwards to Main via ESP-NOW)
- Make sure the URL is correct: `https://printerpot-d96f8-default-rtdb.firebaseio.com/commands.json`

### Python Script Issues

**Connection Failed:**
- Check your internet connection
- Verify Firebase URL is correct
- Make sure ESP32 is online

**Message Not Printing:**
- TZT polls Firebase (e.g. every 30 seconds) and forwards to Main via ESP-NOW — wait for next poll cycle
- Check Main and TZT serial monitors for errors
- Verify printer is connected and powered on

### Web Interface Not Loading
1. **Check ESP32 IP address** - Look in serial monitor for HTTP server message
2. **Verify WiFi connection** - ESP32 must be connected to WiFi
3. **Check port** - Should be port 8080
4. **Same network** - Device must be on same WiFi network as ESP32
5. **Try IP directly** - Use `http://192.168.4.1:8080` format
6. **Check firewall** - Make sure firewall isn't blocking

### Commands Not Working
1. Check Firebase connection on TZT serial monitor (TZT polls Firebase; Main does not)
2. Verify Firebase URL in TZT config
3. Check internet connectivity on TZT
4. Monitor Firebase usage limits

### Web UI buttons (pump, printer, LED) don't control hardware

The web interface runs on the **TZT Display**; hardware is controlled by the **Main ESP32**. They talk over **ESP-NOW** (not HTTP). So if Test Pump / Test Printer / LED controls do nothing:

1. **Same WiFi** – Main and TZT must be on the **same SSID** and ideally the same channel. Check serial on both: `WiFi.SSID()` and `WiFi.channel()` should match.
2. **MAC addresses** – Main must have TZT’s MAC in `config.h` (`TZT_DISPLAY_MAC_ADDRESS`). TZT must have Main’s MAC in `config_tzt.h` (`MAIN_ESP32_MAC_ADDRESS`). Get each device’s MAC from its serial output at boot (e.g. `MAC: 6C:C8:40:55:85:98` → `{0x6C, 0xC8, 0x40, 0x55, 0x85, 0x98}`).
3. **ESP-NOW init** – On Main serial look for “ESP-NOW init OK” or “ESP-NOW init failed”. On TZT look for “ESP-NOW initialized successfully”. If either fails, fix MACs and WiFi first.
4. **Status / sensors empty** – If `/api/status` or the test page shows no sensor data, Main→TZT ESP-NOW (sensor data) isn’t getting through; fix same WiFi/channel and MACs as above.

### Firebase 401 Errors
1. Go to Firebase Console → Realtime Database → Rules
2. Update rules to allow read/write:
```json
{
  "rules": {
    ".read": true,
    ".write": true
  }
}
```
3. Click Publish

---

## Usage Tips

- **Remote Access** - System operates from any location with internet connectivity, as long as ESP32 maintains network connection
- **Emoji Support** - Emojis are converted to text equivalents (e.g., ❤️ becomes <3)
- **Message Length** - No hard character limit, but consider thermal paper width for optimal formatting
- **Delivery Timing** - Messages print within 30 seconds of sending (ESP32 polling interval)
- **Data Privacy** - Messages are automatically deleted from Firebase after processing

## Hardware Testing

The system includes a comprehensive hardware test suite that allows you to test individual components independently.

### Accessing Test Mode

1. **Open Serial Monitor** - Connect to ESP32 via USB and open serial monitor (115200 baud)
2. **Enter Test Mode** - Type `test` or `t` in the serial monitor
3. **Follow Menu** - Use the interactive menu to select which hardware component to test

### Available Tests

#### Test Menu Options:
- **1 - Test Pump** - Tests sanitizer pump (runs for 1 second)
- **2 - Test LED** - Tests 12V LED brightness control (full brightness for 2 seconds)
- **3 - Test LED Ramp** - Tests LED brightness ramp-up/down functionality (1V per second)
- **4 - Test IR Sensor** - Monitors IR motion sensor for 5 seconds (wave hand to test)
- **5 - Test Moisture Sensor** - Reads moisture sensor for 5 seconds (touch sensor to see changes)
- **6 - Test Light Sensor** - Reads light sensor for 5 seconds (cover/uncover to see changes)
- **7 - Test Printer** - Sends test print to thermal printer
- **8 - Test All Sensors** - Reads all sensors simultaneously for 10 seconds
- **9 - Run All Tests** - Executes all hardware tests in sequence
- **0 - Show Menu** - Displays test menu again

### Serial Commands

**Main Commands:**
- `test` or `t` - Enter hardware test mode
- `help` or `h` - Show available commands
- `status` - Show current hardware status (JSON format)
- `diag` - Print detailed hardware diagnostics

**In Test Mode:**
- Enter number `1-9` to run specific test
- Enter `0` to show menu
- Type `exit` or `e` to exit test mode

### Example Test Session

```
>>> Type 'test' to enter hardware test mode
test

>>> Entering Hardware Test Mode
>>> Type 'exit' to return to normal operation

╔════════════════════════════════════════╗
║     HARDWARE TEST MENU                 ║
╠════════════════════════════════════════╣
║  1 - Test Pump                         ║
║  2 - Test LED                          ║
║  3 - Test LED Ramp                     ║
║  4 - Test IR Sensor                    ║
║  5 - Test Moisture Sensor              ║
║  6 - Test Light Sensor                 ║
║  7 - Test Printer                      ║
║  8 - Test All Sensors                  ║
║  9 - Run All Tests                     ║
║  0 - Show Menu                         ║
╚════════════════════════════════════════╝

Enter test number (1-9, 0 for menu): 1

========================================
TEST: PUMP TEST
========================================
Testing pump on GPIO 26
Duration: 1000ms

Starting pump in 2 seconds...
>>> Starting pump...
✅ GPIO 26 confirmed HIGH (MOSFET should be switching)
✅ Pump started - GPIO 26 = HIGH

Pump running for 1000ms...
>>> Stopping pump...
✅ GPIO 26 confirmed LOW (MOSFET should be off)
✅ Pump stopped (duration: 1000ms, total dispenses: 1)

[Pump] ✅ PASS - Complete test passed
```

### Test Features

- **Individual Component Testing** - Test each hardware component independently
- **Real-time Feedback** - See sensor readings and pin states in real-time
- **Safety Features** - Automatic timeouts and cooldown periods
- **Detailed Diagnostics** - Pin state verification and sensor value reporting
- **Interactive Menu** - Easy-to-use menu system for selecting tests

### Troubleshooting with Tests

- **Pump not working?** - Run test #1 to verify GPIO 26 and MOSFET operation
- **LED not turning on?** - Run test #2 to check LED PWM and MOSFET
- **Sensors not reading?** - Run tests #4, #5, #6 to verify sensor connections
- **Printer not responding?** - Run test #7 to check Serial2 communication

## Support and Debugging

- **Serial Monitor** - Monitor ESP32 status and debug messages via serial output
- **Firebase Console** - Monitor database usage and data through Firebase web console
- **Web Interface** - Access full system management at `http://192.168.4.1:8080`
- **System Logs** - Serial monitor provides detailed operation logs for troubleshooting
- **Test Script** - Use `send_message.py` for quick message delivery testing
- **Hardware Tests** - Use built-in test suite to verify individual hardware components
