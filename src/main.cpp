/*
 * ESP32 Print-n-Prick - Hardware Controller (Main ESP32)
 * Receives commands from TZT via ESP-NOW; controls printer, pump, sensors, LED.
 * TZT runs web server, Firebase, and sends all commands here.
 */

#include <Arduino.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <WiFiManager.h>
#include <time.h>
#include <ArduinoJson.h>

#include "version.h"
#include "config.h"
#include "Logger.h"
#include "HardwareAbstraction.h"
#include "PrinterService.h"
#include "OTAUpdateService.h"
#include "EspNowService.h"
#include "EspNowProtocol.h"
#include "HardwareTest.h"

// Global service instances
HardwareAbstraction* hardware;
PrinterService* printerService;
OTAUpdateService* otaService;
EspNowService* espNowService;
HardwareTest* hardwareTest;

String deviceIP = "";  // For logging

// Non-blocking ESP-NOW test state (TEST_PUMP / TEST_LED run for fixed duration without blocking loop)
static uint8_t espNowTestType = 0;           // 0=none, 1=pump, 2=led
static unsigned long espNowTestStartMs = 0;  // millis() when test started
static const unsigned long TEST_PUMP_DURATION_MS = 800;
static const unsigned long TEST_LED_DURATION_MS = 1000;

// Auto-dispense: when true, IR sensor triggers pump; when false, only manual/ESP-NOW commands do
static bool autoDispenseEnabled = true;

// Chunked print reassembly (CMD_PRINT_CHUNK) - in scope for loop() sensor pause
#define PRINT_CHUNK_MAX 64
static String printChunkParts[PRINT_CHUNK_MAX];
static bool printChunkReceived[PRINT_CHUNK_MAX];
static uint8_t printChunkTotal = 0;
static bool printChunkMessageOnly = false; // true = from web (message only); false = from Shortcut (full receipt)
static unsigned long sensorPauseUntil = 0;  // Pause sensor sends during chunked print; resume after this time
static unsigned long lastChunkTime = 0;     // Timeout: abort chunked print if no chunk for 15s
static bool pendingChunkPrintReady = false; // All chunks received; do print from loop after ACK queue drains
static unsigned long lastChunkPrintDone = 0; // When we last completed a chunked print (ignore duplicate chunks for 3s)
#define CHUNK_PRINT_COOLDOWN_MS 3000       // Ignore PRINT_CHUNK_START / PRINT_CHUNK(idx==0) within this window after print

// Function declarations
void setupWiFi();
void setupTime();
void handleEspNowCommand(CommandPacket* cmd);
void handleSerialCommands();
static bool isPreformattedPrint(const String& full);
static String getWeatherOneLine();

// Time configuration
const char* ntpServer = NTP_SERVER;
const long gmtOffset_sec = GMT_OFFSET_SEC;
const int daylightOffset_sec = DAYLIGHT_OFFSET_SEC;

