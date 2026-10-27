/*
 * ESP32 Print-n-Prick - Hardware Controller (Main ESP32)
 * Receives commands from TZT via ESP-NOW; controls printer, pump, sensors, LED.
 * TZT runs web server and HTTP backend, and sends all commands here.
 */

#include <Arduino.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <WiFiManager.h>
#include <WebServer.h>
#include <esp_task_wdt.h>
#include <time.h>
#include <ArduinoJson.h>

#include "version.h"
#include "config.h"
#include "secrets.h"
#include "Logger.h"
#include "HardwareAbstraction.h"
#include "PrinterService.h"
#include "OTAUpdateService.h"
#include "EspNowService.h"
#include "EspNowProtocol.h"
#include "TestServer.h"

// Global service instances
HardwareAbstraction* hardware;
PrinterService* printerService;
OTAUpdateService* otaService;
EspNowService* espNowService;
TestServer* testServer;

String deviceIP = "";  // For logging

// Non-blocking ESP-NOW test state (TEST_PUMP / TEST_LED run for fixed duration without blocking loop)
static uint8_t espNowTestType = 0;           // 0=none, 1=pump, 2=led
static unsigned long espNowTestStartMs = 0;  // millis() when test started
static const unsigned long TEST_PUMP_DURATION_MS = 800;
static const unsigned long TEST_LED_DURATION_MS = 1000;
static unsigned long espNowTestDurationMs = 0;  // actual duration for the in-flight test (set per-call)

// Cancels this file's in-flight pump/LED test pulse without touching the
// actuator - called before TestServer starts its own pulse, so only one
// timer is left owning the shutoff. Defined here so TestServer.cpp can call it.
void cancelEspNowTest() { espNowTestType = 0; }

// Auto-dispense: when true, IR sensor triggers pump; when false, only manual/ESP-NOW commands do
static bool autoDispenseEnabled = true;

// Print state shared between the ESP-NOW receive callback (WiFi task) and loop() (other core).
// Fixed buffers instead of Strings (no reallocation while the other side reads), and every access
// outside the "ready" hand-off below is guarded by printStateMux.
static portMUX_TYPE printStateMux = portMUX_INITIALIZER_UNLOCKED;
#define PRINT_MSG_BYTES sizeof(((CommandPacket*)nullptr)->message)

// Chunked print reassembly (CMD_PRINT_CHUNK) - in scope for loop() sensor pause.
// Once pendingChunkPrintReady is set the callback leaves the buffers alone until loop() has printed them.
#define PRINT_CHUNK_MAX 64
static char printChunkData[PRINT_CHUNK_MAX][PRINT_MSG_BYTES];  // ~12.8 KB
static uint8_t printChunkLen[PRINT_CHUNK_MAX];
static bool printChunkReceived[PRINT_CHUNK_MAX];
static volatile uint8_t printChunkTotal = 0;
static bool printChunkMessageOnly = false; // true = from web (message only); false = from Shortcut (full receipt)
static unsigned long sensorPauseUntil = 0;  // Pause sensor sends during chunked print; resume after this time
static volatile unsigned long lastChunkTime = 0;     // Timeout: abort chunked print if no chunk for 15s
static volatile bool pendingChunkPrintReady = false; // All chunks received; do print from loop after ACK queue drains
static volatile unsigned long lastChunkPrintDone = 0; // When we last completed a chunked print (ignore duplicate chunks for 3s)
#define CHUNK_PRINT_COOLDOWN_MS 3000       // Ignore PRINT_CHUNK_START / PRINT_CHUNK(idx==0) within this window after print
static String lastPrintedChunkMessage;     // Dedupe: skip if TZT retries send same message (ESP-NOW retries)
static unsigned long lastPrintedChunkTime = 0;
#define CHUNK_DEDUPE_MS 45000              // Ignore duplicate message content within 45s (TZT resends on ESP-NOW fail)

