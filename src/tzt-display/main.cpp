/*
 * TZT ESP32 LVGL Display - Web Server + HTTP Backend + ESP-NOW
 * Hosts web UI, talks to HTTP backend (commands, reminders, groceries), sends commands to Main via ESP-NOW.
 * Receives sensor data from Main via ESP-NOW.
 */

#include <Arduino.h>
#include <WiFi.h>
#include <WiFiManager.h>
#include <WebServer.h>
#include <ArduinoJson.h>
#include <time.h>
#if !defined(TZT_HEADLESS)
#include <lvgl.h>
#include <TFT_eSPI.h>
#include <JPEGDecoder.h>
#include "UiTheme.h"
#endif
#include "WebTheme.h"

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
#include <esp_system.h>
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
volatile unsigned long lastSensorDataReceiveTime = 0;  // Track when we last received data from Main
#define SENSOR_DATA_STALE_MS 35000  // 3 missed 10 s heartbeats (+ margin) -> show "main board offline"
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
// Launcher = three big buttons (Messages, Sounds, Pictures); Settings opens from the header gear.
// Each section is its own full screen under the status bar.
static lv_obj_t* messagesScreen = nullptr;   // message list + full-text overlay
static lv_obj_t* soundsScreen = nullptr;     // sound list + now-playing bar
static lv_obj_t* picturesScreen = nullptr;   // picture list (tap -> full-screen viewer)
#define SCREEN_COUNT 4      // Sections: 0 Messages, 1 Sounds, 2 Pictures, 3 Settings
#define SCREEN_MESSAGES 0
#define SCREEN_SOUNDS   1
#define SCREEN_PICTURES 2
#define SCREEN_SETTINGS 3
#define LAUNCHER_BUTTONS 3  // Settings is not a launcher button
#define SCREEN_LAUNCHER -1
static int currentScreenIndex = SCREEN_LAUNCHER;
#define DASHBOARD_IDLE_RETURN_MS  120000  // untouched for 2 min on a section -> back to launcher
static unsigned long lastDashboardTouchTime = 0;   // any touch resets the idle-return timer
static lv_obj_t* launcherScreen = nullptr;
#define NO_MESSAGE_TEXT "(No messages yet)"
static String lastPrintMessage = NO_MESSAGE_TEXT;
// Messages section
static lv_obj_t* historyList = nullptr;
static lv_obj_t* historyDetail = nullptr;
static lv_obj_t* historyDetailLabel = nullptr;
static void refreshHistoryList();
static void historyItemCb(lv_event_t* e);
static void historyBackCb(lv_event_t* e);
// Sounds section
static lv_obj_t* audioList = nullptr;
static lv_obj_t* audioNowPlayingLabel = nullptr;
static lv_obj_t* audioStopBtn = nullptr;
static void updateNowPlaying();
#define MAX_AUDIO_FILES 20
static String audioFileNames[MAX_AUDIO_FILES];
static int audioFileCount = 0;
static void refreshAudioList();
static void audioFileCb(lv_event_t* e);
static void audioStopCb(lv_event_t* e);
// Pictures section
#define MAX_IMAGE_FILES 30
static String imageFileNames[MAX_IMAGE_FILES];
static int imageFileCount = 0;
static lv_obj_t* imageList = nullptr;
static lv_obj_t* imageViewerScreen = nullptr;  // overlay with Back button when viewing
static void refreshImageList();
static void imageFileCb(lv_event_t* e);
static void imageBackCb(lv_event_t* e);
static void drawSdJpegToTft(const char* path, int maxY = 240);
static String currentImagePath;  // set when image viewer is open; JPEG is drawn over LVGL's black screen
static volatile bool imageViewerNeedsDraw = false;  // LVGL flushed (painted over the JPEG) or a new image was picked
static unsigned long lastPanelUpdate = 0;
static uint32_t lastSensorHash = 0;
static void createControlPanel();
static void updateControlPanelStatus();
// Status bar (persistent top layer: title + time + WiFi glyph; SSID and IP shown on Settings)
static lv_obj_t* statusBar = nullptr;
static lv_obj_t* statusTimeLabel = nullptr;
static lv_obj_t* statusIpLabel = nullptr;    // created on Settings screen
static lv_obj_t* statusWifiIcon = nullptr;   // header glyph on every screen: accent = connected, red = not
static lv_obj_t* statusWifiLabel = nullptr;  // created on Settings screen, updated by updateStatusBar()
static lv_obj_t* statusSettingsBtn = nullptr; // gear on the launcher header -> Settings
static void updateStatusBar();
// Volume: inline on the Settings screen
static lv_obj_t* volumeSlider = nullptr;
static lv_obj_t* settingsVolumeValueLabel = nullptr;
static lv_obj_t* settingsScreen = nullptr;
static void volumeSliderCb(lv_event_t* e);
// LED brightness card on Settings (level is owned by Main; sent via the settings queue)
static lv_obj_t* ledLevelSlider = nullptr;
static lv_obj_t* ledLevelValueLabel = nullptr;
static lv_obj_t* ledSwitch = nullptr;
static unsigned long lastLedControlTouch = 0;  // ignore Main's echo for a few seconds after a local change
#define LED_CONTROL_ECHO_HOLD_MS 4000
static void gesture_timer_cb(lv_timer_t* t);

// Layout: status bar (doubles as section header with Back button), then content to the bottom edge
#define TZT_STATUS_H   32
#define TZT_CONTENT_TOP    TZT_STATUS_H
#define TZT_CONTENT_H      (TZT_SCREEN_HEIGHT - TZT_STATUS_H)
// Finger-friendly: minimum touch targets and consistent spacing (48px min for tap targets)
#define TZT_MIN_TOUCH_H    48
static lv_obj_t* statusBackBtn = nullptr;    // "< Home" in status bar; hidden on launcher
static lv_obj_t* statusTitleLabel = nullptr; // section name in status bar; hidden on launcher
static lv_obj_t* statusAppName = nullptr;    // app name in status bar; shown on launcher only
static void updateHeader();
static void launcherTileCb(lv_event_t* e);
static void goToScreenIndex(int index);
static void showStatusBar();
static void hideStatusBar();
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
           (lastSensorData.ledSchedule ? 4 : 0) |
           ((uint32_t)(lastSensorData.ledOnMinute ^ lastSensorData.ledOffMinute) << 3);
}
#endif  // TZT_HEADLESS

// ESP-NOW
bool espNowInitialized = false;
uint8_t mainESP32Mac[6] = MAIN_ESP32_MAC_ADDRESS;

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
const unsigned long AUTH_TOKEN_DURATION = 3600000;  // 1 hour

// Several concurrent sessions so logging in on one device (phone, laptop, script)
// doesn't log out the others. When full, the session closest to expiry is replaced.
#define MAX_AUTH_SESSIONS 4
struct AuthSession { String token; unsigned long expiry; };
AuthSession authSessions[MAX_AUTH_SESSIONS];

bool sessionActive(const AuthSession& s) {
    return s.token.length() > 0 && (long)(s.expiry - millis()) > 0;  // wrap-safe
}

// Failed-login backoff: doubles each consecutive failure (capped), resets on success.
// Beats a flat 1s delay against repeated/scripted password guesses.
unsigned long loginFailCount = 0;
const unsigned long LOGIN_BACKOFF_MAX_MS = 8000;

unsigned long loginBackoffMs() {
    unsigned long ms = 250UL << (loginFailCount < 6 ? loginFailCount : 6);  // 250,500,1000,...,8000
    return ms > LOGIN_BACKOFF_MAX_MS ? LOGIN_BACKOFF_MAX_MS : ms;
}

// Unguessable session token: 16 bytes from the hardware RNG, hex-encoded (32 chars).
// Replaces the old millis()+random(1000,9999) scheme, which was low-entropy and
// predictable from boot time.
String generateAuthToken() {
    uint8_t buf[16];
    for (int i = 0; i < 16; i++) buf[i] = (uint8_t)(esp_random() & 0xFF);
    char hex[33];
    for (int i = 0; i < 16; i++) sprintf(hex + i * 2, "%02x", buf[i]);
    hex[32] = '\0';
    return String(hex);
}

String createSession() {
    int slot = 0;
    for (int i = 0; i < MAX_AUTH_SESSIONS; i++) {
        if (!sessionActive(authSessions[i])) { slot = i; break; }
        if ((long)(authSessions[i].expiry - authSessions[slot].expiry) < 0) slot = i;
    }
    authSessions[slot].token = generateAuthToken();
    authSessions[slot].expiry = millis() + AUTH_TOKEN_DURATION;
    return authSessions[slot].token;
}

bool sessionTokenValid(const String& token) {
    if (token.length() == 0) return false;
    for (int i = 0; i < MAX_AUTH_SESSIONS; i++) {
        if (sessionActive(authSessions[i]) && authSessions[i].token == token) return true;
    }
    return false;
}

void endSession(const String& token) {
    for (int i = 0; i < MAX_AUTH_SESSIONS; i++) {
        if (authSessions[i].token == token) authSessions[i].token = "";
    }
}

// Session token from the auth cookie (requires "Cookie" in server.collectHeaders)
String requestCookieToken() {
    String cookie = server.header("Cookie");
    int start = cookie.indexOf("auth=");
    if (start < 0) return "";
    start += 5;
    int end = cookie.indexOf(";", start);
    return cookie.substring(start, end < 0 ? cookie.length() : end);
}

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
                Logger::warn("ESP-NOW", "send failed, resending chunk");
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
                    Logger::warn("ESP-NOW", "send failed, retrying");
                    lastFailLog = millis();
                }
            }
        }
        if (millis() - lastFailLog > 15000) {
            lastFailLog = millis();
            Logger::warn("ESP-NOW", "send failed status=" + String((int)status));
            esp_err_t peerResult = espNowEnsurePeer(mac_addr, espNowPeerChannel(ESP_NOW_CHANNEL));
            if (peerResult != ESP_OK) Logger::error("ESP-NOW", "Re-add peer failed: " + String(esp_err_to_name(peerResult)));
        }
    }
}