void setup() {
    // CRITICAL: Set unused strapping pins to correct state IMMEDIATELY - before Serial.begin
    // ESP32 DevKit V1 has 5 strapping pins: GPIO 0, 2, 5, 12, 15
    // We've moved our pins away from strapping pins, but ensure unused ones are correct
    
    // GPIO 0: Must be HIGH for normal boot mode (LOW = flash/programming mode)
    pinMode(0, OUTPUT);
    digitalWrite(0, HIGH);  // GPIO 0 must be HIGH during boot
    
    // GPIO 2: Onboard blue LED on most DevKits. After boot we use it for WiFi status.
#if (BOARD_LED_PIN >= 0)
    pinMode(BOARD_LED_PIN, OUTPUT);
    digitalWrite(BOARD_LED_PIN, BOARD_LED_ACTIVE_LOW ? HIGH : LOW);  // Off until WiFi connects
#else
    pinMode(2, INPUT_PULLDOWN);  // No onboard LED; keep safe for boot
#endif
    
    // GPIO 5: Must be HIGH during boot (no longer used - set to safe state)
    pinMode(5, INPUT_PULLUP);  // Set as input with pull-up to ensure HIGH
    
    // GPIO 12: Must be LOW during boot (no longer used - set to safe state)
    pinMode(12, INPUT_PULLDOWN);  // Set as input with pull-down to ensure LOW
    
    // GPIO 15: Must be HIGH during boot (no longer used - set to safe state)
    pinMode(15, INPUT_PULLUP);  // Set as input with pull-up to ensure HIGH
    
    delay(200);  // Ensure all strapping pins are stable before continuing
    
    // CRITICAL: GPIO 0 is now used for Touch IRQ, ensure it stays HIGH during boot
    // GPIO 0 must be HIGH for normal boot mode (already set above, but ensure it's stable)
    // No need to set Serial2 pins (GPIO 4, 33) - they're safe and don't interfere with boot
    delay(100);  // Ensure pins are stable
    
    Serial.begin(115200);
    // Short delay for Serial and power to stabilize
    delay(1500);
    
    // Set up logging - Reduced to WARN to save flash memory (INFO logs use more string storage)
    Logger::setLevel(LOG_LEVEL_WARN);
    Logger::info("Main", "========================================");
    Logger::info("Main", "ESP32 Print-n-Prick v" + String(FIRMWARE_VERSION));
    Logger::info("Main", "Build: " + String(BUILD_DATE) + " " + String(BUILD_TIME));
    Logger::info("Main", "========================================");
    Logger::info("Main", "Strapping pins set to safe state (all pins moved to non-strapping GPIOs)");
    
    // Setup WiFi FIRST - so the "Print-n-Prick" config AP appears even if hardware
    // (printer, sensors, etc.) fails or is disconnected. lets you use WiFi Manager
    // to connect to your network before any peripheral init.
    setupWiFi();
    Serial.println("MAC: " + WiFi.macAddress());
    delay(500);
    
    // Initialize hardware abstraction layer
    hardware = new HardwareAbstraction();
    if (!hardware->initialize()) {
        Logger::error("Main", "Hardware initialization failed!");
        // Don't halt - continue without some hardware features
        Logger::warn("Main", "Continuing with limited hardware functionality");
    }
    
    // Initialize printer service
    printerService = new PrinterService(hardware);
    Logger::info("Main", "Printer service initialized");
    
    // Initialize hardware test suite
    hardwareTest = new HardwareTest(hardware, printerService);
    Logger::info("Main", "Hardware test suite initialized");
    Serial.println("\n>>> Type 'test' to enter hardware test mode");
    
    // Setup time
    setupTime();
    
    // Initialize ESP-NOW (receive commands from TZT, send sensor data to TZT)
    espNowService = new EspNowService();
    espNowService->setCommandCallback(handleEspNowCommand);  // Always set: commands are handled even if peer add fails (we can still receive)
    uint8_t tztMac[6] = TZT_DISPLAY_MAC_ADDRESS;
    if (espNowService->initialize(tztMac)) {
        Logger::info("Main", "ESP-NOW: waiting for commands from TZT");
    } else {
        Logger::warn("Main", "ESP-NOW init failed (check TZT_DISPLAY_MAC_ADDRESS in config.h). Commands from TZT will still be executed if received.");
    }
    
    // Initialize OTA service
    otaService = new OTAUpdateService();
    #ifdef OTA_PASSWORD
    otaService->initialize("Print-n-Prick", OTA_PASSWORD);
    #else
    otaService->initialize("Print-n-Prick");
    #endif
    Logger::info("Main", "OTA update service initialized");
    
    Logger::info("Main", "Setup complete!");
    Logger::info("Main", "Starting main loop...");
}

