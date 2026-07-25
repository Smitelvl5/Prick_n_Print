/*
 * TZT ESP32 LVGL Display - Web Server + HTTP Backend + ESP-NOW
 * Hosts web UI, talks to HTTP backend (commands, reminders, groceries), sends commands to Main via ESP-NOW.
 * Receives sensor data from Main via ESP-NOW.
 */

#include <Arduino.h>
#include <WiFi.h>
#include <WiFiManager.h>
#include <WebServer.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <time.h>
#if !defined(TZT_HEADLESS)
#include <lvgl.h>
#include <TFT_eSPI.h>
#include <JPEGDecoder.h>
#endif

#include "version.h"
#include "config_tzt.h"
#include "secrets.h"
#include "EspNowProtocol.h"
#include "IBackendService.h"
#include "HttpBackendService.h"
#include "ReminderService.h"
#include "SDCardService.h"
#include <SD.h>
#include "AudioService.h"
#include "Logger.h"
#include "OTAUpdateService.h"
#include <esp_now.h>
#include <esp_wifi.h>
#include <esp_task_wdt.h>
#include <esp_heap_caps.h>
#include <string.h>
#include <cmath>

WebServer server(8080);

// HTTP response constants (reduces string allocations)
static const char* const JSON_SUCCESS = "{\"success\":true}";
static const char* const JSON_FAIL_UNAUTHORIZED = "{\"error\":\"Unauthorized\"}";

#define DEBUG_ESPNOW 0  // Set to 1 for ESP-NOW send/receive debugging
#if DEBUG_ESPNOW
    #define ESPNOW_LOG(msg) Serial.println(msg)
#else
    #define ESPNOW_LOG(msg) do {} while(0)
#endif

#define DEBUG_LOOP 0  // 1 = verbose loop/panel/gesture logs; 0 = quiet (errors and key events only)

// Cached sensor data from Main (received via ESP-NOW) - declared early for calculateSensorHash()
SensorDataPacket lastSensorData;
bool lastSensorDataValid = false;
unsigned long lastSensorDataReceiveTime = 0;  // Track when we last received data from Main
volatile bool sensorDataNeedsPanelUpdate = false;  // Set in recv callback; loop() refreshes GUI

#if !defined(TZT_HEADLESS)
// Display and control panel (LVGL) - 3 screens with left/right nav
static TFT_eSPI tft;
static volatile bool displayFlushing = false;  // true while flush callback is writing to TFT (skip touch read to avoid SPI conflict)
// Display buffer: 320*20 fits in ESP32 DRAM; larger = smoother but overflows (320*30/40 overflow)
#define TZT_DISP_BUF_PIXELS  (TZT_SCREEN_WIDTH * 20)
static lv_disp_draw_buf_t draw_buf;
static lv_disp_drv_t disp_drv;
static lv_color_t disp_buf1[TZT_DISP_BUF_PIXELS];
static lv_color_t disp_buf2[TZT_DISP_BUF_PIXELS];
static lv_obj_t* screen1 = nullptr;  // Home: latest message + quick status + History button
static lv_obj_t* screen2 = nullptr;  // Sensors (read-only)
static lv_obj_t* screen3 = nullptr;  // Message history (SD card) - reached via button on Home
static lv_obj_t* audioListScreen = nullptr;   // sub-screen: audio list + now playing
static lv_obj_t* imageListScreen = nullptr;   // sub-screen: image gallery list
static int currentScreenIndex = 0;
#define SCREEN_COUNT 4   // Bottom nav tabs: Home, Sensors, Media (Photo/Audio toggle), Settings
static int mediaSubTab = 0;   // Media tab sub-selection: 0 = Photo, 1 = Audio
#define DASHBOARD_AUTO_ADVANCE_MS  5000   // rotate to next tab every 5s when not touched
#define DASHBOARD_TOUCH_PAUSE_MS  120000  // after touch, stay on tab for 2 min then resume auto-rotate
static unsigned long lastDashboardTouchTime = 0;   // touch or tab tap pauses auto-rotate
static unsigned long lastDashboardAutoAdvance = 0;
static lv_obj_t* dashboardPhotoScreen = nullptr;  // Media tab, Photo sub-tab
static lv_obj_t* dashboardAudioScreen = nullptr;  // Media tab, Audio sub-tab
static void switchMediaSubTab(int which);  // 0 = Photo, 1 = Audio
static String lastDashboardImagePath;   // latest received/shown image for Photo tab
static String lastDashboardAudioName;   // latest received/played audio for Audio tab
static lv_obj_t* dashboardAudioNameLabel = nullptr;  // label on Audio tab, updated when lastDashboardAudioName changes
// Screen 1: message (dashboard tab 1)
static lv_obj_t* labelMessage = nullptr;
static String lastPrintMessage = "(No messages yet)";
// Screen 3: message history
static lv_obj_t* historyList = nullptr;
static lv_obj_t* historyDetail = nullptr;
static lv_obj_t* historyDetailLabel = nullptr;
static void refreshHistoryList();
static void historyItemCb(lv_event_t* e);
static void historyBackCb(lv_event_t* e);
static void historyOpenCb(lv_event_t* e);   // Home -> History
static void historyCloseCb(lv_event_t* e);  // History -> Home
// Screen 4: audio player
static lv_obj_t* audioList = nullptr;
static lv_obj_t* audioHeaderBar = nullptr;
static lv_obj_t* audioStopBtn = nullptr;
static void updateAudioHeaderState();
#define MAX_AUDIO_FILES 20
static String audioFileNames[MAX_AUDIO_FILES];
static int audioFileCount = 0;
static void refreshAudioList();
static void audioFileCb(lv_event_t* e);
static void audioStopCb(lv_event_t* e);
// Screen 5: image gallery
#define MAX_IMAGE_FILES 30
static String imageFileNames[MAX_IMAGE_FILES];
static int imageFileCount = 0;
static lv_obj_t* imageList = nullptr;
static lv_obj_t* imageViewerScreen = nullptr;  // overlay with Back button when viewing
static void refreshImageList();
static void imageFileCb(lv_event_t* e);
static void imageBackCb(lv_event_t* e);
static void drawSdJpegToTft(const char* path, int maxY = 240);
static String currentImagePath;  // set when image viewer is open so we redraw JPEG each frame
// Screen 2: sensor labels + progress bars
static lv_obj_t* labelMoisture = nullptr;
static lv_obj_t* labelSanitizer = nullptr;
static lv_obj_t* labelLED = nullptr;
static lv_obj_t* labelPump = nullptr;
static lv_obj_t* barMoisture = nullptr;
static lv_obj_t* barSanitizer = nullptr;
static lv_obj_t* barLED = nullptr;
static unsigned long lastPanelUpdate = 0;
static uint32_t lastSensorHash = 0;
static void createControlPanel();
static void updateControlPanelStatus();
// Status bar (persistent top layer: title + IP + time; WiFi SSID shown on Settings tab)
static lv_obj_t* statusBar = nullptr;
static lv_obj_t* statusTimeLabel = nullptr;
static lv_obj_t* statusIpLabel = nullptr;
static lv_obj_t* statusWifiLabel = nullptr;  // created on Settings screen, updated by updateStatusBar()
static void updateStatusBar();
// Home screen sensor summary strip
static lv_obj_t* homeSensorStrip = nullptr;
// Volume: inline on Settings tab (Settings is a normal bottom-nav tab, not an overlay)
static lv_obj_t* volumeSlider = nullptr;
static lv_obj_t* settingsVolumeValueLabel = nullptr;
static lv_obj_t* settingsScreen = nullptr;
static void volumeSliderCb(lv_event_t* e);
static void gesture_timer_cb(lv_timer_t* t);

// Layout: status bar, content, bottom nav
#define TZT_STATUS_H   18
#define TZT_NAV_BAR_H  48
#define TZT_CONTENT_TOP    TZT_STATUS_H
#define TZT_CONTENT_H      (TZT_SCREEN_HEIGHT - TZT_STATUS_H - TZT_NAV_BAR_H)
// Finger-friendly: minimum touch targets and consistent spacing (48px min for tap targets)
#define TZT_MIN_TOUCH_H    48
#define TZT_LIST_ITEM_H    48
#define TZT_HEADER_BAR_H   48
#define TZT_BTN_PAD_V      6
#define TZT_LIST_PAD       8
#define TZT_NAV_BTN_GAP    4   // Real gap between bottom-nav tabs so adjacent taps don't misfire
static lv_obj_t* navBarContainer = nullptr;
static lv_obj_t* navBtns[SCREEN_COUNT] = { nullptr };
static void updateNavHighlight(int index);
static void navBtnCb(lv_event_t* e);
static void goToScreenIndex(int index);
static void showNavBar();
static void hideNavBar();
// Splash: show splash.jpg at startup until UI is ready (defer createControlPanel to first loop).
// Copy scripts/splash.jpg to the root of the SD card as /splash.jpg (optional; black screen if missing).
#define SPLASH_PATH       "/splash.jpg"
#define SPLASH_MIN_MS     10000  // Minimum time splash is visible (10 seconds)
static bool splashVisible = false;
static unsigned long splashReadyAt = 0;
static void drawSplashToTft();    // Draw splash to TFT before LVGL (raw); no-op if file missing
static uint32_t calculateSensorHash() {
    if (!lastSensorDataValid) return 0;
    return ((uint32_t)((int)(lastSensorData.moisturePercent * 10)) << 24) |
           ((uint32_t)((int)(lastSensorData.sanitizerLevel * 10)) << 16) |
           ((uint32_t)lastSensorData.ledBrightness << 8) |
           (lastSensorData.isDispensing ? 1 : 0) |
           (lastSensorData.autoDispense ? 2 : 0) |
           (lastSensorData.autoBrightness ? 4 : 0);
}
#endif  // TZT_HEADLESS

// ESP-NOW
bool espNowInitialized = false;
uint8_t mainESP32Mac[6] = MAIN_ESP32_MAC_ADDRESS;

// Optimistic cache for settings not in sensor packet (for HTTP GET /api/status)
static int lastPumpDurationTenths = -1;   // 0-255 = 0-25.5s, -1 = not set
static int lastPumpCooldownTenths = -1;    // 0-255 = 0-25.5s, -1 = not set

// Settings persisted to backend config (loaded on boot, saved when changed)
bool settingsNeedSave = false;
static unsigned long lastSettingsSaveTime = 0;
#define BOOT_SETTINGS_MAX 5
struct BootSetting { uint8_t type; uint8_t p1; uint8_t p2; };
static BootSetting bootSettingsToSend[BOOT_SETTINGS_MAX];
static uint8_t bootSettingsCount = 0;
static uint8_t bootSettingsIndex = 0;
static void applyNextBootSetting();  // Enqueue next boot setting to Main (called after ACK)

// Retry when TZT→Main command send fails (collision with Main's transmit window)
static uint8_t lastSentCommandType = 0;
static uint8_t lastSentParam1 = 0, lastSentParam2 = 0;
static char lastSentMessage[201] = {0};
static bool retryPending = false;
static unsigned long retryAt = 0;
static uint8_t retryCount = 0;  // Only retry once per command
static uint8_t commandSequence = 0;   // Rolling sequence for commands
static uint8_t lastSentSequence = 0;  // So we can match ACK and cancel retry
static uint8_t consecutiveFailures = 0;  // Only run peer verification when send failures occur
static unsigned long lastPeerCheck = 0;

// Chunked print handshake: send chunk N only after Main ACKs chunk N-1
static String pendingChunkMessage;
static uint8_t pendingChunkIndex = 0;
static uint8_t pendingChunkTotal = 0;  // 0 = no chunk send in progress
static unsigned long pendingChunkSendTime = 0;  // Timeout: abort if no CHUNK_ACK for 15s
static unsigned long nextChunkSendAt = 0;      // When to send next chunk from loop (set by CHUNK_ACK; don't send from callback)
static unsigned long chunkResendAt = 0;         // When to resend current chunk after send failed (set by onDataSent FAIL)
static uint8_t chunkResendCount = 0;            // Cap retries to avoid infinite resend loop (Main dedupes late arrivals)
#define PRINT_CHUNK_SIZE 199  // For CHUNK_ACK handler and sendPrintChunked
#define CHUNK_ACK_TIMEOUT_MS 15000
#define CHUNK_RESEND_INTERVAL_MS 2000  // Resend current chunk every 2s until ACK or timeout
#define CHUNK_MAX_RESENDS 8                     // Give up after 8 resends (~16s); Main will dedupe if late copy arrives
bool sendCommandViaESPNow(uint8_t commandType, uint8_t param1 = 0, uint8_t param2 = 0, const char* message = "", bool isRetry = false, bool fromSettingsQueue = false);

// Reject filenames from untrusted sources (backend JSON, HTTP request bodies) that could escape the
// intended SD directory via path traversal (e.g. "../data/config.json") - basenames only, no separators.
static bool isSafeFilename(const String& name) {
    if (name.length() == 0 || name.length() > 128) return false;
    if (name.indexOf("..") >= 0) return false;
    if (name.indexOf('/') >= 0 || name.indexOf('\\') >= 0) return false;
    return true;
}

// Reliable settings: handshake like printer (send → wait for ACK → done; resend on timeout)
static uint8_t pendingSettingCommandType = 0;
static uint8_t pendingSettingParam1 = 0, pendingSettingParam2 = 0;
static char pendingSettingMessage[201] = {0};
static uint8_t pendingSettingRemainingSends = 0;  // 0 = none; else remaining attempts until ACK
static unsigned long pendingSettingNextSendAt = 0;
#define SETTINGS_MAX_ATTEMPTS 8
#define SETTINGS_ACK_TIMEOUT_MS 400   // Resend if no ACK within 400ms (like printer chunk timeout)
void enqueueSettingCommand(uint8_t commandType, uint8_t param1, uint8_t param2, const char* message = "");
static bool sendingFromSettingsQueue = false;  // So onDataSentStatic doesn't set retryPending for queue sends

// OTA updates
OTAUpdateService* otaService = nullptr;

// Backend (HTTP server) + Reminders
IBackendService* backend = nullptr;
ReminderService* reminderService = nullptr;
bool remindersNeedSave = false;  // Flag to save reminders in background

// SD Card
SDCardService sdCard;

// Test endpoints: show image on display from HTTP (path set in handler, drawn in loop)
static String pendingTestImagePath;

// Audio
AudioService audioSvc;

// Groceries (backend)
#define MAX_GROCERY_ITEMS 50
String groceryItems[MAX_GROCERY_ITEMS];
int groceryCount = 0;
bool groceriesNeedSave = false;  // Flag to save groceries in background
static unsigned long lastGrocerySaveTime = 0;
static const unsigned long SAVE_DEBOUNCE_MS = 5000;  // Max one save per 5s

// Todo list (backend)
#define MAX_TODO_ITEMS 50
String todoItems[MAX_TODO_ITEMS];
int todoCount = 0;
bool todosNeedSave = false;  // Flag to save todos in background

String deviceIP = "";
String webPassword = WEB_PASSWORD;
String authToken = "";
unsigned long authTokenExpiry = 0;
const unsigned long AUTH_TOKEN_DURATION = 3600000;  // 1 hour

void onDataSentStatic(const uint8_t* mac_addr, esp_now_send_status_t status) {
    static unsigned long lastFailLog = 0;
    
    if (status == ESP_NOW_SEND_SUCCESS) {
        consecutiveFailures = 0;
    } else {
        consecutiveFailures++;
        // Chunked print: resend current chunk from loop (don't use generic retry to avoid duplicates)
        if (pendingChunkTotal > 0 && (lastSentCommandType == CMD_PRINT_CHUNK || lastSentCommandType == CMD_PRINT_CHUNK_START)) {
            chunkResendAt = millis() + 900;  // Longer backoff to avoid collision with Main's RX
            if (millis() - lastFailLog > 20000) {
                Serial.println("[ESP-NOW] send failed, resending chunk");
                lastFailLog = millis();
            }
        } else if (pendingSettingRemainingSends == 0) {
            // Don't set retryPending when a setting is in flight — settings queue resends on ACK timeout
            bool isPumpCmd = (lastSentCommandType == CMD_DISPENSE_START || lastSentCommandType == CMD_DISPENSE_STOP);
            int maxRetries = isPumpCmd ? 2 : 1;  // Pump: up to 2 retries (3 attempts total)
            if (retryCount < maxRetries) {
                retryPending = true;
                retryAt = millis() + 800;  // Backoff before retry
                retryCount++;
                if (millis() - lastFailLog > 20000) {
                    Serial.println("[ESP-NOW] send failed, retrying");
                    lastFailLog = millis();
                }
            }
        }
        if (millis() - lastFailLog > 15000) {
            lastFailLog = millis();
            Serial.println("[ESP-NOW] send failed status=" + String((int)status));
            if (!esp_now_is_peer_exist(mac_addr)) {
                Serial.println("   Re-adding peer...");
                // Try to re-add peer immediately
                esp_now_peer_info_t peerInfo;
                memset(&peerInfo, 0, sizeof(peerInfo));
                memcpy(peerInfo.peer_addr, mac_addr, 6);
                peerInfo.ifidx = WIFI_IF_STA;
                uint8_t wifiChannel = (WiFi.status() == WL_CONNECTED && WiFi.channel() > 0) ? WiFi.channel() : (uint8_t)ESP_NOW_CHANNEL;
                peerInfo.channel = wifiChannel;
                peerInfo.encrypt = false;
                
                esp_err_t addResult = esp_now_add_peer(&peerInfo);
                if (addResult != ESP_OK) Serial.println("   [ESP-NOW] Re-add peer failed: " + String((int)addResult));
            } else {
                esp_now_peer_info_t peerInfo;
                if (esp_now_get_peer(mac_addr, &peerInfo) == ESP_OK) {
                    uint8_t currentChannel = (WiFi.status() == WL_CONNECTED && WiFi.channel() > 0) ? WiFi.channel() : (uint8_t)ESP_NOW_CHANNEL;
                    if (peerInfo.channel != currentChannel || peerInfo.ifidx != WIFI_IF_STA) {
                        esp_now_del_peer(mac_addr);
                        delay(50);
                        peerInfo.channel = currentChannel;
                        peerInfo.ifidx = WIFI_IF_STA;
                        esp_now_add_peer(&peerInfo);
                    }
                }
            }
        }
    }
}

// ESP-NOW receive: sensor data from Main
// Preserve autoDispense/autoBrightness so HTTP toggles don't get overwritten by Main's packet (avoids UI bounce)
void onDataRecvStatic(const uint8_t* mac_addr, const uint8_t* data, int len) {
    if (len >= (int)sizeof(SensorDataPacket) && data[0] == ESP_NOW_MSG_SENSOR_DATA) {
        if (verifyChecksum((uint8_t*)data, sizeof(SensorDataPacket))) {
            bool hadValid = lastSensorDataValid;
            bool prevAutoDispense = lastSensorData.autoDispense;
            bool prevAutoBrightness = lastSensorData.autoBrightness;
            uint8_t prevLedBrightness = lastSensorData.ledBrightness;
            memcpy(&lastSensorData, data, sizeof(SensorDataPacket));
            if (hadValid) {
                lastSensorData.autoDispense = prevAutoDispense;
                lastSensorData.autoBrightness = prevAutoBrightness;
                lastSensorData.ledBrightness = prevLedBrightness;  // Optimistic: keep our sent value
            }
            lastSensorDataValid = true;
            lastSensorDataReceiveTime = millis();
            sensorDataNeedsPanelUpdate = true;  // Refresh GUI on next loop (don't call LVGL from callback)
        } else {
            Serial.println("⚠️ ESP-NOW: Invalid checksum in sensor data");
        }
    } else if (len >= (int)sizeof(AckPacket) && data[0] == ESP_NOW_MSG_ACK) {
        AckPacket* ack = (AckPacket*)data;
        if (verifyChecksum((uint8_t*)ack, sizeof(AckPacket)) && ack->ackForMsgType == ESP_NOW_MSG_COMMAND) {
            if (ack->sequenceNumber == lastSentSequence) {
                retryPending = false;
                // Setting handshake: Main got it, stop resending
                if (pendingSettingRemainingSends > 0) {
                    pendingSettingRemainingSends = 0;
                    // If we're applying boot settings from backend, enqueue next
                    applyNextBootSetting();
                }
            }
        }
    } else if (len >= (int)sizeof(ChunkAckPacket) && data[0] == ESP_NOW_MSG_CHUNK_ACK) {
        ChunkAckPacket* cap = (ChunkAckPacket*)data;
        if (verifyChecksum((uint8_t*)cap, sizeof(ChunkAckPacket)) && pendingChunkTotal > 0 && cap->chunkIndex == pendingChunkIndex) {
            chunkResendCount = 0;  // Reset on success
            pendingChunkIndex++;
            if (pendingChunkIndex < pendingChunkTotal) {
                nextChunkSendAt = millis() + 120;  // Give Main time to process before next chunk
                pendingChunkSendTime = millis();
            } else {
                pendingChunkTotal = 0;
                nextChunkSendAt = 0;
                chunkResendAt = 0;
            }
        }
    } else if (len > 0) {
        Serial.println("⚠️ ESP-NOW: Received unexpected packet type: " + String(data[0]));
    }
}

// Human-readable name for command type (for serial logs)
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

// Send a long print message in chunks; handshake: send 1 → recv ACK 1 → send 2 → recv ACK 2 → ...
// messageOnly: true = just the text (e.g. from index.html); false = full receipt with title, date, weather (e.g. from Shortcut)
#define PRINT_CHUNK_DELAY_MS 300
bool sendPrintChunked(const String& fullMessage, bool messageOnly = false) {
    if (!espNowInitialized) return false;
    unsigned int len = fullMessage.length();
    if (len == 0) return true;
    unsigned int totalChunks = (len + PRINT_CHUNK_SIZE - 1) / PRINT_CHUNK_SIZE;
    if (totalChunks > 128) totalChunks = 128;
    pendingChunkMessage = fullMessage;
    pendingChunkIndex = 0;
    pendingChunkTotal = (uint8_t)totalChunks;
    chunkResendCount = 0;
    // param1: 0 = full receipt, 1 = message only (no title/date/weather)
    if (!sendCommandViaESPNow(CMD_PRINT_CHUNK_START, messageOnly ? 1 : 0, (uint8_t)totalChunks, "")) return false;
    delay(150);
    pendingChunkSendTime = millis();
    String chunk0 = fullMessage.substring(0, PRINT_CHUNK_SIZE);
    return sendCommandViaESPNow(CMD_PRINT_CHUNK, 0, (uint8_t)totalChunks, chunk0.c_str());
}

// Enqueue a setting command to be sent 4x with 200ms spacing (only when printer idle). Latest overwrites.
void enqueueSettingCommand(uint8_t commandType, uint8_t param1, uint8_t param2, const char* message) {
    if (!espNowInitialized) return;
    pendingSettingCommandType = commandType;
    pendingSettingParam1 = param1;
    pendingSettingParam2 = param2;
    pendingSettingMessage[0] = '\0';
    if (message && strlen(message) > 0) {
        strncpy(pendingSettingMessage, message, sizeof(pendingSettingMessage) - 1);
        pendingSettingMessage[sizeof(pendingSettingMessage) - 1] = '\0';
    }
    pendingSettingRemainingSends = SETTINGS_MAX_ATTEMPTS;
    pendingSettingNextSendAt = millis();  // First send on next loop when printer idle
}

// Send pump Start/Stop once; sendCommandViaESPNow does 2x esp_now_send per call, and loop retries once on FAIL. Main dedupes by state (no-op if pump already in that state).
static bool sendPumpCommandRepeated(uint8_t commandType) {
    if (!espNowInitialized) return false;
    return sendCommandViaESPNow(commandType, 0, 0, "", false, true);  // fromSettingsQueue=true to skip long pre-send delay
}

