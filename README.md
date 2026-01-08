# Print_n_Prick - IoT Message Dispenser System

An integrated IoT system combining a thermal printer and hand sanitizer dispenser with remote messaging capabilities. The system enables remote message delivery through Firebase, printing messages on thermal receipts along with environmental data including weather conditions, moisture sensor readings, and sanitizer level information.

## Web Server Interface

The ESP32 includes an integrated HTTP server providing a web-based management interface accessible from any device on the local network.

### Accessing the Web Interface

1. **Connect to WiFi** - Connect to the 🔵 **ESP32** access point `Print_n_Prick` (password: `08202022`)
2. **Obtain IP Address** - Check serial monitor output for: `HTTP Server started on http://192.168.4.1`
3. **Open Browser** - Navigate to `http://192.168.4.1` (or the IP address displayed in serial monitor)
4. **Authentication** - Enter password: `0820`
5. **Mobile Support** - Interface is optimized for mobile devices (iOS and Android)

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

### Power Management
- **🔷 LM2596 Multi Channel Switching Power Supply Module** (3.3V/5V/9V/12V/ADJ Adjustable)
- **20W USB-C Wall Power Supply** (input power source)
- **Type-C USB Jack 3.1 Type-C 2Pin Female** (power input connector)

### Pin Connections
- **GPIO 2**: Built-in 🟡 **LED** (status indicator)
- **GPIO 4**: ⚫ **IRF520 MOSFET Driver Module** Gate (pump control signal)
- **GPIO 5**: 🟡 **LED** PWM control (12V 🟡 **LED** via ⚫ **MOSFET**, PWM output)
- **GPIO 16**: 🟢 **Thermal printer** RX
- **GPIO 17**: 🟢 **Thermal printer** TX
- **GPIO 32**: 🟣 **IR sensor** (digital input with pull-up, 5V)
- **GPIO 34**: 🟣 **Moisture sensor** (analog input, input-only pin)
- **GPIO 35**: 🟣 **LM393 Light Sensor Module** (analog input, input-only pin)

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
- **Connection 2:** 🟣 **IR Sensor Module**: ~10-20mA
- **Total**: ~210-520mA (worst case ~0.52A)
- **Power**: 0.52A × 5V = **2.6W**

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
- **Combined Output Power:** ~18.3W (peak: printer printing + LED on + pump running)
- **With 80% Efficiency:** ~22.9W input required
- **Recommended USB-C Supply:** 20W minimum (consider 30W for better headroom)

### Power Supply Specifications
- **Input:** 5V USB-C (20W = 5V @ 4A, or 30W = 5V @ 6A recommended)
- **🔷 LM2596 Module:** Typically rated for 2-3A per channel
- **Available Input Current (20W):** 4A
- **Required Input Current:** ~4.58A (22.9W ÷ 5V)
- **Headroom (20W):** Negative margin - **30W supply recommended**

### Notes
- **Each rail has 2 connection spots** - components must be distributed accordingly
- Low-current sensors (🟣 Moisture Sensor, 🟣 Light Sensor) share one connection on the 3.3V rail
- The 9V rail connection 1 is dedicated to the 🟢 **thermal printer** (connection 2 available)
- The 12V rail connection 1 is dedicated to the 🟡 **LED** (connection 2 available)
- Peak power consumption occurs when 🟢 **printer** is printing, 🟡 **LED** is active, and 🟠 **pump** is running simultaneously
- **20W USB-C charger may be insufficient under peak load** - 30W recommended for reliable operation
- 🔵 **ESP32** operates from 5V rail (expansion board handles voltage regulation internally)

---

## Future Expansion: Touch Screen

### Touch Screen Compatibility Analysis

**Yes, there is room for a touch screen!** Here's the detailed breakdown:

#### GPIO Pins Available for Touch Screen

**SPI-Based Touch Screen (Recommended):**
- **Display Interface:** GPIO 12 (MISO), GPIO 13 (MOSI), GPIO 14 (SCK), GPIO 15 (CS)
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

**Recommended Model:**

1. **4.0" SPI TFT Touch Screen (ST7796S/ILI9486) - MSP4021**
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
- GPIO 12: MISO
- GPIO 13: MOSI
- GPIO 14: SCK
- GPIO 15: CS (Display)

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
- ✅ 4-wire SPI interface matches available GPIO pins (GPIO 12/13/14/15)
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