void loop() {
    // Handle Serial commands (for testing)
    handleSerialCommands();
    
    // Handle OTA updates (high priority)
    otaService->handle();
    
    // Check for dispense timeout (safety feature)
    hardware->checkDispenseTimeout();
    
    // IR sensor triggers pump when auto-dispense enabled: when hand detected, start dispense if cooldown passed
    if (autoDispenseEnabled && hardware && !hardware->isDispensing() && hardware->checkCooldown()) {
        if (digitalRead(IR_SENSOR_PIN) == LOW) {  // LOW = detected (same as HardwareAbstraction)
            hardware->startPump();
        }
    }
    
    // Non-blocking ESP-NOW test timeout (TEST_PUMP / TEST_LED)
    if (espNowTestType != 0 && hardware) {
        unsigned long elapsed = millis() - espNowTestStartMs;
        if (espNowTestType == 1 && elapsed >= TEST_PUMP_DURATION_MS) {
            hardware->stopPump();
            espNowTestType = 0;
        } else if (espNowTestType == 2 && elapsed >= TEST_LED_DURATION_MS) {
            hardware->setLEDBrightness(0);
            espNowTestType = 0;
        }
    }
    
    // Update LED status based on WiFi connection
    static unsigned long lastLEDUpdate = 0;
    static int lastWiFiStatus = -1;
    if (millis() - lastLEDUpdate > WIFI_STATUS_CHECK_INTERVAL_MS) {
        int wifiStatus = WiFi.status();
        bool wifiConnected = (wifiStatus == WL_CONNECTED);
        
        // Log WiFi status changes for debugging
        if (wifiStatus != lastWiFiStatus) {
            String statusStr = "";
            switch(wifiStatus) {
                case WL_IDLE_STATUS: statusStr = "IDLE"; break;
                case WL_NO_SSID_AVAIL: statusStr = "NO_SSID_AVAIL"; break;
                case WL_SCAN_COMPLETED: statusStr = "SCAN_COMPLETED"; break;
                case WL_CONNECTED: statusStr = "CONNECTED"; break;
                case WL_CONNECT_FAILED: statusStr = "CONNECT_FAILED"; break;
                case WL_CONNECTION_LOST: statusStr = "CONNECTION_LOST"; break;
                case WL_DISCONNECTED: statusStr = "DISCONNECTED"; break;
                default: statusStr = "UNKNOWN(" + String(wifiStatus) + ")"; break;
            }
            Logger::info("Main", "WiFi Status: " + statusStr + " | LED: " + String(wifiConnected ? "ON" : "OFF"));
            lastWiFiStatus = wifiStatus;
        }
#if (BOARD_LED_PIN >= 0)
        // Onboard blue LED: on when WiFi connected. BOARD_LED_ACTIVE_LOW: 1=LOW=on, 0=HIGH=on
        digitalWrite(BOARD_LED_PIN, wifiConnected
            ? (BOARD_LED_ACTIVE_LOW ? LOW : HIGH) : (BOARD_LED_ACTIVE_LOW ? HIGH : LOW));
#endif
        lastLEDUpdate = millis();
    }
    
    // Fast path: sensor → actuator (light → LED). Only when auto-brightness on (saves CPU when manual).
    static unsigned long lastActuatorUpdate = 0;
    if (hardware->isAutoBrightnessEnabled() && (millis() - lastActuatorUpdate >= SENSOR_ACTUATOR_INTERVAL_MS)) {
        hardware->readLightSensor();   // Keep light cache fresh for LED
        hardware->updateLEDBrightness();
        lastActuatorUpdate = millis();
    }
    
    // Slow path: sensors for status/ESP-NOW (moisture, IR cache). HTTP/display don't need fast updates.
    static unsigned long lastSensorCheck = 0;
    if (millis() - lastSensorCheck > SENSOR_CHECK_INTERVAL) {
        hardware->readMoistureSensor();
        hardware->readIRSensor();      // Cache for ESP-NOW; IR→pump trigger uses digitalRead every loop
        hardware->readLightSensor();   // Keep cache in sync for ESP-NOW payload
        lastSensorCheck = millis();
    }
    
    // If auto-brightness off, still apply manual LED updates at slow interval (e.g. after web command)
    static unsigned long lastLEDBrightnessUpdate = 0;
    if (!hardware->isAutoBrightnessEnabled() && (millis() - lastLEDBrightnessUpdate > LED_UPDATE_INTERVAL)) {
        hardware->updateLEDBrightness();
        lastLEDBrightnessUpdate = millis();
    }
    
    // Update LED PWM ramp-up (1V per second) - call frequently for smooth ramping
    hardware->updateLEDRamp();
    
    // Send sensor data to TZT via ESP-NOW (uses cached sensor values from above)
    // Wait 3 seconds after ESP-NOW init before first send (give TZT time to initialize)
    static unsigned long lastEspNowSend = 0;
    static unsigned long espNowInitTime = 0;
    if (espNowService && espNowService->isInitialized()) {
        if (espNowInitTime == 0) {
            espNowInitTime = millis();
        }
        // Timeout: if chunked print started but no chunk received for 15s, abort and resume sensor
        if (printChunkTotal > 0 && lastChunkTime > 0 && (millis() - lastChunkTime) > 15000) {
            for (int i = 0; i < PRINT_CHUNK_MAX; i++) {
                printChunkReceived[i] = false;
                printChunkParts[i] = "";
            }
            printChunkTotal = 0;
            printChunkMessageOnly = false;
            lastChunkTime = 0;
            pendingChunkPrintReady = false;
            sensorPauseUntil = millis() + 500;
        }
        // Deferred print: all chunks received; drain ACK queue from loop then print (never block in recv callback)
        else if (pendingChunkPrintReady && espNowService && !espNowService->hasPendingChunkAck()) {
            pendingChunkPrintReady = false;
            String full;
            for (uint8_t i = 0; i < printChunkTotal; i++) full += printChunkParts[i];
            uint8_t nChunks = printChunkTotal;
            bool messageOnly = printChunkMessageOnly;  // Use before reset
            for (int i = 0; i < PRINT_CHUNK_MAX; i++) {
                printChunkReceived[i] = false;
                printChunkParts[i] = "";
            }
            printChunkTotal = 0;
            printChunkMessageOnly = false;
            lastChunkTime = 0;
            lastChunkPrintDone = millis();  // Cooldown: ignore duplicate chunks for CHUNK_PRINT_COOLDOWN_MS
            sensorPauseUntil = millis() + 500;
            if (full.length() > 0 && hardware && printerService) {
                if (messageOnly) {
                    printerService->printMessageOnly(full);
                } else if (isPreformattedPrint(full)) {
                    printerService->printPreformatted(full, getWeatherOneLine());
                } else {
                    printerService->setWeather(getWeatherOneLine());
                    printerService->printReceipt(full, true, time(nullptr));
                }
                Serial.println("✅ Executed: PRINT_CHUNK (assembled " + String(nChunks) + " chunks)");
            }
        }
        // Drain CHUNK_ACK queue (never send from callback = no NO_MEM). One ACK per 40ms so prints never stall.
        else if (espNowService->hasPendingChunkAck()) {
            static unsigned long lastChunkAckSend = 0;
            if (millis() - lastChunkAckSend >= 40) {
                lastChunkAckSend = millis();
                espNowService->sendPendingChunkAck();
            }
        }
        // Retry once ~400ms after a send failure (reduces intermittent "Data send failed" when TZT/WiFi busy)
        else if (espNowService->shouldRetrySend()) {
            espNowService->sendSensorData();
            espNowService->clearRetryState();
        }
        // Wait at least 3 seconds after ESP-NOW init before sending.
        // Skip during chunked print (first chunk received → pause until print done + 500ms).
        // Skip if CHUNK_ACK queue has pending (so ACKs drain first; avoids NO_MEM).
        // Skip if a send is still in flight to avoid ESP-NOW NO_MEM (TX queue full).
        else if ((millis() - espNowInitTime) >= 3000 && (millis() - lastEspNowSend >= ESP_NOW_SEND_INTERVAL) && !espNowService->hasSendInFlight()
                 && !espNowService->hasPendingChunkAck()
                 && printChunkTotal == 0 && !pendingChunkPrintReady && (sensorPauseUntil == 0 || millis() >= sensorPauseUntil)) {
            // Cached sensor values (fast path updates light/LED every 50ms when auto-brightness on)
            float lightPercent = hardware->getLightPercent();
            uint8_t ledBrightness = hardware->getLEDBrightness();
            
            // Send synchronized values - LED brightness calculated from same light sensor reading
            // This ensures TZT display shows matching values that correspond to hardware test
            espNowService->updateSensorData(
                hardware->getMoisturePercent(),
                hardware->getSanitizerLevel(),
                hardware->isIRDetected(),
                lightPercent,  // Use cached light sensor value
                ledBrightness,  // Use LED brightness (calculated from same light reading)
                hardware->isDispensing(),
                hardware->isPumpRunning(),
                autoDispenseEnabled,
                hardware->isAutoBrightnessEnabled()
            );
            espNowService->sendSensorData();
            lastEspNowSend = millis();
        }
    }
    
    delay(1);  // Short delay for sensor→actuator responsiveness (IR→pump, light→LED). HTTP latency not a priority.
}