// Helper function to send ESP-NOW command to Main ESP32
// isRetry: true when this is an automatic retry after send callback reported FAIL
// fromSettingsQueue: true when sending from the settings queue (skip long delay; printer has priority)
bool sendCommandViaESPNow(uint8_t commandType, uint8_t param1, uint8_t param2, const char* message, bool isRetry, bool fromSettingsQueue) {
    if (!espNowInitialized) {
        Serial.println("❌ ESP-NOW not initialized");
        return false;
    }
    if (!isRetry) retryCount = 0;  // New user-initiated send; allow one retry on FAIL
    
    // Print commands: send immediately (minimal delay). Settings queue: no long delay (loop sends when idle).
    bool isPrintCommand = (commandType == CMD_PRINT || commandType == CMD_PRINT_CHUNK || commandType == CMD_PRINT_CHUNK_START || commandType == CMD_TEST_PRINTER);
    if (isPrintCommand) {
        delay(50);  // Brief delay to avoid back-to-back TX; print goes out right away
    } else if (!fromSettingsQueue && lastSensorDataReceiveTime > 0) {
        unsigned long timeSinceLastReceive = millis() - lastSensorDataReceiveTime;
        const unsigned long MIN_AFTER_RECEIVE_MS = 700;
        const unsigned long SAFE_WINDOW_END_MS = 1700;
        if (timeSinceLastReceive < MIN_AFTER_RECEIVE_MS) {
            delay(MIN_AFTER_RECEIVE_MS - timeSinceLastReceive);
        } else if (timeSinceLastReceive <= SAFE_WINDOW_END_MS) {
            delay(random(20, 60));
        } else {
            const unsigned long MAIN_SEND_INTERVAL_MS = 10000;
            unsigned long nextMainSend = ((timeSinceLastReceive + MAIN_SEND_INTERVAL_MS - 1) / MAIN_SEND_INTERVAL_MS) * MAIN_SEND_INTERVAL_MS;
            long wait = (long)(nextMainSend - timeSinceLastReceive) + (long)MIN_AFTER_RECEIVE_MS;
            if (wait > 0 && wait < (long)MAIN_SEND_INTERVAL_MS) delay((unsigned long)wait);
            else if (wait <= 0) delay(random(20, 60));
            else delay(MIN_AFTER_RECEIVE_MS);
        }
    } else if (!fromSettingsQueue) {
        delay(100);
    } else {
        delay(random(15, 40));  // Small jitter when sending from settings queue
    }
    // Pump commands: avoid sending in the first ~400ms after Main's sensor (half-duplex collision)
    if (fromSettingsQueue && (commandType == CMD_DISPENSE_START || commandType == CMD_DISPENSE_STOP) && lastSensorDataReceiveTime > 0) {
        unsigned long timeSinceSensor = millis() - lastSensorDataReceiveTime;
        if (timeSinceSensor < 400) delay(400 - timeSinceSensor);
    }
    
    // Verify peer exists and channel is correct before sending
    bool peerExists = esp_now_is_peer_exist(mainESP32Mac);
    uint8_t currentChannel = (WiFi.status() == WL_CONNECTED && WiFi.channel() > 0) ? WiFi.channel() : (uint8_t)ESP_NOW_CHANNEL;
    
    // Get peer info to check channel and interface
    esp_now_peer_info_t existingPeerInfo;
    bool needsReconfig = false;
    String reconfigReason = "";
    
    if (peerExists) {
        if (esp_now_get_peer(mainESP32Mac, &existingPeerInfo) == ESP_OK) {
            if (existingPeerInfo.channel != currentChannel) {
                needsReconfig = true;
                reconfigReason = "channel mismatch (peer.ch=" + String(existingPeerInfo.channel) + " current.ch=" + String(currentChannel) + ")";
            }
            if (existingPeerInfo.ifidx != WIFI_IF_STA) {
                needsReconfig = true;
                reconfigReason = "interface mismatch (peer.ifidx=" + String(existingPeerInfo.ifidx) + " expected=" + String(WIFI_IF_STA) + ")";
            }
        } else {
            // Can't get peer info - might be corrupted, re-add it
            needsReconfig = true;
            reconfigReason = "cannot get peer info";
        }
    } else {
        needsReconfig = true;
        reconfigReason = "peer not found";
    }
    
    // Re-add peer if missing or misconfigured
    if (needsReconfig) {
        Serial.println("🔄 ESP-NOW: Re-configuring peer - " + reconfigReason);
        
        // Remove existing peer first
        if (peerExists) {
            esp_now_del_peer(mainESP32Mac);
            delay(50);
        }
        
        // Add peer with current channel and correct interface
        esp_now_peer_info_t peerInfo;
        memset(&peerInfo, 0, sizeof(peerInfo));
        memcpy(peerInfo.peer_addr, mainESP32Mac, 6);
        peerInfo.ifidx = WIFI_IF_STA;  // Must match WiFi mode
        peerInfo.channel = currentChannel;  // Use current WiFi channel
        peerInfo.encrypt = false;
        
        esp_err_t addResult = esp_now_add_peer(&peerInfo);
        if (addResult != ESP_OK) {
            String errStr = "";
            switch(addResult) {
                case ESP_ERR_ESPNOW_NOT_INIT: errStr = "NOT_INIT"; break;
                case ESP_ERR_ESPNOW_ARG: errStr = "ARG"; break;
                case ESP_ERR_ESPNOW_FULL: errStr = "FULL"; break;
                case ESP_ERR_ESPNOW_NO_MEM: errStr = "NO_MEM"; break;
                case ESP_ERR_ESPNOW_EXIST: errStr = "EXIST"; break;
                default: errStr = "UNKNOWN(" + String((int)addResult) + ")"; break;
            }
            Serial.println("❌ Failed to re-add ESP-NOW peer: " + errStr);
            return false;
        }
        delay(100);  // Wait for peer to be fully registered
        
        // Verify it was added correctly
        if (!esp_now_is_peer_exist(mainESP32Mac)) {
            Serial.println("❌ Peer re-added but verification failed");
            return false;
        }
        
        // Double-check the configuration
        if (esp_now_get_peer(mainESP32Mac, &peerInfo) == ESP_OK) {
            if (peerInfo.channel == currentChannel && peerInfo.ifidx == WIFI_IF_STA) {
                Serial.println("✅ ESP-NOW peer re-configured successfully (ch=" + String(currentChannel) + " ifidx=" + String(WIFI_IF_STA) + ")");
            } else {
                Serial.println("⚠️ ESP-NOW peer re-added but config mismatch (ch=" + String(peerInfo.channel) + "/" + String(currentChannel) + " ifidx=" + String(peerInfo.ifidx) + "/" + String(WIFI_IF_STA) + ")");
            }
        }
    }
    
    bool macSet = false;
    for (int i = 0; i < 6; i++) { if (mainESP32Mac[i] != 0) { macSet = true; break; } }
    if (!macSet) return false;
    
    CommandPacket packet;
    memset(&packet, 0, sizeof(packet));  // Zero-initialize to avoid garbage in padding/checksum
    packet.msgType = ESP_NOW_MSG_COMMAND;
    packet.commandType = commandType;
    packet.param1 = param1;
    packet.param2 = param2;
    
    // Clear message buffer (already done by memset above, but keep for clarity)
    memset(packet.message, 0, sizeof(packet.message));
    if (message && strlen(message) > 0) {
        strncpy(packet.message, message, sizeof(packet.message) - 1);
    }
    
    packet.sequenceNumber = ++commandSequence;  // For ACK matching so TZT can cancel retry
    lastSentSequence = commandSequence;
    packet.checksum = calculateChecksum((uint8_t*)&packet, sizeof(CommandPacket));
    
    // Store for retry if send callback reports FAIL
    lastSentCommandType = commandType;
    lastSentParam1 = param1;
    lastSentParam2 = param2;
    lastSentMessage[0] = '\0';
    if (message && strlen(message) > 0) {
        strncpy(lastSentMessage, message, sizeof(lastSentMessage) - 1);
        lastSentMessage[sizeof(lastSentMessage) - 1] = '\0';
    }
    
    // Send with one retry after 80ms to improve delivery (WiFi half-duplex can cause occasional FAIL)
    for (int attempt = 0; attempt < 2; attempt++) {
        if (attempt > 0) delay(80);
        esp_err_t result = esp_now_send(mainESP32Mac, (uint8_t*)&packet, sizeof(CommandPacket));
        if (result != ESP_OK) {
            String errStr = "";
            switch(result) {
                case ESP_ERR_ESPNOW_NOT_INIT: errStr = "NOT_INIT"; break;
                case ESP_ERR_ESPNOW_ARG: errStr = "ARG"; break;
                case ESP_ERR_ESPNOW_INTERNAL: errStr = "INTERNAL"; break;
                case ESP_ERR_ESPNOW_NO_MEM: errStr = "NO_MEM"; break;
                case ESP_ERR_ESPNOW_NOT_FOUND: errStr = "NOT_FOUND"; break;
                case ESP_ERR_ESPNOW_IF: errStr = "IF (interface mismatch)"; break;
                default: errStr = "UNKNOWN(" + String((int)result) + ")"; break;
            }
            Serial.println("[ESP-NOW] send failed: " + errStr);
            return false;
        }
    }
    if (commandType == CMD_PRINT_CHUNK)
        ESPNOW_LOG("[TZT] Sent: PRINT_CHUNK " + String((int)(param1 + 1)) + "/" + String((int)param2));
    else
        ESPNOW_LOG("[TZT] Sent: " + String(espNowCommandName(commandType)));
    return true;
}

#if !defined(TZT_HEADLESS)
// LVGL touch input: XPT2046 on shared HSPI (TOUCH_CS 33)
// Uses getTouchRaw() + affine calibration (6 coefficients from config_tzt.h)
// Panel idles at rawZ ~725; TOUCH_Z_PRESSED (800) separates idle from real press
// Debounce: 2 consecutive matching reads to change state (filters SPI noise and wrong-position taps)
static uint8_t touchDebounceCount = 0;
static const uint8_t TOUCH_DEBOUNCE = 2;
static bool touchLastState = false;
static int32_t touchLastX = 0, touchLastY = 0;
// Gesture: record start on press, on release set pending for timer to process
static int32_t touchStartX = 0, touchStartY = 0;
static volatile bool gesturePending = false;
static int32_t gestureStartX = 0, gestureStartY = 0;
static int32_t gestureEndX = 0, gestureEndY = 0;
#define SWIPE_THRESHOLD 40
#define GESTURE_COOLDOWN_MS 120
static uint32_t lastGestureTime = 0;
#define MEDIA_SUBMENU_IGNORE_MS 400  // ignore taps on Media hub buttons right after switching to Media tab
static uint32_t lastMediaScreenOpenTime = 0;
static void touch_read_cb(lv_indev_drv_t* indev_drv, lv_indev_data_t* data) {
    (void)indev_drv;
    if (displayFlushing) {
        data->point.x = touchLastX;
        data->point.y = touchLastY;
        data->state = LV_INDEV_STATE_RELEASED;
        return;
    }
    bool pressed = tft.getTouchRawZ() > TOUCH_Z_PRESSED;
    uint16_t rx, ry;
    tft.getTouchRaw(&rx, &ry);
    int32_t sx = (int32_t)(TOUCH_AX * rx + TOUCH_BX * ry + TOUCH_CX);
    int32_t sy = (int32_t)(TOUCH_AY * rx + TOUCH_BY * ry + TOUCH_CY);
    if (sx < 0) sx = 0; if (sx >= TZT_SCREEN_WIDTH) sx = TZT_SCREEN_WIDTH - 1;
    if (sy < 0) sy = 0; if (sy >= TZT_SCREEN_HEIGHT) sy = TZT_SCREEN_HEIGHT - 1;

    if (pressed && touchLastState) {
        touchLastX = sx;
        touchLastY = sy;
    }
    if (pressed != touchLastState) {
        touchDebounceCount++;
        if (touchDebounceCount >= TOUCH_DEBOUNCE) {
            lastDashboardTouchTime = millis();  // any touch pauses auto-rotate 2 min
            if (pressed) {
                touchLastX = sx;
                touchLastY = sy;
                touchStartX = sx;
                touchStartY = sy;
            } else {
                gestureStartX = touchStartX;
                gestureStartY = touchStartY;
                gestureEndX = touchLastX;
                gestureEndY = touchLastY;
                gesturePending = true;
            }
            touchLastState = pressed;
            touchDebounceCount = 0;
        }
    } else {
        touchDebounceCount = 0;
    }
    data->point.x = touchLastX;
    data->point.y = touchLastY;
    data->state = touchLastState ? LV_INDEV_STATE_PRESSED : LV_INDEV_STATE_RELEASED;
}

// LVGL display flush callback: send buffer to TFT
static void lvgl_flush_cb(lv_disp_drv_t* drv, const lv_area_t* area, lv_color_t* color_map) {
    uint32_t w = (area->x2 - area->x1 + 1);
    uint32_t h = (area->y2 - area->y1 + 1);
    displayFlushing = true;
    tft.startWrite();
    tft.setAddrWindow(area->x1, area->y1, w, h);
    tft.pushPixels((uint16_t*)color_map, w * h);
    tft.endWrite();
    displayFlushing = false;
    lv_disp_flush_ready(drv);
}

// Nav: change screen by index (bottom nav: 0=Home, 1=Sensors, 2=Media, 3=Settings)
static lv_obj_t* screenByIndex(int idx) {
    switch (idx) {
        case 0: return screen1;   // Home
        case 1: return screen2;   // Sensors
        case 2: return mediaSubTab == 0 ? dashboardPhotoScreen : dashboardAudioScreen;  // Media
        case 3: return settingsScreen;
        default: return screen1;
    }
}

static void goToScreenIndex(int index) {
    if (index < 0 || index >= SCREEN_COUNT) return;
    if (index == currentScreenIndex) return;
    int dir = (index > currentScreenIndex) ? 1 : -1;
    currentScreenIndex = index;
    lv_obj_t* scr = screenByIndex(currentScreenIndex);
    lv_scr_load_anim(scr,
        dir > 0 ? LV_SCR_LOAD_ANIM_MOVE_LEFT : LV_SCR_LOAD_ANIM_MOVE_RIGHT,
        180, 0, false);
    updateNavHighlight(currentScreenIndex);
}

static void navBtnCb(lv_event_t* e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    int idx = (int)(intptr_t)lv_event_get_user_data(e);
    lastDashboardTouchTime = millis();  // tab tap pauses auto-rotate 2 min
    goToScreenIndex(idx);
}

static void updateNavHighlight(int index) {
    for (int i = 0; i < SCREEN_COUNT && navBtns[i]; i++) {
        if (i == index)
            lv_obj_set_style_bg_color(navBtns[i], lv_color_hex(0x2a5514), 0);
        else
            lv_obj_set_style_bg_color(navBtns[i], lv_color_hex(0x0a1806), 0);
    }
}

static void showNavBar() {
    if (navBarContainer) lv_obj_clear_flag(navBarContainer, LV_OBJ_FLAG_HIDDEN);
    if (statusBar) lv_obj_clear_flag(statusBar, LV_OBJ_FLAG_HIDDEN);
}

static void hideNavBar() {
    if (navBarContainer) lv_obj_add_flag(navBarContainer, LV_OBJ_FLAG_HIDDEN);
    if (statusBar) lv_obj_add_flag(statusBar, LV_OBJ_FLAG_HIDDEN);
}

static void updateStatusBar() {
    if (!statusTimeLabel) return;
    struct tm ti;
    time_t now = time(nullptr);
    localtime_r(&now, &ti);
    if (ti.tm_year > 100) {
        char tbuf[12];
        strftime(tbuf, sizeof(tbuf), "%I:%M %p", &ti);
        if (tbuf[0] == '0') memmove(tbuf, tbuf + 1, strlen(tbuf));
        lv_label_set_text(statusTimeLabel, tbuf);
    }
    if (statusWifiLabel) {
        if (WiFi.status() == WL_CONNECTED) {
            lv_label_set_text(statusWifiLabel, WiFi.SSID().length() ? WiFi.SSID().c_str() : "Connected");
            lv_obj_set_style_text_color(statusWifiLabel, lv_color_hex(0x6ab82e), 0);
        } else {
            lv_label_set_text(statusWifiLabel, "Disconnected");
            lv_obj_set_style_text_color(statusWifiLabel, lv_color_hex(0xaaaaaa), 0);
        }
    }
    if (statusIpLabel) {
        if (WiFi.status() == WL_CONNECTED) {
            String ip = WiFi.localIP().toString();
            if (ip != deviceIP) deviceIP = ip;  // keep cached deviceIP in sync (e.g. after DHCP renewal)
            lv_label_set_text(statusIpLabel, ip.c_str());
        } else {
            lv_label_set_text(statusIpLabel, "no wifi");
        }
    }
}

static void setScreenStyle(lv_obj_t* scr) {
    lv_obj_set_style_bg_color(scr, lv_color_hex(0x1a3d0e), 0);
    lv_obj_set_style_bg_grad_color(scr, lv_color_hex(0x4a7c2a), 0);
    lv_obj_set_style_bg_grad_dir(scr, LV_GRAD_DIR_VER, 0);
    lv_obj_set_style_pad_all(scr, 0, 0);
    lv_obj_clear_flag(scr, LV_OBJ_FLAG_SCROLLABLE);
}

// Pressed-state style applied to every button for tactile feedback
static lv_style_t style_btn_pressed;
static bool btn_style_inited = false;
static void applyBtnPressStyle(lv_obj_t* btn) {
    if (!btn_style_inited) {
        lv_style_init(&style_btn_pressed);
        lv_style_set_bg_color(&style_btn_pressed, lv_color_hex(0x5a9c3a));
        lv_style_set_transform_width(&style_btn_pressed, -2);
        lv_style_set_transform_height(&style_btn_pressed, -2);
        btn_style_inited = true;
    }
    lv_obj_add_style(btn, &style_btn_pressed, LV_STATE_PRESSED);
}

static void gesture_timer_cb(lv_timer_t* t) {
    (void)t;
    // Navigation is tab-only (bottom nav bar); swipe gestures are intentionally not used for nav.
    gesturePending = false;
}

static void volumeSliderCb(lv_event_t* e) {
    if (lv_event_get_code(e) != LV_EVENT_VALUE_CHANGED) return;
    lv_obj_t* slider = lv_event_get_target(e);
    if (!slider) return;
    int32_t v = lv_slider_get_value(slider);
    if (v < 0) v = 0;
    if (v > 100) v = 100;
    audioSvc.setVolume((float)v / 100.0f);
    if (settingsVolumeValueLabel) lv_label_set_text_fmt(settingsVolumeValueLabel, "%d%%", (int)v);
}

static void mediaOpenAudioCb(lv_event_t* e) {
    (void)e;
    if ((uint32_t)millis() - lastMediaScreenOpenTime < MEDIA_SUBMENU_IGNORE_MS) return;
    refreshAudioList();
    hideNavBar();
    lv_scr_load_anim(audioListScreen, LV_SCR_LOAD_ANIM_MOVE_LEFT, 180, 0, false);
}

static void mediaOpenImagesCb(lv_event_t* e) {
    (void)e;
    if ((uint32_t)millis() - lastMediaScreenOpenTime < MEDIA_SUBMENU_IGNORE_MS) return;
    refreshImageList();
    hideNavBar();
    lv_scr_load_anim(imageListScreen, LV_SCR_LOAD_ANIM_MOVE_LEFT, 180, 0, false);
}

static void audioBackToMediaCb(lv_event_t* e) {
    (void)e;
    mediaSubTab = 1;  // Audio
    currentScreenIndex = 2;  // Media tab
    lastMediaScreenOpenTime = (uint32_t)millis();
    lv_scr_load_anim(dashboardAudioScreen, LV_SCR_LOAD_ANIM_MOVE_RIGHT, 180, 0, false);
    showNavBar();
    updateNavHighlight(2);
}

static void imageListBackCb(lv_event_t* e) {
    (void)e;
    mediaSubTab = 0;  // Photo
    currentScreenIndex = 2;  // Media tab
    lastMediaScreenOpenTime = (uint32_t)millis();
    lv_scr_load_anim(dashboardPhotoScreen, LV_SCR_LOAD_ANIM_MOVE_RIGHT, 180, 0, false);
    showNavBar();
    updateNavHighlight(2);
}

// Switch which sub-screen the Media tab shows (Photo/Audio); no-op if Media tab isn't active
static void switchMediaSubTab(int which) {
    if (mediaSubTab == which) return;
    mediaSubTab = which;
    if (currentScreenIndex == 2) {
        lv_scr_load(screenByIndex(2));
    }
}

static void mediaTogglePhotoCb(lv_event_t* e) {
    (void)e;
    lastDashboardTouchTime = millis();
    switchMediaSubTab(0);
}

static void mediaToggleAudioCb(lv_event_t* e) {
    (void)e;
    lastDashboardTouchTime = millis();
    switchMediaSubTab(1);
}

static void historyOpenCb(lv_event_t* e) {
    (void)e;
    refreshHistoryList();
    hideNavBar();
    lv_scr_load_anim(screen3, LV_SCR_LOAD_ANIM_MOVE_LEFT, 180, 0, false);
}

static void historyCloseCb(lv_event_t* e) {
    (void)e;
    if (historyDetail) lv_obj_add_flag(historyDetail, LV_OBJ_FLAG_HIDDEN);  // reset to list view for next visit
    currentScreenIndex = 0;  // Home
    showNavBar();
    lv_scr_load_anim(screen1, LV_SCR_LOAD_ANIM_MOVE_RIGHT, 180, 0, false);
    updateNavHighlight(0);
}

