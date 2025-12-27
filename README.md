# 💌 Print_n_Prick - Romantic Message Dispenser

A unique combination of a thermal printer and hand sanitizer dispenser that sends sweet messages to your girlfriend via the internet. Send messages through Firebase, and they'll print out on thermal receipts along with daily weather, moisture sensor, and sanitizer level information.

## 🌐 Built-in Web Server (ESP32)

The ESP32 has a built-in HTTP server that provides a web interface accessible from any device on your local network.

### **Access the Web Interface:**
1. **Connect to WiFi** - ESP32 creates access point `Print_n_Prick` (password: `08202022`)
2. **Find the IP address** - Check serial monitor for: `🌐 HTTP Server started on http://192.168.4.1`
3. **Open in browser** - Go to `http://192.168.4.1` (or the IP shown in serial monitor)
4. **Login** - Password: `0820`
5. **Works on iPhone/Android** - Mobile-optimized interface

### **Web Interface Features:**

#### **System Status Dashboard:**
- 💧 **Moisture Sensor** - Real-time moisture percentage
- 🧴 **Sanitizer Level** - Current sanitizer percentage
- 🔄 **Reset Sanitizer Button** - Reset to 100% when refilled

#### **Reminders Tab:**
- 📅 **Schedule Reminders** - Set messages to print automatically at specific times
- 💌 **Add Reminders** - Enter message and date/time (quick options: 1 min, 30 mins, 1 hour, 12 hours, 1 day, 1 week)
- 📋 **View Scheduled Reminders** - See all pending and printed reminders
- ✏️ **Edit Reminders** - Modify existing reminders
- 🗑️ **Delete Reminders** - Remove reminders before they print

#### **Groceries Tab:**
- 🛒 **Grocery List** - Add items to remember
- ➕ **Add Items** - Type and add grocery items
- 🖨️ **Print List** - Print formatted grocery list to thermal printer
- 🗑️ **Clear List** - Clear entire grocery list

### **Mobile Optimized:**
- 📱 **iPhone/Android Compatible** - Touch-friendly interface
- 🔄 **Auto-refresh** - Updates every 30 seconds
- 💾 **Persistent Storage** - All data saved to Firebase
- 🌐 **Works Offline** - Interface cached, works even if Firebase is down

---

## 📱 Sending Messages Remotely

Send romantic messages to the Print_n_Prick from anywhere in the world!

### 📱 Method 1: iOS Shortcuts (Recommended for iPhone Users)

Send messages directly from your iPhone using the built-in Shortcuts app - no Python needed!

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

**5. How to Use:**
1. Tap your shortcut (or widget)
2. Type your romantic message
3. Tap **Done**
4. Your message will print on her Print_n_Prick! 💌

#### Pro Tips:
- **Add to Widgets:** Long-press home screen → **"+"** → **Shortcuts** → Add widget
- **Add to Apple Watch:** Shortcut appears automatically on Apple Watch
- **Use Siri:** Say *"Hey Siri, Send Love Note"* for hands-free messages

---

### 🐍 Method 2: Python Script (For Computer Users)

#### Setup:
```bash
python send_message.py "Your romantic message here"
```

#### Features:
- 💌 Send messages that print with weather, moisture sensor, and sanitizer level
- ⚡ Quick and easy - just type your message
- 🌤️ Includes weather in Fahrenheit
- 💧 Includes moisture sensor percentage
- 🧴 Includes sanitizer level

#### Usage:
```bash
# Interactive mode (prompts for message)
python send_message.py

# Command line mode (message as argument)
python send_message.py "I love you! 💕"
```

---

## 📋 What Gets Printed:

### **Your Message Receipt Format:**
When you send a message via iOS Shortcut or Python script, the thermal printer will output:

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

### **Reminder Receipt Format:**
When a reminder prints automatically, it includes:
```
================================
REMINDER
================================
Set on: Dec 25, 2024 10:00 AM

[Her reminder message here]

================================
```

### **Grocery List Format:**
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

## 🚀 Quick Start

### 1. Upload ESP32 Code
```bash
pio run --target upload
```

### 2. Access Web Interface
- Connect to WiFi network `Print_n_Prick` (password: `08202022`)
- Open browser and go to `http://192.168.4.1`
- Enter password: `0820`
- Use the web interface to:
  - View system status (moisture sensor, sanitizer level)
  - Set reminders for automatic printing
  - Manage grocery list
  - Reset sanitizer when refilled

### 3. Send Messages (Your Messages)
```bash
python send_message.py "Good morning beautiful! 💕"
```

Or use iOS Shortcuts for one-tap sending from your iPhone!

---

## 📡 API Documentation

### Base URL
`http://[ESP32-IP]:8080`

### Authentication
- Web interface password: `0820`
- Optional API key authentication (if enabled)

### Rate Limiting
- Default: 60 requests per minute per IP
- Response on limit: HTTP 429 Too Many Requests

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

## 🔧 Firebase Commands

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