void setupWiFi() {
    Logger::info("WiFi", "Setting up WiFi...");
    
    Logger::info("WiFi", "WiFi initialization complete");
    
    // Check initial WiFi status
    int initialStatus = WiFi.status();
    Logger::info("WiFi", "Initial WiFi status: " + String(initialStatus));
    
    WiFiManager wm;
    WiFi.mode(WIFI_STA);
    Logger::info("WiFi", "WiFi mode set to STA (Station)");
    
    // Extended timeout for external power - WiFi may need more time to initialize
    wm.setConfigPortalTimeout(180);  // 3 minutes
    
#if FORCE_WIFI_CONFIG_PORTAL
    // Erase saved WiFi and open config portal this boot only (set FORCE back to 0 and reflash after configuring).
    Logger::info("WiFi", "FORCE_WIFI_CONFIG_PORTAL=1: erasing WiFi, starting config portal (AP: " AP_SSID ")");
    wm.resetSettings();
    wm.startConfigPortal(AP_SSID, AP_PASSWORD);
    bool res = (WiFi.status() == WL_CONNECTED);
#else
    // Try saved credentials first; if none or connection fails, config portal opens once, then credentials are saved for next boot.
    Logger::info("WiFi", "Starting autoConnect (timeout: 180s)...");
    bool res = wm.autoConnect(AP_SSID, AP_PASSWORD);
#endif
    
    // Check WiFi status after connection attempt
    int finalStatus = WiFi.status();
    Logger::info("WiFi", "autoConnect result: " + String(res ? "SUCCESS" : "FAILED"));
    Logger::info("WiFi", "Final WiFi status code: " + String(finalStatus));
    
    if (!res) {
        Logger::error("WiFi", "❌ Failed to connect to WiFi");
        String statusStr = "";
        switch(finalStatus) {
            case WL_IDLE_STATUS: statusStr = "IDLE"; break;
            case WL_NO_SSID_AVAIL: statusStr = "NO_SSID_AVAIL"; break;
            case WL_SCAN_COMPLETED: statusStr = "SCAN_COMPLETED"; break;
            case WL_CONNECTED: statusStr = "CONNECTED"; break;
            case WL_CONNECT_FAILED: statusStr = "CONNECT_FAILED"; break;
            case WL_CONNECTION_LOST: statusStr = "CONNECTION_LOST"; break;
            case WL_DISCONNECTED: statusStr = "DISCONNECTED"; break;
            default: statusStr = "UNKNOWN(" + String(finalStatus) + ")"; break;
        }
        Logger::error("WiFi", "WiFi status: " + statusStr);
    } else {
        deviceIP = WiFi.localIP().toString();
        Logger::info("WiFi", "✅ Connected successfully!");
        Logger::info("WiFi", "IP Address: " + deviceIP);
        Logger::info("WiFi", "RSSI: " + String(WiFi.RSSI()) + " dBm");
        Logger::info("WiFi", "SSID: " + String(WiFi.SSID()));
        Logger::info("WiFi", "MAC Address: " + String(WiFi.macAddress()));
    }
}