// ESP-NOW receive: sensor data from Main. Main owns LED/pump settings (set via its :8081 test server),
// so every packet is taken as-is - the GUIs always show Main's real state.
void onDataRecvStatic(const uint8_t* mac_addr, const uint8_t* data, int len) {
    if (len >= (int)sizeof(SensorDataPacket) && data[0] == ESP_NOW_MSG_SENSOR_DATA) {
        if (verifyChecksum((uint8_t*)data, sizeof(SensorDataPacket))) {
            memcpy(&lastSensorData, data, sizeof(SensorDataPacket));
            lastSensorDataValid = true;
            lastSensorDataReceiveTime = millis();
            sensorDataNeedsPanelUpdate = true;  // Refresh GUI on next loop (don't call LVGL from callback)
        } else {
            Logger::warn("ESP-NOW", "Invalid checksum in sensor data");
        }
    } else if (len >= (int)sizeof(AckPacket) && data[0] == ESP_NOW_MSG_ACK) {
        AckPacket* ack = (AckPacket*)data;
        if (verifyChecksum((uint8_t*)ack, sizeof(AckPacket)) && ack->ackForMsgType == ESP_NOW_MSG_COMMAND) {
            if (ack->sequenceNumber == lastSentSequence) {
                retryPending = false;
                // Setting handshake: Main got it, stop resending
                if (pendingSettingRemainingSends > 0) {
                    pendingSettingRemainingSends = 0;
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
        Logger::warn("ESP-NOW", "Received unexpected packet type: " + String(data[0]));
    }
}

// Human-readable name for command type (for serial logs)
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

// Enqueue a setting command: sent until Main ACKs it (up to SETTINGS_MAX_ATTEMPTS, SETTINGS_ACK_TIMEOUT_MS apart). Latest overwrites.
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

// Send pump Start/Stop once; the loop retries on FAIL. Main dedupes by state (no-op if pump already in that state).
static bool sendPumpCommandRepeated(uint8_t commandType) {
    if (!espNowInitialized) return false;
    return sendCommandViaESPNow(commandType, 0, 0, "", false, true);  // fromSettingsQueue=true to skip long pre-send delay
}

// Helper function to send ESP-NOW command to Main ESP32
// isRetry: true when this is an automatic retry after send callback reported FAIL
// fromSettingsQueue: true when sending from the settings queue (skip long delay; printer has priority)
bool sendCommandViaESPNow(uint8_t commandType, uint8_t param1, uint8_t param2, const char* message, bool isRetry, bool fromSettingsQueue) {
    if (!espNowInitialized) {
        Logger::error("ESP-NOW", "not initialized");
        return false;
    }
    if (!isRetry) retryCount = 0;  // New user-initiated send; allow one retry on FAIL
    (void)fromSettingsQueue;
    // No pre-send waiting: 802.11 carrier sense already keeps the two boards from talking over each
    // other, and unicast ESP-NOW frames are retried by the radio. The old "wait for Main's quiet window"
    // delays blocked the loop (screen, web, OTA) for up to ~10 s per command.

    if (!espNowMacIsSet(mainESP32Mac)) return false;

    // Make sure the peer exists on the current WiFi channel (re-added if missing or the channel moved)
    esp_err_t peerResult = espNowEnsurePeer(mainESP32Mac, espNowPeerChannel(ESP_NOW_CHANNEL));
    if (peerResult != ESP_OK) {
        Logger::error("ESP-NOW", "Failed to re-add peer: " + String(esp_err_to_name(peerResult)));
        return false;
    }
    
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
    
    // Sent once. Delivery failures come back through onDataSentStatic (retry) or a missing ACK (settings queue).
    {
        esp_err_t result = esp_now_send(mainESP32Mac, (uint8_t*)&packet, sizeof(CommandPacket));
        if (result != ESP_OK) {
            Logger::warn("ESP-NOW", "send failed: " + String(esp_err_to_name(result)));
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
#define SECTION_TAP_IGNORE_MS 400  // ignore list taps right after opening a section (the launcher press can land on a row)
static uint32_t lastSectionOpenTime = 0;
static bool sectionJustOpened() { return (uint32_t)millis() - lastSectionOpenTime < SECTION_TAP_IGNORE_MS; }
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
    imageViewerNeedsDraw = true;  // whatever LVGL just drew may cover the viewer's JPEG
    lv_disp_flush_ready(drv);
}

// Nav: launcher (SCREEN_LAUNCHER) or section by index (0=Messages, 1=Sounds, 2=Pictures, 3=Settings)
static const char* const sectionTitles[SCREEN_COUNT] = { "Messages", "Sounds", "Pictures", "Settings" };
static const char* const sectionIcons[SCREEN_COUNT] = {
    LV_SYMBOL_ENVELOPE, LV_SYMBOL_AUDIO, LV_SYMBOL_IMAGE, LV_SYMBOL_SETTINGS };

static lv_obj_t* screenByIndex(int idx) {
    switch (idx) {
        case SCREEN_MESSAGES: return messagesScreen;
        case SCREEN_SOUNDS:   return soundsScreen;
        case SCREEN_PICTURES: return picturesScreen;
        case SCREEN_SETTINGS: return settingsScreen;
        default: return launcherScreen;
    }
}

static void refreshAudioList();
static void refreshImageList();

// Open a section (or the launcher). Lists are re-read from SD on every open so they're never stale.
static void goToScreenIndex(int index) {
    if (index != SCREEN_LAUNCHER && (index < 0 || index >= SCREEN_COUNT)) return;
    lv_obj_t* target = screenByIndex(index);
    if (index == currentScreenIndex && lv_scr_act() == target) return;
    bool opening = (index != SCREEN_LAUNCHER);
    currentImagePath = "";
    if (historyDetail) lv_obj_add_flag(historyDetail, LV_OBJ_FLAG_HIDDEN);  // Messages always opens on the list
    currentScreenIndex = index;
    if (index == SCREEN_MESSAGES) refreshHistoryList();
    else if (index == SCREEN_SOUNDS) refreshAudioList();
    else if (index == SCREEN_PICTURES) refreshImageList();
    if (opening) lastSectionOpenTime = (uint32_t)millis();
    showStatusBar();  // the image viewer hides it
    lv_scr_load_anim(target, opening ? LV_SCR_LOAD_ANIM_MOVE_LEFT : LV_SCR_LOAD_ANIM_MOVE_RIGHT,
        180, 0, false);
    updateHeader();
}

static void launcherTileCb(lv_event_t* e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    goToScreenIndex((int)(intptr_t)lv_event_get_user_data(e));
}

// Jump to the launcher from any screen, including the image viewer
static void returnToLauncher() {
    goToScreenIndex(SCREEN_LAUNCHER);
}

static void statusBackCb(lv_event_t* e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    goToScreenIndex(SCREEN_LAUNCHER);
}

static void statusSettingsCb(lv_event_t* e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    goToScreenIndex(SCREEN_SETTINGS);
}

// Status bar: launcher shows app name + settings gear; sections show "<" back + section title.
// Time and WiFi glyph sit on the right, shifted left on the launcher to make room for the gear.
static void updateHeader() {
    bool onLauncher = (currentScreenIndex == SCREEN_LAUNCHER);
    if (statusTitleLabel && !onLauncher) lv_label_set_text(statusTitleLabel, sectionTitles[currentScreenIndex]);
    lv_obj_t* sectionOnly[] = { statusBackBtn, statusTitleLabel };
    for (lv_obj_t* o : sectionOnly) {
        if (!o) continue;
        if (onLauncher) lv_obj_add_flag(o, LV_OBJ_FLAG_HIDDEN);
        else lv_obj_clear_flag(o, LV_OBJ_FLAG_HIDDEN);
    }
    lv_obj_t* launcherOnly[] = { statusAppName, statusSettingsBtn };
    for (lv_obj_t* o : launcherOnly) {
        if (!o) continue;
        if (onLauncher) lv_obj_clear_flag(o, LV_OBJ_FLAG_HIDDEN);
        else lv_obj_add_flag(o, LV_OBJ_FLAG_HIDDEN);
    }
    int right = onLauncher ? UI_BACK_W : UI_MARGIN;
    if (statusWifiIcon) lv_obj_align(statusWifiIcon, LV_ALIGN_RIGHT_MID, -right, 0);
    if (statusTimeLabel) lv_obj_align(statusTimeLabel, LV_ALIGN_RIGHT_MID, -(right + 24), 0);
}

static void showStatusBar() {
    if (statusBar) lv_obj_clear_flag(statusBar, LV_OBJ_FLAG_HIDDEN);
}

static void hideStatusBar() {
    if (statusBar) lv_obj_add_flag(statusBar, LV_OBJ_FLAG_HIDDEN);
}

static void setLabelIfChanged(lv_obj_t* label, const char* text) {
    if (label && strcmp(lv_label_get_text(label), text) != 0) lv_label_set_text(label, text);
}

static String baseName(const String& path) {
    int slash = path.lastIndexOf('/');
    return slash >= 0 ? path.substring(slash + 1) : path;
}

// Minutes since midnight -> "4:45 PM"
static void formatMinute(uint16_t minute, char* out, size_t len) {
    int h = (minute / 60) % 24, m = minute % 60;
    snprintf(out, len, "%d:%02d %s", h % 12 == 0 ? 12 : h % 12, m, h < 12 ? "AM" : "PM");
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
    bool wifiUp = (WiFi.status() == WL_CONNECTED);
    // Colour alone carries the state, like Spectral's header Bluetooth rune
    if (statusWifiIcon) lv_obj_set_style_text_color(statusWifiIcon, uiColor(wifiUp ? UI_ACCENT : UI_WARNING), 0);
    if (statusWifiLabel) {
        if (wifiUp) {
            lv_label_set_text(statusWifiLabel, WiFi.SSID().length() ? WiFi.SSID().c_str() : "Connected");
            lv_obj_set_style_text_color(statusWifiLabel, uiColor(UI_TEXT), 0);
        } else {
            lv_label_set_text(statusWifiLabel, "Disconnected");
            lv_obj_set_style_text_color(statusWifiLabel, uiColor(UI_WARNING), 0);
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
    lv_obj_set_style_bg_color(scr, uiColor(UI_BACKGROUND), 0);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);
    lv_obj_set_style_pad_all(scr, 0, 0);
    lv_obj_clear_flag(scr, LV_OBJ_FLAG_SCROLLABLE);
}

// Pressed-state style applied to every button for tactile feedback (Spectral's row tap flash)
static lv_style_t style_btn_pressed;
static bool btn_style_inited = false;
static void applyBtnPressStyle(lv_obj_t* btn) {
    if (!btn_style_inited) {
        lv_style_init(&style_btn_pressed);
        lv_style_set_bg_color(&style_btn_pressed, uiColor(UI_SELECTION));
        lv_style_set_transform_width(&style_btn_pressed, -2);
        lv_style_set_transform_height(&style_btn_pressed, -2);
        btn_style_inited = true;
    }
    lv_obj_add_style(btn, &style_btn_pressed, LV_STATE_PRESSED);
}

// ---- Shared components (same family as Spectral's TFT_Display) ----

static lv_obj_t* makeLabel(lv_obj_t* parent, const char* text, const lv_font_t* font, UiColor color) {
    lv_obj_t* l = lv_label_create(parent);
    lv_label_set_text(l, text);
    lv_obj_set_style_text_font(l, font, 0);
    lv_obj_set_style_text_color(l, uiColor(color), 0);
    return l;
}

// Rounded card: header band / tile / row tone, no border
static void styleCard(lv_obj_t* obj, int pad) {
    lv_obj_set_style_bg_color(obj, uiColor(UI_CARD), 0);
    lv_obj_set_style_bg_opa(obj, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(obj, 0, 0);
    lv_obj_set_style_shadow_width(obj, 0, 0);
    lv_obj_set_style_radius(obj, UI_RADIUS, 0);
    lv_obj_set_style_pad_all(obj, pad, 0);
    lv_obj_clear_flag(obj, LV_OBJ_FLAG_SCROLLABLE);
}

// Primary = the next step (filled accent, white text); secondary = BUTTON fill
static void styleButton(lv_obj_t* btn, bool primary) {
    applyBtnPressStyle(btn);
    lv_obj_set_style_bg_color(btn, uiColor(primary ? UI_BORDER : UI_BUTTON), 0);
    lv_obj_set_style_text_color(btn, uiColor(primary ? UI_TITLE : UI_TEXT), 0);
    lv_obj_set_style_shadow_width(btn, 0, 0);
    lv_obj_set_style_radius(btn, UI_RADIUS, 0);
}

static lv_obj_t* makeButton(lv_obj_t* parent, const char* text, int w, int h, bool primary,
                            lv_event_cb_t cb) {
    lv_obj_t* btn = lv_btn_create(parent);
    lv_obj_set_size(btn, w, h);
    styleButton(btn, primary);
    lv_obj_t* lbl = lv_label_create(btn);
    lv_label_set_text(lbl, text);
    lv_obj_set_style_text_font(lbl, &lv_font_montserrat_14, 0);
    lv_obj_center(lbl);
    if (cb) lv_obj_add_event_cb(btn, cb, LV_EVENT_CLICKED, nullptr);
    return btn;
}

// Transparent icon button in the status bar (the "<" back target and the settings gear):
// full bar height, hit area extended to a 48px target, flashes on press
static lv_obj_t* makeBarIconButton(lv_obj_t* bar, const char* icon, lv_event_cb_t cb) {
    lv_obj_t* btn = lv_btn_create(bar);
    lv_obj_set_size(btn, UI_BACK_W, TZT_STATUS_H);
    lv_obj_set_ext_click_area(btn, (TZT_MIN_TOUCH_H - TZT_STATUS_H) / 2);
    applyBtnPressStyle(btn);
    lv_obj_set_style_bg_opa(btn, LV_OPA_TRANSP, 0);
    lv_obj_set_style_bg_opa(btn, LV_OPA_COVER, LV_STATE_PRESSED);
    lv_obj_set_style_shadow_width(btn, 0, 0);
    lv_obj_set_style_radius(btn, 0, 0);
    lv_obj_t* glyph = makeLabel(btn, icon, &lv_font_montserrat_18, UI_TEXT);
    lv_obj_center(glyph);
    lv_obj_add_event_cb(btn, cb, LV_EVENT_CLICKED, nullptr);
    return btn;
}

// Scrolling list under the status bar: transparent, rows are separate cards with a gap between
static lv_obj_t* makeCardList(lv_obj_t* parent, int h = TZT_CONTENT_H) {
    lv_obj_t* list = lv_list_create(parent);
    lv_obj_set_size(list, TZT_SCREEN_WIDTH, h);
    lv_obj_set_pos(list, 0, TZT_STATUS_H);
    lv_obj_set_style_bg_color(list, uiColor(UI_BACKGROUND), 0);
    lv_obj_set_style_bg_opa(list, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(list, 0, 0);
    lv_obj_set_style_radius(list, 0, 0);
    lv_obj_set_style_pad_all(list, UI_MARGIN, 0);
    lv_obj_set_style_pad_row(list, UI_GAP, 0);
    lv_obj_set_scrollbar_mode(list, LV_SCROLLBAR_MODE_AUTO);
    lv_obj_add_flag(list, LV_OBJ_FLAG_SCROLL_MOMENTUM | LV_OBJ_FLAG_SCROLL_ELASTIC);
    lv_obj_set_scroll_dir(list, LV_DIR_VER);
    return list;
}

// One list row card - Spectral's drawMenuRow(): label left, and a chevron
// on the right only when the row opens another page (opensPage).
// Rows share static styles rather than local ones: a list can hold 20+ rows and
// per-object local style props add up in LVGL's heap.
static lv_style_t style_row, style_row_icon;
static lv_obj_t* addListRow(lv_obj_t* list, const char* icon, const char* text, bool opensPage,
                            lv_event_cb_t cb, int idx) {
    static bool inited = false;
    if (!inited) {
        lv_style_init(&style_row);
        lv_style_set_height(&style_row, UI_ROW_H);
        lv_style_set_bg_color(&style_row, uiColor(UI_CARD));
        lv_style_set_bg_opa(&style_row, LV_OPA_COVER);
        lv_style_set_border_width(&style_row, 0);
        lv_style_set_shadow_width(&style_row, 0);
        lv_style_set_radius(&style_row, UI_RADIUS);
        lv_style_set_pad_hor(&style_row, 12);
        lv_style_set_pad_ver(&style_row, 0);
        lv_style_set_text_font(&style_row, &lv_font_montserrat_14);
        lv_style_set_text_color(&style_row, uiColor(UI_TEXT));
        lv_style_init(&style_row_icon);
        lv_style_set_text_color(&style_row_icon, uiColor(UI_ACCENT));
        inited = true;
    }
    lv_obj_t* btn = lv_list_add_btn(list, icon, text);
    lv_obj_add_style(btn, &style_row, 0);
    applyBtnPressStyle(btn);
    lv_obj_t* img = lv_obj_get_child(btn, 0);  // the icon: a symbol, drawn in the text colour
    if (icon && img) lv_obj_add_style(img, &style_row_icon, 0);
    lv_obj_t* lbl = lv_obj_get_child(btn, icon ? 1 : 0);
    if (lbl) lv_label_set_long_mode(lbl, LV_LABEL_LONG_DOT);
    if (opensPage) makeLabel(btn, LV_SYMBOL_RIGHT, &lv_font_montserrat_12, UI_TEXT_MUTED);
    lv_obj_add_event_cb(btn, cb, LV_EVENT_CLICKED, (void*)(uintptr_t)idx);
    return btn;
}

// Empty/error state inside a card list
static void addListNote(lv_obj_t* list, const char* text) {
    lv_obj_t* t = lv_list_add_text(list, text);
    lv_obj_set_style_bg_opa(t, LV_OPA_TRANSP, 0);
    lv_obj_set_style_text_color(t, uiColor(UI_TEXT_MUTED), 0);
    lv_obj_set_style_text_font(t, &lv_font_montserrat_14, 0);
    lv_obj_set_style_pad_ver(t, 12, 0);
}

// App-style toggle: accent pill + white knob when on, dark outlined track when off
static lv_obj_t* makeSwitch(lv_obj_t* parent) {
    lv_obj_t* sw = lv_switch_create(parent);
    lv_obj_set_size(sw, UI_SWITCH_W, UI_SWITCH_H);
    lv_obj_set_ext_click_area(sw, 10);
    lv_obj_set_style_bg_color(sw, uiColor(UI_FIELD), LV_PART_MAIN);
    lv_obj_set_style_border_color(sw, uiColor(UI_TEXT_MUTED), LV_PART_MAIN);
    lv_obj_set_style_border_width(sw, 1, LV_PART_MAIN);
    lv_obj_set_style_border_width(sw, 0, LV_PART_MAIN | LV_STATE_CHECKED);
    lv_obj_set_style_bg_color(sw, uiColor(UI_BORDER), LV_PART_INDICATOR | LV_STATE_CHECKED);
    lv_obj_set_style_bg_color(sw, uiColor(UI_TEXT_MUTED), LV_PART_KNOB);
    lv_obj_set_style_bg_color(sw, uiColor(UI_TITLE), LV_PART_KNOB | LV_STATE_CHECKED);
    lv_obj_set_style_pad_all(sw, -3, LV_PART_KNOB);
    lv_obj_set_style_anim_time(sw, 120, 0);
    return sw;
}

// Slider on a FIELD track with an accent fill and a white knob
static lv_obj_t* makeSlider(lv_obj_t* parent, int w, int min, int max, int value) {
    lv_obj_t* s = lv_slider_create(parent);
    lv_slider_set_range(s, min, max);
    lv_slider_set_value(s, value, LV_ANIM_OFF);
    lv_obj_set_size(s, w, 10);
    lv_obj_set_style_anim_time(s, 0, LV_PART_MAIN);  // no animation so knob follows finger directly
    lv_obj_set_ext_click_area(s, 16);                 // easier to hit knob with finger
    lv_obj_set_style_bg_color(s, uiColor(UI_FIELD), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(s, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_bg_color(s, uiColor(UI_BORDER), LV_PART_INDICATOR);
    lv_obj_set_style_bg_color(s, uiColor(UI_TITLE), LV_PART_KNOB);
    lv_obj_set_style_radius(s, 5, LV_PART_MAIN);
    lv_obj_set_style_radius(s, 5, LV_PART_INDICATOR);
    lv_obj_set_style_radius(s, LV_RADIUS_CIRCLE, LV_PART_KNOB);
    lv_obj_set_style_pad_all(s, 6, LV_PART_KNOB);    // larger knob for easier drag
    return s;
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

static void ledLevelSliderCb(lv_event_t* e) {
    lv_event_code_t code = lv_event_get_code(e);
    int32_t v = lv_slider_get_value(lv_event_get_target(e));
    if (v < 1) v = 1;
    if (v > 100) v = 100;
    lastLedControlTouch = millis();
    if (ledLevelValueLabel) lv_label_set_text_fmt(ledLevelValueLabel, "%d%%", (int)v);
    // Send once on release, not on every drag step (ESP-NOW + Main NVS write per change)
    if (code == LV_EVENT_RELEASED) enqueueSettingCommand(CMD_SET_LED_BRIGHTNESS, (uint8_t)v, 0);
}

static void ledToggleCb(lv_event_t* e) {
    if (lv_event_get_code(e) != LV_EVENT_VALUE_CHANGED) return;
    bool turnOn = lv_obj_has_state(lv_event_get_target(e), LV_STATE_CHECKED);  // switch already flipped (optimistic)
    lastLedControlTouch = millis();
    enqueueSettingCommand(CMD_SET_LED_STATE, turnOn ? 1 : 0, 0);
}

// Icon (accent) + title, the caption row used on tiles and cards
static void makeIconTitle(lv_obj_t* parent, const char* icon, const char* text,
                          const lv_font_t* font, UiColor textColor, int textX) {
    lv_obj_t* i = makeLabel(parent, icon, font, UI_ACCENT);
    lv_obj_set_pos(i, 0, 0);
    lv_obj_t* t = makeLabel(parent, text, font, textColor);
    lv_obj_set_pos(t, textX, 0);
}

static void createControlPanel() {
    const int cardW = TZT_SCREEN_WIDTH - 2 * UI_MARGIN;
    const int top = TZT_CONTENT_TOP + UI_GAP;   // first content row under the status bar

    // ---- Launcher: three tall buttons, a big icon over the section name ----
    launcherScreen = lv_obj_create(NULL);
    setScreenStyle(launcherScreen);
    {
        int btnW = (cardW - (LAUNCHER_BUTTONS - 1) * UI_GAP) / LAUNCHER_BUTTONS;
        int btnH = TZT_SCREEN_HEIGHT - top - UI_MARGIN;
        for (int i = 0; i < LAUNCHER_BUTTONS; i++) {
            lv_obj_t* btn = lv_btn_create(launcherScreen);
            lv_obj_set_size(btn, btnW, btnH);
            lv_obj_set_pos(btn, UI_MARGIN + i * (btnW + UI_GAP), top);
            styleCard(btn, 0);
            applyBtnPressStyle(btn);
            lv_obj_add_event_cb(btn, launcherTileCb, LV_EVENT_CLICKED, (void*)(intptr_t)i);
            lv_obj_t* icon = makeLabel(btn, sectionIcons[i], &lv_font_montserrat_32, UI_ACCENT);
            lv_obj_align(icon, LV_ALIGN_CENTER, 0, -14);
            lv_obj_t* name = makeLabel(btn, sectionTitles[i], &lv_font_montserrat_16, UI_TEXT);
            lv_obj_align(name, LV_ALIGN_CENTER, 0, 26);
        }
    }

    // ---- Messages: list (newest first); tap a row to read it in full ----
    messagesScreen = lv_obj_create(NULL);
    setScreenStyle(messagesScreen);
    historyList = makeCardList(messagesScreen);

    historyDetail = lv_obj_create(messagesScreen);
    lv_obj_set_size(historyDetail, TZT_SCREEN_WIDTH, TZT_CONTENT_H);
    lv_obj_set_pos(historyDetail, 0, TZT_CONTENT_TOP);
    lv_obj_set_style_bg_color(historyDetail, uiColor(UI_BACKGROUND), 0);
    lv_obj_set_style_bg_opa(historyDetail, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(historyDetail, 0, 0);
    lv_obj_set_style_radius(historyDetail, 0, 0);
    lv_obj_set_style_pad_all(historyDetail, UI_MARGIN, 0);
    lv_obj_add_flag(historyDetail, LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_flag(historyDetail, LV_OBJ_FLAG_SCROLLABLE);
    {
        const int backH = 40;
        lv_obj_t* textCard = lv_obj_create(historyDetail);
        lv_obj_set_size(textCard, cardW, TZT_CONTENT_H - 2 * UI_MARGIN - backH - UI_GAP);
        lv_obj_set_pos(textCard, 0, 0);
        styleCard(textCard, 10);
        lv_obj_add_flag(textCard, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_SCROLL_MOMENTUM | LV_OBJ_FLAG_SCROLL_ELASTIC);
        lv_obj_set_scroll_dir(textCard, LV_DIR_VER);
        historyDetailLabel = makeLabel(textCard, "", &lv_font_montserrat_16, UI_TEXT);
        lv_label_set_long_mode(historyDetailLabel, LV_LABEL_LONG_WRAP);
        lv_obj_set_width(historyDetailLabel, cardW - 24);
        lv_obj_t* backBtn = makeButton(historyDetail, LV_SYMBOL_LIST " All messages", 150, backH, false, historyBackCb);
        lv_obj_align(backBtn, LV_ALIGN_BOTTOM_RIGHT, 0, 0);
    }

    // ---- Sounds: list (tap to play) over a now-playing bar with Stop ----
    soundsScreen = lv_obj_create(NULL);
    setScreenStyle(soundsScreen);
    {
        const int barH = UI_ROW_H + UI_MARGIN;
        audioList = makeCardList(soundsScreen, TZT_CONTENT_H - barH);
        lv_obj_t* bar = lv_obj_create(soundsScreen);
        lv_obj_set_size(bar, cardW, UI_ROW_H);
        lv_obj_set_pos(bar, UI_MARGIN, TZT_SCREEN_HEIGHT - UI_MARGIN - UI_ROW_H);
        styleCard(bar, 0);
        lv_obj_set_style_pad_hor(bar, 12, 0);
        lv_obj_t* icon = makeLabel(bar, LV_SYMBOL_AUDIO, &lv_font_montserrat_16, UI_ACCENT);
        lv_obj_align(icon, LV_ALIGN_LEFT_MID, 0, 0);
        const int stopW = 64;
        audioNowPlayingLabel = makeLabel(bar, "Nothing playing", &lv_font_montserrat_14, UI_TEXT_MUTED);
        lv_obj_set_width(audioNowPlayingLabel, cardW - 24 - 26 - stopW - UI_GAP);
        lv_label_set_long_mode(audioNowPlayingLabel, LV_LABEL_LONG_SCROLL_CIRCULAR);
        lv_obj_align(audioNowPlayingLabel, LV_ALIGN_LEFT_MID, 26, 0);
        audioStopBtn = makeButton(bar, LV_SYMBOL_STOP, stopW, UI_ROW_H - 10, false, audioStopCb);
        lv_obj_align(audioStopBtn, LV_ALIGN_RIGHT_MID, 6, 0);
        lv_obj_set_ext_click_area(audioStopBtn, 5);
    }

    // ---- Pictures: list; tap a row for the full-screen viewer ----
    picturesScreen = lv_obj_create(NULL);
    setScreenStyle(picturesScreen);
    imageList = makeCardList(picturesScreen);

    // Image viewer: full screen; tap anywhere to go back to the Pictures list
    imageViewerScreen = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(imageViewerScreen, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(imageViewerScreen, LV_OPA_COVER, 0);
    lv_obj_set_style_border_opa(imageViewerScreen, LV_OPA_0, 0);
    lv_obj_set_style_pad_all(imageViewerScreen, 0, 0);
    lv_obj_add_flag(imageViewerScreen, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(imageViewerScreen, imageBackCb, LV_EVENT_CLICKED, nullptr);

    currentScreenIndex = SCREEN_LAUNCHER;
    lastDashboardTouchTime = millis();
    lv_scr_load(launcherScreen);

    // ---- Status bar (top layer): card-tone header like Spectral's drawTitleBar() ----
    // Launcher: app name + settings gear. Sections: "<" back + section title. Right: time + WiFi glyph.
    {
        lv_obj_t* layer = lv_layer_top();
        statusBar = lv_obj_create(layer);
        lv_obj_set_size(statusBar, TZT_SCREEN_WIDTH, TZT_STATUS_H);
        lv_obj_set_pos(statusBar, 0, 0);
        styleCard(statusBar, 0);
        lv_obj_set_style_radius(statusBar, 0, 0);

        statusAppName = makeLabel(statusBar, "Prick'n'Print", &lv_font_montserrat_16, UI_TEXT);
        lv_obj_align(statusAppName, LV_ALIGN_LEFT_MID, UI_MARGIN + 4, 0);

        statusBackBtn = makeBarIconButton(statusBar, LV_SYMBOL_LEFT, statusBackCb);
        lv_obj_align(statusBackBtn, LV_ALIGN_LEFT_MID, 0, 0);

        statusTitleLabel = makeLabel(statusBar, "", &lv_font_montserrat_16, UI_TEXT);
        lv_obj_align(statusTitleLabel, LV_ALIGN_LEFT_MID, UI_BACK_W - 2, 0);

        statusSettingsBtn = makeBarIconButton(statusBar, LV_SYMBOL_SETTINGS, statusSettingsCb);
        lv_obj_align(statusSettingsBtn, LV_ALIGN_RIGHT_MID, 0, 0);

        statusWifiIcon = makeLabel(statusBar, LV_SYMBOL_WIFI, &lv_font_montserrat_14, UI_WARNING);
        statusTimeLabel = makeLabel(statusBar, "--:--", &lv_font_montserrat_14, UI_TEXT);
        // Right-side positions are set by updateHeader() (they shift for the gear on the launcher)
    }

    // ---- Settings (card rows: WiFi + IP, Volume, LED) ----
    settingsScreen = lv_obj_create(NULL);
    setScreenStyle(settingsScreen);
    const int setCardPad = 10, wifiCardH = 44, sliderCardH = 66;

    // WiFi row: name left, network over IP address on the right (Spectral's value-on-the-right row)
    lv_obj_t* wifiCard = lv_obj_create(settingsScreen);
    lv_obj_set_size(wifiCard, cardW, wifiCardH);
    lv_obj_set_pos(wifiCard, UI_MARGIN, top);
    styleCard(wifiCard, 0);
    lv_obj_set_style_pad_hor(wifiCard, setCardPad, 0);
    lv_obj_t* wifiTitle = makeLabel(wifiCard, LV_SYMBOL_WIFI, &lv_font_montserrat_14, UI_ACCENT);
    lv_obj_align(wifiTitle, LV_ALIGN_LEFT_MID, 0, 0);
    lv_obj_t* wifiName = makeLabel(wifiCard, "WiFi", &lv_font_montserrat_14, UI_TEXT);
    lv_obj_align(wifiName, LV_ALIGN_LEFT_MID, 22, 0);
    const int wifiValueW = cardW - 2 * setCardPad - 80;
    statusWifiLabel = makeLabel(wifiCard, "--", &lv_font_montserrat_14, UI_TEXT);
    lv_label_set_long_mode(statusWifiLabel, LV_LABEL_LONG_DOT);
    lv_obj_set_style_text_align(statusWifiLabel, LV_TEXT_ALIGN_RIGHT, 0);
    lv_obj_set_width(statusWifiLabel, wifiValueW);
    lv_obj_align(statusWifiLabel, LV_ALIGN_TOP_RIGHT, 0, 4);
    statusIpLabel = makeLabel(wifiCard, "", &lv_font_montserrat_12, UI_TEXT_MUTED);
    lv_obj_set_style_text_align(statusIpLabel, LV_TEXT_ALIGN_RIGHT, 0);
    lv_obj_set_width(statusIpLabel, wifiValueW);
    lv_obj_align(statusIpLabel, LV_ALIGN_BOTTOM_RIGHT, 0, -4);

    auto makeSliderCard = [&](int y, const char* icon, const char* name) {
        lv_obj_t* card = lv_obj_create(settingsScreen);
        lv_obj_set_size(card, cardW, sliderCardH);
        lv_obj_set_pos(card, UI_MARGIN, y);
        styleCard(card, setCardPad);
        makeIconTitle(card, icon, name, &lv_font_montserrat_14, UI_TEXT, 22);
        return card;
    };
    const int sliderW = cardW - 2 * setCardPad - 16;  // room for the knob at both ends

    // Volume card
    int volCardY = top + wifiCardH + UI_GAP;
    lv_obj_t* volCard = makeSliderCard(volCardY, LV_SYMBOL_VOLUME_MAX, "Volume");
    int v0 = (int)(audioSvc.getVolume() * 100.0f);
    settingsVolumeValueLabel = makeLabel(volCard, "", &lv_font_montserrat_14, UI_TEXT_MUTED);
    lv_label_set_text_fmt(settingsVolumeValueLabel, "%d%%", v0);
    lv_obj_align(settingsVolumeValueLabel, LV_ALIGN_TOP_RIGHT, 0, 0);
    volumeSlider = makeSlider(volCard, sliderW, 0, 100, v0);
    lv_obj_align(volumeSlider, LV_ALIGN_BOTTOM_MID, 0, -6);
    lv_obj_add_event_cb(volumeSlider, volumeSliderCb, LV_EVENT_VALUE_CHANGED, nullptr);

    // LED card: on/off switch + brightness
    int ledCardY = volCardY + sliderCardH + UI_GAP;
    lv_obj_t* ledCard = makeSliderCard(ledCardY, LV_SYMBOL_CHARGE, "LED");
    ledSwitch = makeSwitch(ledCard);
    lv_obj_align(ledSwitch, LV_ALIGN_TOP_RIGHT, 0, -3);
    lv_obj_add_event_cb(ledSwitch, ledToggleCb, LV_EVENT_VALUE_CHANGED, nullptr);
    ledLevelValueLabel = makeLabel(ledCard, "--", &lv_font_montserrat_14, UI_TEXT_MUTED);
    lv_obj_align(ledLevelValueLabel, LV_ALIGN_TOP_RIGHT, -(UI_SWITCH_W + 10), 0);
    ledLevelSlider = makeSlider(ledCard, sliderW, 1, 100, 10);
    lv_obj_align(ledLevelSlider, LV_ALIGN_BOTTOM_MID, 0, -6);
    lv_obj_add_event_cb(ledLevelSlider, ledLevelSliderCb, LV_EVENT_VALUE_CHANGED, nullptr);
    lv_obj_add_event_cb(ledLevelSlider, ledLevelSliderCb, LV_EVENT_RELEASED, nullptr);

    updateStatusBar();  // fill time, WiFi glyph and the Settings WiFi/IP labels
    updateHeader();
}

// Main's sensor packets carry the LED state; keep the Settings LED controls in sync with it
static void updateControlPanelStatus() {
    if (!launcherScreen) return;
    if (lastSensorDataValid && millis() - lastLedControlTouch > LED_CONTROL_ECHO_HOLD_MS) {
        bool ledOn = lastSensorData.ledBrightness > 0;
        if (ledSwitch) {
            if (ledOn) lv_obj_add_state(ledSwitch, LV_STATE_CHECKED);
            else lv_obj_clear_state(ledSwitch, LV_STATE_CHECKED);
        }
        if (ledLevelSlider && !lv_obj_has_state(ledLevelSlider, LV_STATE_PRESSED) && lastSensorData.ledLevel >= 1) {
            lv_slider_set_value(ledLevelSlider, lastSensorData.ledLevel, LV_ANIM_OFF);
            if (ledLevelValueLabel) lv_label_set_text_fmt(ledLevelValueLabel, "%d%%", (int)lastSensorData.ledLevel);
        }
    }
    updateStatusBar();
}

// ── Message History screen helpers ──────────────────────────────

#define LATEST_MESSAGE_IDX -1  // list row for the in-memory latest message (no SD card)

static void refreshHistoryList() {
    if (!historyList) return;
    lv_obj_clean(historyList);
    bool haveLatest = lastPrintMessage.length() > 0 && lastPrintMessage != NO_MESSAGE_TEXT;
    int n = 0;
    if (sdCard.isReady()) {
        n = sdCard.getMessageCount();
        if (n == 0) {
            sdCard.loadRecentMessages(MAX_CACHED_MESSAGES);
            n = sdCard.getMessageCount();
        }
    }
    if (n == 0) {
        // No history on SD: still show the message received since boot
        if (haveLatest) addListRow(historyList, LV_SYMBOL_ENVELOPE, lastPrintMessage.c_str(), true, historyItemCb, LATEST_MESSAGE_IDX);
        if (!sdCard.isReady()) addListNote(historyList, haveLatest ? "No SD card: only the latest message is kept" : "No messages yet (no SD card)");
        else if (!haveLatest) addListNote(historyList, "No messages yet");
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
        addListRow(historyList, LV_SYMBOL_ENVELOPE, label, true, historyItemCb, i);
    }
}

static void historyItemCb(lv_event_t* e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED || sectionJustOpened()) return;
    int idx = (int)(intptr_t)lv_event_get_user_data(e);
    String full = (idx == LATEST_MESSAGE_IDX) ? lastPrintMessage : sdCard.readFullMessage(idx);
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
        addListNote(audioList, "SD card not available");
        return;
    }
    audioFileCount = sdCard.listAudioFiles(audioFileNames, MAX_AUDIO_FILES);
    if (audioFileCount == 0) {
        addListNote(audioList, "No audio files on SD");
        return;
    }
    for (int i = 0; i < audioFileCount; i++) {
        addListRow(audioList, LV_SYMBOL_PLAY, audioFileNames[i].c_str(), false, audioFileCb, i);
    }
    updateNowPlaying();
}

// Now-playing bar: file name while playing; Stop is the destructive action, red only while playing.
// Called every loop on the Sounds screen, so it only touches LVGL when the state changes.
static void updateNowPlaying() {
    static int shownPlaying = -1;
    static String shownFile;
    bool playing = audioSvc.isPlaying();
    String file = playing ? audioSvc.currentFile() : String();
    if ((int)playing == shownPlaying && file == shownFile) return;
    shownPlaying = playing;
    shownFile = file;
    if (audioNowPlayingLabel) {
        lv_label_set_text(audioNowPlayingLabel, playing ? baseName(file).c_str() : "Nothing playing");
        lv_obj_set_style_text_color(audioNowPlayingLabel, uiColor(playing ? UI_TEXT : UI_TEXT_MUTED), 0);
    }
    if (audioStopBtn) {
        lv_obj_set_style_bg_color(audioStopBtn, uiColor(playing ? UI_WARNING : UI_BUTTON), 0);
        lv_obj_set_style_text_color(audioStopBtn, uiColor(playing ? UI_TITLE : UI_TEXT_MUTED), 0);
    }
}

static void audioFileCb(lv_event_t* e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED || sectionJustOpened()) return;
    int idx = (int)(uintptr_t)lv_event_get_user_data(e);
    if (idx < 0 || idx >= audioFileCount) return;
    String path = String(AUDIO_DIR) + "/" + audioFileNames[idx];
    // Only one audio at a time: if already playing this file, leave it; else stop any current and play
    if (audioSvc.isPlaying() && audioSvc.currentFile() == path) return;
    audioSvc.playFile(path);
    updateNowPlaying();
}

static void audioStopCb(lv_event_t* e) {
    (void)e;
    audioSvc.stop();
    updateNowPlaying();
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
            Logger::warn("Image", "open failed: " + String(path ? path : ""));
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
            Logger::warn("Image", "decode failed: " + String(path));
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
        addListNote(imageList, "SD card not available");
        return;
    }
    imageFileCount = sdCard.listImageFiles(imageFileNames, MAX_IMAGE_FILES);
    if (imageFileCount == 0) {
        addListNote(imageList, "No images on SD");
        return;
    }
    for (int i = 0; i < imageFileCount; i++) {
        addListRow(imageList, LV_SYMBOL_IMAGE, imageFileNames[i].c_str(), true, imageFileCb, i);
    }
}

static void imageFileCb(lv_event_t* e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED || sectionJustOpened()) return;
    int idx = (int)(uintptr_t)lv_event_get_user_data(e);
    if (idx < 0 || idx >= imageFileCount) return;
    String path = String(IMAGES_DIR) + "/" + imageFileNames[idx];
    String lower = imageFileNames[idx];
    lower.toLowerCase();
#if !defined(TZT_HEADLESS)
    if (lower.endsWith(".jpg") || lower.endsWith(".jpeg")) {
        currentImagePath = path;
        imageViewerNeedsDraw = true;
        hideStatusBar();
        lv_scr_load(imageViewerScreen);
    } else {
        Logger::warn("Image", "Only JPEG is displayed; file is not .jpg/.jpeg: " + path);
        currentImagePath = "";
        hideStatusBar();
        lv_scr_load(imageViewerScreen);
    }
#else
    (void)path;
    (void)lower;
#endif
}

// Tap on the viewer: back to the Pictures list (also when the image was pushed from the backend)
static void imageBackCb(lv_event_t* e) {
    (void)e;
    goToScreenIndex(SCREEN_PICTURES);
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

// The request itself must carry a valid session token (cookie or ?token=)
bool isAuthenticated() {
    return sessionTokenValid(requestCookieToken()) || sessionTokenValid(server.arg("token"));
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
            loginFailCount = 0;
            String authToken = createSession();

            String cookieHeader = "auth=" + authToken + "; Path=/; Max-Age=3600; SameSite=Lax";
            server.sendHeader("Set-Cookie", cookieHeader);

            String redirectUrl = "/?token=" + authToken;
            String redirectPage = "<!DOCTYPE html><html><head><meta charset=\"UTF-8\"><script>"
                                 "document.cookie=\"auth=" + authToken + "; Path=/; Max-Age=3600; SameSite=Lax\";"
                                 "setTimeout(function(){window.location.href=\"" + redirectUrl + "\";},50);"
                                 "</script></head><body><p>Login successful! Redirecting...</p></body></html>";

            server.send(200, "text/html", redirectPage);
        } else {
            delay(loginBackoffMs());
            if (loginFailCount < 100) loginFailCount++;  // cap: avoid unbounded growth over uptime
            server.sendHeader("Location", "/login?error=1");
            server.send(302, "text/plain", "");
        }
        return;
    }
    
    static const char LOGIN_HEAD[] PROGMEM = R"HTML(<!DOCTYPE html>
<html lang="en">
<head>
<meta charset="UTF-8">
<meta name="viewport" content="width=device-width, initial-scale=1, viewport-fit=cover">
<meta name="apple-mobile-web-app-capable" content="yes">
<meta name="theme-color" content="#1C2A1F">
<title>Prick'n'Print Login</title>
<style>
)HTML";
    static const char LOGIN_BODY[] PROGMEM = R"HTML(
  body { display: flex; align-items: center; justify-content: center; padding: 16px; }
  .login { width: 100%; max-width: 360px; }
  .login h1 { font-size: 1.4rem; color: var(--text); margin-bottom: 2px; }
  .login .sub { color: var(--text-faint); font-size: 0.85rem; margin-bottom: 20px; }
  .login input { width: 100%; margin-bottom: 12px; }
  .login .btn { width: 100%; min-height: 46px; font-size: 1rem; }
  .error-msg { color: var(--danger); font-size: 0.85rem; margin-top: 12px; text-align: center; }
</style>
</head>
<body>
  <form class="card login" method="POST" action="/login" enctype="application/x-www-form-urlencoded">
    <h1>Prick'n'Print</h1>
    <p class="sub">Sign in to your display</p>
    <input type="password" name="password" placeholder="Password" required autofocus autocomplete="current-password">
    <button type="submit" class="btn btn-primary">Log in</button>
)HTML";
    static const char LOGIN_ERROR[] PROGMEM = R"HTML(    <p class="error-msg">Incorrect password</p>
)HTML";
    static const char LOGIN_TAIL[] PROGMEM = R"HTML(  </form>
</body>
</html>
)HTML";
    sendHtmlParts(server, {LOGIN_HEAD, WEB_THEME_CSS, LOGIN_BODY, server.hasArg("error") ? LOGIN_ERROR : "", LOGIN_TAIL});
}

void handleLogout() {
    endSession(requestCookieToken());
    endSession(server.arg("token"));
    server.sendHeader("Set-Cookie", "auth=; Path=/; Max-Age=0");
    server.sendHeader("Location", "/login");
    server.send(302, "text/plain", "");
}

// API endpoint for shortcuts - accepts password as query parameter
void handleApiLogin() {
    String password = server.arg("password");
    password.trim();
    
    if (password == webPassword) {
        loginFailCount = 0;
        String authToken = createSession();

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
        delay(loginBackoffMs());
        if (loginFailCount < 100) loginFailCount++;
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
    doc["firmware"] = FIRMWARE_VERSION;
    doc["sensorsValid"] = lastSensorDataValid;
    if (lastSensorDataValid) {
        doc["moisture"] = lastSensorData.moisturePercent;
        doc["sanitizer"] = lastSensorData.sanitizerLevel;
        doc["irSensor"] = lastSensorData.irDetected;
        doc["ledBrightness"] = lastSensorData.ledBrightness;
        doc["ledLevel"] = lastSensorData.ledLevel;
        doc["dispensing"] = lastSensorData.isDispensing;
        doc["autoDispense"] = lastSensorData.autoDispense;
        doc["ledSchedule"] = lastSensorData.ledSchedule;
        char t[12];
        formatMinute(lastSensorData.ledOnMinute, t, sizeof(t));
        doc["ledOn"] = String(t);
        formatMinute(lastSensorData.ledOffMinute, t, sizeof(t));
        doc["ledOff"] = String(t);
        doc["pumpRunning"] = lastSensorData.pumpRunning;
    } else {
        doc["moisture"] = 0; doc["sanitizer"] = 0; doc["irSensor"] = false;
        doc["ledBrightness"] = 0; doc["dispensing"] = false;
        doc["autoDispense"] = false; doc["ledSchedule"] = false;
        doc["pumpRunning"] = false;
    }
    String s; serializeJson(doc, s);
    server.send(200, "application/json", s);
}

// LED control from the web dashboard: {"level": 1-100} or {"on": true/false}. Main owns the state.
void handleLedControl() {
    if (!isAuthenticated()) { server.send(401, "application/json", JSON_FAIL_UNAUTHORIZED); return; }
    DynamicJsonDocument doc(128);
    if (deserializeJson(doc, server.arg("plain"))) { server.send(400, "application/json", "{\"success\":false,\"error\":\"bad json\"}"); return; }
    if (doc.containsKey("level")) {
        int lvl = doc["level"].as<int>();
        if (lvl < 1 || lvl > 100) { server.send(400, "application/json", "{\"success\":false,\"error\":\"level must be 1-100\"}"); return; }
        enqueueSettingCommand(CMD_SET_LED_BRIGHTNESS, (uint8_t)lvl, 0);
    } else if (doc.containsKey("on")) {
        enqueueSettingCommand(CMD_SET_LED_STATE, doc["on"].as<bool>() ? 1 : 0, 0);
    } else {
        server.send(400, "application/json", "{\"success\":false,\"error\":\"need level or on\"}"); return;
    }
    server.send(200, "application/json", espNowInitialized ? JSON_SUCCESS : "{\"success\":false,\"error\":\"ESP-NOW not available\"}");
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
    if (currentScreenIndex == SCREEN_MESSAGES) refreshHistoryList();
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
    server.send(200, "application/json", JSON_SUCCESS);
#endif
}

void handleFavicon() {
    server.send(204);
}

void handleRoot() {
    if (!isAuthenticated()) { server.sendHeader("Location", "/login"); server.send(302, "text/plain", ""); return; }
    static const char ROOT_HEAD[] PROGMEM = R"HTML(<!DOCTYPE html>
<html lang="en">
<head>
<meta charset="UTF-8">
<meta name="viewport" content="width=device-width, initial-scale=1, viewport-fit=cover">
<meta name="apple-mobile-web-app-capable" content="yes">
<meta name="apple-mobile-web-app-status-bar-style" content="black-translucent">
<meta name="apple-mobile-web-app-title" content="Prick'n'Print">
<meta name="theme-color" content="#1C2A1F">
<title>Prick'n'Print</title>
<style>
)HTML";
    static const char ROOT_BODY[] PROGMEM = R"HTML(
  /* Page layout only - colours and components come from WebTheme.h */
  .page { max-width: 560px; display: flex; flex-direction: column; gap: 12px; }
  .tile-grid { grid-template-columns: repeat(3, 1fr); }
  @media (max-width: 420px) { .tile-grid { grid-template-columns: 1fr 1fr; } #tLight { grid-column: span 2; } }
  .tile .sub { font-size: 0.75rem; color: var(--text-faint); margin-top: 6px; }
  .tile-action { position: absolute; top: 8px; right: 8px; min-height: 28px; padding: 2px 10px; font-size: 0.72rem; }
  .card h2 { display: flex; align-items: center; gap: 10px; margin-bottom: 12px; }
  .card h2 .ico { color: var(--accent-light); width: 1.2em; text-align: center; }
  .light-row { display: flex; align-items: center; gap: 12px; }
  .light-row input[type=range] { flex: 1; }
  .light-row .pct { min-width: 40px; text-align: right; color: var(--text-muted); font-variant-numeric: tabular-nums; }
  #printMsg { width: 100%; resize: vertical; margin-bottom: 10px; }
  #printResult { font-size: 0.85rem; min-height: 1.2em; }
  #printResult.ok { color: var(--accent-light); }
  #printResult.bad { color: var(--danger); }
  details.card { padding: 0; }
  details.card > summary {
    display: flex; align-items: center; gap: 10px; min-height: 52px; padding: 0 16px; cursor: pointer; list-style: none;
    font-weight: 600; color: var(--text);
  }
  details.card > summary .ico { color: var(--accent-light); width: 1.2em; text-align: center; }
  details.card > summary::-webkit-details-marker { display: none; }
  details.card > summary::after {
    content: ""; margin-left: auto; width: 8px; height: 8px; border-right: 2px solid var(--text-faint);
    border-bottom: 2px solid var(--text-faint); transform: rotate(-45deg); transition: transform 0.15s;
  }
  details.card[open] > summary::after { transform: rotate(45deg); }
  details.card > summary .count { color: var(--text-faint); font-weight: 500; font-size: 0.85rem; }
  .fold { padding: 0 16px 16px; display: flex; flex-direction: column; gap: 10px; }
  .fold textarea, .fold input[type=text], .fold input[type=datetime-local] { width: 100%; }
  .fold textarea { resize: vertical; }
  .fold .list { max-height: 300px; overflow-y: auto; }
  .group-label { font-size: 0.7rem; font-weight: 600; text-transform: uppercase; letter-spacing: 0.06em; color: var(--text-faint); margin: 6px 4px -4px; }
  #audioNow { color: var(--accent-light); font-size: 0.85rem; }
  #audioNow:empty { display: none; }
</style>
</head>
<body>
<header class="topbar">
  <div>
    <h1>Prick'n'Print</h1>
    <div class="sub" id="greeting">Home</div>
  </div>
  <span class="spacer"></span>
  <span class="status-pill" id="liveStatus"><span class="dot"></span><span id="liveLabel">connecting</span></span>
  <a href="/logout" class="btn btn-ghost">Log out</a>
</header>

<div class="page">
  <div class="tile-grid">
    <div class="tile" id="tSa">
      <div class="label">Sanitizer</div><div class="value" id="sa">&mdash;</div><div class="bar"><i id="saBar"></i></div>
      <button type="button" class="btn btn-ghost tile-action" onclick="resetSanitizer()" title="Set the level back to 100% after refilling">Refilled</button>
    </div>
    <div class="tile" id="tMo"><div class="label">Soil moisture</div><div class="value" id="mo">&mdash;</div><div class="bar"><i id="moBar"></i></div></div>
    <div class="tile" id="tLight"><div class="label">Light</div><div class="value" id="light">&mdash;</div><div class="sub" id="lightSched"></div></div>
  </div>

  <div class="card">
    <h2><span class="ico">&#9728;</span>Light</h2>
    <div class="light-row">
      <label class="switch" title="Light on/off"><input type="checkbox" id="ledSwitch" onchange="ledTouch=Date.now();po('/api/led',{on:this.checked})"><span></span></label>
      <input type="range" id="ledLevelRange" min="1" max="100" value="10" aria-label="Brightness"
             oninput="ledTouch=Date.now();el('ledLevelPct').textContent=this.value+'%'"
             onchange="ledTouch=Date.now();po('/api/led',{level:parseInt(this.value)})">
      <span class="pct" id="ledLevelPct">&mdash;</span>
    </div>
  </div>

  <div class="card">
    <h2><span class="ico">&#9998;</span>Print a message</h2>
    <textarea id="printMsg" rows="3" placeholder="Type a note to print and save"></textarea>
    <div class="btn-row">
      <span id="printResult" style="flex:1"></span>
      <button type="button" class="btn btn-primary" id="printBtn" onclick="printMessage()">Print</button>
    </div>
  </div>

  <div class="group-label">On the display</div>

  <details class="card" open>
    <summary><span class="ico">&#9993;</span>Messages<span class="count" id="msgCount"></span></summary>
    <div class="fold"><div class="list" id="msgHist"></div></div>
  </details>

  <details class="card">
    <summary><span class="ico">&#9835;</span>Sounds<span class="count" id="auCount"></span></summary>
    <div class="fold">
      <div id="audioNow"></div>
      <div class="list" id="audioFiles"></div>
      <div class="btn-row"><button type="button" class="btn btn-danger grow" onclick="audioStop()">Stop</button></div>
    </div>
  </details>

  <details class="card">
    <summary><span class="ico">&#9635;</span>Pictures<span class="count" id="imCount"></span></summary>
    <div class="fold"><div class="list" id="imageFiles"></div></div>
  </details>

  <div class="group-label">Lists</div>

  <details class="card">
    <summary><span class="ico">&#9200;</span>Reminders<span class="count" id="rmCount"></span></summary>
    <div class="fold">
      <textarea id="rmMsg" placeholder="Reminder message" rows="3"></textarea>
      <div class="btn-row">
        <button type="button" class="btn" onclick="setQuickReminder(60)">In 1 h</button>
        <button type="button" class="btn" onclick="setQuickReminder(1440)">Tomorrow</button>
        <button type="button" class="btn" onclick="setQuickReminder(10080)">Next week</button>
      </div>
      <div class="btn-row">
        <input type="datetime-local" id="rmTm" style="flex:1;width:auto">
        <button type="button" class="btn btn-primary" onclick="addRem()">Add</button>
      </div>
      <div class="list" id="rm"></div>
    </div>
  </details>

  <details class="card">
    <summary><span class="ico">&#10003;</span>Grocery list<span class="count" id="glCount"></span></summary>
    <div class="fold">
      <div class="btn-row">
        <input type="text" id="glItem" placeholder="Add item" style="flex:1;width:auto" onkeydown="if(event.key==='Enter')addGl()">
        <button type="button" class="btn btn-primary" onclick="addGl()">Add</button>
      </div>
      <div class="list" id="gl"></div>
      <div class="btn-row">
        <button type="button" class="btn grow" onclick="printGl()">Print</button>
        <button type="button" class="btn btn-danger" onclick="clearGl()">Clear</button>
      </div>
    </div>
  </details>

  <details class="card">
    <summary><span class="ico">&#9744;</span>To-do list<span class="count" id="tlCount"></span></summary>
    <div class="fold">
      <div class="btn-row">
        <input type="text" id="tlItem" placeholder="Add task" style="flex:1;width:auto" onkeydown="if(event.key==='Enter')addTl()">
        <button type="button" class="btn btn-primary" onclick="addTl()">Add</button>
      </div>
      <div class="list" id="tl"></div>
      <div class="btn-row">
        <button type="button" class="btn grow" onclick="printTl()">Print</button>
        <button type="button" class="btn btn-danger" onclick="clearTl()">Clear</button>
      </div>
    </div>
  </details>
</div>

<script>
function el(id) { return document.getElementById(id); }
// Session expired (or device rebooted): go back to login instead of silently showing stale data
function chk(r) { if (r.status === 401) { location.href = '/login'; throw new Error('unauthorized'); } return r.json(); }
function q(u) { return fetch(u, {headers: {'Accept': 'application/json'}}).then(chk); }
function po(u, b) { return fetch(u, {method: 'POST', headers: {'Content-Type': 'application/json'}, body: JSON.stringify(b || {})}).then(chk); }
function del(u) { return fetch(u, {method: 'DELETE'}).then(chk); }

// Build list rows with textContent - message text and file names come from outside (Pi, iOS Shortcuts)
function row(text, meta, actions) {
  var r = document.createElement('div'); r.className = 'list-row';
  var t = document.createElement('div'); t.className = 'text'; t.textContent = text;
  if (meta) { var m = document.createElement('div'); m.className = 'meta'; m.textContent = meta; t.appendChild(m); }
  r.appendChild(t);
  if (actions && actions.length) {
    var a = document.createElement('div'); a.className = 'actions';
    actions.forEach(function(x) {
      var b = document.createElement('button'); b.type = 'button';
      b.className = 'btn' + (x.cls ? ' ' + x.cls : ''); b.textContent = x.label; b.onclick = x.fn;
      a.appendChild(b);
    });
    r.appendChild(a);
  }
  return r;
}
function fill(id, rows, emptyText) {
  var c = el(id); c.replaceChildren();
  if (!rows.length) { var e = document.createElement('div'); e.className = 'empty'; e.textContent = emptyText; c.appendChild(e); return; }
  rows.forEach(function(r) { c.appendChild(r); });
}
function count(id, n) { el(id).textContent = n ? n : ''; }

(function() {
  var h = new Date().getHours();
  el('greeting').textContent = h < 12 ? 'Good morning' : h < 18 ? 'Good afternoon' : 'Good evening';
})();

function setQuickReminder(minutesFromNow) {
  var d = new Date(); d.setMinutes(d.getMinutes() + minutesFromNow);
  var p = function(n) { return String(n).padStart(2, '0'); };
  el('rmTm').value = d.getFullYear() + '-' + p(d.getMonth() + 1) + '-' + p(d.getDate()) + 'T' + p(d.getHours()) + ':' + p(d.getMinutes());
}
function resetSanitizer() {
  if (!confirm('Refilled the sanitizer? This sets the level back to 100%.')) return;
  po('/api/reset-sanitizer').then(function(d) { if (d.success) ref(); });
}

// ---- Live status ----
var refBusy = false;
var ledTouch = 0;  // ignore Main's echo briefly after a local light change
function liveStatus(text, state) {
  el('liveStatus').className = 'status-pill ' + state;
  el('liveLabel').textContent = text;
}
// Same tiers as the screen: warn below warnLow, critical below critLow
function tier(v, warnLow, critLow) { return v < critLow ? 'bad' : v < warnLow ? 'warn' : ''; }
function level(id, v, warnLow, critLow) {
  var ok = typeof v === 'number';
  var t = ok ? tier(v, warnLow, critLow) : '';
  el(id).textContent = ok ? v.toFixed(0) + '%' : '—';
  el(id).className = 'value' + (t ? ' ' + t : '');
  el(id + 'Bar').style.width = ok ? Math.max(0, Math.min(100, v)) + '%' : '0';
  el(id + 'Bar').className = t;
  el(id === 'mo' ? 'tMo' : 'tSa').classList.toggle('attention', t === 'bad');
}
function ref() {
  if (refBusy) return;  // device is single-threaded; don't stack requests while one is pending
  refBusy = true;
  q('/api/status').then(function(d) {
    var v = d.sensorsValid;
    if (!v) liveStatus('main board offline', 'warn');
    else liveStatus('live', 'ok');
    level('mo', v ? d.moisture : null, 40, 20);
    level('sa', v ? d.sanitizer : null, 50, 20);
    var on = v && d.ledBrightness > 0;
    el('light').textContent = !v ? '—' : on ? 'On ' + d.ledLevel + '%' : 'Off';
    el('light').className = 'value ' + (on ? 'on' : 'off');
    el('lightSched').textContent = v && d.ledSchedule ? 'Auto ' + d.ledOn + ' – ' + d.ledOff : '';
    if (v && Date.now() - ledTouch > 4000) {
      el('ledSwitch').checked = on;
      var rng = el('ledLevelRange');
      if (d.ledLevel && document.activeElement !== rng) { rng.value = d.ledLevel; el('ledLevelPct').textContent = d.ledLevel + '%'; }
    }
  }).catch(function() {
    liveStatus('display not responding', 'bad');
  }).finally(function() { refBusy = false; });
}

// ---- Print a message ----
function printMessage() {
  var text = el('printMsg').value.trim(), btn = el('printBtn'), res = el('printResult');
  if (!text) { el('printMsg').focus(); return; }
  btn.disabled = true; res.className = ''; res.textContent = 'Printing…';
  po('/api/test/send-message', {text: text}).then(function(d) {
    res.className = d.success ? 'ok' : 'bad';
    res.textContent = d.success ? 'Printed' : (d.error || 'Could not print');
    if (d.success) { el('printMsg').value = ''; loadMessages(); }
  }).catch(function() {
    res.className = 'bad'; res.textContent = 'Could not reach the printer';
  }).finally(function() { btn.disabled = false; });
}

// ---- Reminders (id -> {message, scheduledTime}) ----
var reminders = {};
function drawReminders() {
  var ids = Object.keys(reminders).sort(function(a, b) { return (reminders[a].scheduledTime || 0) - (reminders[b].scheduledTime || 0); });
  fill('rm', ids.map(function(k) {
    var r = reminders[k];
    return row(r.message, new Date((r.scheduledTime || 0) * 1000).toLocaleString(), [{label: 'Delete', cls: 'btn-danger', fn: function() { delRem(k); }}]);
  }), 'No reminders yet');
  count('rmCount', ids.length);
}
function loadReminders() { q('/api/reminders').then(function(d) { reminders = d || {}; drawReminders(); }); }
function addRem() {
  var m = el('rmMsg').value.trim(), t = el('rmTm').value;
  if (!m || !t) return;
  var at = Math.floor(new Date(t).getTime() / 1000), tempId = 'temp_' + Date.now();
  reminders[tempId] = {message: m, scheduledTime: at};  // optimistic; swapped for the server id below
  drawReminders();
  el('rmMsg').value = '';
  po('/api/reminders', {message: m, scheduledTime: at}).then(function(d) {
    delete reminders[tempId];
    if (d.success && d.id) { reminders[d.id] = {message: m, scheduledTime: at}; drawReminders(); }
    else loadReminders();
  }).catch(loadReminders);
}
function delRem(id) {
  if (!confirm('Delete this reminder?')) return;
  delete reminders[id]; drawReminders();
  if (id.indexOf('temp_') === 0) return;
  del('/api/reminders/' + encodeURIComponent(id)).then(function(d) { if (!d.success) loadReminders(); });
}

// ---- Groceries / todos: same shape, index-addressed on the device ----
function itemList(name, listId, countId, inputId, emptyText) {
  var api = '/api/' + name, items = [];
  function draw() {
    fill(listId, items.map(function(it, i) { return row(it, '', [{label: 'Delete', cls: 'btn-danger', fn: function() { remove(i); }}]); }), emptyText);
    count(countId, items.length);
  }
  function load() { q(api).then(function(a) { items = Array.isArray(a) ? a : []; draw(); }); }
  function add() {
    var it = el(inputId).value.trim();
    if (!it) return;
    items.push(it); draw(); el(inputId).value = '';
    po(api, {item: it}).then(function(d) { if (!d.success) load(); }).catch(load);
  }
  function remove(i) {
    items.splice(i, 1); draw();
    del(api + '/' + i).then(function(d) { if (!d.success) load(); }).catch(load);
  }
  function print(what) {
    if (!confirm('Send the ' + what + ' to the printer?')) return;
    po(api + '/print').then(function(d) { if (d.success) load(); });
  }
  function clear(what) {
    if (!confirm('Clear the whole ' + what + '?')) return;
    items = []; draw();
    del(api).then(function(d) { if (!d.success) load(); }).catch(load);
  }
  return {load: load, add: add, print: print, clear: clear};
}
var groceries = itemList('groceries', 'gl', 'glCount', 'glItem', 'Nothing on the list');
var todos = itemList('todos', 'tl', 'tlCount', 'tlItem', 'Nothing to do');
function addGl() { groceries.add(); }
function printGl() { groceries.print('grocery list'); }
function clearGl() { groceries.clear('grocery list'); }
function addTl() { todos.add(); }
function printTl() { todos.print('to-do list'); }
function clearTl() { todos.clear('to-do list'); }

// ---- SD card: messages, sounds, pictures (same three sections as the screen) ----
function sdError(id, countId) { return function() { fill(id, [], 'No SD card in the display'); count(countId, 0); }; }
function loadMessages() {
  q('/api/messages').then(function(d) {
    var msgs = d.messages || [];
    fill('msgHist', msgs.map(function(m) {
      return row(m.preview, m.ts > 1600000000 ? new Date(m.ts * 1000).toLocaleString() : '');
    }), 'No messages yet');
    count('msgCount', msgs.length);
  }).catch(sdError('msgHist', 'msgCount'));
}
function loadAudio() {
  q('/api/audio').then(function(d) {
    var files = d.files || [];
    el('audioNow').textContent = d.playing && d.current ? 'Playing ' + d.current.split('/').pop() : '';
    fill('audioFiles', files.map(function(f) {
      return row(f, '', [{label: 'Play', cls: 'btn-primary', fn: function() { audioPlay(f); }}]);
    }), 'No sounds yet');
    count('auCount', files.length);
  }).catch(sdError('audioFiles', 'auCount'));
}
function audioPlay(f) { po('/api/audio/play', {file: f}).then(function() { setTimeout(loadAudio, 500); }); }
function audioStop() { po('/api/audio/stop').then(function() { setTimeout(loadAudio, 500); }); }
function loadImages() {
  q('/api/images').then(function(d) {
    var files = d.files || [];
    fill('imageFiles', files.map(function(f) {
      return row(f, '', [{label: 'Show', cls: 'btn-primary', fn: function() { po('/api/test/show-image', {file: f}); }}]);
    }), 'No pictures yet');
    count('imCount', files.length);
  }).catch(sdError('imageFiles', 'imCount'));
}

ref();
setInterval(ref, 3000);
setInterval(loadReminders, 30000);
setInterval(loadMessages, 15000);  // new messages arrive from the Pi without a page reload
setInterval(loadAudio, 10000);     // keeps "Playing ..." current
setInterval(loadImages, 60000);
loadReminders();
groceries.load();
todos.load();
loadMessages();
loadAudio();
loadImages();
setQuickReminder(60);
</script>
</body>
</html>
)HTML";
    sendHtmlParts(server, {ROOT_HEAD, WEB_THEME_CSS, ROOT_BODY});
}

void setupWebServer() {
    // WebServer only retains headers registered here; without this the auth cookie is invisible
    static const char* headerKeys[] = {"Cookie"};
    server.collectHeaders(headerKeys, 1);
    server.on("/login", HTTP_GET, handleLogin);
    server.on("/login", HTTP_POST, handleLogin);
    server.on("/logout", HTTP_GET, handleLogout);
    server.on("/", handleRoot);
    
    // API endpoints - proxy to main ESP32
    server.on("/api/login", HTTP_GET, handleApiLogin);  // Shortcut-friendly login endpoint
    server.on("/api/status", HTTP_GET, handleGetStatus);
    server.on("/api/reset-sanitizer", HTTP_POST, handleResetSanitizer);
    server.on("/api/led", HTTP_POST, handleLedControl);
    
    // Hardware pump/LED control, tests, and settings now live only on the Main ESP32's
    // TestServer (port 8081) - see TestServer.h. Keeps a single source of truth so the TZT
    // dashboard and that server can't issue conflicting commands.
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
        if (!isSafeFilename(name)) {
            server.send(400, "application/json", "{\"error\":\"Invalid filename\"}");
            return;
        }
        bool ok = sdCard.downloadToFile(url, String(AUDIO_DIR) + "/" + name);
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
        bool ok = sdCard.downloadToFile(url, String(IMAGES_MEDIA_DIR) + "/" + name);
        server.send(ok ? 200 : 500, "application/json", ok ? JSON_SUCCESS : "{\"error\":\"Download failed\"}");
    });

    server.begin();
}

// Tell the Pi an upload is on the SD card so it isn't offered again (kind: "audio" or "images")
static void markMediaDownloaded(const char* kind, const String& id) {
    if (backend && id.length() > 0)
        backend->put("/" + String(kind) + "/" + id + "/downloaded.json", "true");
}

// Download any audio/image uploads the Pi still lists as not downloaded. Uploads normally arrive as
// download commands on the regular poll; this catches older ones and downloads that failed.
#define MEDIA_BACKLOG_SYNC_MS (30UL * 60UL * 1000UL)
static void syncMediaBacklog() {
    struct { const char* kind; const char* dir; } media[] = { {"audio", AUDIO_DIR}, {"images", IMAGES_MEDIA_DIR} };
    for (auto& m : media) {
        String resp;
        if (!backend->get("/" + String(m.kind) + ".json", resp) || resp.length() <= 4 || resp == "null") continue;
        DynamicJsonDocument doc(4096);
        if (deserializeJson(doc, resp) != DeserializationError::Ok) continue;
        for (JsonPair kv : doc.as<JsonObject>()) {
            JsonObject entry = kv.value();
            String url = entry["url"] | "";
            String name = entry["name"] | "";
            if (url.length() == 0 || name.length() == 0 || (entry["downloaded"] | false) || !isSafeFilename(name)) continue;
            if (sdCard.downloadToFile(url, String(m.dir) + "/" + name))
                markMediaDownloaded(m.kind, kv.key().c_str());
            esp_task_wdt_reset();
        }
    }
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
    Logger::info("WiFi", "FORCE_WIFI_CONFIG_PORTAL: erasing WiFi, opening config portal (AP: " TZT_AP_SSID ")");
    wifiManager.resetSettings();
    wifiManager.startConfigPortal(TZT_AP_SSID, TZT_AP_PASSWORD);
#else
    esp_task_wdt_reset();  // Feed WDT before blocking autoConnect (portal can take 30s+)
    if (!wifiManager.autoConnect(TZT_AP_SSID, TZT_AP_PASSWORD)) {
        Logger::warn("WiFi", "config portal timed out or failed.");
    }
#endif
    
    if (WiFi.status() == WL_CONNECTED) {
        deviceIP = WiFi.localIP().toString();
        Logger::enableNetworkLog("TZT-Display");
        Logger::info("Logger", "Network log broadcast enabled on UDP port 47269");
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

    configTzTime(TIMEZONE_POSIX, NTP_SERVER);  // POSIX TZ handles CST/CDT switch automatically

    esp_task_wdt_reset();  // Feed WDT before backend init
    Logger::setLevel(LOG_LEVEL_INFO);
    backend = new HttpBackendService(BACKEND_URL, BACKEND_TIMEOUT, BACKEND_API_KEY);
    backend->setRetryPolicy(2, 250);  // worst case ~2 x BACKEND_TIMEOUT per call when the Pi is down
    backend->setDeviceIdentity("tzt-display", "TZT Display");  // Pi marks us seen on every poll
    reminderService = new ReminderService(backend);
    // Load reminders from SD first (primary persistence)
    if (sdCard.isReady()) {
        String r;
        if (sdCard.readFile(DATA_DIR "/reminders.json", r) && r.length())
            reminderService->fromJSON(r);
    }
    loadGroceries();
    loadTodos();

    // Initialize SD card (VSPI — independent of display HSPI)
    if (sdCard.begin()) {
        sdCard.loadRecentMessages(MAX_CACHED_MESSAGES);
        audioSvc.begin();
    } else {
        Logger::warn("SD", "Skipped: message history & media will use backend only");
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
    lv_disp_t* disp = lv_disp_drv_register(&disp_drv);
    // Dark default theme seeded with the FI primary/accent, so any widget part not styled
    // explicitly (scrollbars, focus outlines, list text) still matches UiTheme.h
    lv_disp_set_theme(disp, lv_theme_default_init(disp, uiColor(UI_BORDER), uiColor(UI_ACCENT),
                                                  true, LV_FONT_DEFAULT));
    // Touch input (XPT2046, TOUCH_CS 33 — uses getTouchRaw() + affine calibration from config_tzt.h)
    static lv_indev_drv_t indev_drv;
    lv_indev_drv_init(&indev_drv);
    indev_drv.type = LV_INDEV_TYPE_POINTER;
    indev_drv.read_cb = touch_read_cb;
    lv_indev_drv_register(&indev_drv);
    // createControlPanel() deferred to first loop when splash ends (smooth transition to UI)
#else
    Logger::info("Display", "TZT_HEADLESS (no display)");
#endif

    esp_task_wdt_reset();  // Feed WDT before ESP-NOW and web server
    
    if (espNowMacIsSet(mainESP32Mac)) {
        // Wait a bit for WiFi to fully stabilize before initializing ESP-NOW
        delay(500);
        
        // WiFi already in STA from WiFiManager.autoConnect; do not call WiFi.mode(WIFI_STA) here
        // (can briefly glitch connection and wrong channel at add_peer).
        
        // Initialize ESP-NOW
        esp_err_t initResult = esp_now_init();
        if (initResult == ESP_OK) {
            esp_now_register_send_cb(onDataSentStatic);
            esp_now_register_recv_cb(onDataRecvStatic);
            
            // Add peer (Main ESP32) on an explicit channel (ESP-IDF example uses explicit channel, not 0)
            esp_err_t addPeerResult = espNowEnsurePeer(mainESP32Mac, espNowPeerChannel(ESP_NOW_CHANNEL));
            if (addPeerResult == ESP_OK) {
                espNowInitialized = true;
            } else {
                Logger::warn("ESP-NOW", "failed to add peer: " + String(esp_err_to_name(addPeerResult)));
            }
        } else {
            Logger::warn("ESP-NOW", "initialization failed: " + String((int)initResult));
        }
    } else {
        Logger::warn("ESP-NOW", "Main ESP32 MAC address not configured in config_tzt.h");
        Logger::warn("ESP-NOW", "To enable ESP-NOW, set MAIN_ESP32_MAC_ADDRESS in config_tzt.h");
    }

    setupWebServer();
    Logger::info("Boot", "TZT Display ready | http://" + deviceIP + ":8080");
}

void loop() {
    esp_task_wdt_reset();
    // Main sends a heartbeat every 10 s; after 3 missed ones, stop showing its last values as live
    unsigned long lastRx = lastSensorDataReceiveTime;  // read before millis(): the WiFi task may update it meanwhile
    if (lastSensorDataValid && millis() - lastRx > SENSOR_DATA_STALE_MS) {
        lastSensorDataValid = false;
        sensorDataNeedsPanelUpdate = true;
        Logger::warn("ESP-NOW", "No data from Main for " + String(SENSOR_DATA_STALE_MS / 1000) + " s - marked offline");
    }
    if (otaService) otaService->handle();
    server.handleClient();

#if !defined(TZT_HEADLESS)
    // Splash: when time is up, create UI and switch to main screen (one-time)
    if (splashVisible) {
        if (millis() >= splashReadyAt) {
            createControlPanel();
            updateControlPanelStatus();
            lv_scr_load(launcherScreen);
            splashVisible = false;
        }
        delay(2);
        return;
    }
    // Show test image if requested via /api/test/show-image
    if (pendingTestImagePath.length() > 0) {
        String path = pendingTestImagePath;
        pendingTestImagePath = "";
        currentImagePath = path;  // viewer redraw below keeps it on screen (LVGL repaints the black bg on load)
        imageViewerNeedsDraw = true;
        hideStatusBar();
        lv_scr_load(imageViewerScreen);
    }
    if (launcherScreen) {
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
            lastSensorDataValid ? 1 : 0, launcherScreen ? (unsigned long)(millis() - lastPanelUpdate) : 0);
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
                Logger::info("ESP-NOW", "retrying...");
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
            // Re-add the peer if it was lost or the WiFi channel moved (no log to reduce clutter)
            if (espNowMacIsSet(mainESP32Mac))
                espNowEnsurePeer(mainESP32Mac, espNowPeerChannel(ESP_NOW_CHANNEL));
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
    static unsigned long lastMediaSync = 0;
    static bool mediaSynced = false;
    if (millis() - lastCommandFound < 120000) {
        backendPollInterval = 10000;
    } else {
        backendPollInterval = 30000;
    }
    
    if (backend && WiFi.status() == WL_CONNECTED && (millis() - lastCmd >= backendPollInterval)) {
        lastCmd = millis() - (unsigned long)random(0, 2000);  // Jitter backwards: a future lastCmd underflows the elapsed check and polls every loop
        DynamicJsonDocument cmds(2048);
        bool pollOk = backend->pollCommands(cmds);
        if (!pollOk) Logger::warn("Backend", "Poll failed (check Pi URL/WiFi): " + backend->getLastError());
        // Backlog sync right after the first good poll, then every 30 min (piggybacks on a poll that worked)
        if (pollOk && sdCard.isReady() && (!mediaSynced || millis() - lastMediaSync >= MEDIA_BACKLOG_SYNC_MS)) {
            mediaSynced = true;
            lastMediaSync = millis();
            syncMediaBacklog();
        }
        if (pollOk && cmds.size() > 0) {
            Logger::info("Backend", "Commands: " + String(cmds.size()));
            lastCommandFound = millis();
            for (JsonPair kv : cmds.as<JsonObject>()) {
                JsonObject c = kv.value();
                if (c["processed"] | false) continue;
                String typ = c["type"].as<String>(), data = c["data"].as<String>();
                // Commands: text → print + save to SD + display; audio → send/save + play; image → send/save + show
                if (typ == "print") {
                    Logger::info("Backend", "Print: \"" + data.substring(0, data.length() > 40 ? 40 : data.length()) + (data.length() > 40 ? "..." : "") + "\"");
                    String source = c["source"].as<String>();
                    bool messageOnly = !source.equalsIgnoreCase("shortcut");
                    sendPrintChunked(data, messageOnly);
                    sdCard.saveMessage(data, source);
#if !defined(TZT_HEADLESS)
                    lastPrintMessage = data;
                    if (currentScreenIndex == SCREEN_MESSAGES) refreshHistoryList();
#endif
                }
                else if (typ == "download_audio") {
                    String url = c["url"] | "";
                    String name = c["name"] | "";
                    if (url.length() > 0 && name.length() > 0 && isSafeFilename(name) && sdCard.isReady()) {
                        String path = String(AUDIO_DIR) + "/" + name;
                        if (sdCard.downloadToFile(url, path)) {
                            markMediaDownloaded("audio", c["media_id"] | "");
                            if (!(c["silent"] | false)) audioSvc.playFile(path);  // silent = upload, just store it
                        }
                    }
                }
                else if (typ == "play_audio" && data.length() > 0 && isSafeFilename(data)) {
                    String path = String(AUDIO_DIR) + "/" + data;
                    audioSvc.playFile(path);
                }
                else if (typ == "download_image") {
                    String url = c["url"] | "";
                    String name = c["name"] | "";
                    if (url.length() > 0 && name.length() > 0 && isSafeFilename(name) && sdCard.isReady()) {
                        String path = String(IMAGES_MEDIA_DIR) + "/" + name;
                        if (sdCard.downloadToFile(url, path)) {
                            markMediaDownloaded("images", c["media_id"] | "");
#if !defined(TZT_HEADLESS)
                            if (!(c["silent"] | false)) pendingTestImagePath = path;  // silent = upload, just store it
#endif
                        }
                    }
                }
                else if (typ == "show_image" && data.length() > 0 && isSafeFilename(data)) {
#if !defined(TZT_HEADLESS)
                    String showPath = String(IMAGES_MEDIA_DIR) + "/" + data;
                    if (!SD.exists(showPath)) showPath = String(IMAGES_DIR) + "/" + data;
                    pendingTestImagePath = showPath;
#endif
                }
                String cmdPath = "/commands/" + String(kv.key().c_str()) + ".json";
                if (!backend->deleteData(cmdPath)) {
                    Logger::warn("Backend", "DELETE failed for " + String(kv.key().c_str()) + ": " + backend->getLastError());
                }
                esp_task_wdt_reset();  // a batch of downloads + DELETEs can take a while
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

    audioSvc.loop();

#if !defined(TZT_HEADLESS)
    lv_timer_handler();
    // Draw the JPEG only after LVGL has painted over it (screen load, invalidation) or a new image was
    // picked - a static viewer costs nothing, instead of a full SD decode + push 5x per second.
    if (imageViewerNeedsDraw && imageViewerScreen && lv_scr_act() == imageViewerScreen && currentImagePath.length() > 0) {
        imageViewerNeedsDraw = false;
        displayFlushing = true;
        drawSdJpegToTft(currentImagePath.c_str(), TZT_SCREEN_HEIGHT);
        displayFlushing = false;
    }
    // Idle: untouched for 2 min anywhere but the launcher -> back to the launcher
    if (launcherScreen && lv_scr_act() != launcherScreen &&
        (unsigned long)(millis() - lastDashboardTouchTime) > DASHBOARD_IDLE_RETURN_MS) {
        lastDashboardTouchTime = millis();
        returnToLauncher();
    }
    // Keep the now-playing bar in sync when playback ends naturally
    if (soundsScreen && lv_scr_act() == soundsScreen) updateNowPlaying();
#endif
    delay(0);  // Yield only — no fixed delay so LVGL gets more CPU for smoother GUI
}