## 📊 Firebase Configuration

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

**⚠️ Security Note:** These rules allow public access. Fine for personal use, but consider authentication for production.

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

## 💌 How It Works

### **Message Sending Flow (Your Messages):**
1. **You send a message** via iOS Shortcut or `send_message.py` or Firebase `print` command
2. **ESP32 checks Firebase** every 30 seconds
3. **ESP32 finds your message** and prints it on the thermal printer
4. **Receipt includes:**
   - Your custom message
   - Today's weather (Fahrenheit, 12-hour format)
   - Moisture sensor percentage
   - Hand sanitizer level percentage
   - Date and time (12-hour format with AM/PM)

### **Reminder System (Her Reminders):**
1. **She accesses web interface** at `http://192.168.4.1`
2. **Adds a reminder** with message and date/time
3. **Reminder stored in Firebase** - persists across reboots
4. **ESP32 checks reminders** every minute
5. **When time arrives** - prints automatically
6. **Receipt includes:**
   - When the reminder was set (creation time)
   - Her message (normal size, centered)
   - No weather or sensor info (just the message)

### **Grocery List System:**
1. **She accesses Groceries tab** in web interface
2. **Adds items** to the grocery list
3. **List stored in Firebase** - persists across reboots
4. **Print button** - prints formatted list to thermal printer
5. **Clear button** - clears entire list when done shopping

### **Hand Sanitizer Features:**
- **IR Sensor**: Detects when hands are placed under the dispenser
- **Automatic Dispensing**: Can dispense sanitizer when motion is detected
- **Cooldown Period**: 3-second cooldown between dispenses prevents continuous dumping
- **Maximum Duration**: 2-second maximum dispense duration for safety
- **Level Tracking**: Monitors sanitizer percentage

---

## 🔧 System Requirements

- **ESP32** with WiFi connectivity
- **Thermal printer** (optional)
- **Hand sanitizer pump** and infrared sensor
- **Python 3.7+** for control scripts (optional)
- **Firebase** account (free tier works)

---

## 🛠️ Hardware Components

### **Main Controller:**
- **ESP-WROOM-32 Development Expansion Board** with screw terminals
- **ESP32 ESP-WROOM-32 Module** (3.3V) - plugs into expansion board

### **Printing System:**
- **Embedded Thermal Printer QR204 Receipt Ticket Printers** (5V-9V)

### **Sensors:**
- **Infrared (IR) Motion Sensor Module** (5V) - for detecting hands
- **Moisture Sensor** (Analog) - connected to GPIO 34

### **Dispensing System:**
- **CJWP08 Micro M20 Diaphragm Water Pump** (3.3V) - used for hand sanitizer dispensing
- **IRF520 MOSFET Module** - for pump control (handles higher current loads)

### **Power Management:**
- **LM2596 Multi Channel Switching Power Supply Module** (3.3V/5V/12V/ADJ Adjustable)
- **5V USB-C Wall Power Supply** (input power source)
- **Type-C USB Jack 3.1 Type-C 2Pin Female** (power input connector)

### **Pin Connections:**
- **GPIO 2**: Built-in LED (status indicator)
- **GPIO 4**: IRF520 MOSFET Gate (pump control signal)
- **GPIO 17**: Thermal printer TX
- **GPIO 16**: Thermal printer RX
- **GPIO 32**: IR sensor (digital input with pull-up, 5V)
- **GPIO 34**: Moisture sensor (analog input, input-only pin)

---

## 🏗️ System Architecture

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
The system uses an **asynchronous request queue** to eliminate HTTP blocking:
- ⚡ **Instant Web Responses** - No waiting for Firebase
- 🚀 **Non-Blocking** - Web server always responsive  
- 🛡️ **Rate Limit Safe** - Controlled Firebase access (2 second intervals)
- 📊 **Automatic Retry** - Handles failures gracefully

---

## 🛠️ Troubleshooting

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

## 💡 Tips:

- **Send messages anytime** - Works from anywhere in the world as long as ESP32 is online
- **Include emojis** - They'll be converted to text equivalents (❤️ becomes <3)
- **Long messages** - No character limit, but keep it reasonable for thermal paper
- **Timing** - Messages print within 30 seconds of sending (ESP32 polling interval)
- **Privacy** - Messages are deleted from Firebase after processing

---

## 🎁 Perfect For:

- 💕 Surprising your girlfriend with sweet messages
- 📅 Sending reminders throughout the day
- 💌 Expressing love when you're apart
- 🎉 Celebrating special moments
- 🌟 Just because!

---

## 📞 Support

- 🔍 **Serial Monitor** - Check ESP32 status and debug messages
- 📊 **Firebase Console** - Monitor database usage and data
- 🌐 **Web Interface** - Access at `http://192.168.4.1` for all features
- 🐛 **Check Logs** - Serial monitor shows detailed operation logs
- 💡 **send_message.py** - Quick way to send test messages

---

**Send Love! 💌✨**