void setupTime() {
    configTime(gmtOffset_sec, daylightOffset_sec, ntpServer);
    Logger::info("Time", "NTP time server configured");
}

// Strip to printable ASCII only so thermal printer (CP437) shows readable text
static String weatherToPrinterSafe(const String& s) {
    String out;
    for (unsigned int i = 0; i < s.length(); i++) {
        char c = s.charAt(i);
        if (c >= 32 && c <= 126)
            out += c;
        else if ((c & 0x80) != 0)
            while (i + 1 < s.length() && (s.charAt(i + 1) & 0xC0) == 0x80) i++;
    }
    return out;
}

// One receipt row (48 chars) for user-sent messages only (not reminder/grocery)
#define WEATHER_ROW_MAX 48
static String getWeatherOneLine() {
    if (WiFi.status() != WL_CONNECTED) return "";
    WiFiClientSecure client;
    client.setInsecure();
    if (!client.connect("api.openweathermap.org", 443, 8000)) return "";
    String url = "/data/2.5/weather?lat=";
    url += String(WEATHER_LATITUDE, 2);
    url += "&lon=";
    url += String(WEATHER_LONGITUDE, 2);
    url += "&appid=" WEATHER_API_KEY "&units=imperial";
    client.print("GET " + url + " HTTP/1.1\r\nHost: api.openweathermap.org\r\nConnection: close\r\n\r\n");
    unsigned long t = millis();
    while (client.connected() && millis() - t < 5000) {
        if (client.available()) break;
        delay(10);
    }
    // Skip headers until blank line
    while (client.available()) {
        String line = client.readStringUntil('\n');
        line.trim();
        if (line.length() == 0) break;
    }
    String body;
    while (client.available() && body.length() < 1200)
        body += (char)client.read();
    client.stop();
    if (body.length() == 0) return "";
    DynamicJsonDocument doc(1024);
    if (deserializeJson(doc, body) != DeserializationError::Ok) return "";
    float temp = doc["main"]["temp"] | 0.0f;
    int humidity = doc["main"]["humidity"] | 0;
    const char* mainCond = doc["weather"][0]["main"] | "";
    float rain1h = doc["rain"]["1h"] | 0.0f;
    float rain3h = doc["rain"]["3h"] | 0.0f;
    float snow1h = doc["snow"]["1h"] | 0.0f;
    float snow3h = doc["snow"]["3h"] | 0.0f;
    String line;
    line += "T: ";
    line += (int)roundf(temp);
    line += "F H: ";
    line += String(humidity);
    line += "% ";
    line += weatherToPrinterSafe(String(mainCond).substring(0, 12));
    if (rain1h > 0 || rain3h > 0) {
        float r = rain1h > 0 ? rain1h : rain3h;
        line += " ";
        line += String(r, 1);
        line += "mm";
    } else if (snow1h > 0 || snow3h > 0) {
        float s = snow1h > 0 ? snow1h : snow3h;
        line += " ";
        line += String(s, 1);
        line += "mm";
    }
    if (line.length() > (unsigned int)WEATHER_ROW_MAX)
        line = line.substring(0, WEATHER_ROW_MAX);
    return line;
}

