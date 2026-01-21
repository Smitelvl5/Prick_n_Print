# Print_n_Prick - IoT Message Dispenser System

An integrated IoT system combining a thermal printer and hand sanitizer dispenser with remote messaging capabilities. The system enables remote message delivery through Firebase, printing messages on thermal receipts along with environmental data including weather conditions, moisture sensor readings, and sanitizer level information.

## Web Server Interface

The ESP32 includes an integrated HTTP server providing a web-based management interface accessible from any device on the local network.

### Accessing the Web Interface

**Option 1: Connected to Your WiFi Network (Recommended)**
1. **ESP32 connects to your WiFi** - On first boot, ESP32 creates access point `Print_n_Prick` (password: `08202022`)
2. **Configure WiFi** - Connect to `Print_n_Prick` AP, open browser to `192.168.4.1`, and configure your WiFi credentials
3. **Obtain IP Address** - Check serial monitor output for: `HTTP Server started on http://[YOUR-IP]:8080`
   - Example: `HTTP Server started on http://192.168.1.248:8080`
4. **Open Browser** - Navigate to the IP address shown in serial monitor (e.g., `http://192.168.1.248:8080`)
5. **Authentication** - Enter password: `0820`

**Option 2: Direct Access Point Mode**
1. **Connect to ESP32 AP** - Connect to WiFi network `Print_n_Prick` (password: `08202022`)
2. **Open Browser** - Navigate to `http://192.168.4.1:8080`
3. **Authentication** - Enter password: `0820`

**Note:** The IP address is **dynamic** when connected to your WiFi. Always check the serial monitor for the current IP address. The web server runs on **port 8080**.

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
- **Print List** - Print formatted grocery list to 🟢 **thermal printer**
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

#### Step-by-Step Setup:

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
When a message is sent via iOS Shortcut or Python script, the 🟢 **thermal printer** outputs:

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
- **SSID:** `Print_n_Prick`
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
  1. Connect to ESP32 AP (`Print_n_Prick`)
  2. Open browser to `192.168.4.1` (captive portal may open automatically)
  3. Enter your WiFi network SSID and password
  4. ESP32 saves credentials and connects automatically on future boots
- **Note:** Your WiFi credentials are stored on the ESP32 and not hardcoded in the firmware

### Changing Credentials

**To change ESP32 Access Point password:**
Edit `include/config.h`:
```cpp
#define AP_SSID "Print_n_Prick"        // Change AP name here
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

## Quick Start Guide

### 1. Upload Firmware
```bash
pio run --target upload
```

### 2. Access Web Interface
- Connect to WiFi network `Print_n_Prick` (password: `08202022`)
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

### Base URL
`http://[ESP32-IP]:8080`

### Authentication
- Web interface password: `0820`
- Optional API key authentication (if enabled)

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

## Firebase Command Interface

Commands can be sent through Firebase Realtime Database at `/commands/{commandId}`.

### Command Structure
```json
{
  "type": "command_type",
  "data": "command_data",
  "processed": false
}
```

### Available Commands

#### Print Message
```json
{
  "type": "print",
  "data": "Your romantic message here",
  "processed": false
}
```

#### Start Sanitizer Dispense
```json
{
  "type": "dispense_start",
  "data": "",
  "processed": false
}
```

#### Stop Sanitizer Dispense
```json
{
  "type": "dispense_stop",
  "data": "",
  "processed": false
}
```

#### Get Weather
```json
{
  "type": "weather",
  "data": "",
  "processed": false
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
├── config.json (checksPerDay)
├── commands.json (print commands)
├── status.json (ESP32 status updates)
├── reminders.json (scheduled messages)
└── groceries.json (grocery list)
```

---

## System Operation

### Message Delivery Workflow
1. **Message Submission** - Message sent via iOS Shortcut, Python script, or Firebase `print` command
2. **Firebase Polling** - 🔵 **ESP32** polls Firebase every 30 seconds for new commands
3. **Message Processing** - 🔵 **ESP32** detects new message and queues for printing
4. **Receipt Generation** - 🟢 **Thermal printer** outputs receipt containing:
   - Custom message text
   - Current weather conditions (Fahrenheit, 12-hour format)
   - Moisture sensor percentage
   - Sanitizer level percentage
   - Date and time stamp (12-hour format with AM/PM)

### Reminder System
1. **Reminder Creation** - User accesses web interface at `http://192.168.4.1`
2. **Reminder Configuration** - User specifies message content and scheduled time
3. **Data Persistence** - Reminder stored in Firebase, persists across system reboots
4. **Reminder Monitoring** - 🔵 **ESP32** checks scheduled reminders every minute
5. **Automatic Execution** - When scheduled time is reached, reminder prints automatically
6. **Receipt Content**:
   - Reminder creation timestamp
   - Reminder message text (standard size, centered)
   - No additional environmental data

### Grocery List Management
1. **List Access** - User navigates to Groceries tab in web interface
2. **Item Management** - User adds items to grocery list
3. **Data Persistence** - List stored in Firebase, persists across system reboots
4. **Print Functionality** - Print button generates formatted list on thermal printer
5. **List Clearing** - Clear button removes all items from list

### Hand Sanitizer Dispensing System
- **🟣 IR Sensor** Detection - 🟣 **Infrared sensor** detects hand placement under dispenser
- **Automatic Dispensing** - System can automatically dispense sanitizer upon motion detection
- **Cooldown Protection** - 3-second cooldown period between dispenses prevents continuous operation
- **Safety Limit** - Maximum dispense duration of 2 seconds for safety
- **Level Monitoring** - System tracks and reports sanitizer level percentage