// Deferred simple (non-chunked) print / printer test: CMD_PRINT and CMD_TEST_PRINTER arrive on the
// ESP-NOW receive callback (WiFi task). Printing + the weather HTTP fetch can block for seconds, so - like
// chunked printing already does - the actual work is deferred to loop() and only a flag/message is set here.
static char pendingSimplePrintBuf[PRINT_MSG_BYTES + 1];
static volatile bool pendingSimplePrintReady = false;
static volatile bool pendingSimpleTestPrintReady = false;

// Caller holds printStateMux
static void resetChunkState() {
    memset(printChunkReceived, 0, sizeof(printChunkReceived));
    printChunkTotal = 0;
    printChunkMessageOnly = false;
    lastChunkTime = 0;
}

// Bounded copy of CommandPacket::message: the 200-byte buffer isn't guaranteed to be null-terminated,
// so strlen() alone can read past the packet. Returns the copied length (out holds no terminator).
static size_t copyCommandMessage(char* out, const CommandPacket* cmd) {
    size_t len = strnlen(cmd->message, PRINT_MSG_BYTES);
    memcpy(out, cmd->message, len);
    return len;
}

// Cached for WEATHER_CACHE_MS: each fetch is a TLS handshake (1-3 s, ~40 KB heap) and weather barely changes
#define WEATHER_CACHE_MS (15UL * 60UL * 1000UL)
static String cachedWeatherLine;
static unsigned long cachedWeatherAt = 0;

static String wifiStatusName(int status) {
    switch (status) {
        case WL_IDLE_STATUS: return "IDLE";
        case WL_NO_SSID_AVAIL: return "NO_SSID_AVAIL";
        case WL_SCAN_COMPLETED: return "SCAN_COMPLETED";
        case WL_CONNECTED: return "CONNECTED";
        case WL_CONNECT_FAILED: return "CONNECT_FAILED";
        case WL_CONNECTION_LOST: return "CONNECTION_LOST";
        case WL_DISCONNECTED: return "DISCONNECTED";
        default: return "UNKNOWN(" + String(status) + ")";
    }
}

// Function declarations
void setupWiFi();
void setupTime();
void handleEspNowCommand(CommandPacket* cmd);
static bool isPreformattedPrint(const String& full);
static String getWeatherOneLine();

// Time configuration
const char* ntpServer = NTP_SERVER;

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
    // Serial2 pins (GPIO 17, 16) are configured by HardwareAbstraction::initializePrinter()
    delay(100);  // Ensure pins are stable
    
    Serial.begin(115200);
    // Short delay for Serial and power to stabilize
    delay(1500);
    
    // Set up logging - INFO so both Serial and the network log broadcast show normal operation, not just warnings/errors
    Logger::setLevel(LOG_LEVEL_INFO);
    Logger::info("Main", "========================================");
    Logger::info("Main", "ESP32 Print-n-Prick v" + String(FIRMWARE_VERSION));
    Logger::info("Main", "Build: " + String(BUILD_DATE) + " " + String(BUILD_TIME));
    Logger::info("Main", "========================================");
    Logger::info("Main", "Strapping pins set to safe state (all pins moved to non-strapping GPIOs)");

    // 200s so the WiFi config portal (up to 180s) doesn't trip the watchdog; loop() feeds every iteration.
    esp_task_wdt_init(200, true);
    esp_task_wdt_add(NULL);

    // Setup WiFi FIRST - so the "Print-n-Prick" config AP appears even if hardware
    // (printer, sensors, etc.) fails or is disconnected. lets you use WiFi Manager
    // to connect to your network before any peripheral init.
    esp_task_wdt_reset();  // Feed before blocking WiFi connect/portal
    setupWiFi();
    Serial.println("Print-n-Prick ready | MAC: " + WiFi.macAddress() + " | IP: " + deviceIP);
    delay(500);
    esp_task_wdt_reset();

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

    esp_task_wdt_reset();  // Feed before NTP sync (network call, can block)
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

    testServer = new TestServer(hardware, printerService, &autoDispenseEnabled, espNowService);
    testServer->begin();

    Logger::info("Main", "Setup complete!");
    Logger::info("Main", "Starting main loop...");
}