// Human-readable command name for serial logs (matches TZT naming)
static const char* espNowCommandName(uint8_t cmd) {
    switch (cmd) {
        case CMD_PRINT: return "PRINT";
        case CMD_PRINT_CHUNK: return "PRINT_CHUNK";
        case CMD_PRINT_CHUNK_START: return "PRINT_CHUNK_START";
        case CMD_DISPENSE_START: return "DISPENSE_START";
        case CMD_DISPENSE_STOP: return "DISPENSE_STOP";
        case CMD_RESET_SANITIZER: return "RESET_SANITIZER";
        case CMD_SET_LED_BRIGHTNESS: return "SET_LED_BRIGHTNESS";
        case CMD_SET_LED_STATE: return "SET_LED_STATE";
        case CMD_SET_AUTO_BRIGHTNESS: return "SET_AUTO_BRIGHTNESS";
        case CMD_TEST_PRINTER: return "TEST_PRINTER";
        case CMD_TEST_PUMP: return "TEST_PUMP";
        case CMD_TEST_LED: return "TEST_LED";
        case CMD_SET_PUMP_DURATION: return "SET_PUMP_DURATION";
        case CMD_SET_PUMP_COOLDOWN: return "SET_PUMP_COOLDOWN";
        case CMD_SET_AUTO_DISPENSE: return "SET_AUTO_DISPENSE";
        case CMD_SET_LIGHT_CALIBRATION: return "SET_LIGHT_CALIBRATION";
        default: return "CMD_?";
    }
}

// True if message is already formatted (e.g. from TZT: "MESSAGE\n----...", "REMINDER", "GROCERY LIST", "TODO LIST")
static bool isPreformattedPrint(const String& full) {
    return full.startsWith("MESSAGE\n") || full.startsWith("REMINDER") ||
           full.startsWith("GROCERY LIST") || full.startsWith("TODO LIST");
}

// Dedupe setting commands (TZT sends 4x): only execute first within 1s per (type, param1, param2)
#define SETTING_DEDUPE_MS 1000
static uint32_t lastSettingKey = 0;
static unsigned long lastSettingTime = 0;

static bool isSettingCommandType(uint8_t type) {
    return (type == CMD_SET_LED_BRIGHTNESS || type == CMD_SET_LED_STATE || type == CMD_SET_AUTO_BRIGHTNESS
         || type == CMD_SET_PUMP_DURATION || type == CMD_SET_PUMP_COOLDOWN || type == CMD_SET_AUTO_DISPENSE
         || type == CMD_SET_LIGHT_CALIBRATION);
}