---

## System Requirements

- **🔵 ESP32** microcontroller with WiFi connectivity
- **🟢 Thermal printer** (optional component)
- **🟠 Hand sanitizer pump** and 🟣 **infrared sensor**
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
- **🔵 ESP32 ESP-WROOM-32 Module** (5V) - plugs into expansion board

### Printing System
- **🟢 Embedded Thermal Printer QR204** Receipt Ticket Printers (9V)

### Sensors
- **🟣 Infrared (IR) Motion Sensor Module** (5V) - hand detection
- **🟣 Moisture Sensor** (Analog) - connected to GPIO 34
- **🟣 LM393 Light Sensor Module** (Analog) - connected to GPIO 35

### Dispensing System
- **🟠 CJWP08 Micro M20 Diaphragm Water Pump** (3.3V) - hand sanitizer dispensing
- **⚫ IRF520 MOSFET Driver Module** - pump control (handles higher current loads)

### Lighting System
- **🟡 12V 5W LED** - ambient lighting
- **⚫ MOSFET Driver Module** - LED PWM control

### Display System
- **🖥️ 3.5" 480×320 TFT LCD Module Screen Display** (ILI9486 Controller) - SPI interface, 5V power

### Power Management
- **🔷 LM2596 Multi Channel Switching Power Supply Module** (3.3V/5V/9V/12V/ADJ Adjustable)
- **20W USB-C Wall Power Supply** (input power source)
- **Type-C USB Jack 3.1 Type-C 2Pin Female** (power input connector)

### Pin Connections

**⚠️ IMPORTANT: Pin Changes for External Power Compatibility**
- **GPIO 2 → GPIO 21**: Built-in LED moved to avoid strapping pin (GPIO 2 must be LOW during boot)
- **GPIO 5 → GPIO 27**: LED PWM moved to avoid strapping pin (GPIO 5 must be HIGH during boot)
- **GPIO 12 → GPIO 23**: MISO moved to avoid strapping pin (GPIO 12 must be LOW during boot)
- **GPIO 15 → GPIO 22**: LCD CS moved to avoid strapping pin (GPIO 15 must be HIGH during boot)

All strapping pins (GPIO 0, 2, 5, 12, 15) are now free for proper boot mode selection.

#### System Components
- **GPIO 21**: Built-in 🟡 **LED** (status indicator) ⭐ *Moved from GPIO 2 to avoid strapping pin*
- **GPIO 4**: ⚫ **IRF520 MOSFET Driver Module** Gate (pump control signal)
- **GPIO 27**: 🟡 **LED** PWM control (12V 🟡 **LED** via ⚫ **MOSFET**, PWM output) ⭐ *Moved from GPIO 5 to avoid strapping pin*
- **GPIO 16**: 🟢 **Thermal printer** RX (RX2 - ESP32 receives data from printer)
- **GPIO 17**: 🟢 **Thermal printer** TX (TX2 - ESP32 sends data to printer)
- **GPIO 32**: 🟣 **IR sensor** (digital input with pull-up, 5V)
- **GPIO 34**: 🟣 **Moisture sensor** (analog input, input-only pin)
- **GPIO 35**: 🟣 **LM393 Light Sensor Module** (analog input, input-only pin)

#### Display System (TFT LCD + SD Card + Touch Screen)

**LCD Display (SPI Interface):**
- **GPIO 23**: 🖥️ **LCD_D0** / **SD_DO** (SPI MISO - shared with SD card and touch) ⭐ *Changed from GPIO 12 to avoid boot issues*
- **GPIO 13**: 🖥️ **LCD_D1** / **SD_DI** (SPI MOSI - shared with SD card and touch)
- **GPIO 14**: 🖥️ **SD_SCK** (SPI Clock - shared with SD card and touch)
- **GPIO 22**: 🖥️ **LCD_CS** (LCD Chip Select) ⭐ *Moved from GPIO 15 to avoid strapping pin*
- **GPIO 18**: 🖥️ **LCD_RS** (Data/Command Select)
- **GPIO 19**: 🖥️ **LCD_RST** (Reset)

**SD Card (SPI Interface - Shares Bus with Display):**
- **GPIO 23**: 💾 **SD_DO** (MISO - shared with LCD_D0) ⭐ *Changed from GPIO 12*
- **GPIO 13**: 💾 **SD_DI** (MOSI - shared with LCD_D1)
- **GPIO 14**: 💾 **SD_SCK** (Clock - shared with display)
- **GPIO 23**: 💾 **SD_SS** (SD Card Chip Select) ⚠️ *Note: SD_SS uses different pin, see below*

**Touch Screen (SPI Interface - Shares Bus with Display):**
- **GPIO 23**: 👆 **Touch MISO** (shared with LCD_D0 and SD_DO) ⭐ *Changed from GPIO 12*
- **GPIO 13**: 👆 **Touch MOSI** (shared with LCD_D1 and SD_DI)
- **GPIO 14**: 👆 **Touch SCK** (shared with display and SD card)
- **GPIO 25**: 👆 **Touch CS** (Touch Controller Chip Select)
- **GPIO 26**: 👆 **Touch IRQ** (Touch Interrupt - optional but recommended)