void loop() {
    esp_task_wdt_reset();

    testServer->handleClient();
    testServer->update();

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
        if (espNowTestType == 1 && elapsed >= espNowTestDurationMs) {
            hardware->stopPump();
            espNowTestType = 0;
        } else if (espNowTestType == 2 && elapsed >= espNowTestDurationMs) {
            hardware->setLEDBrightness(0);
            hardware->reapplyLEDSchedule();  // restore scheduled state after the test pulse
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
            Logger::info("Main", "WiFi Status: " + wifiStatusName(wifiStatus) + " | LED: " + String(wifiConnected ? "ON" : "OFF"));
            lastWiFiStatus = wifiStatus;
        }
#if (BOARD_LED_PIN >= 0)
        // Onboard blue LED: on when WiFi connected. BOARD_LED_ACTIVE_LOW: 1=LOW=on, 0=HIGH=on
        digitalWrite(BOARD_LED_PIN, wifiConnected
            ? (BOARD_LED_ACTIVE_LOW ? LOW : HIGH) : (BOARD_LED_ACTIVE_LOW ? HIGH : LOW));
#endif
        lastLEDUpdate = millis();
    }
    
    // LED night schedule: check the clock once a second (switches only at the on/off times)
    static unsigned long lastScheduleCheck = 0;
    if (millis() - lastScheduleCheck >= LED_SCHEDULE_CHECK_INTERVAL_MS) {
        hardware->updateLEDSchedule();
        lastScheduleCheck = millis();
    }
    
    // Slow path: sensors for status/ESP-NOW (moisture, IR cache). HTTP/display don't need fast updates.
    static unsigned long lastSensorCheck = 0;
    if (millis() - lastSensorCheck > SENSOR_CHECK_INTERVAL) {
        hardware->readMoistureSensor();
        hardware->readIRSensor();      // Cache for ESP-NOW; IR->pump trigger uses digitalRead every loop
        lastSensorCheck = millis();
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
        bool chunkTimedOut = false;
        if (printChunkTotal > 0 && !pendingChunkPrintReady) {
            portENTER_CRITICAL(&printStateMux);
            if (printChunkTotal > 0 && !pendingChunkPrintReady && lastChunkTime > 0 && (millis() - lastChunkTime) > 15000) {
                resetChunkState();
                chunkTimedOut = true;
            }
            portEXIT_CRITICAL(&printStateMux);
        }
        if (chunkTimedOut) {
            sensorPauseUntil = millis() + 500;
        }
        // Deferred print: all chunks received; drain ACK queue from loop then print (never block in recv callback)
        else if (pendingChunkPrintReady && espNowService && !espNowService->hasPendingChunkAck()) {
            // Callback doesn't touch the buffers while pendingChunkPrintReady is set, so no lock to read them
            uint8_t nChunks = printChunkTotal;
            bool messageOnly = printChunkMessageOnly;
            String full;
            size_t totalLen = 0;
            for (uint8_t i = 0; i < nChunks; i++) totalLen += printChunkLen[i];
            full.reserve(totalLen);
            for (uint8_t i = 0; i < nChunks; i++) full.concat(printChunkData[i], printChunkLen[i]);
            portENTER_CRITICAL(&printStateMux);
            resetChunkState();
            lastChunkPrintDone = millis();  // Cooldown: ignore duplicate chunks for CHUNK_PRINT_COOLDOWN_MS
            pendingChunkPrintReady = false;
            portEXIT_CRITICAL(&printStateMux);
            sensorPauseUntil = millis() + 500;
            // Dedupe: TZT retries on ESP-NOW fail can arrive late; skip same message within 45s
            bool isDuplicate = (full.length() > 0 && full == lastPrintedChunkMessage
                               && (millis() - lastPrintedChunkTime) < CHUNK_DEDUPE_MS);
            if (isDuplicate) {
                // Reset state, don't print again
            } else if (full.length() > 0 && hardware && printerService) {
                lastPrintedChunkMessage = full;
                lastPrintedChunkTime = millis();
                if (messageOnly) {
                    printerService->printMessageOnly(full);
                } else if (isPreformattedPrint(full)) {
                    printerService->printPreformatted(full, getWeatherOneLine());
                } else {
                    printerService->setWeather(getWeatherOneLine());
                    printerService->printReceipt(full, true, time(nullptr));
                }
                Serial.println("✅ Printed (" + String(nChunks) + " chunks)");
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
        // Sensor packet: every ESP_NOW_SEND_INTERVAL as a heartbeat, or as soon as the display would show
        // something different (LED switched, pump started...), at most every ESP_NOW_MIN_SEND_GAP_MS.
        // Wait 3 s after ESP-NOW init; skip during a chunked print (paused until print done + 500 ms),
        // while CHUNK_ACKs are queued (they drain first), and while a send is in flight (avoids NO_MEM).
        else {
            espNowService->updateSensorData(  // cached values only - cheap enough for every pass
                hardware->getMoisturePercent(),
                hardware->getSanitizerLevel(),
                hardware->isIRDetected(),
                hardware->getLedOnMinute(),
                hardware->getLedOffMinute(),
                hardware->getLEDBrightness(),
                hardware->getLedLevel(),
                hardware->isDispensing(),
                hardware->isPumpRunning(),
                autoDispenseEnabled,
                hardware->isLedScheduleEnabled()
            );
            unsigned long sinceSend = millis() - lastEspNowSend;
            bool due = sinceSend >= ESP_NOW_SEND_INTERVAL ||
                       (espNowService->hasUnsentChange() && sinceSend >= ESP_NOW_MIN_SEND_GAP_MS);
            if (due && (millis() - espNowInitTime) >= 3000 && !espNowService->hasSendInFlight()
                && !espNowService->hasPendingChunkAck()
                && printChunkTotal == 0 && !pendingChunkPrintReady && (sensorPauseUntil == 0 || millis() >= sensorPauseUntil)) {
                espNowService->sendSensorData();
                lastEspNowSend = millis();
            }
        }
    }
    
    // Deferred simple print / printer test (see CMD_PRINT / CMD_TEST_PRINTER in handleEspNowCommand):
    // runs here in loop() instead of the ESP-NOW recv callback, which must never block for seconds.
    if (pendingSimplePrintReady && hardware && printerService) {
        char msg[sizeof(pendingSimplePrintBuf)];
        portENTER_CRITICAL(&printStateMux);
        memcpy(msg, pendingSimplePrintBuf, sizeof(msg));
        pendingSimplePrintReady = false;
        portEXIT_CRITICAL(&printStateMux);
        String full(msg);
        if (full.length() > 0) {
            if (isPreformattedPrint(full)) {
                printerService->printPreformatted(full, getWeatherOneLine());
            } else {
                // Raw message (e.g. from index.html or iOS Shortcut): full receipt with MESSAGE title, date, weather
                printerService->setWeather(getWeatherOneLine());
                printerService->printReceipt(full, true, time(nullptr));
            }
            Logger::info("Main", "Executed: PRINT");
        }
    }
    if (pendingSimpleTestPrintReady) {
        pendingSimpleTestPrintReady = false;
        if (printerService) {
            printerService->printTest();
            Logger::info("Main", "Executed: TEST_PRINTER");
        } else {
            Logger::error("Main", "TEST_PRINTER requested but printerService is null");
        }
    }

    delay(1);  // Short delay for sensor->actuator responsiveness (IR->pump). HTTP latency not a priority.
}

void setupWiFi() {
    Logger::info("WiFi", "Setting up WiFi...");
    
    Logger::info("WiFi", "WiFi initialization complete");
    
    // Check initial WiFi status
    int initialStatus = WiFi.status();
    Logger::info("WiFi", "Initial WiFi status: " + String(initialStatus));
    
    WiFiManager wm;
    wm.setDebugOutput(false);  // Suppress *wm: verbose logs
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
        Logger::error("WiFi", "WiFi status: " + wifiStatusName(finalStatus));
    } else {
        deviceIP = WiFi.localIP().toString();
        Logger::info("WiFi", "✅ Connected successfully!");
        Logger::info("WiFi", "IP Address: " + deviceIP);
        Logger::info("WiFi", "RSSI: " + String(WiFi.RSSI()) + " dBm");
        Logger::info("WiFi", "SSID: " + String(WiFi.SSID()));
        Logger::info("WiFi", "MAC Address: " + String(WiFi.macAddress()));
        Logger::enableNetworkLog("Print-n-Prick");
        Logger::info("Logger", "Network log broadcast enabled on UDP port 47269");
    }
}