void handleEspNowCommand(CommandPacket* cmd) {
    if (!cmd || !hardware || !printerService) return;
    if (cmd->commandType == CMD_PRINT_CHUNK)
        Serial.println("[Main] Rcvd: PRINT_CHUNK " + String((int)(cmd->param1 + 1)) + "/" + String((int)cmd->param2));
    else
        Serial.println("[Main] Rcvd: " + String(espNowCommandName(cmd->commandType)));
    // Skip duplicate setting command within 1s (TZT sends 4x for reliability)
    if (isSettingCommandType(cmd->commandType)) {
        uint32_t key = (uint32_t)(cmd->commandType << 16) | (cmd->param1 << 8) | cmd->param2;
        if (lastSettingKey == key && (millis() - lastSettingTime) < SETTING_DEDUPE_MS)
            return;  // Duplicate, already executed
    }
    switch (cmd->commandType) {
        case CMD_PRINT: {
            if (!cmd->message[0]) break;
            String full = String(cmd->message);
            if (isPreformattedPrint(full)) {
                printerService->printPreformatted(full, getWeatherOneLine());
            } else {
                // Raw message (e.g. from index.html or iOS Shortcut): full receipt with MESSAGE title, date, weather
                printerService->setWeather(getWeatherOneLine());
                printerService->printReceipt(full, true, time(nullptr));
            }
            Serial.println("✅ Executed: PRINT");
            break;
        }
        case CMD_PRINT_CHUNK_START: {
            if (millis() - lastChunkPrintDone < CHUNK_PRINT_COOLDOWN_MS) break; // Ignore duplicate session (TZT resend)
            uint8_t total = cmd->param2;
            if (total == 0 || total > PRINT_CHUNK_MAX) break;
            printChunkMessageOnly = (cmd->param1 != 0);  // param1: 0 = full receipt, 1 = message only (from web)
            for (int i = 0; i < PRINT_CHUNK_MAX; i++) {
                printChunkReceived[i] = false;
                printChunkParts[i] = "";
            }
            printChunkTotal = total;
            lastChunkTime = millis();
            break;
        }
        case CMD_PRINT_CHUNK: {
            uint8_t idx = cmd->param1;
            uint8_t total = cmd->param2;
            if (total == 0 || total > PRINT_CHUNK_MAX) break;
            if (idx >= total) break;
            if (idx == 0 && (millis() - lastChunkPrintDone < CHUNK_PRINT_COOLDOWN_MS)) break; // Ignore duplicate chunk 0 (TZT resend)
            if (idx == 0) {
                for (int i = 0; i < PRINT_CHUNK_MAX; i++) {
                    printChunkReceived[i] = false;
                    printChunkParts[i] = "";
                }
                printChunkTotal = total;
            }
            lastChunkTime = millis();
            size_t addLen = strnlen(cmd->message, sizeof(cmd->message));
            printChunkParts[idx] = String(cmd->message).substring(0, addLen);
            printChunkReceived[idx] = true;
            bool haveAll = true;
            for (uint8_t i = 0; i < printChunkTotal; i++) {
                if (!printChunkReceived[i]) { haveAll = false; break; }
            }
            if (haveAll && printChunkTotal > 0) {
                // Defer print to main loop so we drain CHUNK_ACK queue there (no blocking/send in recv callback)
                pendingChunkPrintReady = true;
            }
            break;
        }
        case CMD_DISPENSE_START:
            if (!hardware->isPumpRunning()) {
                hardware->startPump();
                Serial.println("✅ Executed: DISPENSE_START (pump on)");
            }
            break;
        case CMD_DISPENSE_STOP:
            if (hardware->isPumpRunning()) {
                hardware->stopPump();
                Serial.println("✅ Executed: DISPENSE_STOP (pump off)");
            }
            break;
        case CMD_RESET_SANITIZER:
            if (hardware->getSanitizerLevel() < 99.0f) {
                hardware->resetSanitizer();
                Serial.println("✅ Executed: RESET_SANITIZER");
            }
            break;
        case CMD_SET_LED_BRIGHTNESS:
            if (hardware->getLEDBrightness() != cmd->param1) {
                hardware->setLEDBrightness(cmd->param1);
                if (cmd->param1 == 0) hardware->setAutoBrightness(false);  // User turned off LED
                Serial.println("✅ Executed: SET_LED_BRIGHTNESS=" + String(cmd->param1));
            }
            break;
        case CMD_SET_LED_STATE: {
            bool wantOn = (cmd->param1 != 0);
            bool currentlyOn = (hardware->getLEDBrightness() > 0);
            if (wantOn != currentlyOn) {
                hardware->setLEDBrightness(wantOn ? 255 : 0);
                if (!wantOn) hardware->setAutoBrightness(false);
                Serial.println("✅ Executed: SET_LED_STATE=" + String(cmd->param1 ? 1 : 0));
            }
            break;
        }
        case CMD_SET_AUTO_BRIGHTNESS: {
            bool wantOn = (cmd->param1 != 0);
            if (hardware->isAutoBrightnessEnabled() != wantOn) {
                hardware->setAutoBrightness(wantOn);
                Serial.println("✅ Executed: SET_AUTO_BRIGHTNESS=" + String(cmd->param1 ? 1 : 0));
            }
            break;
        }
        case CMD_TEST_PRINTER:
            printerService->printTest();
            Serial.println("✅ Executed: TEST_PRINTER");
            break;
        case CMD_TEST_PUMP:
            espNowTestType = 1;
            espNowTestStartMs = millis();
            hardware->startPump();
            Serial.println("✅ Executed: TEST_PUMP (pump on for " + String(TEST_PUMP_DURATION_MS) + "ms, non-blocking)");
            break;
        case CMD_TEST_LED:
            espNowTestType = 2;
            espNowTestStartMs = millis();
            hardware->setLEDBrightness(255);
            Serial.println("✅ Executed: TEST_LED (LED on for " + String(TEST_LED_DURATION_MS) + "ms, non-blocking)");
            break;
        case CMD_SET_PUMP_DURATION: {
            unsigned long ms = (unsigned long)cmd->param1 * 100;  // param1 = duration in 100ms units (0–25.5s)
            unsigned long cur = hardware->getMaxDispenseDurationMs();
            unsigned long effectiveCur = (cur > 0) ? cur : MAX_DISPENSE_DURATION_MS;
            if (effectiveCur != ms) {
                hardware->setMaxDispenseDurationMs(ms);
                Serial.println("✅ Executed: SET_PUMP_DURATION=" + String(ms) + "ms");
            }
            break;
        }
        case CMD_SET_PUMP_COOLDOWN: {
            unsigned long ms = (unsigned long)cmd->param1 * 100;  // param1 = cooldown in 100ms units (0–25.5s)
            unsigned long cur = hardware->getDispenseCooldownMs();
            unsigned long effectiveCur = (cur > 0) ? cur : DISPENSE_COOLDOWN_MS;
            if (effectiveCur != ms) {
                hardware->setDispenseCooldownMs(ms);
                Serial.println("✅ Executed: SET_PUMP_COOLDOWN=" + String(ms) + "ms");
            }
            break;
        }
        case CMD_SET_AUTO_DISPENSE: {
            bool wantOn = (cmd->param1 != 0);
            if (autoDispenseEnabled != wantOn) {
                autoDispenseEnabled = wantOn;
                Serial.println("✅ Executed: SET_AUTO_DISPENSE=" + String(autoDispenseEnabled ? 1 : 0));
            }
            break;
        }
        case CMD_SET_LIGHT_CALIBRATION: {
            bool isMax = (cmd->param1 != 0);  // 0 = record min (turn off), 1 = record max (turn on)
            hardware->readLightSensor();
            float pct = hardware->getLightPercent();
            if (isMax)
                hardware->setLightCalibrationMax(pct);
            else
                hardware->setLightCalibrationMin(pct);
            // Apply new calibration to LED immediately (uses cached lightPercent)
            if (hardware->isAutoBrightnessEnabled())
                hardware->updateLEDBrightness();
            Serial.println("✅ Executed: SET_LIGHT_CALIBRATION " + String(isMax ? "max" : "min") + " (light=" + String(pct, 1) + "%)");
            break;
        }
        default:
            Logger::warn("Main", "Unknown ESP-NOW command: " + String(cmd->commandType));
            break;
    }
    // Record that we executed this setting command (for 1s dedupe)
    if (isSettingCommandType(cmd->commandType)) {
        lastSettingKey = (uint32_t)(cmd->commandType << 16) | (cmd->param1 << 8) | cmd->param2;
        lastSettingTime = millis();
    }
}