**Notes:** 
- GPIO 16 = RX2 (ESP32 RX pin, connects to Printer TX), GPIO 17 = TX2 (ESP32 TX pin, connects to Printer RX). Serial2 is used with inverted logic enabled.
- Display, SD card, and touch screen all share the same SPI bus (GPIO 23/13/14). Use different chip select pins (LCD_CS, SD_SS, Touch CS) to communicate with each device separately.
- **MISO changed from GPIO 12 to GPIO 23** to avoid boot failure (GPIO 12 must be LOW during boot on ESP32 DevKit V1).
- **LED moved from GPIO 2 to GPIO 21** to avoid strapping pin (GPIO 2 must be LOW during boot).
- **LED PWM moved from GPIO 5 to GPIO 27** to avoid strapping pin (GPIO 5 must be HIGH during boot).
- **LCD CS moved from GPIO 15 to GPIO 22** to avoid strapping pin (GPIO 15 must be HIGH during boot).
- **SD_SS pin**: Check your SD card wiring - may need separate CS pin if not sharing MISO.
- **Touch screen requires GPIO 25 (CS) and GPIO 26 (IRQ)** for proper operation.

### GPIO & Voltage Summary Table

| GPIO Pin | Function | Type | Signal Voltage | Device Voltage | Component |
|----------|----------|------|----------------|---------------|------------|
| **GPIO 21** | LED Control | Digital Output | 3.3V | 3.3V (internal) | 🟡 Built-in LED ⭐ *Moved from GPIO 2* |
| **GPIO 4** | Pump Control | Digital Output | 3.3V | 3.3V (MOSFET VIN) | ⚫ MOSFET Gate (Pump) |
| **GPIO 27** | LED PWM | PWM Output | 3.3V | 12V (LED load) | 🟡 12V LED via MOSFET ⭐ *Moved from GPIO 5* |
| **GPIO 23** | SPI MISO | SPI Input | 3.3V | 5V (Display VCC) | 🖥️ TFT LCD |
| **GPIO 13** | SPI MOSI | SPI Output | 3.3V | 5V (Display VCC) | 🖥️ TFT LCD |
| **GPIO 14** | SPI SCK | SPI Clock | 3.3V | 5V (Display VCC) | 🖥️ TFT LCD |
| **GPIO 22** | SPI CS | SPI Chip Select | 3.3V | 5V (Display VCC) | 🖥️ TFT LCD ⭐ *Moved from GPIO 15* |
| **GPIO 16** | Serial RX2 | Serial Input | 3.3V | 9V (Printer VCC) | 🟢 Thermal Printer |
| **GPIO 17** | Serial TX2 | Serial Output | 3.3V | 9V (Printer VCC) | 🟢 Thermal Printer |
| **GPIO 18** | Display DC | Digital Output | 3.3V | 5V (Display VCC) | 🖥️ TFT LCD |
| **GPIO 19** | Display RST | Digital Output | 3.3V | 5V (Display VCC) | 🖥️ TFT LCD |
| **GPIO 32** | IR Sensor | Digital Input | 3.3V (logic) | 5V (Sensor VCC) | 🟣 IR Sensor |
| **GPIO 34** | Moisture Sensor | Analog Input | 3.3V (ADC) | 3.3V | 🟣 Moisture Sensor |
| **GPIO 35** | Light Sensor | Analog Input | 3.3V (ADC) | 3.3V | 🟣 LM393 Light Sensor |

### Power Rail Summary

| Power Rail | Voltage | Connection 1 | Connection 2 | Total Current |
|------------|---------|--------------|--------------|---------------|
| **3.3V** | 3.3V | 🟠 Pump (~300-500mA) | 🟣 Moisture + Light Sensors (~10-20mA) | ~310-520mA |
| **5V** | 5V | 🔵 ESP32 (~200-500mA) | 🟣 IR Sensor (~10-20mA) + 🖥️ Display (~100mA) | ~310-620mA |
| **9V** | 9V | 🟢 Thermal Printer (~50-100mA idle, ~500-1000mA printing) | *Available* | ~50-1000mA |
| **12V** | 12V | 🟡 12V LED (~417mA) | *Available* | ~417mA |

**Key Points:**
- All ESP32 GPIO signals are **3.3V logic** (regardless of device operating voltage)
- Devices operate at their specified voltages (3.3V, 5V, 9V, 12V) but communicate via 3.3V logic
- **Signal Voltage** = ESP32 GPIO output/input voltage (always 3.3V)
- **Device Voltage** = Component's power supply voltage

### ESP32 Pinout Diagram