void setupTime() {
    configTzTime(TIMEZONE_POSIX, ntpServer);  // POSIX TZ handles CST/CDT switch automatically
    Logger::info("Time", "NTP time server configured (" TIMEZONE_POSIX ")");
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
static String fetchWeatherOneLine();
static String getWeatherOneLine() {
    if (cachedWeatherLine.length() > 0 && millis() - cachedWeatherAt < WEATHER_CACHE_MS)
        return cachedWeatherLine;
    String line = fetchWeatherOneLine();
    if (line.length() > 0) {  // failures aren't cached, so the next print tries again
        cachedWeatherLine = line;
        cachedWeatherAt = millis();
    }
    return line;
}

static String fetchWeatherOneLine() {
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
    return (type == CMD_SET_LED_BRIGHTNESS || type == CMD_SET_LED_STATE || type == CMD_SET_LED_SCHEDULE
         || type == CMD_SET_PUMP_DURATION || type == CMD_SET_PUMP_COOLDOWN || type == CMD_SET_AUTO_DISPENSE
         || type == CMD_SET_LED_SCHEDULE_TIME);
}

void handleEspNowCommand(CommandPacket* cmd) {
    if (!cmd) { Logger::error("ESP-NOW", "handleEspNowCommand: null command packet"); return; }
    Logger::info("ESP-NOW", "Received command: " + String(espNowCommandName(cmd->commandType)));
    if (!hardware || !printerService) {
        Logger::error("ESP-NOW", "Dropped command " + String(espNowCommandName(cmd->commandType)) +
                       " - hardware or printerService not initialized");
        return;
    }
    // Skip duplicate setting command within 1s (TZT sends 4x for reliability)
    if (isSettingCommandType(cmd->commandType)) {
        uint32_t key = (uint32_t)(cmd->commandType << 16) | (cmd->param1 << 8) | cmd->param2;
        if (lastSettingKey == key && (millis() - lastSettingTime) < SETTING_DEDUPE_MS)
            return;  // Duplicate, already executed
    }
    switch (cmd->commandType) {
        case CMD_PRINT: {
            if (!cmd->message[0]) break;
            // Defer actual printing (+ weather HTTP fetch) to loop() - never block the ESP-NOW recv callback.
            portENTER_CRITICAL(&printStateMux);
            size_t len = copyCommandMessage(pendingSimplePrintBuf, cmd);
            pendingSimplePrintBuf[len] = '\0';
            pendingSimplePrintReady = true;
            portEXIT_CRITICAL(&printStateMux);
            break;
        }
        case CMD_PRINT_CHUNK_START: {
            if (millis() - lastChunkPrintDone < CHUNK_PRINT_COOLDOWN_MS) break; // Ignore duplicate session (TZT resend)
            uint8_t total = cmd->param2;
            if (total == 0 || total > PRINT_CHUNK_MAX) break;
            portENTER_CRITICAL(&printStateMux);
            if (!pendingChunkPrintReady) {  // previous message still waiting to print: leave it alone
                resetChunkState();
                printChunkMessageOnly = (cmd->param1 != 0);  // param1: 0 = full receipt, 1 = message only (from web)
                printChunkTotal = total;
                lastChunkTime = millis();
            }
            portEXIT_CRITICAL(&printStateMux);
            break;
        }
        case CMD_PRINT_CHUNK: {
            uint8_t idx = cmd->param1;
            uint8_t total = cmd->param2;
            if (total == 0 || total > PRINT_CHUNK_MAX) break;
            if (idx >= total) break;
            if (idx == 0 && (millis() - lastChunkPrintDone < CHUNK_PRINT_COOLDOWN_MS)) break; // Ignore duplicate chunk 0 (TZT resend)
            portENTER_CRITICAL(&printStateMux);
            if (!pendingChunkPrintReady) {
                if (idx == 0) {
                    bool messageOnly = printChunkMessageOnly;  // set by PRINT_CHUNK_START
                    resetChunkState();
                    printChunkMessageOnly = messageOnly;
                    printChunkTotal = total;
                }
                if (idx < printChunkTotal) {
                    lastChunkTime = millis();
                    printChunkLen[idx] = (uint8_t)copyCommandMessage(printChunkData[idx], cmd);
                    printChunkReceived[idx] = true;
                    bool haveAll = true;
                    for (uint8_t i = 0; i < printChunkTotal; i++) {
                        if (!printChunkReceived[i]) { haveAll = false; break; }
                    }
                    // Defer print to main loop so we drain CHUNK_ACK queue there (no blocking/send in recv callback)
                    if (haveAll) pendingChunkPrintReady = true;
                }
            }
            portEXIT_CRITICAL(&printStateMux);
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
            // param1 = level 1-100%: saved, and applied right away if the LED is on
            if (cmd->param1 >= 1 && cmd->param1 <= 100 && hardware->getLedLevel() != cmd->param1) {
                hardware->setLedLevel(cmd->param1);
                Serial.println("Executed: SET_LED_BRIGHTNESS=" + String(cmd->param1) + "%");
            }
            break;
        case CMD_SET_LED_STATE: {
            // Manual on/off holds until the next scheduled switch
            bool wantOn = (cmd->param1 != 0);
            bool currentlyOn = (hardware->getLEDBrightness() > 0);
            if (wantOn != currentlyOn) {
                hardware->setLEDBrightness(wantOn ? hardware->getLedOnPwm() : 0);
                Serial.println("Executed: SET_LED_STATE=" + String(wantOn ? 1 : 0));
            }
            break;
        }
        case CMD_SET_LED_SCHEDULE: {
            bool wantOn = (cmd->param1 != 0);
            if (hardware->isLedScheduleEnabled() != wantOn) {
                hardware->setLedScheduleEnabled(wantOn);
                Serial.println("Executed: SET_LED_SCHEDULE=" + String(wantOn ? 1 : 0));
            }
            break;
        }
        case CMD_TEST_PRINTER:
            // Defer to loop() - printTest() blocks with many delay() calls; never block the ESP-NOW recv callback.
            pendingSimpleTestPrintReady = true;
            break;
        case CMD_TEST_PUMP:
            if (testServer) testServer->cancelActiveTest();
            espNowTestType = 1;
            espNowTestStartMs = millis();
            espNowTestDurationMs = TEST_PUMP_DURATION_MS;
            hardware->startPump();
            Serial.println("✅ Executed: TEST_PUMP (pump on for " + String(TEST_PUMP_DURATION_MS) + "ms, non-blocking)");
            break;
        case CMD_TEST_LED:
            if (testServer) testServer->cancelActiveTest();
            espNowTestType = 2;
            espNowTestStartMs = millis();
            espNowTestDurationMs = TEST_LED_DURATION_MS;
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
        case CMD_SET_LED_SCHEDULE_TIME: {
            // param1: 0 = on-time, 1 = off-time; param2: quarter-hour of day (0-95)
            if (cmd->param2 > 95) break;
            uint16_t minute = (uint16_t)cmd->param2 * 15;
            uint16_t on = cmd->param1 == 0 ? minute : hardware->getLedOnMinute();
            uint16_t off = cmd->param1 == 0 ? hardware->getLedOffMinute() : minute;
            if (hardware->setLedScheduleTimes(on, off))
                Serial.println("Executed: SET_LED_SCHEDULE_TIME " + String(cmd->param1 == 0 ? "on=" : "off=") +
                               HardwareAbstraction::formatMinute(minute));
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