static void createControlPanel() {
    // ---- Screen 1: Home (latest message + sensor summary + History button) ----
    screen1 = lv_obj_create(NULL);
    setScreenStyle(screen1);
    #define HOME_STRIP_H 22
    #define HOME_HEADER_H 36
    lv_obj_t* homeHeader = lv_obj_create(screen1);
    lv_obj_set_size(homeHeader, TZT_SCREEN_WIDTH - 8, HOME_HEADER_H);
    lv_obj_set_pos(homeHeader, 4, TZT_CONTENT_TOP);
    lv_obj_set_style_bg_opa(homeHeader, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(homeHeader, 0, 0);
    lv_obj_set_style_pad_all(homeHeader, 0, 0);
    lv_obj_clear_flag(homeHeader, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_t* homeTitle = lv_label_create(homeHeader);
    lv_label_set_text(homeTitle, LV_SYMBOL_ENVELOPE " Latest Message");
    lv_obj_set_style_text_color(homeTitle, lv_color_hex(0x99bb88), 0);
    lv_obj_set_style_text_font(homeTitle, &lv_font_montserrat_14, 0);
    lv_obj_align(homeTitle, LV_ALIGN_LEFT_MID, 0, 0);
    lv_obj_t* historyBtn = lv_btn_create(homeHeader);
    applyBtnPressStyle(historyBtn);
    lv_obj_set_size(historyBtn, 92, HOME_HEADER_H - 4);
    lv_obj_align(historyBtn, LV_ALIGN_RIGHT_MID, 0, 0);
    lv_obj_set_style_bg_color(historyBtn, lv_color_hex(0x2d5016), 0);
    lv_obj_set_style_border_color(historyBtn, lv_color_hex(0x3a6820), 0);
    lv_obj_set_style_border_width(historyBtn, 1, 0);
    lv_obj_set_style_radius(historyBtn, 6, 0);
    lv_obj_t* historyLbl = lv_label_create(historyBtn);
    lv_label_set_text(historyLbl, LV_SYMBOL_LIST " History");
    lv_obj_set_style_text_font(historyLbl, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(historyLbl, lv_color_hex(0xe8e8e8), 0);
    lv_obj_center(historyLbl);
    lv_obj_add_event_cb(historyBtn, historyOpenCb, LV_EVENT_CLICKED, nullptr);

    int msgBoxTop = TZT_CONTENT_TOP + HOME_HEADER_H + 2;
    int msgBoxH = TZT_CONTENT_H - HOME_HEADER_H - HOME_STRIP_H - 6;
    lv_obj_t* msgBox = lv_obj_create(screen1);
    lv_obj_set_size(msgBox, TZT_SCREEN_WIDTH - 8, msgBoxH);
    lv_obj_set_pos(msgBox, 4, msgBoxTop);
    lv_obj_set_style_bg_color(msgBox, lv_color_hex(0x142e0a), 0);
    lv_obj_set_style_bg_opa(msgBox, LV_OPA_90, 0);
    lv_obj_set_style_border_color(msgBox, lv_color_hex(0x3a6820), 0);
    lv_obj_set_style_border_width(msgBox, 1, 0);
    lv_obj_set_style_pad_all(msgBox, 6, 0);
    lv_obj_set_style_radius(msgBox, 4, 0);
    lv_obj_add_flag(msgBox, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_SCROLL_MOMENTUM | LV_OBJ_FLAG_SCROLL_ELASTIC);
    lv_obj_set_scrollbar_mode(msgBox, LV_SCROLLBAR_MODE_AUTO);
    lv_obj_set_scroll_dir(msgBox, LV_DIR_VER);
    labelMessage = lv_label_create(msgBox);
    lv_label_set_long_mode(labelMessage, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(labelMessage, TZT_SCREEN_WIDTH - 28);
    lv_label_set_text(labelMessage, lastPrintMessage.c_str());
    lv_obj_set_style_text_color(labelMessage, lv_color_hex(0xe8e8e8), 0);
    lv_obj_set_style_text_font(labelMessage, &lv_font_montserrat_16, 0);
    // Compact sensor summary at bottom of home screen
    homeSensorStrip = lv_label_create(screen1);
    lv_obj_set_style_text_color(homeSensorStrip, lv_color_hex(0x99cc77), 0);
    lv_obj_set_style_text_font(homeSensorStrip, &lv_font_montserrat_14, 0);
    lv_label_set_text(homeSensorStrip, LV_SYMBOL_CHARGE " --   San --   LED --   Pump --");
    lv_obj_set_pos(homeSensorStrip, 8, msgBoxTop + msgBoxH + 4);

    // ---- Screen 2: Sensor Data with progress bars ----
    screen2 = lv_obj_create(NULL);
    setScreenStyle(screen2);
    {
        int pad = 4, gap = 3;
        int boxW = (TZT_SCREEN_WIDTH - pad * 2 - gap) / 2;
        int boxH = (TZT_CONTENT_H - gap) / 2;
        int y0 = TZT_CONTENT_TOP;
        auto addSensorCard = [&](int col, int row, const char* icon, const char* name,
                                  lv_obj_t*& valueLabel, lv_obj_t*& bar) {
            int x = pad + col * (boxW + gap);
            int y = y0 + row * (boxH + gap);
            lv_obj_t* box = lv_obj_create(screen2);
            lv_obj_set_size(box, boxW, boxH);
            lv_obj_set_pos(box, x, y);
            lv_obj_set_style_bg_color(box, lv_color_hex(0x1e4210), 0);
            lv_obj_set_style_border_color(box, lv_color_hex(0x3a6820), 0);
            lv_obj_set_style_border_width(box, 1, 0);
            lv_obj_set_style_pad_all(box, 5, 0);
            lv_obj_set_style_radius(box, 6, 0);
            lv_obj_clear_flag(box, LV_OBJ_FLAG_SCROLLABLE);
            lv_obj_t* header = lv_label_create(box);
            char hbuf[32]; snprintf(hbuf, sizeof(hbuf), "%s %s", icon, name);
            lv_label_set_text(header, hbuf);
            lv_obj_set_style_text_color(header, lv_color_hex(0x99bb88), 0);
            lv_obj_set_style_text_font(header, &lv_font_montserrat_14, 0);
            lv_obj_set_pos(header, 0, 0);
            lv_obj_t* val = lv_label_create(box);
            lv_label_set_text(val, "--");
            lv_obj_set_style_text_color(val, lv_color_hex(0xffffff), 0);
            lv_obj_set_style_text_font(val, &lv_font_montserrat_18, 0);
            lv_obj_align(val, LV_ALIGN_CENTER, 0, 2);
            valueLabel = val;
            if (bar != (lv_obj_t*)1) {
                bar = lv_bar_create(box);
                lv_obj_set_size(bar, boxW - 14, 10);
                lv_obj_align(bar, LV_ALIGN_BOTTOM_MID, 0, -2);
                lv_bar_set_range(bar, 0, 100);
                lv_bar_set_value(bar, 0, LV_ANIM_OFF);
                lv_obj_set_style_bg_color(bar, lv_color_hex(0x0d1f07), LV_PART_MAIN);
                lv_obj_set_style_bg_color(bar, lv_color_hex(0x6ab82e), LV_PART_INDICATOR);
                lv_obj_set_style_radius(bar, 4, LV_PART_MAIN);
                lv_obj_set_style_radius(bar, 4, LV_PART_INDICATOR);
            } else {
                bar = nullptr;
            }
        };
        lv_obj_t* noBar = (lv_obj_t*)1;
        addSensorCard(0, 0, LV_SYMBOL_CHARGE, "Moisture", labelMoisture, barMoisture);
        addSensorCard(1, 0, LV_SYMBOL_REFRESH, "Sanitizer", labelSanitizer, barSanitizer);
        addSensorCard(0, 1, LV_SYMBOL_SETTINGS, "LED", labelLED, barLED);
        addSensorCard(1, 1, LV_SYMBOL_POWER, "Pump", labelPump, noBar);
    }

    // ---- Screen 3: History (messages from SD) - reached via History button on Home ----
    screen3 = lv_obj_create(NULL);
    setScreenStyle(screen3);
    lv_obj_t* historyHeaderBar = lv_obj_create(screen3);
    lv_obj_set_size(historyHeaderBar, TZT_SCREEN_WIDTH, TZT_HEADER_BAR_H);
    lv_obj_set_pos(historyHeaderBar, 0, 0);
    lv_obj_set_style_bg_color(historyHeaderBar, lv_color_hex(0x0d1f07), 0);
    lv_obj_set_style_border_color(historyHeaderBar, lv_color_hex(0x2d5016), 0);
    lv_obj_set_style_border_width(historyHeaderBar, 1, 0);
    lv_obj_set_style_border_side(historyHeaderBar, LV_BORDER_SIDE_BOTTOM, 0);
    lv_obj_set_style_radius(historyHeaderBar, 0, 0);
    lv_obj_set_style_pad_all(historyHeaderBar, 0, 0);
    lv_obj_clear_flag(historyHeaderBar, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_t* historyHomeBtn = lv_btn_create(historyHeaderBar);
    applyBtnPressStyle(historyHomeBtn);
    lv_obj_set_size(historyHomeBtn, 100, TZT_MIN_TOUCH_H);
    lv_obj_align(historyHomeBtn, LV_ALIGN_LEFT_MID, 6, 0);
    lv_obj_set_style_bg_color(historyHomeBtn, lv_color_hex(0x2d5016), 0);
    lv_obj_set_style_radius(historyHomeBtn, 8, 0);
    lv_obj_t* historyHomeLbl = lv_label_create(historyHomeBtn);
    lv_label_set_text(historyHomeLbl, LV_SYMBOL_LEFT " Home");
    lv_obj_set_style_text_font(historyHomeLbl, &lv_font_montserrat_16, 0);
    lv_obj_set_style_text_color(historyHomeLbl, lv_color_hex(0xe8e8e8), 0);
    lv_obj_center(historyHomeLbl);
    lv_obj_add_event_cb(historyHomeBtn, historyCloseCb, LV_EVENT_CLICKED, nullptr);
    lv_obj_t* historyTitleLbl = lv_label_create(historyHeaderBar);
    lv_label_set_text(historyTitleLbl, LV_SYMBOL_LIST " History");
    lv_obj_set_style_text_color(historyTitleLbl, lv_color_hex(0xffffff), 0);
    lv_obj_set_style_text_font(historyTitleLbl, &lv_font_montserrat_18, 0);
    lv_obj_align(historyTitleLbl, LV_ALIGN_CENTER, 0, 0);

    historyList = lv_list_create(screen3);
    lv_obj_set_size(historyList, TZT_SCREEN_WIDTH - 8, TZT_SCREEN_HEIGHT - TZT_HEADER_BAR_H - 8);
    lv_obj_set_pos(historyList, 4, TZT_HEADER_BAR_H + 4);
    lv_obj_set_style_bg_color(historyList, lv_color_hex(0x142e0a), 0);
    lv_obj_set_style_bg_opa(historyList, LV_OPA_90, 0);
    lv_obj_set_style_border_color(historyList, lv_color_hex(0x3a6820), 0);
    lv_obj_set_style_border_width(historyList, 1, 0);
    lv_obj_set_style_pad_all(historyList, TZT_LIST_PAD, 0);
    lv_obj_set_style_radius(historyList, 4, 0);
    lv_obj_set_scrollbar_mode(historyList, LV_SCROLLBAR_MODE_AUTO);
    lv_obj_add_flag(historyList, LV_OBJ_FLAG_SCROLL_MOMENTUM | LV_OBJ_FLAG_SCROLL_ELASTIC);
    lv_obj_set_scroll_dir(historyList, LV_DIR_VER);
    refreshHistoryList();

    historyDetail = lv_obj_create(screen3);
    lv_obj_set_size(historyDetail, TZT_SCREEN_WIDTH - 8, TZT_SCREEN_HEIGHT - TZT_HEADER_BAR_H - 8);
    lv_obj_set_pos(historyDetail, 4, TZT_HEADER_BAR_H + 4);
    lv_obj_set_style_bg_color(historyDetail, lv_color_hex(0x0d1f07), 0);
    lv_obj_set_style_bg_opa(historyDetail, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(historyDetail, lv_color_hex(0x4a7c2a), 0);
    lv_obj_set_style_border_width(historyDetail, 2, 0);
    lv_obj_set_style_radius(historyDetail, 8, 0);
    lv_obj_set_style_pad_all(historyDetail, TZT_LIST_PAD, 0);
    lv_obj_add_flag(historyDetail, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_SCROLL_MOMENTUM | LV_OBJ_FLAG_SCROLL_ELASTIC);
    lv_obj_set_scroll_dir(historyDetail, LV_DIR_VER);
    lv_obj_add_flag(historyDetail, LV_OBJ_FLAG_HIDDEN);

    historyDetailLabel = lv_label_create(historyDetail);
    lv_label_set_long_mode(historyDetailLabel, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(historyDetailLabel, TZT_SCREEN_WIDTH - 56);
    lv_obj_set_style_text_color(historyDetailLabel, lv_color_hex(0xe8e8e8), 0);
    lv_obj_set_style_text_font(historyDetailLabel, &lv_font_montserrat_16, 0);

    lv_obj_t* backBtn = lv_btn_create(historyDetail);
    applyBtnPressStyle(backBtn);
    lv_obj_set_size(backBtn, 100, TZT_MIN_TOUCH_H);
    lv_obj_align(backBtn, LV_ALIGN_BOTTOM_RIGHT, 0, 0);
    lv_obj_set_style_bg_color(backBtn, lv_color_hex(0x2d5016), 0);
    lv_obj_set_style_radius(backBtn, 6, 0);
    lv_obj_t* backLbl = lv_label_create(backBtn);
    lv_label_set_text(backLbl, LV_SYMBOL_LEFT " Back");
    lv_obj_set_style_text_font(backLbl, &lv_font_montserrat_16, 0);
    lv_obj_center(backLbl);
    lv_obj_add_event_cb(backBtn, historyBackCb, LV_EVENT_CLICKED, nullptr);

    // ---- Audio list sub-screen (from Media) ----
    audioListScreen = lv_obj_create(NULL);
    setScreenStyle(audioListScreen);
    audioHeaderBar = lv_obj_create(audioListScreen);
    lv_obj_set_size(audioHeaderBar, TZT_SCREEN_WIDTH, TZT_HEADER_BAR_H);
    lv_obj_set_pos(audioHeaderBar, 0, 0);
    lv_obj_set_style_bg_color(audioHeaderBar, lv_color_hex(0x0d1f07), 0);
    lv_obj_set_style_border_color(audioHeaderBar, lv_color_hex(0x2d5016), 0);
    lv_obj_set_style_border_width(audioHeaderBar, 1, 0);
    lv_obj_set_style_border_side(audioHeaderBar, LV_BORDER_SIDE_BOTTOM, 0);
    lv_obj_set_style_radius(audioHeaderBar, 0, 0);
    lv_obj_set_style_pad_all(audioHeaderBar, 0, 0);
    lv_obj_clear_flag(audioHeaderBar, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_t* audioBackBtn = lv_btn_create(audioHeaderBar);
    applyBtnPressStyle(audioBackBtn);
    lv_obj_set_size(audioBackBtn, 100, TZT_MIN_TOUCH_H);
    lv_obj_align(audioBackBtn, LV_ALIGN_LEFT_MID, 6, 0);
    lv_obj_set_style_bg_color(audioBackBtn, lv_color_hex(0x2d5016), 0);
    lv_obj_set_style_radius(audioBackBtn, 8, 0);
    lv_obj_t* audioBackLbl = lv_label_create(audioBackBtn);
    lv_label_set_text(audioBackLbl, LV_SYMBOL_LEFT " Media");
    lv_obj_set_style_text_font(audioBackLbl, &lv_font_montserrat_16, 0);
    lv_obj_set_style_text_color(audioBackLbl, lv_color_hex(0xe8e8e8), 0);
    lv_obj_center(audioBackLbl);
    lv_obj_add_event_cb(audioBackBtn, audioBackToMediaCb, LV_EVENT_CLICKED, nullptr);
    lv_obj_t* audioTitleLbl = lv_label_create(audioHeaderBar);
    lv_label_set_text(audioTitleLbl, LV_SYMBOL_AUDIO " Audio");
    lv_obj_set_style_text_color(audioTitleLbl, lv_color_hex(0xffffff), 0);
    lv_obj_set_style_text_font(audioTitleLbl, &lv_font_montserrat_18, 0);
    lv_obj_align(audioTitleLbl, LV_ALIGN_CENTER, 0, 0);
    audioStopBtn = lv_btn_create(audioHeaderBar);
    applyBtnPressStyle(audioStopBtn);
    lv_obj_set_size(audioStopBtn, 72, TZT_MIN_TOUCH_H);
    lv_obj_align(audioStopBtn, LV_ALIGN_RIGHT_MID, -8, 0);
    lv_obj_set_style_bg_color(audioStopBtn, lv_color_hex(0xcc3333), 0);
    lv_obj_set_style_radius(audioStopBtn, 8, 0);
    lv_obj_t* stopLbl = lv_label_create(audioStopBtn);
    lv_label_set_text(stopLbl, LV_SYMBOL_STOP);
    lv_obj_set_style_text_font(stopLbl, &lv_font_montserrat_16, 0);
    lv_obj_set_style_text_color(stopLbl, lv_color_white(), 0);
    lv_obj_center(stopLbl);
    lv_obj_add_event_cb(audioStopBtn, audioStopCb, LV_EVENT_CLICKED, nullptr);
    audioList = lv_list_create(audioListScreen);
    lv_obj_set_size(audioList, TZT_SCREEN_WIDTH - 8, TZT_SCREEN_HEIGHT - TZT_HEADER_BAR_H - 8);
    lv_obj_set_pos(audioList, 4, TZT_HEADER_BAR_H + 4);
    lv_obj_set_style_bg_color(audioList, lv_color_hex(0x1a3d0e), 0);
    lv_obj_set_style_bg_opa(audioList, LV_OPA_80, 0);
    lv_obj_set_style_border_color(audioList, lv_color_hex(0x4a7c2a), 0);
    lv_obj_set_style_border_width(audioList, 2, 0);
    lv_obj_set_style_pad_all(audioList, TZT_LIST_PAD, 0);
    lv_obj_set_style_radius(audioList, 6, 0);
    lv_obj_set_scrollbar_mode(audioList, LV_SCROLLBAR_MODE_AUTO);
    lv_obj_add_flag(audioList, LV_OBJ_FLAG_SCROLL_MOMENTUM | LV_OBJ_FLAG_SCROLL_ELASTIC);
    lv_obj_set_scroll_dir(audioList, LV_DIR_VER);

    // ---- Image list sub-screen (from Media) ----
    imageListScreen = lv_obj_create(NULL);
    setScreenStyle(imageListScreen);
    lv_obj_t* imageHeaderBar = lv_obj_create(imageListScreen);
    lv_obj_set_size(imageHeaderBar, TZT_SCREEN_WIDTH, TZT_HEADER_BAR_H);
    lv_obj_set_pos(imageHeaderBar, 0, 0);
    lv_obj_set_style_bg_color(imageHeaderBar, lv_color_hex(0x0d1f07), 0);
    lv_obj_set_style_border_color(imageHeaderBar, lv_color_hex(0x2d5016), 0);
    lv_obj_set_style_border_width(imageHeaderBar, 1, 0);
    lv_obj_set_style_border_side(imageHeaderBar, LV_BORDER_SIDE_BOTTOM, 0);
    lv_obj_set_style_radius(imageHeaderBar, 0, 0);
    lv_obj_set_style_pad_all(imageHeaderBar, 0, 0);
    lv_obj_clear_flag(imageHeaderBar, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_t* imgBackBtn = lv_btn_create(imageHeaderBar);
    applyBtnPressStyle(imgBackBtn);
    lv_obj_set_size(imgBackBtn, 100, TZT_MIN_TOUCH_H);
    lv_obj_align(imgBackBtn, LV_ALIGN_LEFT_MID, 6, 0);
    lv_obj_set_style_bg_color(imgBackBtn, lv_color_hex(0x2d5016), 0);
    lv_obj_set_style_radius(imgBackBtn, 8, 0);
    lv_obj_t* imgBackLbl = lv_label_create(imgBackBtn);
    lv_label_set_text(imgBackLbl, LV_SYMBOL_LEFT " Media");
    lv_obj_set_style_text_font(imgBackLbl, &lv_font_montserrat_16, 0);
    lv_obj_set_style_text_color(imgBackLbl, lv_color_hex(0xe8e8e8), 0);
    lv_obj_center(imgBackLbl);
    lv_obj_add_event_cb(imgBackBtn, imageListBackCb, LV_EVENT_CLICKED, nullptr);
    lv_obj_t* imageTitleLbl = lv_label_create(imageHeaderBar);
    lv_label_set_text(imageTitleLbl, LV_SYMBOL_IMAGE " Images");
    lv_obj_set_style_text_color(imageTitleLbl, lv_color_hex(0xffffff), 0);
    lv_obj_set_style_text_font(imageTitleLbl, &lv_font_montserrat_18, 0);
    lv_obj_align(imageTitleLbl, LV_ALIGN_CENTER, 0, 0);
    imageList = lv_list_create(imageListScreen);
    lv_obj_set_size(imageList, TZT_SCREEN_WIDTH - 8, TZT_SCREEN_HEIGHT - TZT_HEADER_BAR_H - 8);
    lv_obj_set_pos(imageList, 4, TZT_HEADER_BAR_H + 4);
    lv_obj_set_style_bg_color(imageList, lv_color_hex(0x1a3d0e), 0);
    lv_obj_set_style_bg_opa(imageList, LV_OPA_80, 0);
    lv_obj_set_style_border_color(imageList, lv_color_hex(0x4a7c2a), 0);
    lv_obj_set_style_border_width(imageList, 2, 0);
    lv_obj_set_style_pad_all(imageList, TZT_LIST_PAD, 0);
    lv_obj_set_style_radius(imageList, 6, 0);
    lv_obj_set_scrollbar_mode(imageList, LV_SCROLLBAR_MODE_AUTO);
    lv_obj_add_flag(imageList, LV_OBJ_FLAG_SCROLL_MOMENTUM | LV_OBJ_FLAG_SCROLL_ELASTIC);
    lv_obj_set_scroll_dir(imageList, LV_DIR_VER);

    // Image viewer: full screen; tap anywhere to go back to list
    imageViewerScreen = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(imageViewerScreen, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(imageViewerScreen, LV_OPA_COVER, 0);
    lv_obj_set_style_border_opa(imageViewerScreen, LV_OPA_0, 0);
    lv_obj_set_style_pad_all(imageViewerScreen, 0, 0);
    lv_obj_add_flag(imageViewerScreen, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(imageViewerScreen, imageBackCb, LV_EVENT_CLICKED, nullptr);

    #define MEDIA_TOGGLE_H 32
    #define MEDIA_TOGGLE_GAP 6
    #define MEDIA_CONTENT_TOP (TZT_CONTENT_TOP + MEDIA_TOGGLE_H + MEDIA_TOGGLE_GAP)
    int mediaToggleBtnW = (TZT_SCREEN_WIDTH - 16 - 4) / 2;

    // ---- Media tab, Photo sub-tab (full-area image when active; drawn in loop) ----
    dashboardPhotoScreen = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(dashboardPhotoScreen, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(dashboardPhotoScreen, LV_OPA_COVER, 0);
    lv_obj_set_style_pad_all(dashboardPhotoScreen, 0, 0);
    lv_obj_set_size(dashboardPhotoScreen, TZT_SCREEN_WIDTH, TZT_SCREEN_HEIGHT);
    {
        lv_obj_t* toggleRow = lv_obj_create(dashboardPhotoScreen);
        lv_obj_set_size(toggleRow, TZT_SCREEN_WIDTH - 16, MEDIA_TOGGLE_H);
        lv_obj_set_pos(toggleRow, 8, TZT_CONTENT_TOP);
        lv_obj_set_style_bg_opa(toggleRow, LV_OPA_TRANSP, 0);
        lv_obj_set_style_border_width(toggleRow, 0, 0);
        lv_obj_set_style_pad_all(toggleRow, 0, 0);
        lv_obj_clear_flag(toggleRow, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_t* toggleBtnPhoto = lv_btn_create(toggleRow);
        lv_obj_set_size(toggleBtnPhoto, mediaToggleBtnW, MEDIA_TOGGLE_H);
        lv_obj_set_pos(toggleBtnPhoto, 0, 0);
        lv_obj_set_style_bg_color(toggleBtnPhoto, lv_color_hex(0x2a5514), 0);  // active
        lv_obj_set_style_radius(toggleBtnPhoto, 6, 0);
        lv_obj_t* toggleLblPhoto = lv_label_create(toggleBtnPhoto);
        lv_label_set_text(toggleLblPhoto, LV_SYMBOL_IMAGE " Photo");
        lv_obj_set_style_text_font(toggleLblPhoto, &lv_font_montserrat_14, 0);
        lv_obj_center(toggleLblPhoto);
        lv_obj_add_event_cb(toggleBtnPhoto, mediaTogglePhotoCb, LV_EVENT_CLICKED, nullptr);
        lv_obj_t* toggleBtnAudio = lv_btn_create(toggleRow);
        lv_obj_set_size(toggleBtnAudio, mediaToggleBtnW, MEDIA_TOGGLE_H);
        lv_obj_set_pos(toggleBtnAudio, mediaToggleBtnW + 4, 0);
        lv_obj_set_style_bg_color(toggleBtnAudio, lv_color_hex(0x0a1806), 0);  // inactive
        lv_obj_set_style_radius(toggleBtnAudio, 6, 0);
        lv_obj_t* toggleLblAudio = lv_label_create(toggleBtnAudio);
        lv_label_set_text(toggleLblAudio, LV_SYMBOL_AUDIO " Audio");
        lv_obj_set_style_text_font(toggleLblAudio, &lv_font_montserrat_14, 0);
        lv_obj_center(toggleLblAudio);
        lv_obj_add_event_cb(toggleBtnAudio, mediaToggleAudioCb, LV_EVENT_CLICKED, nullptr);
    }
    lv_obj_t* photoTitle = lv_label_create(dashboardPhotoScreen);
    lv_label_set_text(photoTitle, LV_SYMBOL_IMAGE " Latest photo");
    lv_obj_set_style_text_color(photoTitle, lv_color_hex(0x99bb88), 0);
    lv_obj_set_style_text_font(photoTitle, &lv_font_montserrat_16, 0);
    lv_obj_set_pos(photoTitle, 8, MEDIA_CONTENT_TOP);
    lv_obj_t* photoPlaceholder = lv_label_create(dashboardPhotoScreen);
    lv_label_set_text(photoPlaceholder, "No photo yet");
    lv_obj_set_style_text_color(photoPlaceholder, lv_color_hex(0x668866), 0);
    lv_obj_set_style_text_font(photoPlaceholder, &lv_font_montserrat_14, 0);
    lv_obj_center(photoPlaceholder);
    lv_obj_t* galleryBtn = lv_btn_create(dashboardPhotoScreen);
    applyBtnPressStyle(galleryBtn);
    lv_obj_set_size(galleryBtn, 90, TZT_MIN_TOUCH_H);
    lv_obj_align(galleryBtn, LV_ALIGN_TOP_RIGHT, -8, MEDIA_CONTENT_TOP);
    lv_obj_set_style_radius(galleryBtn, 6, 0);
    lv_obj_t* galleryLbl = lv_label_create(galleryBtn);
    lv_label_set_text(galleryLbl, "Gallery");
    lv_obj_center(galleryLbl);
    lv_obj_add_event_cb(galleryBtn, mediaOpenImagesCb, LV_EVENT_CLICKED, nullptr);

    // ---- Media tab, Audio sub-tab ----
    dashboardAudioScreen = lv_obj_create(NULL);
    setScreenStyle(dashboardAudioScreen);
    {
        lv_obj_t* toggleRow = lv_obj_create(dashboardAudioScreen);
        lv_obj_set_size(toggleRow, TZT_SCREEN_WIDTH - 16, MEDIA_TOGGLE_H);
        lv_obj_set_pos(toggleRow, 8, TZT_CONTENT_TOP);
        lv_obj_set_style_bg_opa(toggleRow, LV_OPA_TRANSP, 0);
        lv_obj_set_style_border_width(toggleRow, 0, 0);
        lv_obj_set_style_pad_all(toggleRow, 0, 0);
        lv_obj_clear_flag(toggleRow, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_t* toggleBtnPhoto = lv_btn_create(toggleRow);
        lv_obj_set_size(toggleBtnPhoto, mediaToggleBtnW, MEDIA_TOGGLE_H);
        lv_obj_set_pos(toggleBtnPhoto, 0, 0);
        lv_obj_set_style_bg_color(toggleBtnPhoto, lv_color_hex(0x0a1806), 0);  // inactive
        lv_obj_set_style_radius(toggleBtnPhoto, 6, 0);
        lv_obj_t* toggleLblPhoto = lv_label_create(toggleBtnPhoto);
        lv_label_set_text(toggleLblPhoto, LV_SYMBOL_IMAGE " Photo");
        lv_obj_set_style_text_font(toggleLblPhoto, &lv_font_montserrat_14, 0);
        lv_obj_center(toggleLblPhoto);
        lv_obj_add_event_cb(toggleBtnPhoto, mediaTogglePhotoCb, LV_EVENT_CLICKED, nullptr);
        lv_obj_t* toggleBtnAudio = lv_btn_create(toggleRow);
        lv_obj_set_size(toggleBtnAudio, mediaToggleBtnW, MEDIA_TOGGLE_H);
        lv_obj_set_pos(toggleBtnAudio, mediaToggleBtnW + 4, 0);
        lv_obj_set_style_bg_color(toggleBtnAudio, lv_color_hex(0x2a5514), 0);  // active
        lv_obj_set_style_radius(toggleBtnAudio, 6, 0);
        lv_obj_t* toggleLblAudio = lv_label_create(toggleBtnAudio);
        lv_label_set_text(toggleLblAudio, LV_SYMBOL_AUDIO " Audio");
        lv_obj_set_style_text_font(toggleLblAudio, &lv_font_montserrat_14, 0);
        lv_obj_center(toggleLblAudio);
        lv_obj_add_event_cb(toggleBtnAudio, mediaToggleAudioCb, LV_EVENT_CLICKED, nullptr);
    }
    lv_obj_t* audioTitle = lv_label_create(dashboardAudioScreen);
    lv_label_set_text(audioTitle, LV_SYMBOL_AUDIO " Latest audio");
    lv_obj_set_style_text_color(audioTitle, lv_color_hex(0x99bb88), 0);
    lv_obj_set_style_text_font(audioTitle, &lv_font_montserrat_16, 0);
    lv_obj_set_pos(audioTitle, 8, MEDIA_CONTENT_TOP);
    dashboardAudioNameLabel = lv_label_create(dashboardAudioScreen);
    lv_label_set_text(dashboardAudioNameLabel, "No audio yet");
    lv_obj_set_style_text_color(dashboardAudioNameLabel, lv_color_hex(0xe8e8e8), 0);
    lv_obj_set_style_text_font(dashboardAudioNameLabel, &lv_font_montserrat_14, 0);
    lv_obj_set_pos(dashboardAudioNameLabel, 8, MEDIA_CONTENT_TOP + TZT_MIN_TOUCH_H + 4);
    lv_obj_set_width(dashboardAudioNameLabel, TZT_SCREEN_WIDTH - 16);
    lv_label_set_long_mode(dashboardAudioNameLabel, LV_LABEL_LONG_SCROLL_CIRCULAR);
    lv_obj_t* playlistBtn = lv_btn_create(dashboardAudioScreen);
    applyBtnPressStyle(playlistBtn);
    lv_obj_set_size(playlistBtn, 90, TZT_MIN_TOUCH_H);
    lv_obj_align(playlistBtn, LV_ALIGN_TOP_RIGHT, -8, MEDIA_CONTENT_TOP);
    lv_obj_set_style_radius(playlistBtn, 6, 0);
    lv_obj_t* playlistLbl = lv_label_create(playlistBtn);
    lv_label_set_text(playlistLbl, "Playlist");
    lv_obj_center(playlistLbl);
    lv_obj_add_event_cb(playlistBtn, mediaOpenAudioCb, LV_EVENT_CLICKED, nullptr);

    currentScreenIndex = 0;
    mediaSubTab = 0;
    lastDashboardTouchTime = millis();
    lastDashboardAutoAdvance = millis();
    lv_scr_load(screen1);  // start on Home tab

    // ---- Bottom navigation bar (icons + labels) ----
    {
        lv_obj_t* top = lv_layer_top();
        navBarContainer = lv_obj_create(top);
        lv_obj_set_size(navBarContainer, TZT_SCREEN_WIDTH, TZT_NAV_BAR_H);
        lv_obj_set_pos(navBarContainer, 0, TZT_SCREEN_HEIGHT - TZT_NAV_BAR_H);
        lv_obj_set_style_bg_color(navBarContainer, lv_color_hex(0x0a1806), 0);
        lv_obj_set_style_border_color(navBarContainer, lv_color_hex(0x2d5016), 0);
        lv_obj_set_style_border_width(navBarContainer, 1, 0);
        lv_obj_set_style_border_side(navBarContainer, LV_BORDER_SIDE_TOP, 0);
        lv_obj_set_style_radius(navBarContainer, 0, 0);
        lv_obj_set_style_pad_all(navBarContainer, 0, 0);
        lv_obj_clear_flag(navBarContainer, LV_OBJ_FLAG_SCROLLABLE);
        const char* navIcons[]  = { LV_SYMBOL_HOME, LV_SYMBOL_EYE_OPEN, LV_SYMBOL_IMAGE, LV_SYMBOL_SETTINGS };
        const char* navLabels[] = { "Home", "Sensors", "Media", "Settings" };
        // Real gaps between tabs so adjacent taps don't misfire (see TZT_NAV_BTN_GAP)
        int btnW = (TZT_SCREEN_WIDTH - TZT_NAV_BTN_GAP * (SCREEN_COUNT + 1)) / SCREEN_COUNT;
        for (int i = 0; i < SCREEN_COUNT; i++) {
            lv_obj_t* btn = lv_btn_create(navBarContainer);
            applyBtnPressStyle(btn);
            lv_obj_set_size(btn, btnW, TZT_NAV_BAR_H - 2);
            lv_obj_set_pos(btn, TZT_NAV_BTN_GAP + i * (btnW + TZT_NAV_BTN_GAP), 1);
            lv_obj_set_style_radius(btn, 6, 0);
            lv_obj_set_style_bg_color(btn, i == 0 ? lv_color_hex(0x2a5514) : lv_color_hex(0x0a1806), 0);
            lv_obj_set_style_border_width(btn, 0, 0);
            lv_obj_set_style_shadow_width(btn, 0, 0);
            lv_obj_set_style_pad_ver(btn, TZT_BTN_PAD_V, 0);
            lv_obj_t* ico = lv_label_create(btn);
            lv_label_set_text(ico, navIcons[i]);
            lv_obj_set_style_text_font(ico, &lv_font_montserrat_16, 0);
            lv_obj_set_style_text_color(ico, lv_color_hex(0xccddbb), 0);
            lv_obj_align(ico, LV_ALIGN_TOP_MID, 0, 0);
            lv_obj_t* lbl = lv_label_create(btn);
            lv_label_set_text(lbl, navLabels[i]);
            lv_obj_set_style_text_font(lbl, &lv_font_montserrat_14, 0);
            lv_obj_set_style_text_color(lbl, lv_color_hex(0x99aa88), 0);
            lv_obj_align(lbl, LV_ALIGN_BOTTOM_MID, 0, 0);
            lv_obj_add_event_cb(btn, navBtnCb, LV_EVENT_CLICKED, (void*)(intptr_t)i);
            navBtns[i] = btn;
        }
    }

    // ---- Status bar (top layer: time + WiFi) ----
    {
        lv_obj_t* top = lv_layer_top();
        statusBar = lv_obj_create(top);
        lv_obj_set_size(statusBar, TZT_SCREEN_WIDTH, TZT_STATUS_H);
        lv_obj_set_pos(statusBar, 0, 0);
        lv_obj_set_style_bg_color(statusBar, lv_color_hex(0x0a1806), 0);
        lv_obj_set_style_bg_opa(statusBar, LV_OPA_COVER, 0);
        lv_obj_set_style_border_color(statusBar, lv_color_hex(0x2d5016), 0);
        lv_obj_set_style_border_width(statusBar, 1, 0);
        lv_obj_set_style_border_side(statusBar, LV_BORDER_SIDE_BOTTOM, 0);
        lv_obj_set_style_radius(statusBar, 0, 0);
        lv_obj_set_style_pad_hor(statusBar, 6, 0);
        lv_obj_set_style_pad_ver(statusBar, 1, 0);
        lv_obj_clear_flag(statusBar, LV_OBJ_FLAG_SCROLLABLE);

        lv_obj_t* appName = lv_label_create(statusBar);
        lv_label_set_text(appName, LV_SYMBOL_HOME " Prick'n'Print");
        lv_obj_set_style_text_font(appName, &lv_font_montserrat_14, 0);
        lv_obj_set_style_text_color(appName, lv_color_hex(0x88aa77), 0);
        lv_obj_align(appName, LV_ALIGN_LEFT_MID, 0, 0);

        statusIpLabel = lv_label_create(statusBar);
        lv_label_set_text(statusIpLabel, "");
        lv_obj_set_style_text_font(statusIpLabel, &lv_font_montserrat_14, 0);
        lv_obj_set_style_text_color(statusIpLabel, lv_color_hex(0x668866), 0);
        lv_obj_align_to(statusIpLabel, appName, LV_ALIGN_OUT_RIGHT_MID, 8, 0);

        // Settings is reached via the bottom nav tab now (was a tiny 28x14 corner button - too small to tap reliably)
        statusTimeLabel = lv_label_create(statusBar);
        lv_label_set_text(statusTimeLabel, "--:--");
        lv_obj_set_style_text_font(statusTimeLabel, &lv_font_montserrat_14, 0);
        lv_obj_set_style_text_color(statusTimeLabel, lv_color_hex(0xaabb99), 0);
        lv_obj_align(statusTimeLabel, LV_ALIGN_RIGHT_MID, -4, 0);
    }
    updateStatusBar();

    // ---- Settings tab (card layout: WiFi + Volume; Back to Home) ----
    settingsScreen = lv_obj_create(NULL);
    setScreenStyle(settingsScreen);
    int setPad = TZT_LIST_PAD, setGap = 6;
    int setCardW = TZT_SCREEN_WIDTH - setPad * 2;
    int setCardH = 56;

    // WiFi card (same style as Status/Media cards)
    lv_obj_t* wifiCard = lv_obj_create(settingsScreen);
    lv_obj_set_size(wifiCard, setCardW, setCardH);
    lv_obj_set_pos(wifiCard, setPad, TZT_CONTENT_TOP);
    lv_obj_set_style_bg_color(wifiCard, lv_color_hex(0x1e4210), 0);
    lv_obj_set_style_border_color(wifiCard, lv_color_hex(0x3a6820), 0);
    lv_obj_set_style_border_width(wifiCard, 1, 0);
    lv_obj_set_style_pad_all(wifiCard, TZT_LIST_PAD, 0);
    lv_obj_set_style_radius(wifiCard, 6, 0);
    lv_obj_clear_flag(wifiCard, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_t* wifiHeader = lv_label_create(wifiCard);
    lv_label_set_text(wifiHeader, LV_SYMBOL_WIFI " WiFi");
    lv_obj_set_style_text_color(wifiHeader, lv_color_hex(0x99bb88), 0);
    lv_obj_set_style_text_font(wifiHeader, &lv_font_montserrat_14, 0);
    lv_obj_set_pos(wifiHeader, 0, 0);
    statusWifiLabel = lv_label_create(wifiCard);
    lv_label_set_text(statusWifiLabel, "--");
    lv_obj_set_style_text_color(statusWifiLabel, lv_color_hex(0x6ab82e), 0);
    lv_obj_set_style_text_font(statusWifiLabel, &lv_font_montserrat_16, 0);
    lv_obj_align(statusWifiLabel, LV_ALIGN_CENTER, 0, 2);

    // Volume card
    int volCardY = TZT_CONTENT_TOP + setCardH + setGap;
    lv_obj_t* volCard = lv_obj_create(settingsScreen);
    lv_obj_set_size(volCard, setCardW, setCardH + 20);
    lv_obj_set_pos(volCard, setPad, volCardY);
    lv_obj_set_style_bg_color(volCard, lv_color_hex(0x1e4210), 0);
    lv_obj_set_style_border_color(volCard, lv_color_hex(0x3a6820), 0);
    lv_obj_set_style_border_width(volCard, 1, 0);
    lv_obj_set_style_pad_all(volCard, TZT_LIST_PAD, 0);
    lv_obj_set_style_radius(volCard, 6, 0);
    lv_obj_clear_flag(volCard, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_t* setVolLbl = lv_label_create(volCard);
    lv_label_set_text(setVolLbl, LV_SYMBOL_VOLUME_MAX " Volume");
    lv_obj_set_style_text_color(setVolLbl, lv_color_hex(0x99bb88), 0);
    lv_obj_set_style_text_font(setVolLbl, &lv_font_montserrat_14, 0);
    lv_obj_set_pos(setVolLbl, 0, 0);
    volumeSlider = lv_slider_create(volCard);
    lv_slider_set_range(volumeSlider, 0, 100);
    lv_slider_set_value(volumeSlider, (int32_t)(audioSvc.getVolume() * 100.0f), LV_ANIM_OFF);
    lv_obj_set_size(volumeSlider, setCardW - 50, 24);
    lv_obj_set_pos(volumeSlider, 0, 24);
    lv_obj_set_style_anim_time(volumeSlider, 0, LV_PART_MAIN);  // no animation so knob follows finger directly
    lv_obj_set_ext_click_area(volumeSlider, 16);                 // easier to hit knob with finger
    lv_obj_set_style_bg_color(volumeSlider, lv_color_hex(0x2d5016), LV_PART_MAIN);
    lv_obj_set_style_bg_color(volumeSlider, lv_color_hex(0x6ab82e), LV_PART_INDICATOR);
    lv_obj_set_style_bg_color(volumeSlider, lv_color_hex(0x8bc34a), LV_PART_KNOB);
    lv_obj_set_style_radius(volumeSlider, 10, LV_PART_MAIN);
    lv_obj_set_style_radius(volumeSlider, 10, LV_PART_INDICATOR);
    lv_obj_set_style_radius(volumeSlider, 14, LV_PART_KNOB);
    lv_obj_set_style_pad_all(volumeSlider, 10, LV_PART_KNOB);    // larger knob for easier drag
    lv_obj_add_event_cb(volumeSlider, volumeSliderCb, LV_EVENT_VALUE_CHANGED, nullptr);
    settingsVolumeValueLabel = lv_label_create(volCard);
    int v0 = (int)(audioSvc.getVolume() * 100.0f);
    lv_label_set_text_fmt(settingsVolumeValueLabel, "%d%%", v0);
    lv_obj_set_style_text_font(settingsVolumeValueLabel, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(settingsVolumeValueLabel, lv_color_hex(0xe8e8e8), 0);
    lv_obj_set_pos(settingsVolumeValueLabel, setCardW - 42, 26);

    // No "Back to Home" button needed - Settings is a normal bottom-nav tab, just tap Home to leave.

    updateStatusBar();  // set WiFi label on Settings screen
    updateNavHighlight(0);
    // Gesture is processed in loop() right before lv_timer_handler() for minimal latency (no 80ms timer delay)
}

static lv_color_t sensorColor(float val, float warnLow, float critLow) {
    if (val < critLow) return lv_color_hex(0xff4444);
    if (val < warnLow) return lv_color_hex(0xffcc00);
    return lv_color_hex(0x66dd44);
}

static void updateControlPanelStatus() {
    if (!labelMoisture) return;
    if (!lastSensorDataValid) {
        lv_label_set_text(labelMoisture, "--");
        lv_label_set_text(labelSanitizer, "--");
        lv_label_set_text(labelLED, "--");
        lv_label_set_text(labelPump, "--");
        if (barMoisture) lv_bar_set_value(barMoisture, 0, LV_ANIM_OFF);
        if (barSanitizer) lv_bar_set_value(barSanitizer, 0, LV_ANIM_OFF);
        if (barLED) lv_bar_set_value(barLED, 0, LV_ANIM_OFF);
        if (homeSensorStrip) lv_label_set_text(homeSensorStrip, LV_SYMBOL_CHARGE " --   San --   LED --   Pump --");
        return;
    }
    char buf[32];
    float m = lastSensorData.moisturePercent;
    float s = lastSensorData.sanitizerLevel;
    int ledPct = (lastSensorData.ledBrightness * 100) / 255;
    bool pumping = lastSensorData.isDispensing;

    if (std::isfinite(m) && m >= 0 && m <= 100) {
        snprintf(buf, sizeof(buf), "%.0f%%", (double)m);
        lv_label_set_text(labelMoisture, buf);
        lv_obj_set_style_text_color(labelMoisture, sensorColor(m, 40, 20), 0);
        if (barMoisture) {
            lv_bar_set_value(barMoisture, (int)m, LV_ANIM_ON);
            lv_obj_set_style_bg_color(barMoisture, sensorColor(m, 40, 20), LV_PART_INDICATOR);
        }
    } else {
        lv_label_set_text(labelMoisture, "--");
    }
    if (std::isfinite(s) && s >= 0 && s <= 100) {
        snprintf(buf, sizeof(buf), "%.0f%%", (double)s);
        lv_label_set_text(labelSanitizer, buf);
        lv_obj_set_style_text_color(labelSanitizer, sensorColor(s, 50, 20), 0);
        if (barSanitizer) {
            lv_bar_set_value(barSanitizer, (int)s, LV_ANIM_ON);
            lv_obj_set_style_bg_color(barSanitizer, sensorColor(s, 50, 20), LV_PART_INDICATOR);
        }
    } else {
        lv_label_set_text(labelSanitizer, "--");
    }
    snprintf(buf, sizeof(buf), "%d%%", ledPct);
    lv_label_set_text(labelLED, buf);
    if (barLED) lv_bar_set_value(barLED, ledPct, LV_ANIM_ON);

    lv_label_set_text(labelPump, pumping ? "ACTIVE" : "Idle");
    lv_obj_set_style_text_color(labelPump, pumping ? lv_color_hex(0x66dd44) : lv_color_hex(0x99aa88), 0);

    // Update home screen sensor summary strip
    if (homeSensorStrip) {
        char strip[64];
        snprintf(strip, sizeof(strip), LV_SYMBOL_CHARGE " %.0f%%   San %.0f%%   LED %d%%   %s",
                 (double)m, (double)s, ledPct, pumping ? "PUMP" : "");
        lv_label_set_text(homeSensorStrip, strip);
    }
    updateStatusBar();
}

// ── Message History screen helpers ──────────────────────────────

static void refreshHistoryList() {
    if (!historyList) return;
    lv_obj_clean(historyList);
    if (!sdCard.isReady()) {
        lv_list_add_text(historyList, "SD card not available");
        return;
    }
    int n = sdCard.getMessageCount();
    if (n == 0) {
        sdCard.loadRecentMessages(MAX_CACHED_MESSAGES);
        n = sdCard.getMessageCount();
    }
    if (n == 0) {
        lv_list_add_text(historyList, "No messages saved yet");
        return;
    }
    // Show newest first
    for (int i = n - 1; i >= 0; i--) {
        const SavedMessage& m = sdCard.getCachedMessage(i);
        char label[MSG_PREVIEW_LEN + 20];
        if (m.timestamp > 1600000000) {
            struct tm ti;
            localtime_r(&m.timestamp, &ti);
            char ts[18];
            strftime(ts, sizeof(ts), "%m/%d %I:%M%p", &ti);
            snprintf(label, sizeof(label), "%s  %s", ts, m.preview);
        } else {
            snprintf(label, sizeof(label), "%s", m.preview);
        }
        lv_obj_t* btn = lv_list_add_btn(historyList, LV_SYMBOL_FILE, label);
        lv_obj_set_height(btn, TZT_LIST_ITEM_H);
        lv_obj_set_style_text_font(btn, &lv_font_montserrat_14, 0);
        lv_obj_set_style_text_color(btn, lv_color_hex(0xe8e8e8), 0);
        lv_obj_set_style_bg_color(btn, lv_color_hex(0x1e4210), 0);
        lv_obj_set_style_pad_ver(btn, TZT_BTN_PAD_V, 0);
        lv_obj_add_event_cb(btn, historyItemCb, LV_EVENT_CLICKED, (void*)(uintptr_t)i);
    }
}

static void historyItemCb(lv_event_t* e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    int idx = (int)(uintptr_t)lv_event_get_user_data(e);
    String full = sdCard.readFullMessage(idx);
    if (full.length() == 0) full = "(could not read message)";
    if (historyDetailLabel) lv_label_set_text(historyDetailLabel, full.c_str());
    if (historyDetail) lv_obj_clear_flag(historyDetail, LV_OBJ_FLAG_HIDDEN);
}

static void historyBackCb(lv_event_t* e) {
    (void)e;
    if (historyDetail) lv_obj_add_flag(historyDetail, LV_OBJ_FLAG_HIDDEN);
}

// ── Audio Player screen helpers ─────────────────────────────────

static void refreshAudioList() {
    if (!audioList) return;
    lv_obj_clean(audioList);
    if (!sdCard.isReady()) {
        lv_list_add_text(audioList, "SD card not available");
        return;
    }
    audioFileCount = sdCard.listAudioFiles(audioFileNames, MAX_AUDIO_FILES);
    if (audioFileCount == 0) {
        lv_list_add_text(audioList, "No audio files on SD");
        return;
    }
    for (int i = 0; i < audioFileCount; i++) {
        lv_obj_t* btn = lv_list_add_btn(audioList, LV_SYMBOL_AUDIO, audioFileNames[i].c_str());
        lv_obj_set_height(btn, TZT_LIST_ITEM_H);
        lv_obj_set_style_text_font(btn, &lv_font_montserrat_14, 0);
        lv_obj_set_style_text_color(btn, lv_color_hex(0xe8e8e8), 0);
        lv_obj_set_style_bg_color(btn, lv_color_hex(0x1e4210), 0);
        lv_obj_set_style_pad_ver(btn, TZT_BTN_PAD_V, 0);
        lv_obj_add_event_cb(btn, audioFileCb, LV_EVENT_CLICKED, (void*)(uintptr_t)i);
    }
    updateAudioHeaderState();
}

static void updateAudioHeaderState() {
    bool playing = audioSvc.isPlaying();
    if (audioHeaderBar) {
        lv_obj_set_style_bg_color(audioHeaderBar, playing ? lv_color_hex(0x1a3d1a) : lv_color_hex(0x0d1f07), 0);
    }
    if (audioStopBtn) {
        lv_obj_set_style_bg_color(audioStopBtn, playing ? lv_color_hex(0xe85555) : lv_color_hex(0xcc3333), 0);
    }
}

static void audioFileCb(lv_event_t* e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    int idx = (int)(uintptr_t)lv_event_get_user_data(e);
    if (idx < 0 || idx >= audioFileCount) return;
    String path = String(AUDIO_DIR) + "/" + audioFileNames[idx];
    // Only one audio at a time: if already playing this file, leave it; else stop any current and play
    if (audioSvc.isPlaying() && audioSvc.currentFile() == path) return;
    audioSvc.playFile(path);
    updateAudioHeaderState();
}

static void audioStopCb(lv_event_t* e) {
    (void)e;
    audioSvc.stop();
    updateAudioHeaderState();
}

// ── Splash (raw TFT before LVGL) and Image Gallery ──────────────

#if !defined(TZT_HEADLESS)
static void drawSplashToTft() {
    if (sdCard.isReady() && SD.exists(SPLASH_PATH))
        drawSdJpegToTft(SPLASH_PATH, TZT_SCREEN_HEIGHT);
}
static void drawSdJpegToTft(const char* path, int maxY) {
    if (maxY <= 0) maxY = TZT_SCREEN_HEIGHT;
    if (!path || path[0] == '\0' || !SD.exists(path)) {
        static uint32_t lastOpenFailLog = 0;
        if ((uint32_t)millis() - lastOpenFailLog > 2000) {
            Serial.println("[Image] open failed: " + String(path ? path : ""));
            lastOpenFailLog = (uint32_t)millis();
        }
        return;
    }
    // Decoder opens the file itself (path must have leading / e.g. /data/images/sample.jpg)
    int decodeResult = JpegDec.decodeSdFile(path);
    if (decodeResult == 0) {
        static uint32_t lastDecodeFailLog = 0;
        static int decodeFailCount = 0;
        static String lastDecodeFailPath;
        if (lastDecodeFailPath != path) { lastDecodeFailPath = path; decodeFailCount = 0; }
        decodeFailCount++;
        if ((uint32_t)millis() - lastDecodeFailLog > 2000) {
            Serial.println("[Image] decode failed: " + String(path));
            lastDecodeFailLog = (uint32_t)millis();
        }
        if (decodeFailCount >= 10) {
            currentImagePath = "";
            lastDecodeFailPath = "";
        }
        return;
    }
    int xpos = 0, ypos = 0;
    uint16_t* pImg;
    uint16_t mcu_w = JpegDec.MCUWidth;
    uint16_t mcu_h = JpegDec.MCUHeight;
    uint32_t max_x = JpegDec.width;
    uint32_t max_y = JpegDec.height;
    bool swapBytes = tft.getSwapBytes();
    tft.setSwapBytes(true);
    uint32_t min_w = (mcu_w < (int)(max_x % mcu_w)) ? mcu_w : (max_x % mcu_w);
    uint32_t min_h = (mcu_h < (int)(max_y % mcu_h)) ? mcu_h : (max_y % mcu_h);
    uint32_t win_w = mcu_w, win_h = mcu_h;
    max_x += xpos;
    max_y += ypos;
    int clip_bottom = (maxY < (int)tft.height()) ? maxY : (int)tft.height();
    while (JpegDec.read()) {
        pImg = JpegDec.pImage;
        int mcu_x = JpegDec.MCUx * mcu_w + xpos;
        int mcu_y = JpegDec.MCUy * mcu_h + ypos;
        if (mcu_y >= clip_bottom) { JpegDec.abort(); break; }
        if (mcu_x + mcu_w <= (int)max_x) win_w = mcu_w; else win_w = min_w;
        if (mcu_y + mcu_h <= (int)max_y) win_h = mcu_h; else win_h = min_h;
        if ((mcu_x + win_w) <= (int)tft.width() && (mcu_y + win_h) <= clip_bottom)
            tft.pushImage(mcu_x, mcu_y, win_w, win_h, pImg);
        else if ((mcu_y + win_h) >= (int)tft.height())
            JpegDec.abort();
    }
    tft.setSwapBytes(swapBytes);
}
#endif

static void refreshImageList() {
    if (!imageList) return;
    lv_obj_clean(imageList);
    if (!sdCard.isReady()) {
        lv_list_add_text(imageList, "SD card not available");
        return;
    }
    imageFileCount = sdCard.listImageFiles(imageFileNames, MAX_IMAGE_FILES);
    if (imageFileCount == 0) {
        lv_list_add_text(imageList, "No images on SD");
        return;
    }
    for (int i = 0; i < imageFileCount; i++) {
        lv_obj_t* btn = lv_list_add_btn(imageList, LV_SYMBOL_IMAGE, imageFileNames[i].c_str());
        lv_obj_set_height(btn, TZT_LIST_ITEM_H);
        lv_obj_set_style_text_font(btn, &lv_font_montserrat_14, 0);
        lv_obj_set_style_text_color(btn, lv_color_hex(0xe8e8e8), 0);
        lv_obj_set_style_bg_color(btn, lv_color_hex(0x1e4210), 0);
        lv_obj_set_style_pad_ver(btn, TZT_BTN_PAD_V, 0);
        lv_obj_add_event_cb(btn, imageFileCb, LV_EVENT_CLICKED, (void*)(uintptr_t)i);
    }
}

static void imageFileCb(lv_event_t* e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    int idx = (int)(uintptr_t)lv_event_get_user_data(e);
    if (idx < 0 || idx >= imageFileCount) return;
    String path = String(IMAGES_DIR) + "/" + imageFileNames[idx];
    String lower = imageFileNames[idx];
    lower.toLowerCase();
#if !defined(TZT_HEADLESS)
    lastDashboardImagePath = path;
    if (lower.endsWith(".jpg") || lower.endsWith(".jpeg")) {
        currentImagePath = path;
        hideNavBar();
        lv_scr_load(imageViewerScreen);
    } else {
        Serial.println("[Image] Only JPEG is displayed; file is not .jpg/.jpeg: " + path);
        currentImagePath = "";
        hideNavBar();
        lv_scr_load(imageViewerScreen);
    }
#else
    (void)path;
    (void)lower;
#endif
}

static void imageBackCb(lv_event_t* e) {
    (void)e;
    currentImagePath = "";
    lv_scr_load(imageListScreen);  // stay in Media flow; nav still hidden until Back to Media
}
#endif

void loadGroceries() {
    groceryCount = 0;
    String response;
    if (!sdCard.isReady() || !sdCard.readFile(DATA_DIR "/groceries.json", response)) return;
    if (response == "null" || response.length() == 0 || response == "{}") return;
    size_t capacity = response.length() + 200;
    DynamicJsonDocument doc(capacity);
    if (deserializeJson(doc, response)) return;
    JsonArray arr = doc.as<JsonArray>();
    for (size_t i = 0; i < arr.size() && groceryCount < MAX_GROCERY_ITEMS; i++)
        groceryItems[groceryCount++] = arr[i].as<String>();
}

void saveGroceries() {
    if (!sdCard.isReady()) return;
    DynamicJsonDocument doc(4096);
    JsonArray arr = doc.to<JsonArray>();
    for (int i = 0; i < groceryCount; i++) arr.add(groceryItems[i]);
    String json; serializeJson(doc, json);
    sdCard.writeFile(DATA_DIR "/groceries.json", json);
}

void loadSettingsFromSD() {
    if (!sdCard.isReady()) return;
    String response;
    if (!sdCard.readFile(DATA_DIR "/config.json", response) || response.length() == 0) return;
    DynamicJsonDocument doc(512);
    if (deserializeJson(doc, response) != DeserializationError::Ok) return;
    if (doc["ledBrightness"].is<uint8_t>()) {
        lastSensorData.ledBrightness = doc["ledBrightness"].as<uint8_t>();
        lastSensorDataValid = true;
    }
    if (doc["ledBrightness"].is<int>()) {
        int v = doc["ledBrightness"].as<int>();
        if (v >= 0 && v <= 255) { lastSensorData.ledBrightness = (uint8_t)v; lastSensorDataValid = true; }
    }
    if (doc["autoDispense"].is<bool>()) lastSensorData.autoDispense = doc["autoDispense"].as<bool>();
    if (doc["autoBrightness"].is<bool>()) lastSensorData.autoBrightness = doc["autoBrightness"].as<bool>();
    if (doc["pumpDurationTenths"].is<int>()) { int v = doc["pumpDurationTenths"].as<int>(); if (v >= 0 && v <= 255) lastPumpDurationTenths = v; }
    if (doc["pumpCooldownTenths"].is<int>()) { int v = doc["pumpCooldownTenths"].as<int>(); if (v >= 0 && v <= 255) lastPumpCooldownTenths = v; }
    // Queue settings to send to Main so it remembers after restart
    bootSettingsCount = 0;
    if (bootSettingsCount < BOOT_SETTINGS_MAX && (doc["ledBrightness"].is<int>() || doc["ledBrightness"].is<uint8_t>())) {
        int b = doc["ledBrightness"].is<int>() ? doc["ledBrightness"].as<int>() : (int)doc["ledBrightness"].as<uint8_t>();
        if (b >= 0 && b <= 255) {
            bootSettingsToSend[bootSettingsCount].type = CMD_SET_LED_BRIGHTNESS;
            bootSettingsToSend[bootSettingsCount].p1 = (uint8_t)b;
            bootSettingsToSend[bootSettingsCount].p2 = b > 0 ? 1 : 0;
            bootSettingsCount++;
        }
    }
    if (bootSettingsCount < BOOT_SETTINGS_MAX && doc["autoDispense"].is<bool>()) {
        bootSettingsToSend[bootSettingsCount].type = CMD_SET_AUTO_DISPENSE;
        bootSettingsToSend[bootSettingsCount].p1 = doc["autoDispense"].as<bool>() ? 1 : 0;
        bootSettingsToSend[bootSettingsCount].p2 = 0;
        bootSettingsCount++;
    }
    if (bootSettingsCount < BOOT_SETTINGS_MAX && doc["autoBrightness"].is<bool>()) {
        bootSettingsToSend[bootSettingsCount].type = CMD_SET_AUTO_BRIGHTNESS;
        bootSettingsToSend[bootSettingsCount].p1 = doc["autoBrightness"].as<bool>() ? 1 : 0;
        bootSettingsToSend[bootSettingsCount].p2 = 0;
        bootSettingsCount++;
    }
    if (bootSettingsCount < BOOT_SETTINGS_MAX && doc["pumpDurationTenths"].is<int>()) {
        int v = doc["pumpDurationTenths"].as<int>();
        if (v >= 0 && v <= 255) {
            bootSettingsToSend[bootSettingsCount].type = CMD_SET_PUMP_DURATION;
            bootSettingsToSend[bootSettingsCount].p1 = (uint8_t)v;
            bootSettingsToSend[bootSettingsCount].p2 = 0;
            bootSettingsCount++;
        }
    }
    if (bootSettingsCount < BOOT_SETTINGS_MAX && doc["pumpCooldownTenths"].is<int>()) {
        int v = doc["pumpCooldownTenths"].as<int>();
        if (v >= 0 && v <= 255) {
            bootSettingsToSend[bootSettingsCount].type = CMD_SET_PUMP_COOLDOWN;
            bootSettingsToSend[bootSettingsCount].p1 = (uint8_t)v;
            bootSettingsToSend[bootSettingsCount].p2 = 0;
            bootSettingsCount++;
        }
    }
    bootSettingsIndex = 0;
    if (bootSettingsCount > 0 && espNowInitialized)
        applyNextBootSetting();
}

static void applyNextBootSetting() {
    if (bootSettingsIndex >= bootSettingsCount) return;
    pendingSettingCommandType = bootSettingsToSend[bootSettingsIndex].type;
    pendingSettingParam1 = bootSettingsToSend[bootSettingsIndex].p1;
    pendingSettingParam2 = bootSettingsToSend[bootSettingsIndex].p2;
    pendingSettingMessage[0] = '\0';
    pendingSettingRemainingSends = SETTINGS_MAX_ATTEMPTS;
    pendingSettingNextSendAt = millis();
    bootSettingsIndex++;
}

void saveSettingsToSD() {
    if (!sdCard.isReady()) return;
    DynamicJsonDocument doc(512);
    doc["ledBrightness"] = lastSensorDataValid ? (int)lastSensorData.ledBrightness : 0;
    doc["autoDispense"] = lastSensorDataValid ? lastSensorData.autoDispense : false;
    doc["autoBrightness"] = lastSensorDataValid ? lastSensorData.autoBrightness : false;
    if (lastPumpDurationTenths >= 0) doc["pumpDurationTenths"] = lastPumpDurationTenths;
    if (lastPumpCooldownTenths >= 0) doc["pumpCooldownTenths"] = lastPumpCooldownTenths;
    String json; serializeJson(doc, json);
    sdCard.writeFile(DATA_DIR "/config.json", json);
}

void loadTodos() {
    todoCount = 0;
    String response;
    if (!sdCard.isReady() || !sdCard.readFile(DATA_DIR "/todos.json", response)) return;
    if (response == "null" || response.length() == 0 || response == "{}") return;
    size_t capacity = response.length() + 200;
    DynamicJsonDocument doc(capacity);
    if (deserializeJson(doc, response)) return;
    JsonArray arr = doc.as<JsonArray>();
    for (size_t i = 0; i < arr.size() && todoCount < MAX_TODO_ITEMS; i++)
        todoItems[todoCount++] = arr[i].as<String>();
}

void saveTodos() {
    if (!sdCard.isReady()) return;
    DynamicJsonDocument doc(4096);
    JsonArray arr = doc.to<JsonArray>();
    for (int i = 0; i < todoCount; i++) arr.add(todoItems[i]);
    String json; serializeJson(doc, json);
    sdCard.writeFile(DATA_DIR "/todos.json", json);
}

bool isAuthenticated() {
    // Check token expiry
    if (authToken.length() > 0 && millis() < authTokenExpiry) {
        return true;
    }
    
    // Check cookie
    String cookie = server.header("Cookie");
    if (cookie.indexOf("auth=") >= 0) {
        int start = cookie.indexOf("auth=") + 5;
        int end = cookie.indexOf(";", start);
        if (end < 0) end = cookie.length();
        String token = cookie.substring(start, end);
        if (token == authToken && authToken.length() > 0 && millis() < authTokenExpiry) {
            return true;
        }
    }
    
    // Check URL parameter
    String tokenParam = server.arg("token");
    if (tokenParam.length() > 0 && tokenParam == authToken && authToken.length() > 0 && millis() < authTokenExpiry) {
        return true;
    }
    
    return false;
}

void handleLogin() {
    if (isAuthenticated() && server.method() == HTTP_GET) {
        server.sendHeader("Location", "/");
        server.send(302, "text/plain", "");
        return;
    }
    
    if (server.method() == HTTP_POST) {
        String password = server.arg("password");
        password.trim();
        
        if (password == webPassword) {
            authToken = String(millis()) + String(random(1000, 9999));
            authTokenExpiry = millis() + AUTH_TOKEN_DURATION;
            
            String cookieHeader = "auth=" + authToken + "; Path=/; Max-Age=3600; SameSite=Lax";
            server.sendHeader("Set-Cookie", cookieHeader);
            
            String redirectUrl = "/?token=" + authToken;
            String redirectPage = "<!DOCTYPE html><html><head><meta charset=\"UTF-8\"><script>"
                                 "document.cookie=\"auth=" + authToken + "; Path=/; Max-Age=3600; SameSite=Lax\";"
                                 "setTimeout(function(){window.location.href=\"" + redirectUrl + "\";},50);"
                                 "</script></head><body><p>Login successful! Redirecting...</p></body></html>";
            
            server.send(200, "text/html", redirectPage);
        } else {
            delay(1000);
            server.sendHeader("Location", "/login?error=1");
            server.send(302, "text/plain", "");
        }
        return;
    }
    
    String errorMsg = server.hasArg("error") ? "<p class=\"error-msg\">❌ Incorrect password</p>" : "";
    
    String loginPage = R"HTML(
<!DOCTYPE html>
<html lang="en">
<head>
    <meta charset="UTF-8">
    <meta name="viewport" content="width=device-width, initial-scale=1, viewport-fit=cover, maximum-scale=1, user-scalable=no">
    <meta name="apple-mobile-web-app-capable" content="yes">
    <meta name="theme-color" content="#1a3d0e">
    <title>Login - Print-n-Prick</title>
    <style>
        * { margin: 0; padding: 0; box-sizing: border-box; -webkit-tap-highlight-color: rgba(74, 124, 42, 0.2); }
        body {
            font-family: -apple-system, BlinkMacSystemFont, 'Segoe UI', Roboto, sans-serif;
            background: linear-gradient(180deg, #0d1f07 0%, #1a3d0e 50%, #2d5016 100%);
            min-height: 100vh;
            min-height: 100dvh;
            margin: 0;
            padding: max(12px, env(safe-area-inset-top)) max(12px, env(safe-area-inset-right)) max(12px, env(safe-area-inset-bottom)) max(12px, env(safe-area-inset-left));
            color: #333;
            position: relative;
            -webkit-font-smoothing: antialiased;
        }
        body::before { content: '🌵'; position: fixed; font-size: 150px; opacity: 0.03; top: -50px; left: -50px; z-index: 0; }
        body::after { content: '🖨️'; position: fixed; font-size: 120px; opacity: 0.03; bottom: -40px; right: -40px; z-index: 0; }
        @media (min-width: 768px) {
            body::before { font-size: 300px; top: -100px; left: -100px; }
            body::after { font-size: 250px; bottom: -80px; right: -80px; }
        }
        body.login-page {
            display: flex;
            align-items: center;
            justify-content: center;
            padding: 16px;
            overflow: hidden;
        }
        body.login-page::before { font-size: 120px; opacity: 0.05; top: -30px; left: -30px; transform: rotate(-15deg); }
        body.login-page::after { font-size: 100px; opacity: 0.05; bottom: -20px; right: -20px; transform: rotate(15deg); }
        @media (min-width: 768px) {
            body.login-page { padding: 20px; }
            body.login-page::before { font-size: 200px; top: -50px; left: -50px; }
            body.login-page::after { font-size: 150px; bottom: -30px; right: -30px; }
        }
        .login-container {
            background: linear-gradient(135deg, #f5f5f0 0%, #ffffff 100%);
            border-radius: 20px;
            padding: 32px 24px;
            box-shadow: 0 20px 60px rgba(26, 18, 12, 0.5), 0 0 0 3px rgba(74, 124, 42, 0.2);
            border: 3px solid #4a7c2a;
            max-width: 420px;
            width: 100%;
            position: relative;
            z-index: 1;
        }
        @media (min-width: 768px) {
            .login-container { border-radius: 24px; padding: 50px 40px; }
        }
        .logo { text-align: center; margin-bottom: 32px; }
        @media (min-width: 768px) { .logo { margin-bottom: 40px; } }
        .logo-icon { font-size: 56px; margin-bottom: 10px; display: block; animation: float 3s ease-in-out infinite; }
        @media (min-width: 768px) { .logo-icon { font-size: 64px; } }
        @keyframes float { 0%, 100% { transform: translateY(0px); } 50% { transform: translateY(-10px); } }
        h1 { color: #2d5016; font-weight: 700; text-shadow: 2px 2px 4px rgba(0,0,0,0.1); }
        .tagline { font-style: italic; }
        input[type="password"] {
            width: 100%;
            padding: 16px 18px;
            border: 3px solid #4a7c2a;
            border-bottom: 3px solid #3e2723;
            border-radius: 12px;
            font-size: 16px;
            margin-bottom: 20px;
            font-family: inherit;
            background: #fff;
            transition: all 0.3s;
            box-shadow: inset 0 2px 4px rgba(0,0,0,0.1);
        }
        @media (min-width: 768px) {
            input[type="password"] { padding: 18px 20px; font-size: 18px; }
        }
        input[type="password"]:focus {
            outline: none;
            border-color: #6b9f3d;
            border-bottom-color: #8b4513;
            box-shadow: 0 0 0 4px rgba(74, 124, 42, 0.2), inset 0 2px 4px rgba(0,0,0,0.1);
        }
        .login-page h1 { font-size: 32px; text-align: center; margin-bottom: 10px; }
        @media (min-width: 768px) { .login-page h1 { font-size: 36px; } }
        .login-page .tagline { color: #5d4037; font-size: 13px; margin-bottom: 28px; text-align: center; }
        @media (min-width: 768px) { .login-page .tagline { font-size: 14px; margin-bottom: 30px; } }
        .login-page button {
            width: 100%;
            min-height: 44px;
            padding: 14px 18px;
            font-size: 17px;
            border: none;
            border-radius: 12px;
            background: linear-gradient(180deg, #2d5016 0%, #4a7c2a 100%);
            color: white;
            font-weight: 600;
            cursor: pointer;
            box-shadow: 0 4px 12px rgba(26, 18, 12, 0.4);
            -webkit-touch-callout: none;
            user-select: none;
        }
        @media (min-width: 768px) { .login-page button { font-size: 18px; } }
        .login-page button:active { box-shadow: 0 2px 8px rgba(45, 80, 22, 0.4); transform: scale(0.98); }
        @media (hover: hover) { .login-page button:hover { box-shadow: 0 6px 16px rgba(45, 80, 22, 0.5); } }
        .error-msg { color: #8b4513; text-align: center; margin-top: 10px; font-size: 14px; }
        @supports (padding: env(safe-area-inset-bottom)) {
            body { padding-top: max(12px, env(safe-area-inset-top)); padding-bottom: max(12px, env(safe-area-inset-bottom)); padding-left: max(12px, env(safe-area-inset-left)); padding-right: max(12px, env(safe-area-inset-right)); }
        }
    </style>
</head>
<body class="login-page">
    <div class="login-container">
        <div class="logo">
            <span class="logo-icon">🌵💌</span>
            <h1>Print-n-Prick</h1>
            <p class="tagline">Smart Cactus Care & Thermal Printing</p>
        </div>
        <form method="POST" action="/login" enctype="application/x-www-form-urlencoded">
            <input type="password" name="password" placeholder="🔒 Enter password..." required autofocus autocomplete="current-password">
            <button type="submit">Login</button>
        </form>
        )HTML" + errorMsg + R"HTML(
    </div>
</body>
</html>
)HTML";
    
    server.send(200, "text/html", loginPage);
}

void handleLogout() {
    authToken = "";
    authTokenExpiry = 0;
    server.sendHeader("Set-Cookie", "auth=; Path=/; Max-Age=0");
    server.sendHeader("Location", "/login");
    server.send(302, "text/plain", "");
}

// API endpoint for shortcuts - accepts password as query parameter
void handleApiLogin() {
    String password = server.arg("password");
    password.trim();
    
    if (password == webPassword) {
        authToken = String(millis()) + String(random(1000, 9999));
        authTokenExpiry = millis() + AUTH_TOKEN_DURATION;
        
        String cookieHeader = "auth=" + authToken + "; Path=/; Max-Age=3600; SameSite=Lax";
        server.sendHeader("Set-Cookie", cookieHeader);
        
        DynamicJsonDocument doc(256);
        doc["success"] = true;
        doc["message"] = "Login successful";
        doc["token"] = authToken;
        String response;
        serializeJson(doc, response);
        server.send(200, "application/json", response);
    } else {
        DynamicJsonDocument doc(128);
        doc["success"] = false;
        doc["error"] = "Invalid password";
        String response;
        serializeJson(doc, response);
        server.send(401, "application/json", response);
    }
}

void handleGetStatus() {
    if (!isAuthenticated()) { server.send(401, "application/json", JSON_FAIL_UNAUTHORIZED); return; }
    DynamicJsonDocument doc(384);
    if (lastSensorDataValid) {
        doc["moisture"] = lastSensorData.moisturePercent;
        doc["sanitizer"] = lastSensorData.sanitizerLevel;
        doc["irSensor"] = lastSensorData.irDetected;
        doc["light"] = lastSensorData.lightPercent;
        doc["ledBrightness"] = lastSensorData.ledBrightness;
        doc["dispensing"] = lastSensorData.isDispensing;
        doc["autoDispense"] = lastSensorData.autoDispense;
        doc["autoBrightness"] = lastSensorData.autoBrightness;
    } else {
        doc["moisture"] = 0; doc["sanitizer"] = 0; doc["irSensor"] = false;
        doc["light"] = 0; doc["ledBrightness"] = 0; doc["dispensing"] = false;
        doc["autoDispense"] = false; doc["autoBrightness"] = false;
    }
    if (lastPumpDurationTenths >= 0) doc["pumpDurationTenths"] = lastPumpDurationTenths;
    if (lastPumpCooldownTenths >= 0) doc["pumpCooldownTenths"] = lastPumpCooldownTenths;
    String s; serializeJson(doc, s);
    server.send(200, "application/json", s);
}

void handleResetSanitizer() {
    if (!isAuthenticated()) { server.send(401, "application/json", JSON_FAIL_UNAUTHORIZED); return; }
    bool ok = sendCommandViaESPNow(CMD_RESET_SANITIZER);
    server.send(200, "application/json", ok ? JSON_SUCCESS : "{\"success\":false,\"error\":\"ESP-NOW not available\"}");
}

void handleGetReminders() {
    if (!isAuthenticated()) { server.send(401, "application/json", JSON_FAIL_UNAUTHORIZED); return; }
    // Return only pending reminders (not yet printed) so list hides them after they fire
    String j = reminderService ? reminderService->toJSONForDisplay() : "{}";
    server.send(200, "application/json", j);
}

void handleAddReminder() {
    if (!isAuthenticated()) { server.send(401, "application/json", JSON_FAIL_UNAUTHORIZED); return; }
    DynamicJsonDocument doc(1024);  // Allow long messages (e.g. 600+ chars)
    if (deserializeJson(doc, server.arg("plain")) || !doc.containsKey("message") || !doc.containsKey("scheduledTime")) {
        server.send(400, "application/json", "{\"success\":false,\"error\":\"Invalid body\"}"); return;
    }
    // Update in-memory reminder list immediately
    String id = reminderService->addReminder(doc["message"].as<String>(), (time_t)doc["scheduledTime"].as<long>());
    if (id.length() == 0) { server.send(400, "application/json", "{\"success\":false}"); return; }
    // Return response immediately (UI updates instantly)
    server.send(200, "application/json", "{\"success\":true,\"id\":\"" + id + "\"}");
    // Mark for background save (non-blocking)
    remindersNeedSave = true;
}

void handleDeleteReminder() {
    if (!isAuthenticated()) { server.send(401, "application/json", JSON_FAIL_UNAUTHORIZED); return; }
    String uri = server.uri();
    int i = uri.lastIndexOf('/');
    String id = (i >= 0 && (size_t)(i+1) < uri.length()) ? uri.substring(i + 1) : "";
    if (id.length() == 0 || !reminderService->deleteReminder(id)) {
        server.send(404, "application/json", "{\"success\":false}"); return;
    }
    // Return response immediately (UI updates instantly)
    server.send(200, "application/json", JSON_SUCCESS);
    // Mark for background save (non-blocking)
    remindersNeedSave = true;
}

void handleGetGroceries() {
    if (!isAuthenticated()) { server.send(401, "application/json", JSON_FAIL_UNAUTHORIZED); return; }
    DynamicJsonDocument doc(2048);
    JsonArray arr = doc.to<JsonArray>();
    for (int i = 0; i < groceryCount; i++) arr.add(groceryItems[i]);
    String s; serializeJson(doc, s);
    server.send(200, "application/json", s);
}

void handleAddGrocery() {
    if (!isAuthenticated()) { server.send(401, "application/json", JSON_FAIL_UNAUTHORIZED); return; }
    DynamicJsonDocument doc(256);
    if (deserializeJson(doc, server.arg("plain")) || !doc["item"]) {
        server.send(400, "application/json", "{\"success\":false}"); return;
    }
    String item = doc["item"].as<String>();
    if (item.length() == 0 || groceryCount >= MAX_GROCERY_ITEMS) {
        server.send(400, "application/json", "{\"success\":false}"); return;
    }
    // Update in-memory array immediately
    groceryItems[groceryCount++] = item;
    // Return response immediately (UI updates instantly)
    server.send(200, "application/json", JSON_SUCCESS);
    // Mark for background save (non-blocking)
    groceriesNeedSave = true;
}

void handleClearGroceries() {
    if (!isAuthenticated()) { server.send(401, "application/json", JSON_FAIL_UNAUTHORIZED); return; }
    // Update in-memory array immediately
    groceryCount = 0;
    // Return response immediately (UI updates instantly)
    server.send(200, "application/json", JSON_SUCCESS);
    // Mark for background save (non-blocking)
    groceriesNeedSave = true;
}

void handleDeleteGrocery() {
    if (!isAuthenticated()) { server.send(401, "application/json", JSON_FAIL_UNAUTHORIZED); return; }
    String uri = server.uri();
    int i = uri.lastIndexOf('/');
    int idx = (i >= 0 && (size_t)(i+1) < uri.length()) ? uri.substring(i + 1).toInt() : -1;
    if (idx < 0 || idx >= groceryCount) { server.send(404, "application/json", "{\"success\":false}"); return; }
    // Update in-memory array immediately
    for (int k = idx; k < groceryCount - 1; k++) groceryItems[k] = groceryItems[k + 1];
    groceryCount--;
    // Return response immediately (UI updates instantly)
    server.send(200, "application/json", JSON_SUCCESS);
    // Mark for background save (non-blocking)
    groceriesNeedSave = true;
}

void handlePrintGroceries() {
    if (!isAuthenticated()) { server.send(401, "application/json", JSON_FAIL_UNAUTHORIZED); return; }
    size_t estimatedSize = 100;
    for (int i = 0; i < groceryCount; i++) estimatedSize += 10 + groceryItems[i].length();
    String msg;
    msg.reserve(estimatedSize);
    msg = "GROCERY LIST\n--------------------------------\n";
    for (int i = 0; i < groceryCount; i++) {
        msg += String(i + 1);
        msg += ". ";
        msg += groceryItems[i];
        msg += "\n";
    }
    if (groceryCount == 0) msg += "(empty)\n";
    msg += "--------------------------------\n";
    time_t now = time(nullptr);
    struct tm* t = localtime(&now);
    char dateBuf[32];
    strftime(dateBuf, sizeof(dateBuf), "%b %d, %Y %I:%M %p", t);
    msg += "Date: ";
    msg += dateBuf;
    bool ok = sendPrintChunked(msg);
    server.send(200, "application/json", ok ? JSON_SUCCESS : "{\"success\":false,\"error\":\"ESP-NOW not available\"}");
}

void handleGetTodos() {
    if (!isAuthenticated()) { server.send(401, "application/json", JSON_FAIL_UNAUTHORIZED); return; }
    DynamicJsonDocument doc(2048);
    JsonArray arr = doc.to<JsonArray>();
    for (int i = 0; i < todoCount; i++) arr.add(todoItems[i]);
    String s; serializeJson(doc, s);
    server.send(200, "application/json", s);
}

void handleAddTodo() {
    if (!isAuthenticated()) { server.send(401, "application/json", JSON_FAIL_UNAUTHORIZED); return; }
    DynamicJsonDocument doc(256);
    if (deserializeJson(doc, server.arg("plain")) || !doc["item"]) {
        server.send(400, "application/json", "{\"success\":false}"); return;
    }
    String item = doc["item"].as<String>();
    if (item.length() == 0 || todoCount >= MAX_TODO_ITEMS) {
        server.send(400, "application/json", "{\"success\":false}"); return;
    }
    todoItems[todoCount++] = item;
    server.send(200, "application/json", JSON_SUCCESS);
    todosNeedSave = true;
}

void handleClearTodos() {
    if (!isAuthenticated()) { server.send(401, "application/json", JSON_FAIL_UNAUTHORIZED); return; }
    todoCount = 0;
    server.send(200, "application/json", JSON_SUCCESS);
    todosNeedSave = true;
}

void handleDeleteTodo() {
    if (!isAuthenticated()) { server.send(401, "application/json", JSON_FAIL_UNAUTHORIZED); return; }
    String uri = server.uri();
    int i = uri.lastIndexOf('/');
    int idx = (i >= 0 && (size_t)(i+1) < uri.length()) ? uri.substring(i + 1).toInt() : -1;
    if (idx < 0 || idx >= todoCount) { server.send(404, "application/json", "{\"success\":false}"); return; }
    for (int k = idx; k < todoCount - 1; k++) todoItems[k] = todoItems[k + 1];
    todoCount--;
    server.send(200, "application/json", JSON_SUCCESS);
    todosNeedSave = true;
}

void handlePrintTodos() {
    if (!isAuthenticated()) { server.send(401, "application/json", JSON_FAIL_UNAUTHORIZED); return; }
    time_t now = time(nullptr);
    struct tm* t = localtime(&now);
    char dateBuf[32];
    strftime(dateBuf, sizeof(dateBuf), "%b %d, %Y %I:%M %p", t);
    String msg = "TODO LIST\n--------------------------------\n";
    for (int i = 0; i < todoCount; i++) msg += String(i+1) + ". " + todoItems[i] + "\n";
    if (todoCount == 0) msg += "(empty)\n";
    msg += "--------------------------------\n";
    msg += "Date: ";
    msg += dateBuf;
    bool ok = sendPrintChunked(msg);
    server.send(200, "application/json", ok ? JSON_SUCCESS : "{\"success\":false,\"error\":\"ESP-NOW not available\"}");
}

void handleHealth() {
    if (!isAuthenticated()) { server.send(401, "application/json", JSON_FAIL_UNAUTHORIZED); return; }
    DynamicJsonDocument doc(512);
    doc["healthy"] = true;
    doc["firmware"] = FIRMWARE_VERSION;
    doc["sensorsValid"] = lastSensorDataValid;
    if (lastSensorDataValid) {
        doc["moisture"] = lastSensorData.moisturePercent;
        doc["sanitizer"] = lastSensorData.sanitizerLevel;
    }
    String s; serializeJson(doc, s);
    server.send(200, "application/json", s);
}

void handleQueueStatus() {
    if (!isAuthenticated()) { server.send(401, "application/json", JSON_FAIL_UNAUTHORIZED); return; }
    server.send(200, "application/json", "{\"size\":0,\"isEmpty\":true}");
}

void handleTestPump() {
    if (!isAuthenticated()) { server.send(401, "application/json", JSON_FAIL_UNAUTHORIZED); return; }
    bool ok = sendCommandViaESPNow(CMD_TEST_PUMP);
    server.send(200, "application/json", ok ? JSON_SUCCESS : "{\"success\":false,\"error\":\"ESP-NOW not available\"}");
}

void handlePumpControl() {
    if (!isAuthenticated()) { server.send(401, "application/json", JSON_FAIL_UNAUTHORIZED); return; }
    String body = server.arg("plain");
    DynamicJsonDocument doc(128);
    if (body.length() == 0 || deserializeJson(doc, body)) {
        // Invalid body (no log to reduce clutter)
        server.send(400, "application/json", "{\"success\":false,\"error\":\"Invalid JSON\"}");
        return;
    }
    String action = doc["action"].as<String>();
    action.trim();
    action.toLowerCase();
    bool ok = false;
    if (action == "start") {
        lastSensorData.isDispensing = true;
        lastSensorDataValid = true;
#if !defined(TZT_HEADLESS)
        updateControlPanelStatus();
#endif
        ok = sendPumpCommandRepeated(CMD_DISPENSE_START);
    } else if (action == "stop") {
        lastSensorData.isDispensing = false;
        lastSensorDataValid = true;
#if !defined(TZT_HEADLESS)
        updateControlPanelStatus();
#endif
        ok = sendPumpCommandRepeated(CMD_DISPENSE_STOP);
    } else {
        // Invalid action (no log)
        server.send(400, "application/json", "{\"success\":false,\"error\":\"Invalid action. Use 'start' or 'stop'\"}");
        return;
    }
    if (!ok) Serial.println("❌ Pump control: send failed");
    server.send(200, "application/json", ok ? JSON_SUCCESS : "{\"success\":false,\"error\":\"ESP-NOW send failed or not available\"}");
}

void handleTestLED12V() {
    if (!isAuthenticated()) { server.send(401, "application/json", JSON_FAIL_UNAUTHORIZED); return; }
    String body = server.arg("plain");
    uint8_t b = 0; bool st = false;
    if (body.length() > 0) { 
        DynamicJsonDocument d(128); 
        if (!deserializeJson(d, body)) {
            if (d["brightness"].is<uint8_t>()) b = d["brightness"].as<uint8_t>();
            else if (d["brightness"].is<int>()) b = (uint8_t)d["brightness"].as<int>();
            if (d["state"].is<bool>()) st = d["state"].as<bool>();
            else if (b > 0) st = true;  // If brightness > 0, assume state is true
        }
    }
    lastSensorData.ledBrightness = b;
    lastSensorDataValid = true;
    settingsNeedSave = true;
#if !defined(TZT_HEADLESS)
    updateControlPanelStatus();
#endif
    enqueueSettingCommand(CMD_SET_LED_BRIGHTNESS, b, st ? 1 : 0, "");
    server.send(200, "application/json", JSON_SUCCESS);
}

void handleSetPumpDuration() {
    if (!isAuthenticated()) { server.send(401, "application/json", JSON_FAIL_UNAUTHORIZED); return; }
    DynamicJsonDocument d(128);
    if (deserializeJson(d, server.arg("plain")) || !d["duration"].is<int>()) {
        server.send(400, "application/json", "{\"success\":false,\"error\":\"Invalid body, need duration 0-255\"}"); return;
    }
    int v = d["duration"].as<int>();
    if (v < 0) v = 0; if (v > 255) v = 255;
    lastPumpDurationTenths = v;
    settingsNeedSave = true;
    enqueueSettingCommand(CMD_SET_PUMP_DURATION, (uint8_t)v, 0, "");
    server.send(200, "application/json", JSON_SUCCESS);
}

void handleSetPumpCooldown() {
    if (!isAuthenticated()) { server.send(401, "application/json", JSON_FAIL_UNAUTHORIZED); return; }
    DynamicJsonDocument d(128);
    if (deserializeJson(d, server.arg("plain")) || !d["cooldown"].is<int>()) {
        server.send(400, "application/json", "{\"success\":false,\"error\":\"Invalid body, need cooldown 0-255\"}"); return;
    }
    int v = d["cooldown"].as<int>();
    if (v < 0) v = 0; if (v > 255) v = 255;
    lastPumpCooldownTenths = v;
    settingsNeedSave = true;
    enqueueSettingCommand(CMD_SET_PUMP_COOLDOWN, (uint8_t)v, 0, "");
    server.send(200, "application/json", JSON_SUCCESS);
}

void handleSetLightCalibration() {
    if (!isAuthenticated()) { server.send(401, "application/json", JSON_FAIL_UNAUTHORIZED); return; }
    DynamicJsonDocument d(128);
    if (deserializeJson(d, server.arg("plain")) || !d["on"].is<bool>()) {
        server.send(400, "application/json", "{\"success\":false,\"error\":\"Invalid body, need on: true|false\"}"); return;
    }
    bool on = d["on"].as<bool>();
    enqueueSettingCommand(CMD_SET_LIGHT_CALIBRATION, on ? 1 : 0, 0, "");
    server.send(200, "application/json", JSON_SUCCESS);
}

void handleSetAutoDispense() {
    if (!isAuthenticated()) { server.send(401, "application/json", JSON_FAIL_UNAUTHORIZED); return; }
    DynamicJsonDocument d(128);
    if (deserializeJson(d, server.arg("plain")) || !d["enabled"].is<bool>()) {
        server.send(400, "application/json", "{\"success\":false,\"error\":\"Invalid body, need enabled: true|false\"}"); return;
    }
    bool en = d["enabled"].as<bool>();
    lastSensorData.autoDispense = en;
    lastSensorDataValid = true;
    settingsNeedSave = true;
#if !defined(TZT_HEADLESS)
    updateControlPanelStatus();
#endif
    enqueueSettingCommand(CMD_SET_AUTO_DISPENSE, en ? 1 : 0, 0, "");
    server.send(200, "application/json", JSON_SUCCESS);
}

void handleGetAutomation() {
    if (!isAuthenticated()) { server.send(401, "application/json", JSON_FAIL_UNAUTHORIZED); return; }
    DynamicJsonDocument doc(128);
    doc["autoBrightness"] = lastSensorDataValid ? lastSensorData.autoBrightness : false;
    doc["autoDispense"] = lastSensorDataValid ? lastSensorData.autoDispense : false;
    if (lastPumpDurationTenths >= 0) doc["pumpDurationTenths"] = lastPumpDurationTenths;
    if (lastPumpCooldownTenths >= 0) doc["pumpCooldownTenths"] = lastPumpCooldownTenths;
    String s; serializeJson(doc, s);
    server.send(200, "application/json", s);
}

void handleSetAutomation() {
    if (!isAuthenticated()) { server.send(401, "application/json", JSON_FAIL_UNAUTHORIZED); return; }
    DynamicJsonDocument d(128);
    if (!deserializeJson(d, server.arg("plain")) && d["autoBrightness"].is<bool>()) {
        bool on = d["autoBrightness"].as<bool>();
        lastSensorData.autoBrightness = on;
        lastSensorDataValid = true;
        settingsNeedSave = true;
#if !defined(TZT_HEADLESS)
        updateControlPanelStatus();
#endif
        enqueueSettingCommand(CMD_SET_AUTO_BRIGHTNESS, on ? 1 : 0, 0, "");
    }
    server.send(200, "application/json", JSON_SUCCESS);
}

void handleTestPrinter() {
    if (!isAuthenticated()) { server.send(401, "application/json", JSON_FAIL_UNAUTHORIZED); return; }
    bool ok = sendCommandViaESPNow(CMD_TEST_PRINTER);
    server.send(200, "application/json", ok ? JSON_SUCCESS : "{\"success\":false,\"error\":\"ESP-NOW not available\"}");
}

void handleTestSensors() {
    if (!isAuthenticated()) { server.send(401, "application/json", JSON_FAIL_UNAUTHORIZED); return; }
    DynamicJsonDocument doc(384);
    if (lastSensorDataValid) {
        doc["moisture"] = lastSensorData.moisturePercent;
        doc["sanitizer"] = lastSensorData.sanitizerLevel;
        doc["irDetected"] = lastSensorData.irDetected;
        doc["light"] = lastSensorData.lightPercent;
        doc["ledBrightness"] = lastSensorData.ledBrightness;
        doc["pumpRunning"] = lastSensorData.pumpRunning;
        doc["isDispensing"] = lastSensorData.isDispensing;
    } else { 
        doc["moisture"] = 0; 
        doc["sanitizer"] = 0; 
        doc["irDetected"] = false; 
        doc["light"] = 0; 
        doc["ledBrightness"] = 0;
        doc["pumpRunning"] = false;
        doc["isDispensing"] = false;
    }
    String s; serializeJson(doc, s);
    server.send(200, "application/json", s);
}

void handleTestSendMessage() {
    if (!isAuthenticated()) { server.send(401, "application/json", JSON_FAIL_UNAUTHORIZED); return; }
    String text = "Test message from TZT at " + String(millis());
    String body = server.arg("plain");
    if (body.length() > 0) {
        DynamicJsonDocument doc(512);
        if (deserializeJson(doc, body) == DeserializationError::Ok && doc["text"].as<String>().length() > 0)
            text = doc["text"].as<String>();
    }
    if (sdCard.isReady())
        sdCard.saveMessage(text, "test");
    bool ok = sendPrintChunked(text, true);
#if !defined(TZT_HEADLESS)
    lastPrintMessage = text;
    if (labelMessage) lv_label_set_text(labelMessage, text.c_str());
#endif
    if (ok)
        server.send(200, "application/json", JSON_SUCCESS);
    else
        server.send(500, "application/json", "{\"success\":false,\"error\":\"Send failed\"}");
}

void handleTestPlayAudio() {
    if (!isAuthenticated()) { server.send(401, "application/json", JSON_FAIL_UNAUTHORIZED); return; }
    if (!sdCard.isReady()) { server.send(503, "application/json", "{\"error\":\"SD card not available\"}"); return; }
    String names[MAX_AUDIO_FILES];
    int n = sdCard.listAudioFiles(names, MAX_AUDIO_FILES);
    String file;
    String body = server.arg("plain");
    if (body.length() > 0) {
        DynamicJsonDocument doc(256);
        if (deserializeJson(doc, body) == DeserializationError::Ok && doc["file"].as<String>().length() > 0)
            file = doc["file"].as<String>();
    }
    if (file.length() == 0 && n > 0) file = names[0];
    if (file.length() == 0) { server.send(404, "application/json", "{\"error\":\"No audio files on SD. Put files in " AUDIO_DIR "\"}"); return; }
    if (!isSafeFilename(file)) { server.send(400, "application/json", "{\"error\":\"Invalid filename\"}"); return; }
    String path = String(AUDIO_DIR) + "/" + file;
    bool ok = audioSvc.playFile(path);
    server.send(ok ? 200 : 500, "application/json", ok ? JSON_SUCCESS : "{\"error\":\"Playback failed\"}");
}

void handleTestShowImage() {
    if (!isAuthenticated()) { server.send(401, "application/json", JSON_FAIL_UNAUTHORIZED); return; }
#if defined(TZT_HEADLESS)
    server.send(501, "application/json", "{\"error\":\"Display not available\"}");
    return;
#else
    if (!sdCard.isReady()) { server.send(503, "application/json", "{\"error\":\"SD card not available\"}"); return; }
    String names[MAX_IMAGE_FILES];
    int n = sdCard.listImageFiles(names, MAX_IMAGE_FILES);
    String file;
    String body = server.arg("plain");
    if (body.length() > 0) {
        DynamicJsonDocument doc(256);
        if (deserializeJson(doc, body) == DeserializationError::Ok && doc["file"].as<String>().length() > 0)
            file = doc["file"].as<String>();
    }
    if (file.length() == 0 && n > 0) file = names[0];
    if (file.length() == 0) { server.send(404, "application/json", "{\"error\":\"No images on SD. Put files in " IMAGES_DIR "\"}"); return; }
    if (!isSafeFilename(file)) { server.send(400, "application/json", "{\"error\":\"Invalid filename\"}"); return; }
    pendingTestImagePath = String(IMAGES_DIR) + "/" + file;
    lastDashboardImagePath = pendingTestImagePath;
    server.send(200, "application/json", JSON_SUCCESS);
#endif
}

void handleFavicon() {
    server.send(204);
}

void handleRoot() {
    if (!isAuthenticated()) { server.sendHeader("Location", "/login"); server.send(302, "text/plain", ""); return; }
    // Optional: if client sends Accept-Encoding: gzip, serve pre-compressed .gz from SPIFFS/LittleFS for large HTML
    const char* html = R"HTML(
<!DOCTYPE html>
<html lang="en">
<head>
    <meta charset="UTF-8">
    <meta name="viewport" content="width=device-width, initial-scale=1, viewport-fit=cover, maximum-scale=1, user-scalable=no">
    <meta name="apple-mobile-web-app-capable" content="yes">
    <meta name="apple-mobile-web-app-status-bar-style" content="black-translucent">
    <meta name="apple-mobile-web-app-title" content="Prick'n'Print">
    <meta name="theme-color" content="#1a3d0e">
    <title>Print-n-Prick</title>
    <style>
        * { margin: 0; padding: 0; box-sizing: border-box; -webkit-tap-highlight-color: rgba(74, 124, 42, 0.25); }
        html { -webkit-text-size-adjust: 100%; }
        body {
            font-family: -apple-system, BlinkMacSystemFont, 'Segoe UI', Roboto, sans-serif;
            background: linear-gradient(180deg, #0d1f07 0%, #1a3d0e 35%, #2d5016 70%, #1a3d0e 100%);
            min-height: 100vh;
            min-height: 100dvh;
            margin: 0;
            padding: env(safe-area-inset-top) env(safe-area-inset-right) env(safe-area-inset-bottom) env(safe-area-inset-left);
            padding-left: max(10px, env(safe-area-inset-left));
            padding-right: max(10px, env(safe-area-inset-right));
            padding-top: max(10px, env(safe-area-inset-top));
            padding-bottom: max(16px, env(safe-area-inset-bottom));
            color: #333;
            position: relative;
            -webkit-font-smoothing: antialiased;
        }
        body::before {
            content: '🌵';
            position: fixed;
            font-size: 140px;
            opacity: 0.04;
            top: -30px;
            left: -30px;
            z-index: 0;
        }
        .container {
            max-width: 480px;
            margin: 0 auto;
            background: rgba(255,255,255,0.97);
            border-radius: 20px;
            padding: 16px;
            box-shadow: 0 8px 32px rgba(0,0,0,0.25);
            border: 2px solid #4a7c2a;
            position: relative;
            z-index: 1;
        }
        .header {
            text-align: center;
            margin-bottom: 16px;
            padding-bottom: 12px;
            border-bottom: 2px solid #4a7c2a;
        }
        .header-icon { font-size: 36px; margin-bottom: 4px; display: block; }
        h1 {
            color: #2d5016;
            font-size: 24px;
            font-weight: 700;
            margin-bottom: 2px;
        }
        .tagline { color: #555; font-size: 13px; }
        .sensor-grid {
            display: grid;
            grid-template-columns: repeat(2, 1fr);
            gap: 10px;
            margin: 14px 0;
        }
        .sensor-card {
            background: linear-gradient(135deg, #fff 0%, #f0f5ec 100%);
            padding: 14px;
            border-radius: 14px;
            text-align: center;
            border: 2px solid #4a7c2a;
            min-height: 72px;
            display: flex;
            flex-direction: column;
            justify-content: center;
            -webkit-touch-callout: none;
            user-select: none;
        }
        .sensor-card:active { transform: scale(0.98); opacity: 0.95; }
        .sensor-icon { font-size: 24px; margin-bottom: 4px; }
        .sensor-label {
            font-size: 11px;
            color: #666;
            font-weight: 600;
            text-transform: uppercase;
            letter-spacing: 0.3px;
        }
        .sensor-value { font-size: 20px; font-weight: 700; color: #2d5016; }
        .btn-group {
            display: flex;
            gap: 10px;
            margin: 14px 0;
            flex-wrap: wrap;
        }
        button, .btn {
            flex: 1;
            min-width: 0;
            min-height: 44px;
            padding: 12px 16px;
            border: none;
            border-radius: 12px;
            background: linear-gradient(180deg, #2d5016 0%, #3a6820 100%);
            color: white;
            font-weight: 600;
            cursor: pointer;
            font-size: 16px;
            transition: transform 0.15s, box-shadow 0.15s;
            box-shadow: 0 3px 8px rgba(45, 80, 22, 0.35);
            text-decoration: none;
            display: inline-flex;
            align-items: center;
            justify-content: center;
            -webkit-touch-callout: none;
            user-select: none;
        }
        button:active, .btn:active {
            transform: scale(0.97);
            box-shadow: 0 1px 4px rgba(26, 18, 12, 0.3);
        }
        .btn-danger { background: linear-gradient(180deg, #8b4513 0%, #a0522d 100%); }
        .btn-secondary { background: linear-gradient(180deg, #3e2723 0%, #5d4037 100%); }
        .section {
            margin: 14px 0;
            padding: 14px;
            background: linear-gradient(135deg, #fff 0%, #f8faf6 100%);
            border-radius: 14px;
            border: 2px solid #4a7c2a;
            border-left: 4px solid #3e2723;
        }
        .section-title {
            font-size: 17px;
            font-weight: 700;
            color: #2d5016;
            margin-bottom: 12px;
            display: flex;
            align-items: center;
            gap: 8px;
            min-height: 44px;
            padding: 4px 0;
        }
        .collapsible .section-title {
            cursor: pointer;
            -webkit-tap-highlight-color: transparent;
            list-style: none;
        }
        .collapsible .section-title::after {
            content: '▼';
            font-size: 12px;
            margin-left: auto;
            transition: transform 0.2s;
            color: #4a7c2a;
        }
        .collapsible[open] .section-title::after { transform: rotate(-180deg); }
        .input-group {
            display: flex;
            gap: 8px;
            margin: 10px 0;
            flex-wrap: wrap;
        }
        input[type="text"], input[type="datetime-local"] {
            flex: 1 1 100%;
            min-width: 0;
            min-height: 44px;
            padding: 12px 14px;
            border: 2px solid #4a7c2a;
            border-radius: 10px;
            font-size: 16px;
            font-family: inherit;
            background: #fff;
        }
        input[type="text"]:focus, input[type="datetime-local"]:focus {
            outline: none;
            border-color: #6b9f3d;
            box-shadow: 0 0 0 3px rgba(74, 124, 42, 0.2);
        }
        .list-container {
            max-height: 200px;
            overflow-y: auto;
            -webkit-overflow-scrolling: touch;
            background: #fff;
            padding: 10px;
            border-radius: 10px;
            margin: 10px 0;
            border: 1px solid #c5d4b8;
            min-height: 60px;
        }
        .section.reminders-section .list-container,
        .section.grocery-section .list-container {
            max-height: 220px;
        }
        .list-item {
            padding: 12px 10px;
            margin: 6px 0;
            background: #f0f5ec;
            border-radius: 10px;
            border-left: 4px solid #4a7c2a;
            display: flex;
            justify-content: space-between;
            align-items: center;
            gap: 10px;
            font-size: 15px;
            min-height: 48px;
        }
        .list-item:nth-child(odd) { border-left-color: #8b4513; }
        .list-item small { color: #5d4037; font-size: 12px; }
        .list-item button {
            min-height: 40px;
            padding: 10px 14px;
            font-size: 15px;
            min-width: 0;
            flex: 0 0 auto;
            border-radius: 10px;
        }
        .empty-state { text-align: center; color: #777; font-style: italic; padding: 16px; font-size: 14px; }
        .control-grid {
            display: grid;
            grid-template-columns: 1fr;
            gap: 14px;
            margin: 14px 0;
        }
        .control-half {
            background: #fff;
            border: 2px solid #4a7c2a;
            border-left: 4px solid #8b4513;
            border-radius: 12px;
            padding: 14px;
            display: flex;
            flex-direction: column;
        }
        .control-half .ctrl-title {
            font-size: 16px;
            font-weight: 700;
            color: #2d5016;
            margin-bottom: 10px;
        }
        .slider-row {
            display: flex;
            justify-content: space-between;
            align-items: center;
            margin-bottom: 6px;
            font-size: 15px;
            color: #5d4037;
            font-weight: 600;
            min-height: 28px;
        }
        .slider-row .val { color: #2d5016; min-width: 40px; text-align: right; }
        .ctrl-slider {
            width: 100%;
            height: 12px;
            border-radius: 6px;
            margin-bottom: 12px;
            -webkit-appearance: none;
            background: #c5d4b8;
            box-sizing: border-box;
        }
        .ctrl-slider:disabled { opacity: 0.5; pointer-events: none; }
        .ctrl-slider::-webkit-slider-thumb {
            -webkit-appearance: none;
            width: 28px;
            height: 28px;
            border-radius: 50%;
            background: #4a7c2a;
            border: 2px solid #2d5016;
            cursor: pointer;
            margin-top: -8px;
        }
        .ctrl-slider::-moz-range-thumb {
            width: 28px;
            height: 28px;
            border-radius: 50%;
            background: #4a7c2a;
            border: 2px solid #2d5016;
            cursor: pointer;
        }
        .cal-btns, .pump-btns { display: flex; gap: 10px; margin-top: 12px; }
        .cal-btns button, .pump-btns button {
            flex: 1;
            min-height: 44px;
            padding: 12px;
            font-size: 16px;
            border-radius: 10px;
        }
        .cal-btns button:disabled, .pump-btns button:disabled { opacity: 0.5; }
        .cal-btns .cal-off { background: #3e2723; color: #e8ecd8; }
        .cal-btns .cal-on { background: #4a7c2a; color: #fff; }
        .sensor-toggle-row {
            display: grid;
            grid-template-columns: 1fr 1fr;
            gap: 10px;
            margin-top: 12px;
            padding-top: 12px;
            border-top: 1px solid #c5d4b8;
        }
        .sensor-toggle-item {
            display: flex;
            justify-content: space-between;
            align-items: center;
            font-size: 15px;
            color: #5d4037;
            font-weight: 600;
            min-height: 44px;
        }
        .tog {
            width: 52px;
            height: 30px;
            border-radius: 15px;
            background: #3e2723;
            cursor: pointer;
            position: relative;
            flex-shrink: 0;
            transition: background 0.2s;
        }
        .tog.on { background: #4a7c2a; }
        .tog::after {
            content: '';
            position: absolute;
            width: 26px;
            height: 26px;
            border-radius: 50%;
            background: #fff;
            top: 2px;
            left: 2px;
            transition: left 0.2s;
            box-shadow: 0 2px 4px rgba(0,0,0,0.2);
        }
        .tog.on::after { left: 24px; }
        textarea.reminder-msg, textarea.todo-msg {
            width: 100%;
            min-height: 100px;
            padding: 12px 14px;
            border: 2px solid #4a7c2a;
            border-radius: 10px;
            font-size: 16px;
            font-family: inherit;
            background: #fff;
            resize: vertical;
            box-sizing: border-box;
        }
        textarea.reminder-msg:focus, textarea.todo-msg:focus {
            outline: none;
            border-color: #6b9f3d;
            box-shadow: 0 0 0 3px rgba(74, 124, 42, 0.2);
        }
        details.collapsible { margin: 10px 0; }
        details.collapsible .section { margin: 0; border-radius: 14px; }
        details.collapsible > summary { list-style: none; }
        details.collapsible > summary::-webkit-details-marker { display: none; }
        .section-title-wrap { display: flex; align-items: center; width: 100%; }
    </style>
</head>
<body>
    <div class="container">
        <div class="header">
            <span class="header-icon">🌵💌</span>
            <h1>Print-n-Prick</h1>
            <p class="tagline">Smart cactus care & thermal printing</p>
        </div>
        
        <div class="sensor-grid">
            <div class="sensor-card">
                <div class="sensor-icon">💧</div>
                <div class="sensor-label">Moisture</div>
                <div class="sensor-value" id="mo">—</div>
            </div>
            <div class="sensor-card">
                <div class="sensor-icon">🧴</div>
                <div class="sensor-label">Sanitizer</div>
                <div class="sensor-value" id="sa">—</div>
            </div>
            <div class="sensor-card">
                <div class="sensor-icon">☀️</div>
                <div class="sensor-label">Light</div>
                <div class="sensor-value" id="light">—</div>
            </div>
            <div class="sensor-card">
                <div class="sensor-icon">👁️</div>
                <div class="sensor-label">IR Motion</div>
                <div class="sensor-value" id="ir">—</div>
            </div>
            <div class="sensor-card">
                <div class="sensor-icon">💡</div>
                <div class="sensor-label">LED Level</div>
                <div class="sensor-value" id="ledLevel">—</div>
            </div>
            <div class="sensor-card">
                <div class="sensor-icon">💧</div>
                <div class="sensor-label">Pump</div>
                <div class="sensor-value" id="pump">—</div>
            </div>
        </div>
        
        <div class="btn-group">
            <button onclick="resetSanitizer()">🔄 Reset Sanitizer</button>
            <a href="/logout" class="btn btn-secondary">🚪 Logout</a>
        </div>
        
        <details class="collapsible">
        <summary class="section-title">⏰ Reminders</summary>
        <div class="section reminders-section">
            <div class="input-group">
                <textarea class="reminder-msg" id="rmMsg" placeholder="📝 Reminder message..." rows="5"></textarea>
            </div>
            <div class="input-group" style="flex-wrap: wrap; gap: 8px;">
                <button type="button" onclick="setQuickReminder(1)" style="min-width: 70px;">Now</button>
                <button type="button" onclick="setQuickReminder(1440)" style="min-width: 70px;">24 Hr</button>
                <button type="button" onclick="setQuickReminder(10080)" style="min-width: 70px;">7 days</button>
            </div>
            <div class="input-group">
                <input type="datetime-local" id="rmTm">
                <button onclick="addRem()" style="min-width: 80px;">Add</button>
            </div>
            <div class="list-container" id="rm">
                <div class="empty-state">No reminders yet</div>
            </div>
        </div>
        </details>
        
        <details class="collapsible">
        <summary class="section-title">🛒 Grocery List</summary>
        <div class="section grocery-section">
            <div class="input-group" style="flex-wrap: wrap;">
                <input type="text" id="glItem" placeholder="🛒 Add item..." style="flex: 1 1 100%; min-width: 100%;">
            </div>
            <div class="input-group">
                <button onclick="addGl()" style="min-width: 80px;">Add</button>
                <button onclick="printGl()" style="min-width: 100px;">🖨️ Print</button>
                <button class="btn-danger" onclick="clearGl()" style="min-width: 80px;">Clear</button>
            </div>
            <div class="list-container" id="gl">
                <div class="empty-state">No items yet</div>
            </div>
        </div>
        </details>
        
        <details class="collapsible">
        <summary class="section-title">✅ Todo List</summary>
        <div class="section grocery-section">
            <div class="input-group" style="flex-wrap: wrap;">
                <textarea class="todo-msg" id="tlItem" placeholder="✅ Add task..." rows="2" style="flex: 1 1 100%; min-width: 100%; resize: vertical; box-sizing: border-box;"></textarea>
            </div>
            <div class="input-group">
                <button onclick="addTl()" style="min-width: 80px;">Add</button>
                <button onclick="printTl()" style="min-width: 100px;">🖨️ Print</button>
                <button class="btn-danger" onclick="clearTl()" style="min-width: 80px;">Clear</button>
            </div>
            <div class="list-container" id="tl">
                <div class="empty-state">No tasks yet</div>
            </div>
        </div>
        </details>
        
        <details class="collapsible" open>
        <summary class="section-title">💧 Pump & LED</summary>
        <div class="section">
            <div class="control-grid">
                <div class="control-half">
                    <div class="ctrl-title">Pump</div>
                    <div class="slider-row"><span>Duration</span><span class="val" id="durVal">2.0s</span></div>
                    <input type="range" class="ctrl-slider" id="durSl" min="5" max="255" value="20">
                    <div class="slider-row"><span>Cooldown</span><span class="val" id="coVal">3.0s</span></div>
                    <input type="range" class="ctrl-slider" id="coSl" min="10" max="255" value="30">
                    <div class="pump-btns">
                        <button type="button" id="pumpStartBtn" class="btn-success" onclick="pumpStart()">Start</button>
                        <button type="button" id="pumpStopBtn" class="btn-danger" onclick="pumpStop()">Stop</button>
                    </div>
                </div>
                <div class="control-half">
                    <div class="ctrl-title">LED</div>
                    <div class="slider-row"><span>Level</span><span class="val" id="ledVal">0%</span></div>
                    <input type="range" class="ctrl-slider" id="ledSl" min="0" max="100" value="0">
                    <div class="cal-btns">
                        <button type="button" class="cal-off" onclick="setCalibrate(false)">Min</button>
                        <button type="button" class="cal-on" onclick="setCalibrate(true)">Max</button>
                    </div>
                </div>
            </div>
            <div class="sensor-toggle-row">
                <div class="sensor-toggle-item"><span>Pump sensor</span><div class="tog" id="togPump" onclick="togglePumpSensor(this)" title="Auto dispense"></div></div>
                <div class="sensor-toggle-item"><span>LED sensor</span><div class="tog" id="togLed" onclick="toggleLedSensor(this)" title="Auto brightness"></div></div>
            </div>
        </div>
        </details>

        <details class="collapsible">
        <summary class="section-title">📜 Message History</summary>
        <div class="section">
            <div class="list-container" id="msgHist" style="max-height:200px;overflow-y:auto;">
                <div class="empty-state">Loading messages...</div>
            </div>
        </div>
        </details>

        <details class="collapsible">
        <summary class="section-title">🔊 Audio Files</summary>
        <div class="section">
            <div id="audioNow" style="color:#FFD700;font-size:13px;margin-bottom:6px;"></div>
            <div class="list-container" id="audioFiles" style="max-height:180px;overflow-y:auto;">
                <div class="empty-state">Loading audio files...</div>
            </div>
            <div style="display:flex;gap:8px;margin-top:8px;">
                <button type="button" class="btn-danger" onclick="audioStop()" style="flex:1;">Stop</button>
                <button type="button" class="btn-success" onclick="loadAudio()" style="flex:1;">Refresh</button>
            </div>
        </div>
        </details>

        <details class="collapsible">
        <summary class="section-title">🖼️ Images</summary>
        <div class="section">
            <div class="list-container" id="imageFiles" style="max-height:180px;overflow-y:auto;">
                <div class="empty-state">Loading images...</div>
            </div>
            <div style="display:flex;gap:8px;margin-top:8px;">
                <button type="button" class="btn-success" onclick="loadImages()" style="flex:1;">Refresh</button>
            </div>
            <div style="margin-top:8px;font-size:12px;color:#888;">Add via backend /api/images → url, name; TZT downloads to SD.</div>
        </div>
        </details>
    </div>
    
    <script>
        function q(u) { return fetch(u, {headers: {'Accept': 'application/json'}}).then(r => r.json()); }
        function po(u, b) { return fetch(u, {method: 'POST', headers: {'Content-Type': 'application/json'}, body: JSON.stringify(b || {})}).then(r => r.json()); }
        function del(u) { return fetch(u, {method: 'DELETE'}).then(r => r.json()); }
        
        function setQuickReminder(minutesFromNow) {
            var d = new Date();
            d.setMinutes(d.getMinutes() + minutesFromNow);
            var y = d.getFullYear(), m = String(d.getMonth() + 1).padStart(2, '0'), day = String(d.getDate()).padStart(2, '0');
            var h = String(d.getHours()).padStart(2, '0'), min = String(d.getMinutes()).padStart(2, '0');
            document.getElementById('rmTm').value = y + '-' + m + '-' + day + 'T' + h + ':' + min;
        }
        
        function resetSanitizer() {
            fetch('/api/reset-sanitizer', {method: 'POST'})
                .then(r => r.json())
                .then(d => { if (d.success) ref(); });
        }
        
        function pumpStart() {
            var pumpEl = document.getElementById('pump');
            var prev = pumpEl ? pumpEl.textContent : 'OFF';
            if (pumpEl) pumpEl.textContent = 'ON';
            po('/api/test/pump-control', {action: 'start'}).then(d => {
                if (!d.success) { if (pumpEl) pumpEl.textContent = prev; }
                else setTimeout(ref, 400);
            });
        }
        function pumpStop() {
            var pumpEl = document.getElementById('pump');
            var prev = pumpEl ? pumpEl.textContent : 'ON';
            if (pumpEl) pumpEl.textContent = 'OFF';
            po('/api/test/pump-control', {action: 'stop'}).then(d => {
                if (!d.success) { if (pumpEl) pumpEl.textContent = prev; }
                else setTimeout(ref, 400);
            });
        }
        function setCalibrate(on) { po('/api/led/calibrate', {on: on}).then(d => { if (d.success) ref(); }); }
        function applyPumpSensorUI(autoDispenseOn) {
            var togPump = document.getElementById('togPump');
            var durSl = document.getElementById('durSl'), coSl = document.getElementById('coSl');
            var pumpStartBtn = document.getElementById('pumpStartBtn'), pumpStopBtn = document.getElementById('pumpStopBtn');
            if (togPump) { if (autoDispenseOn) togPump.classList.add('on'); else togPump.classList.remove('on'); }
            if (durSl) durSl.disabled = !autoDispenseOn;
            if (coSl) coSl.disabled = !autoDispenseOn;
            if (pumpStartBtn) pumpStartBtn.disabled = autoDispenseOn;
            if (pumpStopBtn) pumpStopBtn.disabled = autoDispenseOn;
        }
        function applyLedSensorUI(autoBrightnessOn) {
            var togLed = document.getElementById('togLed');
            var ledSl = document.getElementById('ledSl');
            var calBtns = document.querySelectorAll('.cal-btns button');
            if (togLed) { if (autoBrightnessOn) togLed.classList.add('on'); else togLed.classList.remove('on'); }
            if (ledSl) ledSl.disabled = autoBrightnessOn;
            if (calBtns) calBtns.forEach(function(btn){ btn.disabled = !autoBrightnessOn; });
        }
        function togglePumpSensor(el) {
            el.classList.toggle('on');
            var on = el.classList.contains('on');
            applyPumpSensorUI(on);
            po('/api/pump/auto-dispense', {enabled: on}).then(d => { if (!d.success) { applyPumpSensorUI(!on); } });
        }
        function toggleLedSensor(el) {
            el.classList.toggle('on');
            var on = el.classList.contains('on');
            applyLedSensorUI(on);
            po('/api/test/automation', {autoBrightness: on}).then(d => { if (!d.success) { applyLedSensorUI(!on); } });
        }
        
        function ref() {
            q('/api/status').then(d => {
                document.getElementById('mo').textContent = typeof d.moisture === 'number' ? d.moisture.toFixed(1) + '%' : (d.moisture || '—');
                document.getElementById('sa').textContent = typeof d.sanitizer === 'number' ? d.sanitizer.toFixed(1) + '%' : (d.sanitizer || '—');
                var lightEl = document.getElementById('light');
                if (lightEl) lightEl.textContent = typeof d.light === 'number' ? d.light.toFixed(1) + '%' : (d.light != null ? d.light : '—');
                var irEl = document.getElementById('ir');
                if (irEl) irEl.textContent = d.irSensor ? '✓' : '✗';
                var ledLevelEl = document.getElementById('ledLevel');
                if (ledLevelEl) { var p = (typeof d.ledBrightness === 'number') ? Math.round((d.ledBrightness / 255) * 100) : 0; ledLevelEl.textContent = p + '%'; }
                var pumpEl = document.getElementById('pump');
                if (pumpEl) pumpEl.textContent = d.dispensing ? 'ON' : 'OFF';
                var ledSl = document.getElementById('ledSl');
                if (ledSl && typeof d.ledBrightness === 'number') { var p = Math.round((d.ledBrightness / 255) * 100); ledSl.value = p; document.getElementById('ledVal').textContent = p + '%'; }
                // Sync sensor toggles and dependent controls (uses same helpers as optimistic toggle)
                if (d.autoDispense !== undefined) applyPumpSensorUI(d.autoDispense);
                if (d.autoBrightness !== undefined) applyLedSensorUI(d.autoBrightness);
            });
        }
        ref();
        setInterval(ref, 3000);
        setInterval(loadReminders, 30000);
        
        (function() {
            var durSl = document.getElementById('durSl'), coSl = document.getElementById('coSl'), ledSl = document.getElementById('ledSl');
            function updateDur() { var v = parseInt(durSl.value, 10); document.getElementById('durVal').textContent = (v/10).toFixed(1) + 's'; po('/api/pump/duration', {duration: v}); }
            function updateCo() { var v = parseInt(coSl.value, 10); document.getElementById('coVal').textContent = (v/10).toFixed(1) + 's'; po('/api/pump/cooldown', {cooldown: v}); }
            function updateLed() { var v = parseInt(ledSl.value, 10); document.getElementById('ledVal').textContent = v + '%'; var b = Math.round((v/100)*255); po('/api/test/led12v', {brightness: b, state: v > 0}); }
            if (durSl) { durSl.addEventListener('input', function() { document.getElementById('durVal').textContent = (parseInt(durSl.value,10)/10).toFixed(1) + 's'; }); durSl.addEventListener('change', updateDur); }
            if (coSl) { coSl.addEventListener('input', function() { document.getElementById('coVal').textContent = (parseInt(coSl.value,10)/10).toFixed(1) + 's'; }); coSl.addEventListener('change', updateCo); }
            if (ledSl) { ledSl.addEventListener('input', function() { var v = parseInt(ledSl.value,10); document.getElementById('ledVal').textContent = v + '%'; }); ledSl.addEventListener('change', function() { var v = parseInt(ledSl.value,10); var b = Math.round((v/100)*255); po('/api/test/led12v', {brightness: b, state: v > 0}); }); }
        })();
        
        function updateReminderListUI(reminders) {
            let h = '';
            if (!reminders || Object.keys(reminders).length === 0) {
                h = '<div class="empty-state">No reminders yet</div>';
            } else {
                for (let k in reminders) {
                    let r = reminders[k];
                    h += '<div class="list-item"><div><strong>' + r.message + '</strong><br><small>' + 
                         new Date((r.scheduledTime || 0) * 1000).toLocaleString() + '</small></div>' +
                         '<button onclick="delRem(\'' + k + '\')">Delete</button></div>';
                }
            }
            document.getElementById('rm').innerHTML = h;
        }
        
        function loadReminders() {
            q('/api/reminders').then(d => {
                updateReminderListUI(d);
            });
        }
        
        function updateGroceryListUI(items) {
            let h = '';
            if (!items || items.length === 0) {
                h = '<div class="empty-state">No items yet</div>';
            } else {
                items.forEach((it, i) => {
                    h += '<div class="list-item"><div>' + it + '</div><button onclick="delGl(' + i + ')">Delete</button></div>';
                });
            }
            document.getElementById('gl').innerHTML = h;
        }
        
        function loadGroceries() {
            q('/api/groceries').then(arr => {
                updateGroceryListUI(Array.isArray(arr) ? arr : []);
            });
        }
        
        function updateTodoListUI(items) {
            let h = '';
            if (!items || items.length === 0) {
                h = '<div class="empty-state">No tasks yet</div>';
            } else {
                items.forEach((it, i) => {
                    h += '<div class="list-item"><div>' + it + '</div><button onclick="delTl(' + i + ')">Delete</button></div>';
                });
            }
            document.getElementById('tl').innerHTML = h;
        }
        
        function loadTodos() {
            q('/api/todos').then(arr => {
                updateTodoListUI(Array.isArray(arr) ? arr : []);
            });
        }
        
        function addRem() {
            let m = document.getElementById('rmMsg').value;
            let t = document.getElementById('rmTm').value;
            if (!m || !t) return;
            let scheduledTime = Math.floor(new Date(t).getTime() / 1000);
            // Optimistic update: add to UI immediately with temp ID
            let tempId = 'temp_' + Date.now();
            let currentReminders = {};
            // Get existing reminders from current UI state
            document.querySelectorAll('#rm .list-item').forEach(el => {
                let btn = el.querySelector('button');
                if (btn) {
                    let onclick = btn.getAttribute('onclick');
                    let idMatch = onclick ? onclick.match(/delRem\('([^']+)'\)/) : null;
                    if (idMatch) {
                        let reminderId = idMatch[1];
                        if (!reminderId.startsWith('temp_')) {  // Don't duplicate temp entries
                            let msg = el.querySelector('strong').textContent;
                            let timeText = el.querySelector('small').textContent;
                            // Parse time from text or use scheduledTime
                            currentReminders[reminderId] = {
                                message: msg,
                                scheduledTime: scheduledTime  // Will be corrected from server
                            };
                        }
                    }
                }
            });
            // Add new reminder optimistically
            currentReminders[tempId] = {
                message: m,
                scheduledTime: scheduledTime
            };
            updateReminderListUI(currentReminders);
            document.getElementById('rmMsg').value = '';
            document.getElementById('rmTm').value = '';
            // Sync with server in background
            po('/api/reminders', {message: m, scheduledTime: scheduledTime}).then(d => {
                if (d.success && d.id) {
                    // Replace temp ID with real ID from server
                    delete currentReminders[tempId];
                    currentReminders[d.id] = {
                        message: m,
                        scheduledTime: scheduledTime
                    };
                    updateReminderListUI(currentReminders);
                } else {
                    loadReminders();
                }
            });
        }
        
        function delRem(id) {
            if (!confirm('🗑️ Delete this reminder?')) return;
            let itemToRemove = document.querySelector('#rm .list-item button[onclick*="' + id + '"]');
            if (itemToRemove) {
                itemToRemove.closest('.list-item').remove();
                if (document.querySelectorAll('#rm .list-item').length === 0) {
                    document.getElementById('rm').innerHTML = '<div class="empty-state">No reminders yet</div>';
                }
            }
            del('/api/reminders/' + id).then(d => {
                if (!d.success) loadReminders();
            });
        }
        
        function addGl() {
            let it = document.getElementById('glItem').value;
            if (!it) return;
            // Optimistic update: add to UI immediately
            let currentItems = [];
            document.querySelectorAll('#gl .list-item').forEach(el => {
                let text = el.querySelector('div').textContent.trim();
                if (text) currentItems.push(text);
            });
            currentItems.push(it);
            updateGroceryListUI(currentItems);
            document.getElementById('glItem').value = '';
            // Sync with server in background
            po('/api/groceries', {item: it}).then(d => {
                if (!d.success) {
                    currentItems.pop();
                    updateGroceryListUI(currentItems);
                }
            });
        }
        
        function delGl(i) {
            // Optimistic update: remove from UI immediately
            let currentItems = [];
            document.querySelectorAll('#gl .list-item').forEach((el, idx) => {
                if (idx !== i) {
                    let text = el.querySelector('div').textContent.trim();
                    if (text) currentItems.push(text);
                }
            });
            updateGroceryListUI(currentItems);
            // Sync with server in background
            del('/api/groceries/' + i).then(d => {
                if (!d.success) loadGroceries();
            });
        }
        
        function printGl() {
            if (!confirm('🖨️ Send grocery list to printer?')) return;
            po('/api/groceries/print').then(d => { if (d.success) loadGroceries(); });
        }
        
        function clearGl() {
            if (!confirm('🗑️ Clear all groceries?')) return;
            updateGroceryListUI([]);
            del('/api/groceries').then(d => {
                if (!d.success) loadGroceries();
            });
        }
        
        function addTl() {
            let it = document.getElementById('tlItem').value.trim();
            if (!it) return;
            let currentItems = [];
            document.querySelectorAll('#tl .list-item').forEach(el => {
                let text = el.querySelector('div').textContent.trim();
                if (text) currentItems.push(text);
            });
            currentItems.push(it);
            updateTodoListUI(currentItems);
            document.getElementById('tlItem').value = '';
            po('/api/todos', {item: it}).then(d => {
                if (!d.success) {
                    currentItems.pop();
                    updateTodoListUI(currentItems);
                }
            });
        }
        
        function delTl(i) {
            let currentItems = [];
            document.querySelectorAll('#tl .list-item').forEach((el, idx) => {
                if (idx !== i) {
                    let text = el.querySelector('div').textContent.trim();
                    if (text) currentItems.push(text);
                }
            });
            updateTodoListUI(currentItems);
            del('/api/todos/' + i).then(d => {
                if (!d.success) loadTodos();
            });
        }
        
        function printTl() {
            if (!confirm('🖨️ Send todo list to printer?')) return;
            po('/api/todos/print').then(d => { if (d.success) loadTodos(); });
        }
        
        function clearTl() {
            if (!confirm('🗑️ Clear all todos?')) return;
            updateTodoListUI([]);
            del('/api/todos').then(d => {
                if (!d.success) loadTodos();
            });
        }
        
        function loadMessages() {
            q('/api/messages').then(d => {
                var c = document.getElementById('msgHist');
                if (!d.messages || d.messages.length === 0) { c.innerHTML = '<div class="empty-state">No messages saved yet</div>'; return; }
                c.innerHTML = '';
                d.messages.forEach(m => {
                    var el = document.createElement('div');
                    el.style.cssText = 'padding:6px 8px;border-bottom:1px solid rgba(255,255,255,0.1);font-size:13px;color:#e0e0e0;';
                    var ts = m.ts > 1600000000 ? new Date(m.ts * 1000).toLocaleString() + ' - ' : '';
                    el.textContent = ts + m.preview;
                    c.appendChild(el);
                });
            }).catch(function() { document.getElementById('msgHist').innerHTML = '<div class="empty-state">SD card not available</div>'; });
        }

        function loadAudio() {
            q('/api/audio').then(d => {
                var c = document.getElementById('audioFiles');
                var now = document.getElementById('audioNow');
                if (d.playing && d.current) now.textContent = 'Now playing: ' + d.current;
                else now.textContent = '';
                if (!d.files || d.files.length === 0) { c.innerHTML = '<div class="empty-state">No audio files on SD</div>'; return; }
                c.innerHTML = '';
                d.files.forEach(f => {
                    var el = document.createElement('div');
                    el.style.cssText = 'padding:6px 8px;border-bottom:1px solid rgba(255,255,255,0.1);display:flex;justify-content:space-between;align-items:center;';
                    el.innerHTML = '<span style="color:#e0e0e0;font-size:13px;">' + f + '</span><button onclick="audioPlay(\'' + f.replace(/'/g,"\\'") + '\')" style="padding:4px 12px;border:none;border-radius:4px;background:#4a7c2a;color:#fff;cursor:pointer;">Play</button>';
                    c.appendChild(el);
                });
            }).catch(function() { document.getElementById('audioFiles').innerHTML = '<div class="empty-state">SD card not available</div>'; });
        }

        function audioPlay(f) { po('/api/audio/play', {file: f}).then(function() { setTimeout(loadAudio, 500); }); }
        function audioStop() { po('/api/audio/stop').then(function() { setTimeout(loadAudio, 500); }); }

        function loadImages() {
            q('/api/images').then(d => {
                var c = document.getElementById('imageFiles');
                if (!d.files || d.files.length === 0) { c.innerHTML = '<div class="empty-state">No images on SD</div>'; return; }
                c.innerHTML = '';
                d.files.forEach(f => {
                    var el = document.createElement('div');
                    el.style.cssText = 'padding:6px 8px;border-bottom:1px solid rgba(255,255,255,0.1);font-size:13px;color:#e0e0e0;';
                    el.textContent = f;
                    c.appendChild(el);
                });
            }).catch(function() { document.getElementById('imageFiles').innerHTML = '<div class="empty-state">SD card not available</div>'; });
        }

        loadReminders();
        loadGroceries();
        loadTodos();
        loadMessages();
        loadAudio();
        loadImages();
        setQuickReminder(0);  // Pre-fill reminder date/time with current so it's not empty
    </script>
</body>
</html>)HTML";
    server.send(200, "text/html", html);
}

void setupWebServer() {
    server.on("/login", HTTP_GET, handleLogin);
    server.on("/login", HTTP_POST, handleLogin);
    server.on("/logout", HTTP_GET, handleLogout);
    server.on("/", handleRoot);
    
    // API endpoints - proxy to main ESP32
    server.on("/api/login", HTTP_GET, handleApiLogin);  // Shortcut-friendly login endpoint
    server.on("/api/status", HTTP_GET, handleGetStatus);
    server.on("/api/health", HTTP_GET, handleHealth);
    server.on("/api/queue", HTTP_GET, handleQueueStatus);
    server.on("/api/reset-sanitizer", HTTP_POST, handleResetSanitizer);
    
    // Hardware test API (sensors, pump, LED, etc. used by main dashboard)
    server.on("/api/test/pump", HTTP_POST, handleTestPump);
    server.on("/api/test/pump-control", HTTP_POST, handlePumpControl);
    server.on("/api/test/led12v", HTTP_POST, handleTestLED12V);
    server.on("/api/pump/duration", HTTP_POST, handleSetPumpDuration);
    server.on("/api/pump/cooldown", HTTP_POST, handleSetPumpCooldown);
    server.on("/api/pump/auto-dispense", HTTP_POST, handleSetAutoDispense);
    server.on("/api/led/calibrate", HTTP_POST, handleSetLightCalibration);
    server.on("/api/test/automation", HTTP_GET, handleGetAutomation);
    server.on("/api/test/automation", HTTP_POST, handleSetAutomation);
    server.on("/api/test/printer", HTTP_POST, handleTestPrinter);
    server.on("/api/test/sensors", HTTP_GET, handleTestSensors);
    server.on("/api/test/send-message", HTTP_POST, handleTestSendMessage);
    server.on("/api/test/play-audio", HTTP_POST, handleTestPlayAudio);
    server.on("/api/test/show-image", HTTP_POST, handleTestShowImage);
    
    // Reminder endpoints
    server.on("/api/reminders", HTTP_GET, handleGetReminders);
    server.on("/api/reminders", HTTP_POST, handleAddReminder);
    
    // Grocery endpoints
    server.on("/api/groceries", HTTP_GET, handleGetGroceries);
    server.on("/api/groceries", HTTP_POST, handleAddGrocery);
    server.on("/api/groceries", HTTP_DELETE, handleClearGroceries);
    server.on("/api/groceries/print", HTTP_POST, handlePrintGroceries);
    
    // Todo endpoints
    server.on("/api/todos", HTTP_GET, handleGetTodos);
    server.on("/api/todos", HTTP_POST, handleAddTodo);
    server.on("/api/todos", HTTP_DELETE, handleClearTodos);
    server.on("/api/todos/print", HTTP_POST, handlePrintTodos);
    
    server.on("/favicon.ico", handleFavicon);
    
    // Handle dynamic DELETE routes
    server.onNotFound([]() {
        String uri = server.uri();
        HTTPMethod method = server.method();
        
        if (uri.startsWith("/api/reminders/") && method == HTTP_DELETE) {
            handleDeleteReminder();
            return;
        }
        if (uri.startsWith("/api/groceries/") && method == HTTP_DELETE) {
            handleDeleteGrocery();
            return;
        }
        if (uri.startsWith("/api/todos/") && method == HTTP_DELETE) {
            handleDeleteTodo();
            return;
        }
        
        if (uri == "/favicon.ico" || uri == "/robots.txt") {
            server.send(204);
            return;
        }
        
        server.send(404, "application/json", "{\"success\":false,\"message\":\"Not Found\"}");
    });
    
    // ── SD Card API endpoints ─────────────────────────────────
    server.on("/api/messages", HTTP_GET, []() {
        if (!isAuthenticated()) { server.send(401, "application/json", JSON_FAIL_UNAUTHORIZED); return; }
        if (!sdCard.isReady()) { server.send(503, "application/json", "{\"error\":\"SD card not available\"}"); return; }
        int offset = server.hasArg("offset") ? server.arg("offset").toInt() : 0;
        int limit  = server.hasArg("limit")  ? server.arg("limit").toInt()  : 50;
        if (limit > 50) limit = 50;
        sdCard.loadRecentMessages(MAX_CACHED_MESSAGES);
        int total = sdCard.getMessageCount();
        DynamicJsonDocument doc(4096);
        doc["total"] = total;
        JsonArray arr = doc.createNestedArray("messages");
        int start = (total > limit + offset) ? total - limit - offset : 0;
        int end   = total - offset;
        if (start < 0) start = 0;
        if (end > total) end = total;
        for (int i = end - 1; i >= start; i--) {
            const SavedMessage& m = sdCard.getCachedMessage(i);
            JsonObject o = arr.createNestedObject();
            o["id"]  = m.id;
            o["ts"]  = (unsigned long)m.timestamp;
            o["src"] = m.source;
            o["preview"] = m.preview;
        }
        String out;
        serializeJson(doc, out);
        server.send(200, "application/json", out);
    });

    server.on("/api/audio", HTTP_GET, []() {
        if (!isAuthenticated()) { server.send(401, "application/json", JSON_FAIL_UNAUTHORIZED); return; }
        if (!sdCard.isReady()) { server.send(503, "application/json", "{\"error\":\"SD card not available\"}"); return; }
        String names[MAX_AUDIO_FILES];
        int count = sdCard.listAudioFiles(names, MAX_AUDIO_FILES);
        DynamicJsonDocument doc(2048);
        JsonArray arr = doc.createNestedArray("files");
        for (int i = 0; i < count; i++) arr.add(names[i]);
        doc["playing"] = audioSvc.isPlaying();
        doc["current"] = audioSvc.currentFile();
        String out;
        serializeJson(doc, out);
        server.send(200, "application/json", out);
    });

    server.on("/api/audio/play", HTTP_POST, []() {
        if (!isAuthenticated()) { server.send(401, "application/json", JSON_FAIL_UNAUTHORIZED); return; }
        String body = server.arg("plain");
        DynamicJsonDocument doc(256);
        if (deserializeJson(doc, body) != DeserializationError::Ok) {
            server.send(400, "application/json", "{\"error\":\"Invalid JSON\"}");
            return;
        }
        String file = doc["file"] | "";
        if (file.length() == 0) { server.send(400, "application/json", "{\"error\":\"Missing file\"}"); return; }
        if (!isSafeFilename(file)) { server.send(400, "application/json", "{\"error\":\"Invalid filename\"}"); return; }
        String path = String(AUDIO_DIR) + "/" + file;
        bool ok = audioSvc.playFile(path);
        server.send(ok ? 200 : 500, "application/json", ok ? JSON_SUCCESS : "{\"error\":\"Playback failed\"}");
    });

    server.on("/api/audio/stop", HTTP_POST, []() {
        if (!isAuthenticated()) { server.send(401, "application/json", JSON_FAIL_UNAUTHORIZED); return; }
        audioSvc.stop();
        server.send(200, "application/json", JSON_SUCCESS);
    });

    server.on("/api/audio/download", HTTP_POST, []() {
        if (!isAuthenticated()) { server.send(401, "application/json", JSON_FAIL_UNAUTHORIZED); return; }
        String body = server.arg("plain");
        DynamicJsonDocument doc(512);
        if (deserializeJson(doc, body) != DeserializationError::Ok) {
            server.send(400, "application/json", "{\"error\":\"Invalid JSON\"}");
            return;
        }
        String url = doc["url"] | "";
        String name = doc["name"] | "";
        if (url.length() == 0 || name.length() == 0) {
            server.send(400, "application/json", "{\"error\":\"Missing url or name\"}");
            return;
        }
        bool ok = audioSvc.downloadToSD(url, name);
        server.send(ok ? 200 : 500, "application/json", ok ? JSON_SUCCESS : "{\"error\":\"Download failed\"}");
    });

    server.on("/api/images", HTTP_GET, []() {
        if (!isAuthenticated()) { server.send(401, "application/json", JSON_FAIL_UNAUTHORIZED); return; }
        if (!sdCard.isReady()) { server.send(503, "application/json", "{\"error\":\"SD card not available\"}"); return; }
        String names[MAX_IMAGE_FILES];
        int count = sdCard.listImageFiles(names, MAX_IMAGE_FILES);
        DynamicJsonDocument doc(2048);
        JsonArray arr = doc.createNestedArray("files");
        for (int i = 0; i < count; i++) arr.add(names[i]);
        String out;
        serializeJson(doc, out);
        server.send(200, "application/json", out);
    });

    server.on("/api/images/download", HTTP_POST, []() {
        if (!isAuthenticated()) { server.send(401, "application/json", JSON_FAIL_UNAUTHORIZED); return; }
        String body = server.arg("plain");
        DynamicJsonDocument doc(512);
        if (deserializeJson(doc, body) != DeserializationError::Ok) {
            server.send(400, "application/json", "{\"error\":\"Invalid JSON\"}");
            return;
        }
        String url = doc["url"] | "";
        String name = doc["name"] | "";
        if (url.length() == 0 || name.length() == 0) {
            server.send(400, "application/json", "{\"error\":\"Missing url or name\"}");
            return;
        }
        if (!isSafeFilename(name)) {
            server.send(400, "application/json", "{\"error\":\"Invalid filename\"}");
            return;
        }
        String path = String(IMAGES_MEDIA_DIR) + "/" + name;
        if (SD.exists(path)) { server.send(200, "application/json", JSON_SUCCESS); return; }
        HTTPClient http;
        http.begin(url);
        http.setTimeout(15000);
        bool ok = false;
        if (http.GET() == HTTP_CODE_OK) {
            File f = SD.open(path, FILE_WRITE);
            if (f) {
                WiFiClient* stream = http.getStreamPtr();
                uint8_t buf[1024];
                int total = 0;
                while (http.connected() && stream->available()) {
                    int n = stream->readBytes(buf, sizeof(buf));
                    if (n > 0) { f.write(buf, n); total += n; }
                }
                f.close();
                ok = (total > 0);
            }
        }
        http.end();
        server.send(ok ? 200 : 500, "application/json", ok ? JSON_SUCCESS : "{\"error\":\"Download failed\"}");
    });

    server.begin();
}

void setup() {
    Serial.begin(115200);
    delay(1000);
    
    esp_task_wdt_init(60, true);   // 60s so setup (WiFi portal + backend + display) doesn't trigger
    esp_task_wdt_add(NULL);        // Current task (loop) is watched
    
    // Initialize WiFi
    WiFiManager wifiManager;
    wifiManager.setDebugOutput(false);  // Suppress *wm: verbose logs
    wifiManager.setAPStaticIPConfig(IPAddress(192, 168, 4, 1), 
                                     IPAddress(192, 168, 4, 1), 
                                     IPAddress(255, 255, 255, 0));
#if FORCE_WIFI_CONFIG_PORTAL
    Serial.println("FORCE_WIFI_CONFIG_PORTAL: erasing WiFi, opening config portal (AP: " TZT_AP_SSID ")");
    wifiManager.resetSettings();
    wifiManager.startConfigPortal(TZT_AP_SSID, TZT_AP_PASSWORD);
#else
    esp_task_wdt_reset();  // Feed WDT before blocking autoConnect (portal can take 30s+)
    if (!wifiManager.autoConnect(TZT_AP_SSID, TZT_AP_PASSWORD)) {
        Serial.println("WiFi config portal timed out or failed.");
    }
#endif
    
    if (WiFi.status() == WL_CONNECTED) {
        deviceIP = WiFi.localIP().toString();
    } else {
        deviceIP = "192.168.4.1";
    }

#if (BOARD_LED_PIN >= 0)
    pinMode(BOARD_LED_PIN, OUTPUT);
    digitalWrite(BOARD_LED_PIN, BOARD_LED_ACTIVE_LOW ? HIGH : LOW);  // Off until WiFi connected
#endif

    // Initialize OTA service
    otaService = new OTAUpdateService();
    #ifdef OTA_PASSWORD
    otaService->initialize("TZT-Display", OTA_PASSWORD);
    #else
    otaService->initialize("TZT-Display");
    #endif

    configTime(GMT_OFFSET_SEC, DAYLIGHT_OFFSET_SEC, NTP_SERVER);

    esp_task_wdt_reset();  // Feed WDT before backend init
    Logger::setLevel(LOG_LEVEL_WARN);
    backend = new HttpBackendService(BACKEND_URL, BACKEND_TIMEOUT);
    backend->setRetryPolicy(3, 1000);
    backend->setRateLimit(60);
    reminderService = new ReminderService(backend);
    // Load reminders from SD first (primary persistence)
    if (sdCard.isReady()) {
        String r;
        if (sdCard.readFile(DATA_DIR "/reminders.json", r) && r.length())
            reminderService->fromJSON(r);
    }
    loadGroceries();
    loadTodos();
    loadSettingsFromSD();  // Restore LED, autoDispense, autoBrightness, pump duration/cooldown; send to Main

    // Initialize SD card (VSPI — independent of display HSPI)
    if (sdCard.begin()) {
        sdCard.loadRecentMessages(MAX_CACHED_MESSAGES);
        audioSvc.begin();
    } else {
        Serial.println("[SD] Skipped: message history & media will use backend only");
    }

    esp_task_wdt_reset();  // Feed WDT before display init
#if !defined(TZT_HEADLESS)
    tft.init();
    tft.setRotation(1);  // 2.4" 240x320 panel: rotation 1 = 320x240 landscape (matches TZT_SCREEN_*)
    tft.fillScreen(TFT_BLACK);
    // Backlight PWM for full brightness (digital HIGH can be dim on some boards)
    ledcSetup(TZT_TFT_BL_LEDC_CHANNEL, TZT_TFT_BL_LEDC_FREQ, TZT_TFT_BL_LEDC_RES);
    ledcAttachPin(TZT_TFT_BL_PIN, TZT_TFT_BL_LEDC_CHANNEL);
    ledcWrite(TZT_TFT_BL_LEDC_CHANNEL, TZT_TFT_BL_BRIGHTNESS);
    tft.fillScreen(TFT_BLACK);
    bool drewSplash = sdCard.isReady() && SD.exists(SPLASH_PATH);
    drawSplashToTft();  // Show splash.jpg from SD root if present
    splashVisible = true;
    // If no splash image, show main UI on first loop; else show splash for SPLASH_MIN_MS
    splashReadyAt = millis() + (drewSplash ? SPLASH_MIN_MS : 0);
    lv_init();
    lv_disp_draw_buf_init(&draw_buf, disp_buf1, disp_buf2, TZT_DISP_BUF_PIXELS);
    lv_disp_drv_init(&disp_drv);
    disp_drv.hor_res = TZT_SCREEN_WIDTH;
    disp_drv.ver_res = TZT_SCREEN_HEIGHT;
    disp_drv.draw_buf = &draw_buf;
    disp_drv.flush_cb = lvgl_flush_cb;
    lv_disp_drv_register(&disp_drv);
    // Touch input (XPT2046, TOUCH_CS 33 — uses getTouchRaw() + affine calibration from config_tzt.h)
    static lv_indev_drv_t indev_drv;
    lv_indev_drv_init(&indev_drv);
    indev_drv.type = LV_INDEV_TYPE_POINTER;
    indev_drv.read_cb = touch_read_cb;
    lv_indev_drv_register(&indev_drv);
    // createControlPanel() deferred to first loop when splash ends (smooth transition to UI)
#else
    Serial.println("TZT_HEADLESS (no display)");
#endif

    esp_task_wdt_reset();  // Feed WDT before ESP-NOW and web server
    
    // Check if MAC address is configured
    bool macSet = false;
    for (int i = 0; i < 6; i++) {
        if (mainESP32Mac[i] != 0) {
            macSet = true;
            break;
        }
    }
    
    if (macSet) {
        // Wait a bit for WiFi to fully stabilize before initializing ESP-NOW
        delay(500);
        
        // WiFi already in STA from WiFiManager.autoConnect; do not call WiFi.mode(WIFI_STA) here
        // (can briefly glitch connection and wrong channel at add_peer).
        
        // Initialize ESP-NOW
        esp_err_t initResult = esp_now_init();
        if (initResult == ESP_OK) {
            esp_now_register_send_cb(onDataSentStatic);
            esp_now_register_recv_cb(onDataRecvStatic);
            
            // Add peer (Main ESP32): zero struct so ifidx is not garbage (fixes "Peer interface is invalid")
            esp_now_peer_info_t peerInfo;
            memset(&peerInfo, 0, sizeof(peerInfo));
            memcpy(peerInfo.peer_addr, mainESP32Mac, 6);
            peerInfo.ifidx = WIFI_IF_STA;
            // Use explicit WiFi channel (ESP-IDF example uses explicit channel, not 0)
            // Get current WiFi channel, fallback to ESP_NOW_CHANNEL if not connected
            uint8_t wifiChannel = (WiFi.status() == WL_CONNECTED && WiFi.channel() > 0) ? WiFi.channel() : (uint8_t)ESP_NOW_CHANNEL;
            peerInfo.channel = wifiChannel;
            peerInfo.encrypt = false;
            
            // Remove peer if it already exists (in case of re-initialization)
            esp_now_del_peer(mainESP32Mac);
            delay(50);
            
            esp_err_t addPeerResult = esp_now_add_peer(&peerInfo);
            if (addPeerResult == ESP_OK) {
                // Verify peer was added successfully
                delay(100);  // Small delay for peer to be fully registered
                if (esp_now_is_peer_exist(mainESP32Mac)) {
                    espNowInitialized = true;
                    if (bootSettingsCount > 0 && bootSettingsIndex < bootSettingsCount)
                        applyNextBootSetting();
                } else {
                    Serial.println("⚠️ ESP-NOW: peer verify failed");
                }
            } else {
                String errStr = "";
                switch(addPeerResult) {
                    case ESP_ERR_ESPNOW_NOT_INIT: errStr = "NOT_INIT"; break;
                    case ESP_ERR_ESPNOW_ARG: errStr = "ARG"; break;
                    case ESP_ERR_ESPNOW_FULL: errStr = "FULL"; break;
                    case ESP_ERR_ESPNOW_NO_MEM: errStr = "NO_MEM"; break;
                    case ESP_ERR_ESPNOW_EXIST: errStr = "EXIST"; break;
                    default: errStr = "UNKNOWN(" + String((int)addPeerResult) + ")"; break;
                }
                Serial.println("⚠️ ESP-NOW failed to add peer: " + errStr);
            }
        } else {
            Serial.println("⚠️ ESP-NOW initialization failed: " + String((int)initResult));
        }
    } else {
        Serial.println("⚠️ Main ESP32 MAC address not configured in config_tzt.h");
        Serial.println("   To enable ESP-NOW, set MAIN_ESP32_MAC_ADDRESS in config_tzt.h");
    }
    
    setupWebServer();
    Serial.println("TZT Display ready | http://" + deviceIP + ":8080");
}

void loop() {
    esp_task_wdt_reset();
    if (otaService) otaService->handle();
    server.handleClient();

#if !defined(TZT_HEADLESS)
    // Splash: when time is up, create UI and switch to main screen (one-time)
    if (splashVisible) {
        if (millis() >= splashReadyAt) {
            createControlPanel();
            updateControlPanelStatus();
            lv_scr_load(screen1);
            splashVisible = false;
        }
        delay(2);
        return;
    }
    // Show test image if requested via /api/test/show-image
    if (pendingTestImagePath.length() > 0) {
        String path = pendingTestImagePath;
        pendingTestImagePath = "";
        displayFlushing = true;
        drawSdJpegToTft(path.c_str());
        displayFlushing = false;
        hideNavBar();
        lv_scr_load(imageViewerScreen);
    }
    if (labelMoisture) {
        if (sensorDataNeedsPanelUpdate) {
            sensorDataNeedsPanelUpdate = false;
            lastPanelUpdate = millis();
            lastSensorHash = calculateSensorHash();
            updateControlPanelStatus();
#if DEBUG_LOOP
            Serial.println("[Loop] panel update (sensor_flag)");
#endif
        } else if (millis() - lastPanelUpdate >= 2000) {
            uint32_t currentHash = calculateSensorHash();
            if (currentHash != lastSensorHash) {
                lastPanelUpdate = millis();
                lastSensorHash = currentHash;
                updateControlPanelStatus();
#if DEBUG_LOOP
                Serial.println("[Loop] panel update (hash_changed)");
#endif
            }
        }
        // Update status bar clock every ~10s
        static unsigned long lastStatusBarUpdate = 0;
        if (millis() - lastStatusBarUpdate >= 10000) {
            lastStatusBarUpdate = millis();
            updateStatusBar();
        }
        // Fallback: refresh only if no update for 10s (sensor_flag + hash keep panel fresh)
        if (lastSensorDataValid && (millis() - lastPanelUpdate >= 10000)) {
            lastPanelUpdate = millis();
            lastSensorHash = calculateSensorHash();
            updateControlPanelStatus();
#if DEBUG_LOOP
            Serial.println("[Loop] panel update (2s_fallback)");
#endif
        }
    }
    // Process pending gesture immediately so nav feels responsive
    if (gesturePending) {
        gesture_timer_cb(nullptr);
    }
#if DEBUG_LOOP
    static unsigned long lastLoopLog = 0;
    if (millis() - lastLoopLog >= 2000) {
        lastLoopLog = millis();
        Serial.printf("[Loop] tick | sensorValid=%d panelAge=%lums\n",
            lastSensorDataValid ? 1 : 0, labelMoisture ? (unsigned long)(millis() - lastPanelUpdate) : 0);
    }
#endif
#endif

    // Timeout: if chunked send in progress but no CHUNK_ACK for 15s, abort so next print can start
    if (pendingChunkTotal > 0 && pendingChunkSendTime > 0 && (millis() - pendingChunkSendTime) > CHUNK_ACK_TIMEOUT_MS) {
        pendingChunkTotal = 0;
        pendingChunkSendTime = 0;
        nextChunkSendAt = 0;
        chunkResendAt = 0;
        chunkResendCount = 0;
    }
    // Cap chunk resends to stop infinite retry loop; Main dedupes if late copy arrives
    if (pendingChunkTotal > 0 && chunkResendCount >= CHUNK_MAX_RESENDS) {
        pendingChunkTotal = 0;
        nextChunkSendAt = 0;
        chunkResendAt = 0;
        chunkResendCount = 0;
    }
    // Send next chunk from loop (after CHUNK_ACK received) — never send from callback
    if (espNowInitialized && pendingChunkTotal > 0 && nextChunkSendAt > 0 && millis() >= nextChunkSendAt) {
        nextChunkSendAt = 0;
        pendingChunkSendTime = millis();
        String chunk = pendingChunkMessage.substring(pendingChunkIndex * PRINT_CHUNK_SIZE, (pendingChunkIndex + 1) * PRINT_CHUNK_SIZE);
        sendCommandViaESPNow(CMD_PRINT_CHUNK, (uint8_t)pendingChunkIndex, pendingChunkTotal, chunk.c_str());
    }
    // Resend current chunk after send failed (chunk-level retry so prints never fail)
    if (espNowInitialized && pendingChunkTotal > 0 && chunkResendCount < CHUNK_MAX_RESENDS && chunkResendAt > 0 && millis() >= chunkResendAt) {
        chunkResendAt = 0;
        chunkResendCount++;
        pendingChunkSendTime = millis();
        String chunk = pendingChunkMessage.substring(pendingChunkIndex * PRINT_CHUNK_SIZE, (pendingChunkIndex + 1) * PRINT_CHUNK_SIZE);
        sendCommandViaESPNow(CMD_PRINT_CHUNK, (uint8_t)pendingChunkIndex, pendingChunkTotal, chunk.c_str(), true);
    }
    // Periodic resend: if no CHUNK_ACK for 2s, resend current chunk (handshake never stalls)
    if (espNowInitialized && pendingChunkTotal > 0 && chunkResendCount < CHUNK_MAX_RESENDS && pendingChunkSendTime > 0 && (millis() - pendingChunkSendTime) > CHUNK_RESEND_INTERVAL_MS) {
        chunkResendCount++;
        pendingChunkSendTime = millis();
        String chunk = pendingChunkMessage.substring(pendingChunkIndex * PRINT_CHUNK_SIZE, (pendingChunkIndex + 1) * PRINT_CHUNK_SIZE);
        sendCommandViaESPNow(CMD_PRINT_CHUNK, (uint8_t)pendingChunkIndex, pendingChunkTotal, chunk.c_str(), true);
    }
    // Reliable settings: send once, wait for ACK; resend every 400ms if no ACK (handshake like printer)
    if (espNowInitialized && pendingChunkTotal == 0 && pendingSettingRemainingSends > 0 && millis() >= pendingSettingNextSendAt) {
        sendingFromSettingsQueue = true;
        sendCommandViaESPNow(pendingSettingCommandType, pendingSettingParam1, pendingSettingParam2, pendingSettingMessage, false, true);
        sendingFromSettingsQueue = false;
        // Guard against underflow: the ACK can arrive (on the ESP-NOW task) while the blocking send
        // above was still running, already zeroing this out - an unconditional decrement would wrap
        // a uint8_t 0 to 255, causing ~100s of unnecessary resends of an already-applied setting.
        if (pendingSettingRemainingSends > 0) pendingSettingRemainingSends--;
        pendingSettingNextSendAt = millis() + SETTINGS_ACK_TIMEOUT_MS;  // Next attempt only if no ACK
    }
    // Retry TZT→Main command once if previous send callback reported FAIL (skip for chunked print — chunk resend handles it)
    if (espNowInitialized && retryPending && millis() >= retryAt) {
        retryPending = false;
        if (pendingChunkTotal > 0 && (lastSentCommandType == CMD_PRINT_CHUNK || lastSentCommandType == CMD_PRINT_CHUNK_START)) {
            // Chunk resend / nextChunkSendAt handles it; don't duplicate
        } else {
            static unsigned long lastRetryLog = 0;
            if (millis() - lastRetryLog > 15000) {
                Serial.println("[ESP-NOW] retrying...");
                lastRetryLog = millis();
            }
            sendCommandViaESPNow(lastSentCommandType, lastSentParam1, lastSentParam2, lastSentMessage, true);
        }
    }

#if (BOARD_LED_PIN >= 0)
    // Onboard blue LED: on when WiFi connected. BOARD_LED_ACTIVE_LOW: 1=LOW=on, 0=HIGH=on
    digitalWrite(BOARD_LED_PIN, (WiFi.status() == WL_CONNECTED)
        ? (BOARD_LED_ACTIVE_LOW ? LOW : HIGH) : (BOARD_LED_ACTIVE_LOW ? HIGH : LOW));
#endif

    // ESP-NOW peer verification only when we see send failures (reduces overhead when healthy)
    if (espNowInitialized && consecutiveFailures > 3) {
        unsigned long now = millis();
        if (now - lastPeerCheck > 5000) {  // Check every 5s when failing
            lastPeerCheck = now;
            
            bool macSet = false;
            for (int i = 0; i < 6; i++) { if (mainESP32Mac[i] != 0) { macSet = true; break; } }
            
            if (macSet) {
                uint8_t currentChannel = (WiFi.status() == WL_CONNECTED && WiFi.channel() > 0) ? WiFi.channel() : (uint8_t)ESP_NOW_CHANNEL;
                
                if (!esp_now_is_peer_exist(mainESP32Mac)) {
                    // Peer lost - re-add it
                    esp_now_peer_info_t peerInfo;
                    memset(&peerInfo, 0, sizeof(peerInfo));
                    memcpy(peerInfo.peer_addr, mainESP32Mac, 6);
                    peerInfo.ifidx = WIFI_IF_STA;
                    peerInfo.channel = currentChannel;
                    peerInfo.encrypt = false;
                    
                    if (esp_now_add_peer(&peerInfo) == ESP_OK) {
                        // Peer re-added (no log to reduce clutter)
                    }
                } else {
                    // Verify channel matches
                    esp_now_peer_info_t peerInfo;
                    if (esp_now_get_peer(mainESP32Mac, &peerInfo) == ESP_OK) {
                        if (peerInfo.channel != currentChannel) {
                            // Channel changed - update peer
                            uint8_t oldChannel = peerInfo.channel;
                            esp_now_del_peer(mainESP32Mac);
                            delay(50);
                            // Recreate peer info with new channel
                            memset(&peerInfo, 0, sizeof(peerInfo));
                            memcpy(peerInfo.peer_addr, mainESP32Mac, 6);
                            peerInfo.ifidx = WIFI_IF_STA;
                            peerInfo.channel = currentChannel;
                            peerInfo.encrypt = false;
                            if (esp_now_add_peer(&peerInfo) == ESP_OK) {
                                // Channel updated (no log to reduce clutter)
                            }
                        }
                    }
                }
            }
            consecutiveFailures = 0;  // Reset after running peer verification
        }
    }

    // Save groceries to SD (debounced: max once per 5s)
    if (groceriesNeedSave && sdCard.isReady()) {
        unsigned long now = millis();
        if (now - lastGrocerySaveTime >= SAVE_DEBOUNCE_MS) {
            groceriesNeedSave = false;
            lastGrocerySaveTime = now;
            saveGroceries();
        }
    }

    // Save settings to SD (debounced: max once per 5s) so restart remembers LED, toggles, pump duration/cooldown
    if (settingsNeedSave && sdCard.isReady()) {
        unsigned long now = millis();
        if (now - lastSettingsSaveTime >= SAVE_DEBOUNCE_MS) {
            settingsNeedSave = false;
            lastSettingsSaveTime = now;
            saveSettingsToSD();
        }
    }
    
    // Save todos to SD
    if (todosNeedSave && sdCard.isReady()) {
        todosNeedSave = false;
        saveTodos();
    }
    
    // Save reminders to SD
    if (remindersNeedSave && reminderService && sdCard.isReady()) {
        remindersNeedSave = false;
        sdCard.writeFile(DATA_DIR "/reminders.json", reminderService->toJSON());
    }

    // Poll backend /commands with adaptive interval (HTTP server)
    static unsigned long lastCmd = 0;
    static unsigned long backendPollInterval = 30000;
    static unsigned long lastCommandFound = 0;
    if (millis() - lastCommandFound < 120000) {
        backendPollInterval = 10000;
    } else {
        backendPollInterval = 30000;
    }
    
    if (backend && (millis() - lastCmd >= backendPollInterval)) {
        lastCmd = millis() + (unsigned long)random(0, 2000);
        DynamicJsonDocument cmds(2048);
        bool pollOk = backend->pollCommands(cmds);
        if (!pollOk) Serial.println("[Backend] Poll failed (check Pi URL/WiFi)");
        if (pollOk) {
            static unsigned long lastHeartbeat = 0;
            if (millis() - lastHeartbeat > 120000) {  // Every 2 min
                if (backend->sendHeartbeat("tzt-display", "TZT Display")) {
                    lastHeartbeat = millis();
                }
            }
        }
        if (pollOk && cmds.size() > 0) {
            Serial.println("[Backend] Commands: " + String(cmds.size()));
            lastCommandFound = millis();
            for (JsonPair kv : cmds.as<JsonObject>()) {
                JsonObject c = kv.value();
                if (c["processed"] | false) continue;
                String typ = c["type"].as<String>(), data = c["data"].as<String>();
                // Commands: text → print + save to SD + display; audio → send/save + play; image → send/save + show
                if (typ == "print") {
                    Serial.println("[Backend] Print: \"" + data.substring(0, data.length() > 40 ? 40 : data.length()) + (data.length() > 40 ? "..." : "") + "\"");
                    String source = c["source"].as<String>();
                    bool messageOnly = !source.equalsIgnoreCase("shortcut");
                    sendPrintChunked(data, messageOnly);
                    sdCard.saveMessage(data, source);
#if !defined(TZT_HEADLESS)
                    lastPrintMessage = data;
                    if (labelMessage) {
                        lv_label_set_text(labelMessage, lastPrintMessage.c_str());
                    }
#endif
                }
                else if (typ == "download_audio") {
                    String url = c["url"] | "";
                    String name = c["name"] | "";
                    if (url.length() > 0 && name.length() > 0 && isSafeFilename(name) && sdCard.isReady()) {
                        if (audioSvc.downloadToSD(url, name)) {
                            lastDashboardAudioName = name;
                            if (dashboardAudioNameLabel) lv_label_set_text(dashboardAudioNameLabel, name.c_str());
                            String path = String(AUDIO_DIR) + "/" + name;
                            audioSvc.playFile(path);
                        }
                    }
                }
                else if (typ == "play_audio" && data.length() > 0 && isSafeFilename(data)) {
                    lastDashboardAudioName = data;
                    if (dashboardAudioNameLabel) lv_label_set_text(dashboardAudioNameLabel, data.c_str());
                    String path = String(AUDIO_DIR) + "/" + data;
                    audioSvc.playFile(path);
                }
                else if (typ == "download_image") {
                    String url = c["url"] | "";
                    String name = c["name"] | "";
                    if (url.length() > 0 && name.length() > 0 && isSafeFilename(name) && sdCard.isReady()) {
                        String path = String(IMAGES_MEDIA_DIR) + "/" + name;
                        HTTPClient http;
                        http.begin(url);
                        http.setTimeout(15000);
                        bool ok = false;
                        if (http.GET() == HTTP_CODE_OK) {
                            File f = SD.open(path, FILE_WRITE);
                            if (f) {
                                WiFiClient* stream = http.getStreamPtr();
                                uint8_t buf[1024];
                                int total = 0;
                                while (http.connected() && stream->available()) {
                                    int n = stream->readBytes(buf, sizeof(buf));
                                    if (n > 0) { f.write(buf, n); total += n; }
                                }
                                f.close();
                                ok = (total > 0);
                            }
                        }
                        http.end();
#if !defined(TZT_HEADLESS)
                        if (ok) {
                            pendingTestImagePath = path;
                            lastDashboardImagePath = path;
                        }
#endif
                    }
                }
                else if (typ == "show_image" && data.length() > 0 && isSafeFilename(data)) {
#if !defined(TZT_HEADLESS)
                    String showPath = String(IMAGES_MEDIA_DIR) + "/" + data;
                    if (!SD.exists(showPath)) showPath = String(IMAGES_DIR) + "/" + data;
                    pendingTestImagePath = showPath;
                    lastDashboardImagePath = showPath;
#endif
                }
                String cmdPath = "/commands/" + String(kv.key().c_str()) + ".json";
                if (!backend->deleteData(cmdPath)) {
                    Serial.println("[Backend] DELETE failed for " + String(kv.key().c_str()) + ": " + backend->getLastError());
                }
                delay(300);  // brief pause between DELETEs so server/connection can handle
            }
        }
    }

    // Reminder check every 10s when time is synced (NTP) -> print via ESP-NOW, mark, save
    static unsigned long lastRem = 0;
    time_t nowSec = time(nullptr);
    const time_t minValidTime = 1600000000;  // Sep 2020 - skip if NTP not synced yet
    if (reminderService && (millis() - lastRem >= 10000) && nowSec > minValidTime) {
        lastRem = millis();
        reminderService->checkReminders([&](const Reminder& r) {
            String msg = "REMINDER\n--------------------------------\n";
            msg += r.message;
            sendPrintChunked(msg);
            remindersNeedSave = true;  // Only save when a reminder actually fired
        });
    }

    // Audio: poll backend /audio for new entries, download to SD
    static unsigned long lastAudioPoll = 0;
    if (backend && sdCard.isReady() && (millis() - lastAudioPoll >= 60000)) {
        lastAudioPoll = millis();
        String resp;
        if (backend->get("/audio.json", resp) && resp.length() > 4 && resp != "null") {
            DynamicJsonDocument adoc(2048);
            if (deserializeJson(adoc, resp) == DeserializationError::Ok) {
                for (JsonPair kv : adoc.as<JsonObject>()) {
                    JsonObject entry = kv.value();
                    String url = entry["url"] | "";
                    String name = entry["name"] | "";
                    bool downloaded = entry["downloaded"] | false;
                    if (url.length() > 0 && name.length() > 0 && !downloaded && isSafeFilename(name)) {
                        if (audioSvc.downloadToSD(url, name)) {
                            backend->put("/audio/" + String(kv.key().c_str()) + "/downloaded.json", "true");
                        }
                    }
                }
            }
        }
    }

    // Images: poll backend /images for new entries, download to SD
    static unsigned long lastImagesPoll = 0;
    if (backend && sdCard.isReady() && (millis() - lastImagesPoll >= 60000)) {
        lastImagesPoll = millis();
        String resp;
        if (backend->get("/images.json", resp) && resp.length() > 4 && resp != "null") {
            DynamicJsonDocument idoc(2048);
            if (deserializeJson(idoc, resp) == DeserializationError::Ok) {
                for (JsonPair kv : idoc.as<JsonObject>()) {
                    JsonObject entry = kv.value();
                    String url = entry["url"] | "";
                    String name = entry["name"] | "";
                    bool downloaded = entry["downloaded"] | false;
                    if (url.length() > 0 && name.length() > 0 && !downloaded && isSafeFilename(name)) {
                        String path = String(IMAGES_MEDIA_DIR) + "/" + name;
                        if (SD.exists(path)) {
                            backend->put("/images/" + String(kv.key().c_str()) + "/downloaded.json", "true");
                        } else {
                            HTTPClient http;
                            http.begin(url);
                            http.setTimeout(15000);
                            if (http.GET() == HTTP_CODE_OK) {
                                File f = SD.open(path, FILE_WRITE);
                                if (f) {
                                    WiFiClient* stream = http.getStreamPtr();
                                    uint8_t buf[1024];
                                    int total = 0;
                                    while (http.connected() && stream->available()) {
                                        int n = stream->readBytes(buf, sizeof(buf));
                                        if (n > 0) { f.write(buf, n); total += n; }
                                    }
                                    f.close();
                                    if (total > 0)
                                        backend->put("/images/" + String(kv.key().c_str()) + "/downloaded.json", "true");
                                }
                            }
                            http.end();
                        }
                    }
                }
            }
        }
    }

    audioSvc.loop();

#if !defined(TZT_HEADLESS)
    lv_timer_handler();
    // Redraw JPEG when image viewer is active so it stays visible (LVGL would overwrite with screen bg).
    // Throttle to every 200ms to avoid GUI lag (full decode+push every frame was too heavy).
    static unsigned long lastImageViewerRedraw = 0;
    if (imageViewerScreen && lv_scr_act() == imageViewerScreen && currentImagePath.length() > 0) {
        unsigned long now = millis();
        if (now - lastImageViewerRedraw >= 200) {
            lastImageViewerRedraw = now;
            displayFlushing = true;
            drawSdJpegToTft(currentImagePath.c_str(), TZT_SCREEN_HEIGHT);
            displayFlushing = false;
        }
    }
    // Dashboard Photo tab: draw latest image when that tab is active
    static unsigned long lastDashboardPhotoRedraw = 0;
    if (dashboardPhotoScreen && lv_scr_act() == dashboardPhotoScreen && lastDashboardImagePath.length() > 0) {
        unsigned long now = millis();
        if (now - lastDashboardPhotoRedraw >= 500) {
            lastDashboardPhotoRedraw = now;
            displayFlushing = true;
            drawSdJpegToTft(lastDashboardImagePath.c_str(), TZT_SCREEN_HEIGHT);
            displayFlushing = false;
        }
    }
    // Dashboard: auto-rotate Home/Sensors/Media every 5s when untouched for 2 min.
    // Never auto-advance into or out of Settings (tab index 3) - only cycle 0..2.
    if (dashboardPhotoScreen && currentScreenIndex >= 0 && currentScreenIndex < 3 &&
        (unsigned long)(millis() - lastDashboardTouchTime) > DASHBOARD_TOUCH_PAUSE_MS &&
        (unsigned long)(millis() - lastDashboardAutoAdvance) >= DASHBOARD_AUTO_ADVANCE_MS) {
        lastDashboardAutoAdvance = millis();
        goToScreenIndex((currentScreenIndex + 1) % 3);
    }
    // Keep audio header/Stop button in sync when playback ends naturally
    if (audioListScreen && lv_scr_act() == audioListScreen) updateAudioHeaderState();
#endif
    delay(0);  // Yield only — no fixed delay so LVGL gets more CPU for smoother GUI
}