```
                    ┌─────────────────────────────┐
                    │      ESP32 Development       │
                    │         Board (Top)          │
                    └─────────────────────────────┘
                              │
                              │
    ┌─────────────────────────┴─────────────────────────┐
    │                                                     │
    │  [3.3V] ────────────────────────────────────────┐ │
    │  [EN]                                            │ │
    │  [GND] ────────────────────────────────────────┐ │ │
    │  [GPIO 21] ─────> 🟡 Built-in LED ⭐ Moved from GPIO 2 │ │ │
    │  [GPIO 4] ──────> ⚫ MOSFET Gate (Pump)       │ │ │
    │  [GPIO 27] ─────> ⚫ MOSFET Gate (LED PWM) ⭐ Moved from GPIO 5 │ │ │
    │  [GND]                                           │ │ │
    │  [GPIO 16] ─────> 🟢 Thermal Printer RX        │ │ │
    │  [GPIO 17] ─────> 🟢 Thermal Printer TX       │ │ │
    │  [5V] ────────────────────────────────────────┐ │ │ │
    │  [GND] ────────────────────────────────────────┐ │ │ │ │
    │  [GPIO 32] ────> 🟣 IR Sensor (Digital)       │ │ │ │ │
    │  [GPIO 34] ────> 🟣 Moisture Sensor (Analog) │ │ │ │ │
    │  [GPIO 35] ────> 🟣 Light Sensor (Analog)    │ │ │ │ │
    │  [GND]                                           │ │ │ │ │
    │  [3.3V] ───────────────────────────────────────┐ │ │ │ │ │
    │                                                 │ │ │ │ │ │
    └─────────────────────────────────────────────────┘ │ │ │ │ │ │
                                                       │ │ │ │ │ │
    Power Connections:                                 │ │ │ │ │ │
    ┌─────────────────────────────────────────────────┘ │ │ │ │ │
    │ 3.3V Rail (LM2596):                              │ │ │ │
    │   └─> 🟠 Pump (CJWP08)                           │ │ │
    │   └─> 🟣 Moisture Sensor                         │ │
    │   └─> 🟣 Light Sensor (LM393)                    │
    │                                                   │
    │ 5V Rail (LM2596):                                │
    │   └─> 🔵 ESP32 (via expansion board)             │
    │   └─> 🟣 IR Sensor Module                        │
    │                                                   │
    │ 9V Rail (LM2596):                                │
    │   └─> 🟢 Thermal Printer (QR204)                 │
    │                                                   │
    │ 12V Rail (LM2596):                               │
    │   └─> 🟡 12V LED (via MOSFET on GPIO 27) ⭐ Moved from GPIO 5 │
    │                                                   │
    │ GND: All components share common ground          │
    └───────────────────────────────────────────────────┘

    Component Connections:
    
    🟢 Thermal Printer (QR204):
       RX ──────< GPIO 16 (ESP32 TX)
       TX ──────> GPIO 17 (ESP32 RX)
       VCC ─────> 9V Rail
       GND ─────> GND
    
    🟠 Pump (CJWP08):
       + ───────> MOSFET Module V+
       - ───────> MOSFET Module V-
       (Pump powered through MOSFET module)
    
    ⚫ MOSFET Module #1 (Pump):
       VIN ─────> 3.3V Rail (module power)
       GND ─────> Power Supply GND
       Signal ──> GPIO 4 (ESP32 control)
       GND ─────> ESP32 GND (also common)
       V+ ──────> Pump positive (+)
       V- ──────> Pump negative (-)
    
    🟡 12V LED:
       + ───────> 12V Rail (same rail as MOSFET V+)
       - ───────> MOSFET Module V-
       (LED powered through MOSFET module V+ and V-)
    
    ⚫ MOSFET Module #2 (LED):
       VIN ─────> 5V Rail (module power - or 3.3V)
       GND ─────> Power Supply GND
       Signal ──> GPIO 27 (ESP32 PWM control) ⭐ Moved from GPIO 5
       GND ─────> ESP32 GND (also common)
       V+ ──────> 12V Rail ⚠️ (MUST connect to 12V - power source for LED)
       V- ──────> LED negative (-)
    
    🟣 IR Sensor Module:
       VCC ─────> 5V Rail
       GND ─────> GND
       OUT ─────> GPIO 32
    
    🟣 Moisture Sensor:
       VCC ─────> 3.3V Rail
       GND ─────> GND
       A0 ──────> GPIO 34 (Analog)
    
    🟣 Light Sensor (LM393):
       VCC ─────> 3.3V Rail
       GND ─────> GND
       A0 ──────> GPIO 35 (Analog)
```

### MOSFET Module Wiring Guide

The system uses **2 IRF520 MOSFET Driver Modules** to control high-current devices (pump and LED).

#### MOSFET Module #1: Pump Control (GPIO 4)

```
    ┌─────────────────────────────────────────┐
    │  ⚫ IRF520 MOSFET Module #1              │
    │                                         │
    │  ┌─────┐  ┌─────┐  ┌─────┐  ┌─────┐  ┌─────┐    │
    │  │ VIN │  │ GND │  │Signal│  │ V+  │  │ V-  │    │
    │  └──┬──┘  └──┬──┘  └──┬──┘  └──┬──┘  └──┬──┘    │
    │     │        │        │        │        │       │
    └─────┼────────┼────────┼────────┼────────┼───────┘
          │        │        │        │        │
          │        │        │        │        │
    3.3V Rail   GND    GPIO 4    🟠 Pump+  🟠 Pump-
    (Module     (Common)  (Control)  (Positive) (Negative
     Power)              Signal)    Terminal)  Terminal)
    
    🟠 Pump (CJWP08) Connections:
       + (Positive) ──────> MOSFET Module V+
       - (Negative) ──────> MOSFET Module V-
       (Pump is powered through MOSFET module V+ and V- terminals)
```

#### MOSFET Module #2: LED PWM Control (GPIO 27) ⭐ Moved from GPIO 5