void handleSerialCommands() {
    // Check for Serial input (non-blocking)
    if (Serial.available() > 0) {
        String command = Serial.readStringUntil('\n');
        command.replace("\r", "");
        command.trim();
        if (command.length() == 0) return;  // Ignore empty line (e.g. Enter only)
        command.toLowerCase();
        
        if (command == "test" || command == "t") {
            Serial.println("\n>>> Entering Hardware Test Mode");
            Serial.println(">>> Type 'exit' to return to normal operation\n");
            
            if (!hardwareTest) {
                Serial.println("Test suite not initialized!");
                return;
            }
            
            // Show initial menu
            hardwareTest->printTestMenu();
            
            // Enter test mode loop
            while (true) {
                // Run interactive test menu (handles input and tests)
                // Returns false if user wants to exit
                bool continueTesting = hardwareTest->runInteractiveTests();
                if (!continueTesting) {
                    Serial.println("\n>>> Exiting test mode");
                    break;
                }
            }
        } else if (command == "help" || command == "h") {
            Serial.println("\n=== Available Commands ===");
            Serial.println("  test, t  - Enter hardware test mode");
            Serial.println("  help, h  - Show this help message");
            Serial.println("  status   - Show hardware status");
            Serial.println("  diag     - Print diagnostics");
            Serial.println("  light    - Quick light sensor check");
        } else if (command == "status") {
            if (hardware) {
                Serial.println("\n=== Hardware Status ===");
                Serial.println(hardware->getStatusJSON());
            }
        } else if (command == "diag") {
            if (hardware) {
                hardware->printDiagnostics();
            }
        } else if (command == "light" || command == "l") {
            // Quick light sensor diagnostic
            Serial.println("\n=== Light Sensor Quick Check ===");
            Serial.println("GPIO Pin: " + String(LIGHT_SENSOR_PIN));
            Serial.println("\nTaking 5 readings...");
            for (int i = 0; i < 5; i++) {
                int rawValue = analogRead(LIGHT_SENSOR_PIN);
                float voltage = (rawValue * 3.3) / 4095.0;
                float light = hardware->readLightSensor();
                Serial.println("[" + String(i+1) + "] Raw: " + String(rawValue) + "/4095 | " +
                             "Voltage: " + String(voltage, 3) + "V | " +
                             "Light: " + String(light, 1) + "%");
                delay(500);
            }
            Serial.println("\n💡 Tip: Cover/uncover sensor and run 'light' again to see if values change");
        } else if (command.length() > 0) {
            Serial.println("Unknown command: " + command);
            Serial.println("Type 'help' for available commands");
        }
    }
}