```
    ┌─────────────────────────────────────────┐
    │  ⚫ IRF520 MOSFET Module #2              │
    │                                         │
    │  ┌─────┐  ┌─────┐  ┌─────┐  ┌─────┐  ┌─────┐    │
    │  │ VIN │  │ GND │  │Signal│  │ V+  │  │ V-  │    │
    │  └──┬──┘  └──┬──┘  └──┬──┘  └──┬──┘  └──┬──┘    │
    │     │        │        │        │        │       │
    └─────┼────────┼────────┼────────┼────────┼───────┘
          │        │        │        │        │
          │        │        │        │        │
    5V Rail    GND    GPIO 27   12V Rail  🟡 LED (-) ⭐ Moved from GPIO 5
    (Module     (Common)  (PWM      (Load     (Negative
     Power)              Control)   Power)    Terminal)
    
    🟡 12V LED Connections:
       + (Positive) ──────> 12V Rail (same as V+)
       - (Negative) ──────> MOSFET Module V-
       (LED powered through MOSFET module - V+ provides 12V power, V- switches the ground)
```

#### Complete MOSFET Wiring Diagram

```
    ┌─────────────────────────────────────────────────────────────┐
    │                    🔵 ESP32 Development Board                │
    │                                                             │
    │  GPIO 4 ────────────────────────────────────────────────┐ │
    │  GPIO 27 ─────────────────────────────────────────────┐ │ │ ⭐ Moved from GPIO 5
    │  GND ────────────────────────────────────────────────┐ │ │ │
    └───────────────────────────────────────────────────────┼─┼─┼─┘
                                                           │ │ │
                                                           │ │ │
    ┌───────────────────────────────────────────────────────┼─┼─┼──────────────────┐
    │                                                       │ │ │                  │
    │  ┌─────────────────────────────────────────────────────┘ │ │                  │
    │  │                                                       │ │                  │
    │  │  ⚫ MOSFET Module #1 (Pump Control)                  │ │                  │
    │  │  ┌─────────────────────────────────────────────┐   │ │                  │
    │  │  │  VIN ──────> 3.3V Rail (Module Power)       │   │ │                  │
    │  │  │  GND ──────> Power Supply GND               │   │ │                  │
    │  │  │  Signal ───< GPIO 4 (Control Signal)        │   │ │                  │
    │  │  │  GND ──────> ESP32 GND (also common)         │   │ │                  │
    │  │  │  V+ ───────> 🟠 Pump (+) Terminal            │   │ │                  │
    │  │  │  V- ───────> 🟠 Pump (-) Terminal           │   │ │                  │
    │  │  └─────────────────────────────────────────────┘   │ │                  │
    │  │                                                       │ │                  │
    │  │  🟠 Pump (CJWP08):                                   │ │                  │
    │  │     + Terminal ──────> MOSFET Module V+           │ │                  │
    │  │     - Terminal ──────> MOSFET Module V-            │ │                  │
    │  │                                                       │ │                  │
    │  └───────────────────────────────────────────────────────┘ │                  │
    │                                                           │                  │
    │  ┌─────────────────────────────────────────────────────────┘                  │
    │  │                                                           │                  │
    │  │  ⚫ MOSFET Module #2 (LED PWM Control)                   │                  │
    │  │  ┌─────────────────────────────────────────────┐       │                  │
    │  │  │  VIN ──────> 5V Rail (Module Power)         │       │                  │
    │  │  │  GND ──────> Power Supply GND               │       │                  │
    │  │  │  Signal ───< GPIO 27 (PWM Control Signal) ⭐ Moved from GPIO 5 │       │                  │
    │  │  │  GND ──────> ESP32 GND (also common)        │       │                  │
    │  │  │  V+ ───────> 12V Rail ⚠️ (Load Power Source)│       │                  │
    │  │  │  V- ───────> 🟡 LED (-) Terminal           │       │                  │
    │  │  └─────────────────────────────────────────────┘       │                  │
    │  │                                                           │                  │
    │  │  🟡 12V LED:                                             │                  │
    │  │     + Terminal ──────> 12V Rail (same as V+)          │                  │
    │  │     - Terminal ──────> MOSFET Module V-                 │                  │
    │  │                                                           │                  │
    │  └───────────────────────────────────────────────────────────┘                  │
    └──────────────────────────────────────────────────────────────────────────────────┘
```

#### Wiring Steps

**MOSFET Module #1 (Pump) - Correct Wiring:**
1. Connect **VIN** pin → **3.3V Rail** (module power supply - supports 3.3V or 5V)
2. Connect **GND** pin → **Power Supply GND** (common ground)
3. Connect **Signal** pin → **ESP32 GPIO 4 (D4)** (control signal - 3.3V)
4. Connect **GND** pin → **ESP32 GND** (also connect to common ground)
5. Connect **V+** pin → **Pump positive (+) terminal**
6. Connect **V-** pin → **Pump negative (-) terminal**

**Note:** 
- The pump is powered through the MOSFET module's V+ and V- terminals, not directly from the power supply
- Module supports 3.3V input voltage, so 3.3V rail is correct
- The Load LED on the module should turn ON when GPIO 4 goes HIGH

**MOSFET Module #2 (LED) - Correct Wiring:**
1. Connect **VIN** pin → **5V Rail** (module power supply - use 5V for better logic compatibility, or 3.3V works too)
2. Connect **GND** pin → **Power Supply GND** (common ground)
3. Connect **Signal** pin → **ESP32 GPIO 27** (PWM control signal - 3.3V) ⭐ Moved from GPIO 5
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
    1. **Signal pin not connected** - Verify GPIO 4 is connected to Signal terminal
    2. **GND not connected** - Verify both module GND pins are connected to common ground
    3. **VIN not connected** - Verify VIN is connected to 3.3V rail (or 5V if preferred)
    4. **Load not connected** - Verify pump V+ and V- are connected to module V+ and V- terminals
    5. **Check Serial Monitor** - Look for "GPIO 4 state after write: HIGH" message
    6. **Test with multimeter** - Measure voltage on Signal pin when pump test is ON (should be ~3.3V)
- **PWM on GPIO 27** - The LED MOSFET receives PWM signals for brightness control (0-255 levels) ⭐ Moved from GPIO 5
- **Digital on GPIO 4** - The pump MOSFET receives simple ON/OFF signals
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
   - Signal → ESP32 GPIO 4 (D4)
   - GND → ESP32 GND (also common)
   - V+ → Pump positive (+) terminal
   - V- → Pump negative (-) terminal
   
   Pump Connections:
   - Pump positive (+) → MOSFET Module V+
   - Pump negative (-) → MOSFET Module V-
   ```

3. **Check Serial Monitor:**
   - Look for: "Setting GPIO 4 to HIGH"
   - Look for: "GPIO 4 state after write: HIGH"
   - If GPIO shows HIGH but Load LED doesn't turn on, check Signal pin connection

3. **Test with Multimeter:**
   - Measure voltage on GPIO 4 when pump test is ON (should be ~3.3V)
   - Measure voltage on MOSFET Gate terminal (should match GPIO 4)
   - Measure voltage on MOSFET Drain when ON (should be close to 0V if MOSFET is conducting)

4. **Check Serial Monitor:**
   - Look for messages: "Setting GPIO 4 to HIGH" and "GPIO 4 state after write: HIGH"
   - If GPIO shows HIGH but Load LED doesn't turn on, it's a logic level issue

---

## Power Calculations

### Power Requirements by Rail

> **Note:** Each rail has 2 connection spots. Low-current sensors can share a connection.

#### 3.3V Rail (2 connections)
- **Connection 1:** 🟠 **Pump (CJWP08)**: ~300-500mA when running
- **Connection 2:** 🟣 **Moisture Sensor** + 🟣 **Light Sensor (LM393)**: ~5-10mA + ~5-10mA = ~10-20mA (shared connection)
- **Total**: ~310-520mA (worst case ~0.52A)
- **Power**: 0.52A × 3.3V = **1.7W**

#### 5V Rail (2 connections)
- **Connection 1:** 🔵 **ESP32**: ~200-500mA (spikes during WiFi transmission)
- **Connection 2:** 🟣 **IR Sensor Module** + 🖥️ **TFT LCD Display**: ~10-20mA + ~100mA = ~110-120mA (shared connection)
- **Total**: ~310-620mA (worst case ~0.62A)
- **Power**: 0.62A × 5V = **3.1W**

#### 9V Rail (Custom) (2 connections)
- **Connection 1:** 🟢 **Thermal Printer (QR204)**: ~50-100mA idle, ~500-1000mA when printing
- **Connection 2:** *Available*
- **Total**: ~50-100mA idle, ~500-1000mA when printing
- **Power**: 1A × 9V = **9W** (peak during printing)

#### 12V Rail (2 connections)
- **Connection 1:** 🟡 **12V 5W LED**: ~417mA (5W ÷ 12V = 0.417A)
- **Connection 2:** *Available*
- **Total**: ~417mA
- **Power**: 0.417A × 12V = **5W**

### Total Power Consumption
- **Combined Output Power:** ~19.3W (peak: printer printing + LED on + pump running + display active)
- **With 80% Efficiency:** ~24.1W input required
- **Recommended USB-C Supply:** 30W minimum (20W insufficient with display)

### Power Supply Specifications
- **Input:** 5V USB-C (20W = 5V @ 4A, or 30W = 5V @ 6A recommended)
- **🔷 LM2596 Module:** Typically rated for 2-3A per channel
- **Available Input Current (20W):** 4A
- **Required Input Current:** ~4.82A (24.1W ÷ 5V)
- **Headroom (20W):** Negative margin - **30W supply required**

### Notes
- **Each rail has 2 connection spots** - components must be distributed accordingly
- Low-current sensors (🟣 Moisture Sensor, 🟣 Light Sensor) share one connection on the 3.3V rail
- The 5V rail connection 2 is shared by 🟣 **IR Sensor Module** and 🖥️ **TFT LCD Display**
- The 9V rail connection 1 is dedicated to the 🟢 **thermal printer** (connection 2 available)
- The 12V rail connection 1 is dedicated to the 🟡 **LED** (connection 2 available)
- Peak power consumption occurs when 🟢 **printer** is printing, 🟡 **LED** is active, 🟠 **pump** is running, and 🖥️ **display** is active simultaneously
- **20W USB-C charger is insufficient with display** - 30W required for reliable operation
- 🔵 **ESP32** operates from 5V rail (expansion board handles voltage regulation internally)

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
| **LCD_CS** | Module chip selection signal (low level enable) | GPIO 22 | LCD Control ⭐ *Moved from GPIO 15* |
| **LCD_RS** | Command/data selection signal (low=command, high=data) | GPIO 18 | LCD Control |
| **LCD_WR** | Module write control signal | GPIO 23 (parallel) or N/A (SPI) | LCD Control (Parallel) |
| **LCD_RD** | Module read control signal | GPIO 25 (parallel) or N/A (SPI) | LCD Control (Parallel) |
| **LCD_D0** | LCD data bit 0 / SPI MISO | GPIO 23 | LCD Data (SPI/Parallel) ⭐ *Changed from GPIO 12* |
| **LCD_D1** | LCD data bit 1 / SPI MOSI | GPIO 13 | LCD Data (SPI/Parallel) |
| **SD_SCK** | SD card clock (SPI clock) | GPIO 14 | SD Card SPI |
| **SD_DO** | SD card data out (MISO) | GPIO 23 (shared with LCD_D0) ⭐ *Changed from GPIO 12* | SD Card SPI |
| **SD_DI** | SD card data in (MOSI) | GPIO 13 (shared with LCD_D1) | SD Card SPI |
| **SD_SS** | SD card chip select | GPIO 23 (or separate pin if needed) | SD Card SPI |
| **F_CS** | Flash memory chip select | GPIO 4 (or available GPIO) | Flash Memory |

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
- **GPIO 23**: MISO (Master In Slave Out) - shared by LCD, SD card, and touch screen ⭐ *Changed from GPIO 12 to avoid boot issues*
- **GPIO 13**: MOSI (Master Out Slave In) - shared by LCD, SD card, and touch screen
- **GPIO 14**: SCK (SPI Clock) - shared by LCD, SD card, and touch screen

**LCD Display:**
- **GPIO 22**: LCD_CS (LCD Chip Select) ⭐ *Moved from GPIO 15*
- **GPIO 18**: LCD_RS (Data/Command - low=command, high=data)
- **GPIO 19**: LCD_RST (Reset - low level reset)

**SD Card:**
- **GPIO 23**: SD_SS (SD Card Chip Select) ⚠️ *Note: May need separate CS pin*

**Touch Screen:**
- **GPIO 25**: Touch CS (Touch Controller Chip Select)
- **GPIO 26**: Touch IRQ (Touch Interrupt - optional but recommended for efficient operation)

**Total Pins Required:** 9 pins (3 shared SPI + 3 LCD control + 1 SD CS + 2 Touch control)
**Advantages:** Fewer pins, simpler wiring, standard interface, SD card support, touch screen support, all devices share SPI bus efficiently

#### Option 2: 8-Bit Parallel Interface (Uses 13+ pins)

**Control Pins:**
- **GPIO 22**: LCD_CS (Chip Select) ⭐ *Moved from GPIO 15*
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
🖥️ 3.5" TFT LCD (ILI9486) - SPI Mode:
   Power:
   5V ──────> 5V Rail (Connection 2, shared with IR sensor)
   GND ─────> GND (common ground)
   
   LCD Control:
   LCD_CS ──> GPIO 22 (Chip Select) ⭐ Moved from GPIO 15
   LCD_RS ──> GPIO 18 (Data/Command)
   LCD_RST ─> GPIO 19 (Reset)
   
   LCD SPI (shared with SD card):
   LCD_D0 ──> GPIO 23 (MISO - ESP32 receives) ⭐ Changed from GPIO 12
   LCD_D1 ──> GPIO 13 (MOSI - ESP32 sends)
   SD_SCK ──> GPIO 14 (Clock - shared with SD card)
   
   SD Card SPI:
   SD_DO ───> GPIO 23 (MISO - shared with LCD_D0) ⭐ Changed from GPIO 12
   SD_DI ───> GPIO 13 (MOSI - shared with LCD_D1)
   SD_SCK ──> GPIO 14 (Clock - shared with LCD)
   SD_SS ───> GPIO 23 (SD Card Chip Select) ⚠️ Note: May need separate CS pin
   
   Touch Screen SPI:
   Touch MISO ─> GPIO 23 (shared with LCD_D0 and SD_DO) ⭐ Changed from GPIO 12
   Touch MOSI ─> GPIO 13 (shared with LCD_D1 and SD_DI)
   Touch SCK ──> GPIO 14 (shared with LCD and SD card)
   Touch CS ───> GPIO 25 (Touch Controller Chip Select)
   Touch IRQ ──> GPIO 26 (Touch Interrupt - optional but recommended)
   
   LED ──────> 5V (backlight, optional - can share VCC)
```

**Notes:** 
- Display, SD card, and touch screen all share the same SPI bus (GPIO 23/13/14). Use different chip select pins (LCD_CS, SD_SS, Touch CS) to communicate with each device separately.
- **MISO changed from GPIO 12 to GPIO 23** to avoid boot failure (GPIO 12 must be LOW during boot on ESP32 DevKit V1).
- **SD_SS pin**: May need a separate CS pin if SD card doesn't share MISO properly.
- **Touch screen uses GPIO 25 (CS) and GPIO 26 (IRQ)** for proper operation.

#### 8-Bit Parallel Interface Wiring (Alternative)

```
🖥️ 3.5" TFT LCD (ILI9486) - Parallel Mode:
   Power:
   5V ──────> 5V Rail (Connection 2)
   GND ─────> GND (common ground)
   
   LCD Control:
   LCD_RST ─> GPIO 19 (Reset)
   LCD_CS ──> GPIO 22 (Chip Select) ⭐ Moved from GPIO 15
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
| **GPIO 23** | SPI MISO | LCD_D0 / SD_DO / Touch MISO | All (SPI Bus) | ✅ Yes - All devices ⭐ *Changed from GPIO 12* |
| **GPIO 13** | SPI MOSI | LCD_D1 / SD_DI / Touch MOSI | All (SPI Bus) | ✅ Yes - All devices |
| **GPIO 14** | SPI SCK | SD_SCK / Touch SCK | All (SPI Bus) | ✅ Yes - All devices |
| **GPIO 22** | LCD CS | LCD_CS | LCD Display | ❌ No ⭐ *Moved from GPIO 15* |
| **GPIO 18** | LCD RS | LCD_RS | LCD Display | ❌ No |
| **GPIO 19** | LCD RST | LCD_RST | LCD Display | ❌ No |
| **GPIO 23** | SD CS | SD_SS | SD Card | ❌ No ⚠️ *May need separate pin* |
| **GPIO 25** | Touch CS | Touch CS | Touch Screen | ❌ No |
| **GPIO 26** | Touch IRQ | Touch IRQ | Touch Screen | ❌ No |

**Total GPIO Pins Used:** 9 pins

**Quick Reference:**
- **Shared SPI Bus (3 pins):** GPIO 23, 13, 14 ⭐ *MISO changed from GPIO 12 to GPIO 23*
- **LCD Control (3 pins):** GPIO 22, 18, 19 ⭐ *CS moved from GPIO 15 to GPIO 22*
- **SD Card (1 pin):** GPIO 23 (or separate CS pin if needed)
- **Touch Screen (2 pins):** GPIO 25, 26

**All Other System Pins (Unchanged):**
- GPIO 21: Built-in LED ⭐ *Moved from GPIO 2*
- GPIO 4: Pump MOSFET
- GPIO 27: LED PWM ⭐ *Moved from GPIO 5*
- GPIO 16: Printer RX2
- GPIO 17: Printer TX2
- GPIO 32: IR Sensor
- GPIO 34: Moisture Sensor
- GPIO 35: Light Sensor

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
- **Display Interface:** GPIO 23 (MISO), GPIO 13 (MOSI), GPIO 14 (SCK), GPIO 22 (CS) ⭐ *MISO changed from GPIO 12, CS moved from GPIO 15*
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

1. **3.5" SPI TFT LCD (ILI9486)** ⭐ **Currently Added**
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

2. **2.4" TFT Touch Screen (ILI9341 + XPT2046)**
   - **Interface:** SPI
   - **Power:** 3.3V or 5V, ~80-150mA
   - **GPIO Pins:** 7-9 pins
   - **Compatibility:** ✅ **Excellent** - Smaller size option

3. **3.2" TFT Touch Screen (ILI9341 + XPT2046)**
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
- GPIO 23: MISO ⭐ Changed from GPIO 12
- GPIO 13: MOSI
- GPIO 14: SCK
- GPIO 22: CS (Display) ⭐ Moved from GPIO 15

Display Control:
- GPIO 18: DC (Data/Command)
- GPIO 19: RST (Reset)

Touch Controller:
- GPIO 25: Touch CS
- GPIO 26: Touch IRQ (optional)
- GPIO 27: Touch MOSI/MISO (if separate SPI)
```

#### Summary

✅ **4.0" Touch Screen (ST7796S/ILI9486) is Fully Compatible:**
- ✅ Sufficient GPIO pins available (7-9 pins for 4-wire SPI interface)
- ✅ Power consumption (~90mA) fits perfectly on 3.3V or 5V rail (can share connections)
- ✅ 4-wire SPI interface matches available GPIO pins (GPIO 23/13/14/22) ⭐ *MISO changed from GPIO 12, CS moved from GPIO 15*
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

### iOS Shortcut Issues:

**"Invalid JSON" Error:**
- Make sure Request Body is set to **"File"** (not JSON or Text)
- Verify the Text variable contains proper JSON format
- Check that Content-Type header is exactly `application/json`

**Message Not Printing:**
- Verify ESP32 is online and connected to WiFi
- Check Firebase connection (ESP32 polls every 30 seconds)
- Make sure the URL is correct: `https://printerpot-d96f8-default-rtdb.firebaseio.com/commands.json`

### Python Script Issues:

**Connection Failed:**
- Check your internet connection
- Verify Firebase URL is correct
- Make sure ESP32 is online

**Message Not Printing:**
- ESP32 polls Firebase every 30 seconds - wait up to 30 seconds
- Check ESP32 serial monitor for errors
- Verify printer is connected and powered on

### Web Interface Not Loading:
1. **Check ESP32 IP address** - Look in serial monitor for HTTP server message
2. **Verify WiFi connection** - ESP32 must be connected to WiFi
3. **Check port** - Should be port 80 (default)
4. **Same network** - Device must be on same WiFi network as ESP32
5. **Try IP directly** - Use `http://192.168.4.1` format
6. **Check firewall** - Make sure firewall isn't blocking

### Commands Not Working:
1. Check Firebase connection in ESP32 serial monitor
2. Verify Firebase URL in code
3. Check internet connectivity
4. Monitor Firebase usage limits

### Firebase 401 Errors:
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

## Support and Debugging

- **Serial Monitor** - Monitor ESP32 status and debug messages via serial output
- **Firebase Console** - Monitor database usage and data through Firebase web console
- **Web Interface** - Access full system management at `http://192.168.4.1`
- **System Logs** - Serial monitor provides detailed operation logs for troubleshooting
- **Test Script** - Use `send_message.py` for quick message delivery testing
