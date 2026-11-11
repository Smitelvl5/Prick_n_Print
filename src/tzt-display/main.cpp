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
#include <Preferences.h>
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
#include "AlarmService.h"
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
#define LAUNCHER_BUTTONS 3  // menu tiles that can carry a NEW badge (Settings is the 4th tile, never new)
#define SCREEN_LAUNCHER -1  // the menu: Messages / Sounds / Pictures tiles + settings gear
#define SCREEN_HOME     -2  // big Dispense button over a Menu button; idle always returns here
static int currentScreenIndex = SCREEN_HOME;
#define DASHBOARD_IDLE_RETURN_MS  120000  // untouched for 2 min anywhere but home -> back to home
static unsigned long lastDashboardTouchTime = 0;   // any touch resets the idle-return timer
static lv_obj_t* launcherScreen = nullptr;
// Home: Dispense fills the top 75%, Menu the bottom 25%
static lv_obj_t* homeScreen = nullptr;
static lv_obj_t* homeDispenseBtn = nullptr;
static lv_obj_t* homeDispenseLabel = nullptr;  // "Dispense" / "Dispensing..."
static lv_obj_t* homeDispenseInfo = nullptr;   // sanitizer level, or why it may not work
static lv_obj_t* homeMenuBadge = nullptr;      // NEW on the Menu button while a section has something unseen
static lv_obj_t* homeLevelArc = nullptr;       // sanitizer level ring
static lv_obj_t* homeLevelLabel = nullptr;     // "72%" inside the ring
static lv_obj_t* homeDoseBar = nullptr;        // fills while the pump runs
#define MAX_DOSE_MS 600                        // Main's MAX_DISPENSE_DURATION_MS (3 ml)
// Toast: short confirmation pill over everything (top layer), gone after a moment
static lv_obj_t* toastLabel = nullptr;
static unsigned long toastHideAt = 0;
static void showToast(const char* text);
static unsigned long dispenseLockUntil = 0;    // one dose per tap: button rests until Main's dose + cooldown is over
#define DISPENSE_LOCK_MS 2000                  // one dose (0.6 s) plus a moment, so taps don't stack
static void updateHomeScreen();
// Sensors (Settings): switch a broken sensor off so it can't misfire. The TZT keeps the wanted
// state in its own NVS and re-sends it whenever Main reports something else (e.g. after Main
// rebooted on firmware that forgets it), so a broken IR sensor stays ignored.
static bool wantIrSensor = true;        // IR hand sensor starts the pump (Main: CMD_SET_AUTO_DISPENSE)
static bool wantMoistureSensor = true;  // moisture readings used (Main: CMD_SET_MOISTURE_SENSOR)
static lv_obj_t* irSensorSwitch = nullptr;
static lv_obj_t* moistureSensorSwitch = nullptr;
static unsigned long lastSensorResyncAt = 0;
#define SENSOR_RESYNC_MS 15000  // at most this often while Main disagrees
static void loadSensorPrefs();
static void saveSensorPrefs();
static void syncSensorSettingsToMain();
// Screen dimming: after dimAfterMin minutes without a touch the backlight drops to DIM_BRIGHTNESS.
// The first touch on a dim screen only wakes it (no accidental dose); new items and alarms wake it too.
#define DIM_BRIGHTNESS 38                  // ~15% of TZT_TFT_BL_BRIGHTNESS: still shows where the dispenser is
static const uint8_t dimChoices[] = { 1, 2, 5, 0 };  // Settings cycles these; 0 = never
static uint8_t dimAfterMin = 2;            // saved in NVS "display"/"dimMin"
static bool screenDimmed = false;
static unsigned long lastScreenActivity = 0;
static bool touchSwallow = false;          // the waking touch: hidden from LVGL until the finger lifts
static lv_obj_t* dimValueLabel = nullptr;  // Settings: "2 min" / "Never"
static void setBacklight(uint8_t level) { ledcWrite(TZT_TFT_BL_LEDC_CHANNEL, level); }
// Full brightness and a fresh idle timer (touch, new item, alarm)
static void wakeScreen() {
    lastScreenActivity = millis();
    if (!screenDimmed) return;
    screenDimmed = false;
    setBacklight(TZT_TFT_BL_BRIGHTNESS);
}
#define NO_MESSAGE_TEXT "(No messages yet)"
static String lastPrintMessage = NO_MESSAGE_TEXT;
// Each section opens on its newest item; Newer/Older buttons (or a sideways swipe) step back through
// the newest few. No list menus: on a 320x240 resistive panel one big item beats a row of small ones.
#define LIST_SHOW_NEWEST 3  // Messages / Sounds / Pictures step through only the newest few (older stay on the SD card)
// Messages section: one message at a time
static lv_obj_t* msgCard = nullptr;
static lv_obj_t* msgMetaLabel = nullptr;   // "Today 4:45 PM"
static lv_obj_t* msgPosLabel = nullptr;    // "1 of 3"
static lv_obj_t* msgTextLabel = nullptr;
static lv_obj_t* msgNewerBtn = nullptr;
static lv_obj_t* msgOlderBtn = nullptr;
static lv_obj_t* msgEmptyNote = nullptr;     // shown in place of the card when there are none
static int msgCount = 0;    // messages that can be stepped through
static int msgSdCount = 0;  // messages in the SD cache (0 = only the in-memory latest)
static int msgShown = 0;    // 0 = newest
static void refreshMessages();
static void showMessageAt(int pos);
static void messageStepCb(lv_event_t* e);
static void messageCardCb(lv_event_t* e);
// Sounds section: a player on one sound at a time
static lv_obj_t* soundCard = nullptr;
static lv_obj_t* soundNameLabel = nullptr;
static lv_obj_t* soundPosLabel = nullptr;
static lv_obj_t* soundNewBadge = nullptr;
static lv_obj_t* soundPrevBtn = nullptr;
static lv_obj_t* soundNextBtn = nullptr;
static lv_obj_t* soundPlayBtn = nullptr;
static lv_obj_t* soundPlayLabel = nullptr;
static lv_obj_t* soundEmptyNote = nullptr;
static int soundShown = 0;  // 0 = newest
static void updateNowPlaying(bool force = false);
#define MAX_AUDIO_FILES 20
static String audioFileNames[MAX_AUDIO_FILES];
static int audioFileCount = 0;
static void refreshSounds(bool toNewest);
static void soundStepCb(lv_event_t* e);
static void soundPlayCb(lv_event_t* e);
static void soundCardCb(lv_event_t* e);
// Pictures section: straight to the full-screen viewer; picturesScreen is only the empty state
#define MAX_IMAGE_FILES 30
static String imageFileNames[MAX_IMAGE_FILES];
static int imageFileCount = 0;
static int imageShown = -1;  // index into imageFileNames; -1 = a picture not in the list (test image)
static lv_obj_t* picturesEmptyNote = nullptr;
static lv_obj_t* imageViewerScreen = nullptr;  // full screen, tap/swipe to browse or close
static void refreshImages();
static void showImageAt(int idx);
static void imageViewerTapCb(lv_event_t* e);
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
#define TZT_NAV_Y          (TZT_SCREEN_HEIGHT - UI_MARGIN - UI_ROW_H)  // bottom bar of big buttons on a section
// Finger-friendly: minimum touch targets and consistent spacing (48px min for tap targets)
#define TZT_MIN_TOUCH_H    48
static lv_obj_t* statusBackBtn = nullptr;    // "<" in status bar: section -> Menu, Menu -> home
static lv_obj_t* statusTitleLabel = nullptr; // "Menu" or the section name
static void updateHeader();
// New items from the backend: the item opens itself, its launcher tile shows a "NEW" badge until
// someone has looked at it, and Main blinks the LED
static lv_obj_t* launcherBadges[LAUNCHER_BUTTONS] = {};
static bool sectionHasNew[LAUNCHER_BUTTONS] = {};
static int newItemShownSection = -1;        // section the last new item was opened on
static unsigned long newItemShownAt = 0;
static unsigned long lastUserTouchTime = 0;  // real touches only (lastDashboardTouchTime is also reset by code)
static String newSoundName;                  // marked NEW on the Sounds player until played
#define NEW_ITEM_BUSY_MS 15000  // touched this recently: someone is using the screen, badge it instead of jumping
#define NEW_ITEM_LED_FLASHES 3
static void setSectionNew(int section, bool isNew);
static void openImageViewer(const String& path);
// Settings > Restart (both boards)
static lv_obj_t* restartConfirm = nullptr;
static unsigned long restartAt = 0;
static void launcherTileCb(lv_event_t* e);
static void goToScreenIndex(int index);
static void showStatusBar();
static void hideStatusBar();
// Splash: startup image until the UI is ready (createControlPanel is deferred to the first loop).
// A 320x240 JPEG as /splash.jpg on the SD root, or a picture sent from the Pi named "splash.jpg";
// without one, a drawn title card.
#define SPLASH_PATH       "/splash.jpg"
#define SPLASH_PHOTO_MS   6000   // minimum time a startup photo stays up (counted from boot, so WiFi time is included)
#define SPLASH_CARD_MS    2500   // minimum time the drawn title card stays up
static bool splashVisible = false;
static unsigned long splashReadyAt = 0;
static bool drawSplashToTft();    // Draw splash to TFT before LVGL (raw); true if it was a photo
static void drawWifiSetupHelp();  // WiFi setup portal is open: how to connect
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
#define PRINT_CHUNK_SIZE PRINT_CHUNK_BYTES  // For CHUNK_ACK handler and sendPrintChunked
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
// The command poll's client (see commandPollTask). Its connection is the only TLS session the TZT has
// RAM for, so Pi requests made while loop() handles a polled batch (downloads, "downloaded" marks,
// the media sync) go through it too; `backend` (its own fresh connection) fails while that's open.
static HttpBackendService* pollBackend = nullptr;
// Health of the command poll and loop(), reported in /api/status
static volatile unsigned long pollStartedAt = 0;  // set while a poll request is in flight (0 = none)
static volatile unsigned long lastPollEndedAt = 0;
static unsigned long maxLoopStallMs = 0;          // longest gap between loop() runs since boot
#define POLL_HANG_RESTART_MS 120000  // a poll is answered within ~30 s; longer = a stalled connection
ReminderService* reminderService = nullptr;
bool remindersNeedSave = false;  // Flag to save reminders in background

extern SDCardService sdCard;
extern AudioService audioSvc;

// Alarms (set from the web UI / API on :8080, or remotely via the Pi's /api/commands)
AlarmService alarms;
#define ALARMS_FILE DATA_DIR "/alarms.json"
#define ALARM_RING_MS        120000  // rings until Stop, or this long
#define ALARM_LED_EVERY_MS   6000    // re-send the LED blink while ringing
#define ALARM_MIN_VOLUME     0.7f    // alarms ring at least this loud (restored after)
static bool alarmRinging = false;
static String alarmLabel;
static unsigned long alarmStartedAt = 0;
static unsigned long alarmLastLedAt = 0;
static float alarmSavedVolume = 0;
static void saveAlarms() { sdCard.writeFile(ALARMS_FILE, alarms.toJSON()); }
static void stopAlarm();
static void alarmLoop();
static String addAlarmFrom(const String& hm, bool daily, const String& label);

// Event chimes (RTTTL, played from flash): a short, distinct one per event
#define TUNE_STARTUP  "start:d=16,o=6,b=160:c,e,g,4c7"
#define TUNE_MESSAGE  "msg:d=16,o=6,b=200:g,8c7"
#define TUNE_SOUND    "snd:d=16,o=6,b=200:c,e,c,8g"
#define TUNE_PICTURE  "pic:d=16,o=6,b=200:e,g,8e7"
#define TUNE_ALARM_SET "set:d=16,o=7,b=220:c,p,c"
#define TUNE_ALARM    "alarm:d=8,o=7,b=200:c,p,c,p,c,p,c,2p"
// Chime unless the user's own sound is playing (don't cut it off) or an alarm is ringing
static void playChime(const char* tune) {
    if (alarmRinging || (audioSvc.isPlaying() && !audioSvc.isPlayingTune())) return;
    audioSvc.playTune(tune);
}

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
// Only call when no chunked send is in progress: it replaces pendingChunkMessage. Use sendPrintChunked().
static bool startPrintChunked(const String& fullMessage, bool messageOnly) {
    unsigned int len = fullMessage.length();
    unsigned int totalChunks = (len + PRINT_CHUNK_SIZE - 1) / PRINT_CHUNK_SIZE;
    if (totalChunks > PRINT_MAX_CHUNKS) {  // Main would drop the whole message; print what fits
        Logger::warn("Print", "Message too long (" + String(len) + " bytes), printing the first " +
                     String(PRINT_MAX_CHUNKS * PRINT_CHUNK_SIZE));
        totalChunks = PRINT_MAX_CHUNKS;
    }
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

// Print queue. Main takes one message at a time: after the last chunk it prints (its loop is busy, so
// it can't ACK) and it ignores a new PRINT_CHUNK_START for 3 s (CHUNK_PRINT_COOLDOWN_MS in main.cpp).
// Starting the next message straight away would overwrite this one mid-send, or get it dropped. So
// messages (from the Pi, reminders, this board's web page) wait here and go one after another.
#define PRINT_QUEUE_MAX       4
#define PRINT_QUEUE_MAX_BYTES (2 * PRINT_MAX_CHUNKS * PRINT_CHUNK_BYTES)  // caps the RAM it holds (~25 KB)
#define PRINT_GAP_MS          4000  // after Main ACKs the last chunk: longer than its 3 s cooldown...
#define PRINT_GAP_MS_PER_BYTE 2     // ...plus printing time, roughly 16 lines of 32 characters a second
struct QueuedPrint { String text; bool messageOnly; };
static QueuedPrint printQueue[PRINT_QUEUE_MAX];
static uint8_t printQueueHead = 0, printQueueCount = 0;
static size_t printQueueBytes = 0;
static bool printSending = false;          // the message startPrintChunked() last began is still going out
static unsigned long printSendDoneAt = 0;  // when it finished (all chunks ACKed, or the send gave up)
static unsigned int printSendDoneLen = 0;  // its length, for the printing-time part of the gap

static bool printQueueHasRoom(size_t len, uint8_t count = 1) {
    if (printQueueCount == 0) return true;  // any one message fits an empty queue
    return printQueueCount + count <= PRINT_QUEUE_MAX && printQueueBytes + len <= PRINT_QUEUE_MAX_BYTES;
}

// Called from loop(): starts the next queued message once Main is ready for it
static void printQueueLoop() {
    if (printSending) {
        if (pendingChunkTotal > 0) return;
        printSending = false;
        printSendDoneAt = millis();
    }
    if (printQueueCount == 0) return;
    if (millis() - printSendDoneAt < PRINT_GAP_MS + (unsigned long)PRINT_GAP_MS_PER_BYTE * printSendDoneLen) return;
    QueuedPrint& q = printQueue[printQueueHead];
    String text = q.text;
    bool messageOnly = q.messageOnly;
    q.text = String();  // free it now, not when the slot is reused
    printQueueHead = (printQueueHead + 1) % PRINT_QUEUE_MAX;
    printQueueCount--;
    printQueueBytes -= text.length();
    printSending = true;
    printSendDoneLen = text.length();
    startPrintChunked(text, messageOnly);  // a failed first send is retried by the chunk resend logic
}

// Queue a message for the printer. false = not queued (ESP-NOW down, or the queue is full).
bool sendPrintChunked(const String& fullMessage, bool messageOnly = false) {
    if (!espNowInitialized) return false;
    if (fullMessage.length() == 0) return true;
    if (!printQueueHasRoom(fullMessage.length())) {
        Logger::warn("Print", "Print queue full, message not printed");
        return false;
    }
    QueuedPrint& q = printQueue[(printQueueHead + printQueueCount) % PRINT_QUEUE_MAX];
    q.text = fullMessage;
    q.messageOnly = messageOnly;
    printQueueCount++;
    printQueueBytes += fullMessage.length();
    printQueueLoop();  // starts at once if the printer is free
    return true;
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
            lastUserTouchTime = millis();
            if (pressed && screenDimmed) touchSwallow = true;  // this touch only wakes the screen
            wakeScreen();
            if (touchSwallow) {
                if (!pressed) touchSwallow = false;  // finger lifted: the next touch is a real one
            } else if (pressed) {
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
    data->state = (touchLastState && !touchSwallow) ? LV_INDEV_STATE_PRESSED : LV_INDEV_STATE_RELEASED;
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

// Nav: home (SCREEN_HOME) -> menu (SCREEN_LAUNCHER) -> section by index (0=Messages, 1=Sounds, 2=Pictures, 3=Settings)
static const char* const sectionTitles[SCREEN_COUNT] = { "Messages", "Sounds", "Pictures", "Settings" };

static lv_obj_t* screenByIndex(int idx) {
    switch (idx) {
        case SCREEN_MESSAGES: return messagesScreen;
        case SCREEN_SOUNDS:   return soundsScreen;
        case SCREEN_PICTURES: return picturesScreen;
        case SCREEN_SETTINGS: return settingsScreen;
        case SCREEN_HOME:     return homeScreen;
        default: return launcherScreen;
    }
}

// Home 0, menu 1, sections (and the image viewer, which counts as Pictures) 2
static int screenDepth(int idx) {
    return idx == SCREEN_HOME ? 0 : idx == SCREEN_LAUNCHER ? 1 : 2;
}

// Open home, the menu or a section. Files are re-read from SD on every open so they're never stale,
// and each section opens on its newest item. Going deeper slides in from the right, back from the left.
static void goToScreenIndex(int index) {
    if (index != SCREEN_HOME && index != SCREEN_LAUNCHER && (index < 0 || index >= SCREEN_COUNT)) return;
    lv_obj_t* target = screenByIndex(index);
    if (!target || (index == currentScreenIndex && lv_scr_act() == target)) return;
    bool deeper = screenDepth(index) > screenDepth(currentScreenIndex);
    currentImagePath = "";
    currentScreenIndex = index;
    if (index == SCREEN_MESSAGES) refreshMessages();
    else if (index == SCREEN_SOUNDS) refreshSounds(true);
    else if (index == SCREEN_PICTURES) refreshImages();
    else if (index == SCREEN_HOME) updateHomeScreen();
    if (deeper) lastSectionOpenTime = (uint32_t)millis();  // the opening tap mustn't also hit the new screen
    if (index == SCREEN_HOME) hideStatusBar();
    else showStatusBar();  // home and the image viewer hide it
    lv_scr_load_anim(target, deeper ? LV_SCR_LOAD_ANIM_MOVE_LEFT : LV_SCR_LOAD_ANIM_MOVE_RIGHT,
        180, 0, false);
    updateHeader();
}

static void launcherTileCb(lv_event_t* e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    int idx = (int)(intptr_t)lv_event_get_user_data(e);
    setSectionNew(idx, false);
    if (idx == SCREEN_PICTURES) {
        refreshImages();
        if (imageFileCount > 0) {  // straight to the newest picture; the Pictures screen is only "none yet"
            showImageAt(0);
            return;
        }
    }
    goToScreenIndex(idx);
}

static void setSectionNew(int section, bool isNew) {
    if (section < 0 || section >= LAUNCHER_BUTTONS) return;
    sectionHasNew[section] = isNew;
    if (launcherBadges[section]) {
        if (isNew) lv_obj_clear_flag(launcherBadges[section], LV_OBJ_FLAG_HIDDEN);
        else lv_obj_add_flag(launcherBadges[section], LV_OBJ_FLAG_HIDDEN);
    }
    // Home's Menu button carries a NEW pill while any section has something unseen
    if (homeMenuBadge) {
        bool any = false;
        for (int i = 0; i < LAUNCHER_BUTTONS; i++) any = any || sectionHasNew[i];
        if (any) lv_obj_clear_flag(homeMenuBadge, LV_OBJ_FLAG_HIDDEN);
        else lv_obj_add_flag(homeMenuBadge, LV_OBJ_FLAG_HIDDEN);
    }
}

// Jump to the menu from any screen, including the image viewer
static void returnToLauncher() {
    goToScreenIndex(SCREEN_LAUNCHER);
}

// "<": a section goes back to the menu, the menu back to home
static void statusBackCb(lv_event_t* e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    goToScreenIndex(currentScreenIndex == SCREEN_LAUNCHER ? SCREEN_HOME : SCREEN_LAUNCHER);
}


// Status bar (hidden on home): "<" back + "Menu" or the section title
static void updateHeader() {
    if (statusTitleLabel)
        lv_label_set_text(statusTitleLabel, currentScreenIndex >= 0 ? sectionTitles[currentScreenIndex] : "Menu");
}

static void showToast(const char* text) {
    if (!toastLabel) {
        toastLabel = lv_label_create(lv_layer_top());
        lv_obj_set_style_text_font(toastLabel, &lv_font_montserrat_14, 0);
        lv_obj_set_style_text_color(toastLabel, uiColor(UI_TEXT), 0);
        lv_obj_set_style_bg_color(toastLabel, uiColor(UI_FIELD), 0);
        lv_obj_set_style_bg_opa(toastLabel, LV_OPA_COVER, 0);
        lv_obj_set_style_border_color(toastLabel, uiColor(UI_BORDER), 0);
        lv_obj_set_style_border_width(toastLabel, 1, 0);
        lv_obj_set_style_radius(toastLabel, LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_pad_hor(toastLabel, 14, 0);
        lv_obj_set_style_pad_ver(toastLabel, 7, 0);
    }
    lv_label_set_text(toastLabel, text);
    lv_obj_clear_flag(toastLabel, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(toastLabel);
    lv_obj_align(toastLabel, LV_ALIGN_TOP_MID, 0, 4);  // over the status bar; taps pass through a label
    toastHideAt = millis() + 2200;
    if (toastHideAt == 0) toastHideAt = 1;
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

// Hairline: cactus green 22% over the background (--border in theme.css)
static lv_color_t uiHairline() {
    return lv_color_mix(uiColor(UI_BORDER), uiColor(UI_BACKGROUND), 56);
}

// Short cactus-green rule: the brand mark under the splash title, reused on tiles and empty states
static lv_obj_t* makeAccentRule(lv_obj_t* parent, int w) {
    lv_obj_t* r = lv_obj_create(parent);
    lv_obj_remove_style_all(r);
    lv_obj_set_size(r, w, 3);
    lv_obj_set_style_bg_color(r, uiColor(UI_BORDER), 0);
    lv_obj_set_style_bg_opa(r, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(r, 2, 0);
    lv_obj_clear_flag(r, LV_OBJ_FLAG_CLICKABLE);
    return r;
}

// Rounded card: header band / tile / row tone with a hairline edge (.card in theme.css)
static void styleCard(lv_obj_t* obj, int pad) {
    lv_obj_set_style_bg_color(obj, uiColor(UI_CARD), 0);
    lv_obj_set_style_bg_opa(obj, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(obj, uiHairline(), 0);
    lv_obj_set_style_border_width(obj, 1, 0);
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

// Disabled buttons (nothing newer/older) sink into the card tone and stop sending CLICKED.
// Local disabled-state colours, or the LVGL theme's grey disabled style would win.
static void setButtonEnabled(lv_obj_t* btn, bool on) {
    if (!btn) return;
    lv_obj_set_style_bg_color(btn, uiColor(UI_CARD), LV_STATE_DISABLED);
    lv_obj_set_style_text_color(btn, uiColor(UI_FIELD), LV_STATE_DISABLED);
    if (on) lv_obj_clear_state(btn, LV_STATE_DISABLED);
    else lv_obj_add_state(btn, LV_STATE_DISABLED);
}

// Big button for a section's bottom bar; user_data carries the step (-1 newer, +1 older) or 0
static lv_obj_t* makeBarButton(lv_obj_t* parent, const char* text, int x, int w, bool primary,
                               lv_event_cb_t cb, int step) {
    lv_obj_t* btn = makeButton(parent, text, w, UI_ROW_H, primary, nullptr);
    lv_obj_set_pos(btn, x, TZT_NAV_Y);
    lv_obj_add_event_cb(btn, cb, LV_EVENT_CLICKED, (void*)(intptr_t)step);
    return btn;
}

// Card filling the content area above the bottom bar; tap/swipe on it is handled by cb
static lv_obj_t* makeItemCard(lv_obj_t* parent, int pad, lv_event_cb_t cb) {
    lv_obj_t* card = lv_obj_create(parent);
    lv_obj_set_size(card, TZT_SCREEN_WIDTH - 2 * UI_MARGIN, TZT_NAV_Y - UI_GAP - (TZT_CONTENT_TOP + UI_GAP));
    lv_obj_set_pos(card, UI_MARGIN, TZT_CONTENT_TOP + UI_GAP);
    styleCard(card, pad);
    lv_obj_add_event_cb(card, cb, LV_EVENT_CLICKED, nullptr);
    return card;
}

// "Nothing here yet": a short cactus rule over a line of text, filling the content area.
// The text label is the note's child 1 (set by showItemView).
static lv_obj_t* makeEmptyNote(lv_obj_t* parent) {
    lv_obj_t* note = lv_obj_create(parent);
    lv_obj_remove_style_all(note);
    lv_obj_set_size(note, TZT_SCREEN_WIDTH, TZT_CONTENT_H);
    lv_obj_set_pos(note, 0, TZT_CONTENT_TOP);
    lv_obj_clear_flag(note, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_t* rule = makeAccentRule(note, 28);
    lv_obj_align(rule, LV_ALIGN_CENTER, 0, -22);
    lv_obj_t* l = makeLabel(note, "", &lv_font_montserrat_16, UI_TEXT_MUTED);
    lv_obj_set_width(l, TZT_SCREEN_WIDTH - 4 * UI_MARGIN);
    lv_obj_set_style_text_align(l, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(l, LV_ALIGN_CENTER, 0, 22);
    return note;
}

// "NEW" pill, shown while something nobody has looked at yet is waiting
static lv_obj_t* makeNewBadge(lv_obj_t* parent) {
    lv_obj_t* badge = lv_label_create(parent);
    lv_label_set_text(badge, "NEW");
    lv_obj_set_style_text_font(badge, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_color(badge, uiColor(UI_BACKGROUND), 0);
    lv_obj_set_style_bg_color(badge, uiColor(UI_ACCENT), 0);
    lv_obj_set_style_bg_opa(badge, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(badge, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_pad_hor(badge, 8, 0);
    lv_obj_set_style_pad_ver(badge, 3, 0);
    return badge;
}

// Sideways swipe of the touch that just ended (set by touch_read_cb before LVGL handles the release):
// finger moved left = +1 (older, like flicking to the next page), right = -1 (newer), 0 = a tap
static int lastSwipeStep() {
    int32_t dx = gestureEndX - gestureStartX, dy = gestureEndY - gestureStartY;
    if (abs(dx) < SWIPE_THRESHOLD || abs(dx) < 2 * abs(dy)) return 0;
    return dx < 0 ? 1 : -1;
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

// ---- Restart (Settings): confirm, then reboot Main over ESP-NOW and this board a moment later ----

static void closeRestartConfirm() {
    if (restartConfirm) lv_obj_del_async(restartConfirm);
    restartConfirm = nullptr;
}

static void restartCancelCb(lv_event_t* e) {
    (void)e;
    closeRestartConfirm();
}

static void restartGoCb(lv_event_t* e) {
    (void)e;
    closeRestartConfirm();
    enqueueSettingCommand(CMD_RESTART, 0, 0);  // resent until Main ACKs; Main reboots ~0.5 s after
    restartAt = millis() + 2500;               // give that time to land before we reboot too
    if (restartAt == 0) restartAt = 1;
    lv_obj_t* cover = lv_obj_create(lv_layer_top());
    lv_obj_remove_style_all(cover);
    lv_obj_set_size(cover, TZT_SCREEN_WIDTH, TZT_SCREEN_HEIGHT);
    lv_obj_set_style_bg_color(cover, uiColor(UI_BACKGROUND), 0);
    lv_obj_set_style_bg_opa(cover, LV_OPA_COVER, 0);
    lv_obj_add_flag(cover, LV_OBJ_FLAG_CLICKABLE);  // swallow taps while we go down
    lv_obj_t* msg = makeLabel(cover, "Restarting...", &lv_font_montserrat_18, UI_TEXT);
    lv_obj_center(msg);
}

// Modal card over everything (top layer); the dimmed backdrop swallows taps
static void restartAskCb(lv_event_t* e) {
    (void)e;
    if (restartConfirm || restartAt) return;
    restartConfirm = lv_obj_create(lv_layer_top());
    lv_obj_remove_style_all(restartConfirm);
    lv_obj_set_size(restartConfirm, TZT_SCREEN_WIDTH, TZT_SCREEN_HEIGHT);
    lv_obj_set_style_bg_color(restartConfirm, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(restartConfirm, LV_OPA_60, 0);
    lv_obj_add_flag(restartConfirm, LV_OBJ_FLAG_CLICKABLE);
    const int w = 260, h = 132, btnW = (w - 3 * 12) / 2;
    lv_obj_t* card = lv_obj_create(restartConfirm);
    lv_obj_set_size(card, w, h);
    lv_obj_center(card);
    styleCard(card, 12);
    lv_obj_t* title = makeLabel(card, "Restart both boards?", &lv_font_montserrat_16, UI_TEXT);
    lv_obj_align(title, LV_ALIGN_TOP_LEFT, 0, 0);
    lv_obj_t* body = makeLabel(card, "The screen and Main come back in ~15 s.", &lv_font_montserrat_12, UI_TEXT_MUTED);
    lv_obj_set_width(body, w - 24);
    lv_label_set_long_mode(body, LV_LABEL_LONG_WRAP);
    lv_obj_align(body, LV_ALIGN_TOP_LEFT, 0, 26);
    lv_obj_t* cancel = makeButton(card, "Cancel", btnW, 40, false, restartCancelCb);
    lv_obj_align(cancel, LV_ALIGN_BOTTOM_LEFT, 0, 0);
    lv_obj_t* go = makeButton(card, "Restart", btnW, 40, true, restartGoCb);
    lv_obj_align(go, LV_ALIGN_BOTTOM_RIGHT, 0, 0);
}

// ---- Home: Dispense + Menu ----

static bool dispenseLocked() { return dispenseLockUntil && (long)(millis() - dispenseLockUntil) < 0; }

// Ring, label, status line and enabled state of the Dispense card, from Main's last sensor packet
static void updateHomeScreen() {
    if (!homeDispenseBtn) return;
    if (dispenseLockUntil && !dispenseLocked()) {
        dispenseLockUntil = 0;
        if (homeDoseBar) lv_obj_add_flag(homeDoseBar, LV_OBJ_FLAG_HIDDEN);
    }
    bool busy = dispenseLocked() || (lastSensorDataValid && lastSensorData.isDispensing);
    setLabelIfChanged(homeDispenseLabel, busy ? "Dispensing" : "Dispense");
    setButtonEnabled(homeDispenseBtn, !busy);
    int level = lastSensorDataValid ? (int)(lastSensorData.sanitizerLevel + 0.5f) : -1;
    bool low = level >= 0 && level <= 15;
    const char* info;
    if (busy) info = "Hands under nozzle";
    else if (!lastSensorDataValid) info = "Main board offline";
    else if (low) info = "Refill soon";
    else if (!wantIrSensor) info = "Hand sensor off";
    else info = "One dose per tap";
    setLabelIfChanged(homeDispenseInfo, info);
    if (homeLevelArc) {
        lv_arc_set_value(homeLevelArc, level < 0 ? 0 : constrain(level, 0, 100));
        lv_obj_set_style_arc_color(homeLevelArc, uiColor(low ? UI_AMBER : UI_ACCENT), LV_PART_INDICATOR);
    }
    if (homeLevelLabel) {
        char pct[8];
        if (level < 0) snprintf(pct, sizeof(pct), "--");
        else snprintf(pct, sizeof(pct), "%d%%", level);
        setLabelIfChanged(homeLevelLabel, pct);
    }
}

// One dose: Main runs the pump for its set dose (0.6 s = 3 ml) and stops it on its own
static void homeDispenseCb(lv_event_t* e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED || dispenseLocked() || sectionJustOpened()) return;
    dispenseLockUntil = millis() + DISPENSE_LOCK_MS;
    if (dispenseLockUntil == 0) dispenseLockUntil = 1;
    bool sent = sendPumpCommandRepeated(CMD_DISPENSE_START);
    Logger::info("UI", sent ? "Dispense tapped" : "Dispense tapped, but ESP-NOW send failed");
    if (!lastSensorDataValid) showToast("Main board isn't answering");
    if (homeDoseBar) {  // fills over the dose time
        lv_obj_clear_flag(homeDoseBar, LV_OBJ_FLAG_HIDDEN);
        lv_bar_set_value(homeDoseBar, 0, LV_ANIM_OFF);
        lv_bar_set_value(homeDoseBar, 100, LV_ANIM_ON);
    }
    updateHomeScreen();
}

static void homeMenuCb(lv_event_t* e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    goToScreenIndex(SCREEN_LAUNCHER);
}

// ---- Sensors (Settings) ----

static void loadSensorPrefs() {
    Preferences prefs;
    if (!prefs.begin("sensors", true)) return;  // first boot: namespace not created yet, keep defaults
    wantIrSensor = prefs.getBool("ir", true);
    wantMoistureSensor = prefs.getBool("moist", true);
    prefs.end();
}

static void loadDisplayPrefs() {
    Preferences prefs;
    if (!prefs.begin("display", true)) return;  // first boot: keep the default
    uint8_t m = prefs.getUChar("dimMin", dimAfterMin);
    for (uint8_t c : dimChoices) if (c == m) dimAfterMin = m;  // ignore anything unexpected
    prefs.end();
}

static void dimLabelText(char* out, size_t len) {
    if (dimAfterMin) snprintf(out, len, "%u min", (unsigned)dimAfterMin);
    else snprintf(out, len, "Never");
}

// Settings: tap cycles 1 -> 2 -> 5 min -> Never
static void dimCycleCb(lv_event_t* e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    size_t n = sizeof(dimChoices), i = 0;
    while (i < n && dimChoices[i] != dimAfterMin) i++;
    dimAfterMin = dimChoices[(i + 1) % n];
    Preferences prefs;
    if (prefs.begin("display", false)) {
        prefs.putUChar("dimMin", dimAfterMin);
        prefs.end();
    }
    char t[12];
    dimLabelText(t, sizeof(t));
    if (dimValueLabel) lv_label_set_text(dimValueLabel, t);
    wakeScreen();
}

static void saveSensorPrefs() {
    Preferences prefs;
    if (!prefs.begin("sensors", false)) return;
    prefs.putBool("ir", wantIrSensor);
    prefs.putBool("moist", wantMoistureSensor);
    prefs.end();
}

// Main reports moisture < 0 while its moisture sensor is switched off
static bool mainMoistureOn() { return lastSensorData.moisturePercent >= 0; }

// Re-send a sensor setting Main doesn't match (it rebooted, or missed the command). One setting per
// call: the settings queue holds a single command, and the next packet catches the other one.
static void syncSensorSettingsToMain() {
    if (!lastSensorDataValid || pendingSettingRemainingSends > 0) return;
    if (lastSensorResyncAt && millis() - lastSensorResyncAt < SENSOR_RESYNC_MS) return;
    if (lastSensorData.autoDispense != wantIrSensor) {
        enqueueSettingCommand(CMD_SET_AUTO_DISPENSE, wantIrSensor ? 1 : 0, 0);
    } else if (mainMoistureOn() != wantMoistureSensor) {
        enqueueSettingCommand(CMD_SET_MOISTURE_SENSOR, wantMoistureSensor ? 1 : 0, 0);
    } else {
        return;
    }
    lastSensorResyncAt = millis();
}

static void sensorSwitchCb(lv_event_t* e) {
    if (lv_event_get_code(e) != LV_EVENT_VALUE_CHANGED) return;
    lv_obj_t* sw = lv_event_get_target(e);
    bool on = lv_obj_has_state(sw, LV_STATE_CHECKED);
    if (sw == irSensorSwitch) {
        wantIrSensor = on;
        enqueueSettingCommand(CMD_SET_AUTO_DISPENSE, on ? 1 : 0, 0);
        showToast(on ? "Hand sensor on" : "Hand sensor off - Dispense only");
        Logger::info("UI", String("IR hand sensor ") + (on ? "on" : "off"));
    } else {
        wantMoistureSensor = on;
        enqueueSettingCommand(CMD_SET_MOISTURE_SENSOR, on ? 1 : 0, 0);
        showToast(on ? "Moisture sensor on" : "Moisture sensor off");
        Logger::info("UI", String("Moisture sensor ") + (on ? "on" : "off"));
    }
    saveSensorPrefs();
    lastSensorResyncAt = millis();  // give Main time to apply it before checking its packets
    updateHomeScreen();
}

// ---- Layout pieces shared by the menu and Settings ----

// Small uppercase caption over a group (".caption" in theme.css)
static lv_obj_t* makeCaption(lv_obj_t* parent, const char* text, int x, int y) {
    lv_obj_t* c = makeLabel(parent, text, &lv_font_montserrat_12, UI_TEXT_MUTED);
    lv_obj_set_style_text_letter_space(c, 1, 0);
    lv_obj_set_pos(c, x, y);
    return c;
}

// Settings row inside a group card: title (+ optional muted line under it) on the left
static void makeRowText(lv_obj_t* card, int y, int rowH, const char* title, const char* sub) {
    lv_obj_t* t = makeLabel(card, title, &lv_font_montserrat_14, UI_TEXT);
    if (sub) {
        lv_obj_t* s = makeLabel(card, sub, &lv_font_montserrat_12, UI_TEXT_MUTED);
        lv_obj_set_pos(t, 0, y + rowH / 2 - 17);
        lv_obj_set_pos(s, 0, y + rowH / 2 + 2);
    } else {
        lv_obj_set_pos(t, 0, y + rowH / 2 - 8);
    }
}

// Hairline between rows of a group card
static void makeRowDivider(lv_obj_t* card, int y, int w) {
    lv_obj_t* d = lv_obj_create(card);
    lv_obj_remove_style_all(d);
    lv_obj_set_size(d, w, 1);
    lv_obj_set_pos(d, 0, y);
    lv_obj_set_style_bg_color(d, uiHairline(), 0);
    lv_obj_set_style_bg_opa(d, LV_OPA_COVER, 0);
}

static const char* const tileSubtitles[SCREEN_COUNT] = {
    "Notes sent to you", "Voice messages", "From the phone", "Sensors, light, sound" };

static void createControlPanel() {
    const int cardW = TZT_SCREEN_WIDTH - 2 * UI_MARGIN;

    // ---- Home (no status bar): Dispense card over the top 75%, Menu bar in the bottom 25% ----
    homeScreen = lv_obj_create(NULL);
    setScreenStyle(homeScreen);
    {
        const int split = TZT_SCREEN_HEIGHT * 3 / 4;  // 180 of 240
        const int cardH = split - UI_MARGIN - UI_GAP / 2;
        homeDispenseBtn = lv_btn_create(homeScreen);
        lv_obj_set_size(homeDispenseBtn, cardW, cardH);
        lv_obj_set_pos(homeDispenseBtn, UI_MARGIN, UI_MARGIN);
        styleButton(homeDispenseBtn, true);
        lv_obj_set_style_bg_grad_color(homeDispenseBtn, uiColor(UI_BUTTON), 0);  // cactus green, deepening downwards
        lv_obj_set_style_bg_grad_dir(homeDispenseBtn, LV_GRAD_DIR_VER, 0);
        lv_obj_set_style_bg_grad_dir(homeDispenseBtn, LV_GRAD_DIR_NONE, LV_STATE_DISABLED);
        lv_obj_set_style_pad_all(homeDispenseBtn, 0, 0);
        lv_obj_add_event_cb(homeDispenseBtn, homeDispenseCb, LV_EVENT_CLICKED, nullptr);

        // Sanitizer level ring on the left
        const int ring = 104;
        homeLevelArc = lv_arc_create(homeDispenseBtn);
        lv_obj_set_size(homeLevelArc, ring, ring);
        lv_obj_align(homeLevelArc, LV_ALIGN_LEFT_MID, 18, -4);
        lv_obj_remove_style(homeLevelArc, nullptr, LV_PART_KNOB);
        lv_obj_clear_flag(homeLevelArc, LV_OBJ_FLAG_CLICKABLE);  // taps go to the card
        lv_arc_set_rotation(homeLevelArc, 270);
        lv_arc_set_bg_angles(homeLevelArc, 0, 360);
        lv_arc_set_range(homeLevelArc, 0, 100);
        lv_obj_set_style_arc_width(homeLevelArc, 8, LV_PART_MAIN);
        lv_obj_set_style_arc_width(homeLevelArc, 8, LV_PART_INDICATOR);
        lv_obj_set_style_arc_color(homeLevelArc, uiColor(UI_BUTTON), LV_PART_MAIN);
        lv_obj_set_style_arc_rounded(homeLevelArc, true, LV_PART_INDICATOR);
        lv_obj_set_style_pad_all(homeLevelArc, 0, 0);
        homeLevelLabel = makeLabel(homeLevelArc, "--", &lv_font_montserrat_24, UI_TITLE);
        lv_obj_align(homeLevelLabel, LV_ALIGN_CENTER, 0, -6);
        lv_obj_t* left = makeLabel(homeLevelArc, "LEFT", &lv_font_montserrat_12, UI_ACCENT);
        lv_obj_set_style_text_letter_space(left, 1, 0);
        lv_obj_align(left, LV_ALIGN_CENTER, 0, 16);

        // Action text on the right
        const int textX = 18 + ring + 20;
        homeDispenseLabel = makeLabel(homeDispenseBtn, "Dispense", &lv_font_montserrat_24, UI_TITLE);
        lv_obj_align(homeDispenseLabel, LV_ALIGN_LEFT_MID, textX, -16);
        homeDispenseInfo = makeLabel(homeDispenseBtn, "", &lv_font_montserrat_14, UI_ACCENT);
        lv_obj_set_width(homeDispenseInfo, cardW - textX - 12);
        lv_label_set_long_mode(homeDispenseInfo, LV_LABEL_LONG_DOT);
        lv_obj_align(homeDispenseInfo, LV_ALIGN_LEFT_MID, textX, 14);

        // Dose progress along the bottom edge while the pump runs
        homeDoseBar = lv_bar_create(homeDispenseBtn);
        lv_obj_set_size(homeDoseBar, cardW - 36, 4);
        lv_obj_align(homeDoseBar, LV_ALIGN_BOTTOM_MID, 0, -12);
        lv_bar_set_range(homeDoseBar, 0, 100);
        lv_obj_set_style_bg_color(homeDoseBar, uiColor(UI_FIELD), LV_PART_MAIN);
        lv_obj_set_style_bg_opa(homeDoseBar, LV_OPA_COVER, LV_PART_MAIN);
        lv_obj_set_style_bg_color(homeDoseBar, uiColor(UI_ACCENT), LV_PART_INDICATOR);
        lv_obj_set_style_anim_time(homeDoseBar, MAX_DOSE_MS, 0);
        lv_obj_clear_flag(homeDoseBar, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_flag(homeDoseBar, LV_OBJ_FLAG_HIDDEN);

        int menuY = split + UI_GAP / 2;
        lv_obj_t* menuBtn = makeButton(homeScreen, "Menu", cardW, TZT_SCREEN_HEIGHT - menuY - UI_MARGIN, false, homeMenuCb);
        lv_obj_set_pos(menuBtn, UI_MARGIN, menuY);
        lv_obj_set_style_bg_color(menuBtn, uiColor(UI_CARD), 0);
        lv_obj_set_style_border_color(menuBtn, uiHairline(), 0);
        lv_obj_set_style_border_width(menuBtn, 1, 0);
        lv_obj_set_style_text_font(lv_obj_get_child(menuBtn, 0), &lv_font_montserrat_18, 0);
        homeMenuBadge = makeNewBadge(menuBtn);
        lv_obj_align(homeMenuBadge, LV_ALIGN_RIGHT_MID, -4, 0);
        lv_obj_add_flag(homeMenuBadge, LV_OBJ_FLAG_HIDDEN);
    }

    const int top = TZT_CONTENT_TOP + UI_GAP;   // first content row under the status bar

    // ---- Menu: 2x2 text tiles (Messages, Sounds, Pictures, Settings) ----
    launcherScreen = lv_obj_create(NULL);
    setScreenStyle(launcherScreen);
    {
        int tileW = (cardW - UI_GAP) / 2;
        int tileH = (TZT_SCREEN_HEIGHT - top - UI_MARGIN - UI_GAP) / 2;
        for (int i = 0; i < SCREEN_COUNT; i++) {
            lv_obj_t* btn = lv_btn_create(launcherScreen);
            lv_obj_set_size(btn, tileW, tileH);
            lv_obj_set_pos(btn, UI_MARGIN + (i % 2) * (tileW + UI_GAP), top + (i / 2) * (tileH + UI_GAP));
            styleCard(btn, 12);
            applyBtnPressStyle(btn);
            lv_obj_add_event_cb(btn, launcherTileCb, LV_EVENT_CLICKED, (void*)(intptr_t)i);
            lv_obj_t* rule = makeAccentRule(btn, 22);
            lv_obj_align(rule, LV_ALIGN_TOP_LEFT, 0, 2);
            lv_obj_t* name = makeLabel(btn, sectionTitles[i], &lv_font_montserrat_18, UI_TEXT);
            lv_obj_align(name, LV_ALIGN_BOTTOM_LEFT, 0, -18);
            lv_obj_t* sub = makeLabel(btn, tileSubtitles[i], &lv_font_montserrat_12, UI_TEXT_MUTED);
            lv_obj_set_width(sub, tileW - 24);
            lv_label_set_long_mode(sub, LV_LABEL_LONG_DOT);
            lv_obj_align(sub, LV_ALIGN_BOTTOM_LEFT, 0, 0);
            if (i < LAUNCHER_BUTTONS) {
                lv_obj_t* badge = makeNewBadge(btn);
                lv_obj_align(badge, LV_ALIGN_TOP_RIGHT, 0, -4);
                if (!sectionHasNew[i]) lv_obj_add_flag(badge, LV_OBJ_FLAG_HIDDEN);
                launcherBadges[i] = badge;
            }
        }
    }

    const int halfW = (cardW - UI_GAP) / 2;

    // ---- Messages: the newest message in full; Newer / Older below (or swipe the card) ----
    messagesScreen = lv_obj_create(NULL);
    setScreenStyle(messagesScreen);
    msgCard = makeItemCard(messagesScreen, 12, messageCardCb);
    lv_obj_add_flag(msgCard, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_SCROLL_MOMENTUM | LV_OBJ_FLAG_SCROLL_ELASTIC);
    lv_obj_set_scroll_dir(msgCard, LV_DIR_VER);
    msgMetaLabel = makeLabel(msgCard, "", &lv_font_montserrat_12, UI_TEXT_MUTED);
    lv_obj_set_style_text_letter_space(msgMetaLabel, 1, 0);
    lv_obj_set_pos(msgMetaLabel, 0, 0);
    msgPosLabel = makeLabel(msgCard, "", &lv_font_montserrat_12, UI_TEXT_MUTED);
    lv_obj_align(msgPosLabel, LV_ALIGN_TOP_RIGHT, 0, 0);
    msgTextLabel = makeLabel(msgCard, "", &lv_font_montserrat_16, UI_TEXT);
    lv_label_set_long_mode(msgTextLabel, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(msgTextLabel, cardW - 24);
    lv_obj_set_style_text_line_space(msgTextLabel, 3, 0);
    lv_obj_set_pos(msgTextLabel, 0, 24);
    msgNewerBtn = makeBarButton(messagesScreen, LV_SYMBOL_LEFT "  Newer", UI_MARGIN, halfW, false, messageStepCb, -1);
    msgOlderBtn = makeBarButton(messagesScreen, "Older  " LV_SYMBOL_RIGHT, UI_MARGIN + halfW + UI_GAP, halfW, false, messageStepCb, 1);
    msgEmptyNote = makeEmptyNote(messagesScreen);

    // ---- Sounds: a player on the newest sound; tap the card or Play, arrows pick another ----
    soundsScreen = lv_obj_create(NULL);
    setScreenStyle(soundsScreen);
    {
        soundCard = makeItemCard(soundsScreen, 12, soundCardCb);
        applyBtnPressStyle(soundCard);  // the whole card is the play/stop target
        makeCaption(soundCard, "VOICE MESSAGE", 0, 0);
        soundPosLabel = makeLabel(soundCard, "", &lv_font_montserrat_12, UI_TEXT_MUTED);
        lv_obj_align(soundPosLabel, LV_ALIGN_TOP_RIGHT, 0, 0);
        soundNameLabel = makeLabel(soundCard, "", &lv_font_montserrat_18, UI_TEXT);
        lv_label_set_long_mode(soundNameLabel, LV_LABEL_LONG_DOT);
        lv_obj_set_size(soundNameLabel, cardW - 24, 44);  // two lines at most
        lv_obj_set_style_text_align(soundNameLabel, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_align(soundNameLabel, LV_ALIGN_CENTER, 0, 6);
        lv_obj_t* hint = makeLabel(soundCard, "Tap to play or stop", &lv_font_montserrat_12, UI_TEXT_MUTED);
        lv_obj_align(hint, LV_ALIGN_BOTTOM_MID, 0, 0);
        soundNewBadge = makeNewBadge(soundCard);
        lv_obj_align(soundNewBadge, LV_ALIGN_TOP_MID, 0, 24);
        const int arrowW = 64, playW = cardW - 2 * (arrowW + UI_GAP);
        soundPrevBtn = makeBarButton(soundsScreen, LV_SYMBOL_LEFT, UI_MARGIN, arrowW, false, soundStepCb, -1);
        soundPlayBtn = makeBarButton(soundsScreen, "", UI_MARGIN + arrowW + UI_GAP, playW, true, soundPlayCb, 0);
        soundPlayLabel = lv_obj_get_child(soundPlayBtn, 0);
        soundNextBtn = makeBarButton(soundsScreen, LV_SYMBOL_RIGHT, UI_MARGIN + arrowW + UI_GAP + playW + UI_GAP,
                                     arrowW, false, soundStepCb, 1);
        soundEmptyNote = makeEmptyNote(soundsScreen);
    }

    // ---- Pictures: the menu opens the viewer directly; this screen only says there are none ----
    picturesScreen = lv_obj_create(NULL);
    setScreenStyle(picturesScreen);
    picturesEmptyNote = makeEmptyNote(picturesScreen);

    // Image viewer: full screen. Swipe or tap an edge for newer/older, tap the middle to close.
    imageViewerScreen = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(imageViewerScreen, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(imageViewerScreen, LV_OPA_COVER, 0);
    lv_obj_set_style_border_opa(imageViewerScreen, LV_OPA_0, 0);
    lv_obj_set_style_pad_all(imageViewerScreen, 0, 0);
    lv_obj_add_flag(imageViewerScreen, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(imageViewerScreen, imageViewerTapCb, LV_EVENT_CLICKED, nullptr);

    currentScreenIndex = SCREEN_HOME;
    lastDashboardTouchTime = millis();
    lv_scr_load(homeScreen);

    // ---- Status bar (top layer, hidden on home): card band with a hairline under it, like the web
    // pages' top bar. "<" back + title on the left; time and WiFi glyph on the right.
    {
        lv_obj_t* layer = lv_layer_top();
        statusBar = lv_obj_create(layer);
        lv_obj_set_size(statusBar, TZT_SCREEN_WIDTH, TZT_STATUS_H);
        lv_obj_set_pos(statusBar, 0, 0);
        styleCard(statusBar, 0);
        lv_obj_set_style_radius(statusBar, 0, 0);
        lv_obj_set_style_border_side(statusBar, LV_BORDER_SIDE_BOTTOM, 0);

        statusBackBtn = makeBarIconButton(statusBar, LV_SYMBOL_LEFT, statusBackCb);
        lv_obj_align(statusBackBtn, LV_ALIGN_LEFT_MID, 0, 0);

        statusTitleLabel = makeLabel(statusBar, "", &lv_font_montserrat_18, UI_TEXT);
        lv_obj_align(statusTitleLabel, LV_ALIGN_LEFT_MID, UI_BACK_W - 4, 0);

        statusWifiIcon = makeLabel(statusBar, LV_SYMBOL_WIFI, &lv_font_montserrat_14, UI_WARNING);
        lv_obj_align(statusWifiIcon, LV_ALIGN_RIGHT_MID, -UI_MARGIN - 2, 0);
        statusTimeLabel = makeLabel(statusBar, "--:--", &lv_font_montserrat_14, UI_TEXT_MUTED);
        lv_obj_align(statusTimeLabel, LV_ALIGN_RIGHT_MID, -UI_MARGIN - 26, 0);
    }

    // ---- Settings: grouped list (caption over a card of rows), scrolls under the status bar ----
    settingsScreen = lv_obj_create(NULL);
    setScreenStyle(settingsScreen);
    lv_obj_add_flag(settingsScreen, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_SCROLL_MOMENTUM);
    lv_obj_set_scroll_dir(settingsScreen, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(settingsScreen, LV_SCROLLBAR_MODE_AUTO);
    const int pad = 12, innerW = cardW - 2 * pad, rowH = 46, sliderRowH = 64, capH = 20;
    const int sliderW = innerW - 16;  // room for the knob at both ends
    int y = top + 6;

    auto makeGroup = [&](const char* caption, int h) {
        makeCaption(settingsScreen, caption, UI_MARGIN + 4, y);
        y += capH;
        lv_obj_t* card = lv_obj_create(settingsScreen);
        lv_obj_set_size(card, cardW, h);
        lv_obj_set_pos(card, UI_MARGIN, y);
        styleCard(card, 0);
        lv_obj_set_style_pad_hor(card, pad, 0);
        y += h + 14;
        return card;
    };

    // Connection: network on the right, IP under it
    {
        lv_obj_t* card = makeGroup("CONNECTION", rowH);
        makeRowText(card, 0, rowH, "Wi-Fi", nullptr);
        const int valueW = innerW - 60;
        statusWifiLabel = makeLabel(card, "--", &lv_font_montserrat_14, UI_TEXT);
        lv_label_set_long_mode(statusWifiLabel, LV_LABEL_LONG_DOT);
        lv_obj_set_style_text_align(statusWifiLabel, LV_TEXT_ALIGN_RIGHT, 0);
        lv_obj_set_width(statusWifiLabel, valueW);
        lv_obj_align(statusWifiLabel, LV_ALIGN_TOP_RIGHT, 0, 5);
        statusIpLabel = makeLabel(card, "", &lv_font_montserrat_12, UI_TEXT_MUTED);
        lv_obj_set_style_text_align(statusIpLabel, LV_TEXT_ALIGN_RIGHT, 0);
        lv_obj_set_width(statusIpLabel, valueW);
        lv_obj_align(statusIpLabel, LV_ALIGN_BOTTOM_RIGHT, 0, -5);
    }

    // Sound & light: volume slider; light switch + level slider
    {
        lv_obj_t* card = makeGroup("SOUND & LIGHT", 2 * sliderRowH + 1);
        lv_obj_t* vt = makeLabel(card, "Volume", &lv_font_montserrat_14, UI_TEXT);
        lv_obj_set_pos(vt, 0, 12);
        int v0 = (int)(audioSvc.getVolume() * 100.0f);
        settingsVolumeValueLabel = makeLabel(card, "", &lv_font_montserrat_14, UI_TEXT_MUTED);
        lv_label_set_text_fmt(settingsVolumeValueLabel, "%d%%", v0);
        lv_obj_align(settingsVolumeValueLabel, LV_ALIGN_TOP_RIGHT, 0, 12);
        volumeSlider = makeSlider(card, sliderW, 0, 100, v0);
        lv_obj_set_pos(volumeSlider, 8, 42);
        lv_obj_add_event_cb(volumeSlider, volumeSliderCb, LV_EVENT_VALUE_CHANGED, nullptr);

        makeRowDivider(card, sliderRowH, innerW);

        int ly = sliderRowH + 1;
        lv_obj_t* lt = makeLabel(card, "Light", &lv_font_montserrat_14, UI_TEXT);
        lv_obj_set_pos(lt, 0, ly + 12);
        ledSwitch = makeSwitch(card);
        lv_obj_align(ledSwitch, LV_ALIGN_TOP_RIGHT, 0, ly + 9);
        lv_obj_add_event_cb(ledSwitch, ledToggleCb, LV_EVENT_VALUE_CHANGED, nullptr);
        ledLevelValueLabel = makeLabel(card, "--", &lv_font_montserrat_14, UI_TEXT_MUTED);
        lv_obj_align(ledLevelValueLabel, LV_ALIGN_TOP_RIGHT, -(UI_SWITCH_W + 10), ly + 12);
        ledLevelSlider = makeSlider(card, sliderW, 1, 100, 10);
        lv_obj_set_pos(ledLevelSlider, 8, ly + 42);
        lv_obj_add_event_cb(ledLevelSlider, ledLevelSliderCb, LV_EVENT_VALUE_CHANGED, nullptr);
        lv_obj_add_event_cb(ledLevelSlider, ledLevelSliderCb, LV_EVENT_RELEASED, nullptr);
    }

    // Sensors: switch off one that broke, so it can't misfire (an IR sensor stuck on "hand detected"
    // would otherwise pump every few seconds). Dispense on home always works.
    {
        lv_obj_t* card = makeGroup("SENSORS", 2 * rowH + 1);
        auto row = [&](int ry, const char* title, const char* sub, bool on) {
            makeRowText(card, ry, rowH, title, sub);
            lv_obj_t* sw = makeSwitch(card);
            lv_obj_align(sw, LV_ALIGN_TOP_RIGHT, 0, ry + (rowH - UI_SWITCH_H) / 2);
            if (on) lv_obj_add_state(sw, LV_STATE_CHECKED);
            lv_obj_add_event_cb(sw, sensorSwitchCb, LV_EVENT_VALUE_CHANGED, nullptr);
            return sw;
        };
        irSensorSwitch = row(0, "Hand sensor", "Starts the pump on its own", wantIrSensor);
        makeRowDivider(card, rowH, innerW);
        moistureSensorSwitch = row(rowH + 1, "Moisture sensor", "Printed on receipts", wantMoistureSensor);
    }

    // Screen: dim after a while untouched
    {
        lv_obj_t* card = makeGroup("SCREEN", rowH);
        makeRowText(card, 0, rowH, "Dim after", "First tap only wakes it");
        char t[12];
        dimLabelText(t, sizeof(t));
        lv_obj_t* btn = makeButton(card, t, 84, rowH - 12, false, dimCycleCb);
        lv_obj_align(btn, LV_ALIGN_RIGHT_MID, 0, 0);
        dimValueLabel = lv_obj_get_child(btn, 0);
    }

    // Device: restart both boards (asks first)
    {
        lv_obj_t* card = makeGroup("DEVICE", rowH);
        makeRowText(card, 0, rowH, "Restart", "Screen and main board");
        lv_obj_t* restartBtn = makeButton(card, "Restart", 84, rowH - 12, false, restartAskCb);
        lv_obj_align(restartBtn, LV_ALIGN_RIGHT_MID, 0, 0);
    }
    // Bottom breathing room so the last card can scroll fully into view
    lv_obj_t* spacer = lv_obj_create(settingsScreen);
    lv_obj_remove_style_all(spacer);
    lv_obj_set_size(spacer, 1, 1);
    lv_obj_set_pos(spacer, 0, y);

    updateStatusBar();  // fill time, WiFi glyph and the Settings WiFi/IP labels
    updateHeader();
    hideStatusBar();    // starts on home
    updateHomeScreen();
}

// Main's sensor packets carry the LED state; keep the Settings LED controls in sync with it
static void updateControlPanelStatus() {
    if (!launcherScreen) return;
    updateHomeScreen();
    syncSensorSettingsToMain();
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

// ── Messages screen ─────────────────────────────────────────────

#define LATEST_MESSAGE_IDX -1  // the in-memory latest message (no SD card)

// "Today 4:45 PM" / "Yesterday 9:02 AM" / "Nov 3, 4:45 PM"; empty if the clock wasn't set
static void formatWhen(time_t t, char* out, size_t len) {
    out[0] = '\0';
    if (t < 1600000000) return;
    struct tm ti, today;
    localtime_r(&t, &ti);
    time_t now = time(nullptr);
    localtime_r(&now, &today);
    today.tm_hour = today.tm_min = today.tm_sec = 0;
    time_t midnight = mktime(&today);
    char hm[10];
    formatMinute(ti.tm_hour * 60 + ti.tm_min, hm, sizeof(hm));
    if (t >= midnight) snprintf(out, len, "Today %s", hm);
    else if (t >= midnight - 86400) snprintf(out, len, "Yesterday %s", hm);
    else {
        char day[8];
        strftime(day, sizeof(day), "%b %d", &ti);
        snprintf(out, len, "%s, %s", day, hm);
    }
}

// Item views share this: the card + bottom bar, or the "none yet" note in their place
static void showItemView(lv_obj_t* const* parts, int n, lv_obj_t* emptyNote, const char* emptyText) {
    bool empty = emptyText != nullptr;
    for (int i = 0; i < n; i++) {
        if (empty) lv_obj_add_flag(parts[i], LV_OBJ_FLAG_HIDDEN);
        else lv_obj_clear_flag(parts[i], LV_OBJ_FLAG_HIDDEN);
    }
    if (!empty) { lv_obj_add_flag(emptyNote, LV_OBJ_FLAG_HIDDEN); return; }
    lv_obj_clear_flag(emptyNote, LV_OBJ_FLAG_HIDDEN);
    lv_label_set_text(lv_obj_get_child(emptyNote, 1), emptyText);
}

// Re-read from SD and show the newest message
static void refreshMessages() {
    if (!msgCard) return;
    bool haveLatest = lastPrintMessage.length() > 0 && lastPrintMessage != NO_MESSAGE_TEXT;
    msgSdCount = 0;
    if (sdCard.isReady()) {
        msgSdCount = sdCard.getMessageCount();
        if (msgSdCount == 0) {
            sdCard.loadRecentMessages(MAX_CACHED_MESSAGES);
            msgSdCount = sdCard.getMessageCount();
        }
    }
    // No history on SD: still show the message received since boot
    msgCount = msgSdCount > 0 ? min(msgSdCount, LIST_SHOW_NEWEST) : (haveLatest ? 1 : 0);
    showMessageAt(0);
}

static void showMessageAt(int pos) {
    lv_obj_t* parts[] = { msgCard, msgNewerBtn, msgOlderBtn };
    if (msgCount == 0) {
        showItemView(parts, 3, msgEmptyNote, sdCard.isReady() ? "No messages yet" : "No messages yet\n(no SD card)");
        return;
    }
    showItemView(parts, 3, msgEmptyNote, nullptr);
    msgShown = constrain(pos, 0, msgCount - 1);
    int idx = msgSdCount > 0 ? msgSdCount - 1 - msgShown : LATEST_MESSAGE_IDX;  // SD cache is oldest first
    String full = (idx == LATEST_MESSAGE_IDX) ? lastPrintMessage : sdCard.readFullMessage(idx);
    if (full.length() == 0) full = "(could not read message)";
    lv_label_set_text(msgTextLabel, full.c_str());
    char when[32];
    if (idx == LATEST_MESSAGE_IDX) snprintf(when, sizeof(when), "Latest");
    else formatWhen(sdCard.getCachedMessage(idx).timestamp, when, sizeof(when));
    lv_label_set_text(msgMetaLabel, when);
    if (msgCount > 1) lv_label_set_text_fmt(msgPosLabel, "%d of %d", msgShown + 1, msgCount);
    else lv_label_set_text(msgPosLabel, "");
    lv_obj_scroll_to_y(msgCard, 0, LV_ANIM_OFF);
    setButtonEnabled(msgNewerBtn, msgShown > 0);
    setButtonEnabled(msgOlderBtn, msgShown < msgCount - 1);
}

static void messageStepCb(lv_event_t* e) {
    if (sectionJustOpened()) return;
    showMessageAt(msgShown + (int)(intptr_t)lv_event_get_user_data(e));
}

// Card tap: a sideways swipe steps (vertical drags scroll the text instead and never get here)
static void messageCardCb(lv_event_t* e) {
    (void)e;
    int step = lastSwipeStep();
    if (step) showMessageAt(msgShown + step);
}

// ── Sounds screen ───────────────────────────────────────────────

static String soundPath(int idx) { return String(AUDIO_DIR) + "/" + audioFileNames[idx]; }

static void showSoundAt(int pos) {
    lv_obj_t* parts[] = { soundCard, soundPrevBtn, soundPlayBtn, soundNextBtn };
    if (audioFileCount == 0) {
        showItemView(parts, 4, soundEmptyNote, sdCard.isReady() ? "No sounds yet" : "SD card not available");
        return;
    }
    showItemView(parts, 4, soundEmptyNote, nullptr);
    soundShown = constrain(pos, 0, audioFileCount - 1);
    String name = audioFileNames[soundShown];
    int dot = name.lastIndexOf('.');
    lv_label_set_text(soundNameLabel, (dot > 0 ? name.substring(0, dot) : name).c_str());
    if (audioFileCount > 1) lv_label_set_text_fmt(soundPosLabel, "%d of %d", soundShown + 1, audioFileCount);
    else lv_label_set_text(soundPosLabel, "");
    if (newSoundName.length() && name == newSoundName) lv_obj_clear_flag(soundNewBadge, LV_OBJ_FLAG_HIDDEN);
    else lv_obj_add_flag(soundNewBadge, LV_OBJ_FLAG_HIDDEN);
    setButtonEnabled(soundPrevBtn, soundShown > 0);
    setButtonEnabled(soundNextBtn, soundShown < audioFileCount - 1);
    updateNowPlaying(true);
}

// Re-read from SD. toNewest: show the newest; else stay on the sound shown (it may have moved down)
static void refreshSounds(bool toNewest) {
    if (!soundCard) return;
    String keep = (!toNewest && soundShown < audioFileCount) ? audioFileNames[soundShown] : String();
    audioFileCount = sdCard.isReady() ? sdCard.listAudioFiles(audioFileNames, LIST_SHOW_NEWEST) : 0;  // newest first
    int pos = 0;
    for (int i = 0; keep.length() && i < audioFileCount; i++)
        if (audioFileNames[i] == keep) pos = i;
    showSoundAt(pos);
}

// Play button: Stop (red) while the sound shown is playing, else Play.
// Called every loop on the Sounds screen, so it only touches LVGL when the state changes.
static void updateNowPlaying(bool force) {
    static int shownPlaying = -1;
    if (!soundPlayBtn || audioFileCount == 0) return;
    bool playing = audioSvc.isPlaying() && audioSvc.currentFile() == soundPath(soundShown);
    if (!force && (int)playing == shownPlaying) return;
    shownPlaying = playing;
    lv_label_set_text(soundPlayLabel, playing ? LV_SYMBOL_STOP "  Stop" : LV_SYMBOL_PLAY "  Play");
    lv_obj_set_style_bg_color(soundPlayBtn, uiColor(playing ? UI_WARNING : UI_BORDER), 0);
}

static void playShownSound() {
    audioSvc.playFile(soundPath(soundShown));  // one sound at a time: replaces whatever is playing
    if (audioFileNames[soundShown] == newSoundName) {
        newSoundName = "";
        setSectionNew(SCREEN_SOUNDS, false);
        lv_obj_add_flag(soundNewBadge, LV_OBJ_FLAG_HIDDEN);
    }
}

static void toggleShownSound() {
    if (audioFileCount == 0) return;
    if (audioSvc.isPlaying() && audioSvc.currentFile() == soundPath(soundShown)) audioSvc.stop();
    else playShownSound();
    updateNowPlaying();
}

static void soundPlayCb(lv_event_t* e) {
    (void)e;
    if (!sectionJustOpened()) toggleShownSound();
}

// Pick another sound; if one was playing, the new pick plays straight away
static void stepSound(int step) {
    bool wasPlaying = audioSvc.isPlaying();
    showSoundAt(soundShown + step);
    if (wasPlaying) { playShownSound(); updateNowPlaying(); }
}

static void soundStepCb(lv_event_t* e) {
    if (!sectionJustOpened()) stepSound((int)(intptr_t)lv_event_get_user_data(e));
}

// Card: tap plays/stops, a sideways swipe picks another sound
static void soundCardCb(lv_event_t* e) {
    (void)e;
    if (sectionJustOpened()) return;
    int step = lastSwipeStep();
    if (step) stepSound(step);
    else toggleShownSound();
}

// ── Splash (raw TFT before LVGL) and Image Gallery ──────────────

#if !defined(TZT_HEADLESS)
static uint16_t tftColor(UiColor c) {
    return tft.color565((c >> 16) & 0xFF, (c >> 8) & 0xFF, c & 0xFF);
}

// Background + centered title (font 4) with an accent rule under it; raw TFT, before LVGL runs
static void drawTitleCard(const char* title) {
    tft.fillScreen(tftColor(UI_BACKGROUND));
    tft.setTextDatum(MC_DATUM);
    tft.setTextColor(tftColor(UI_TEXT), tftColor(UI_BACKGROUND));
    tft.drawString(title, TZT_SCREEN_WIDTH / 2, 70, 4);
    tft.fillRoundRect(TZT_SCREEN_WIDTH / 2 - 40, 92, 80, 4, 2, tftColor(UI_BORDER));
}

static bool drawSplashToTft() {
    const char* photos[] = { SPLASH_PATH, IMAGES_MEDIA_DIR "/splash.jpg" };
    for (const char* p : photos) {
        if (sdCard.isReady() && SD.exists(p)) {
            tft.fillScreen(TFT_BLACK);
            drawSdJpegToTft(p, TZT_SCREEN_HEIGHT);
            return true;
        }
    }
    drawTitleCard("Prick'n'Print");
    tft.setTextColor(tftColor(UI_TEXT_MUTED), tftColor(UI_BACKGROUND));
    tft.drawString("Starting...", TZT_SCREEN_WIDTH / 2, 140, 2);
    return false;
}

static void drawWifiSetupHelp() {
    drawTitleCard("Set up WiFi");
    tft.setTextDatum(TL_DATUM);
    tft.setTextColor(tftColor(UI_TEXT), tftColor(UI_BACKGROUND));
    const int x = 28;
    tft.drawString("1. On your phone, join WiFi:", x, 116, 2);
    tft.setTextColor(tftColor(UI_ACCENT), tftColor(UI_BACKGROUND));
    tft.drawString("   " TZT_AP_SSID "   pw: " TZT_AP_PASSWORD, x, 136, 2);
    tft.setTextColor(tftColor(UI_TEXT), tftColor(UI_BACKGROUND));
    tft.drawString("2. Pick your home WiFi on the page", x, 162, 2);
    tft.drawString("   that opens (or go to 192.168.4.1)", x, 182, 2);
    tft.setTextColor(tftColor(UI_TEXT_MUTED), tftColor(UI_BACKGROUND));
    tft.drawString("Carries on by itself once connected.", x, 212, 2);
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

// Re-read from SD (newest first); the Pictures screen itself only shows when there are none
static void refreshImages() {
    imageFileCount = sdCard.isReady() ? sdCard.listImageFiles(imageFileNames, LIST_SHOW_NEWEST) : 0;
    if (picturesEmptyNote)
        lv_label_set_text(lv_obj_get_child(picturesEmptyNote, 1), sdCard.isReady() ? "No pictures yet" : "SD card not available");
}

// idx into imageFileNames, or -1 for a picture outside the newest few (currentImagePath already set)
static void showImageAt(int idx) {
    imageShown = idx;
    if (idx >= 0) currentImagePath = String(IMAGES_DIR) + "/" + imageFileNames[idx];
    currentScreenIndex = SCREEN_PICTURES;
    imageViewerNeedsDraw = true;
    if (lv_scr_act() != imageViewerScreen) {
        lastSectionOpenTime = (uint32_t)millis();
        hideStatusBar();
        lv_scr_load(imageViewerScreen);
    }
}

// Full-screen viewer for any picture path; if it's one of the newest few, newer/older work from it
static void openImageViewer(const String& path) {
    refreshImages();
    int idx = -1;
    for (int i = 0; i < imageFileCount; i++)
        if (String(IMAGES_DIR) + "/" + imageFileNames[i] == path) idx = i;
    currentImagePath = path;
    showImageAt(idx);
}

// Picture, then (when there's more than one) a "< 2 / 3 >" pill at the bottom. Raw TFT over LVGL's
// black screen, redrawn whenever LVGL repaints it or another picture is picked.
static void drawImageViewer() {
    tft.fillScreen(TFT_BLACK);
    String lower = currentImagePath;
    lower.toLowerCase();
    tft.setTextDatum(MC_DATUM);
    if (lower.endsWith(".jpg") || lower.endsWith(".jpeg")) {
        drawSdJpegToTft(currentImagePath.c_str(), TZT_SCREEN_HEIGHT);
    } else {
        tft.setTextColor(tftColor(UI_TEXT_MUTED), TFT_BLACK);
        tft.drawString("Only JPEG pictures can be shown", TZT_SCREEN_WIDTH / 2, TZT_SCREEN_HEIGHT / 2, 2);
    }
    if (imageShown < 0 || imageFileCount < 2) return;
    const int w = 96, h = 24, x = (TZT_SCREEN_WIDTH - w) / 2, y = TZT_SCREEN_HEIGHT - h - 6, cy = y + h / 2;
    uint16_t bg = tftColor(UI_CARD);
    tft.fillRoundRect(x, y, w, h, h / 2, bg);
    char pos[12];
    snprintf(pos, sizeof(pos), "%d / %d", imageShown + 1, imageFileCount);
    tft.setTextColor(tftColor(UI_TEXT), bg);
    tft.drawString(pos, TZT_SCREEN_WIDTH / 2, cy, 2);
    tft.setTextColor(tftColor(imageShown > 0 ? UI_ACCENT : UI_FIELD), bg);
    tft.drawString("<", x + 12, cy, 2);
    tft.setTextColor(tftColor(imageShown < imageFileCount - 1 ? UI_ACCENT : UI_FIELD), bg);
    tft.drawString(">", x + w - 12, cy, 2);
}

// Swipe, or tap the left/right quarter: newer/older. Tap the middle: close, back to the launcher.
static void imageViewerTapCb(lv_event_t* e) {
    (void)e;
    if (sectionJustOpened()) return;
    int step = lastSwipeStep();
    if (!step && gestureEndX < TZT_SCREEN_WIDTH / 4) step = -1;
    else if (!step && gestureEndX >= TZT_SCREEN_WIDTH * 3 / 4) step = 1;
    if (!step || imageShown < 0 || imageFileCount < 2) { returnToLauncher(); return; }
    int next = constrain(imageShown + step, 0, imageFileCount - 1);
    if (next != imageShown) showImageAt(next);
}
#endif

// Groceries / todos: a JSON array of strings on the SD card. Parsed in place and saved by pointer,
// so the document needs only its fixed slots (16 bytes an item), however long the items are. Sizing it
// from the file length, as before, failed the whole parse for a list of short items, and the list came
// back empty after a reboot.
static int loadStringList(const char* path, String* items, int maxItems) {
    String response;
    if (!sdCard.isReady() || !sdCard.readFile(path, response) || response.length() < 2) return 0;
    DynamicJsonDocument doc(JSON_ARRAY_SIZE(maxItems + 8));
    DeserializationError err = deserializeJson(doc, response.begin(), response.length());
    if (err) {
        Logger::warn("SD", String(path) + " unreadable: " + err.c_str());
        return 0;
    }
    int n = 0;
    for (JsonVariant v : doc.as<JsonArray>()) {
        if (n >= maxItems) break;
        items[n++] = v.as<String>();  // copy out while `response` is alive
    }
    return n;
}

static void saveStringList(const char* path, const String* items, int count) {
    if (!sdCard.isReady()) return;
    DynamicJsonDocument doc(JSON_ARRAY_SIZE(count) + 16);
    JsonArray arr = doc.to<JsonArray>();
    for (int i = 0; i < count; i++) arr.add(items[i].c_str());  // const char*: stored by pointer
    String json;
    serializeJson(doc, json);
    sdCard.writeFile(path, json);
}

void loadGroceries() { groceryCount = loadStringList(DATA_DIR "/groceries.json", groceryItems, MAX_GROCERY_ITEMS); }
void saveGroceries() { saveStringList(DATA_DIR "/groceries.json", groceryItems, groceryCount); }
void loadTodos() { todoCount = loadStringList(DATA_DIR "/todos.json", todoItems, MAX_TODO_ITEMS); }
void saveTodos() { saveStringList(DATA_DIR "/todos.json", todoItems, todoCount); }

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
    DynamicJsonDocument doc(512);
    doc["firmware"] = FIRMWARE_VERSION;
    doc["freeHeap"] = ESP.getFreeHeap();
    doc["minFreeHeap"] = ESP.getMinFreeHeap();  // lowest since boot: TLS (two at once with the command poll) is the big user
    unsigned long pollEnd = lastPollEndedAt;
    doc["lastPollAgoS"] = pollEnd ? (long)((millis() - pollEnd) / 1000) : -1;  // a long poll ends every ~20 s
    doc["maxLoopStallMs"] = maxLoopStallMs;  // screen/touch/audio freeze, worst since boot
    doc["sensorsValid"] = lastSensorDataValid;
    if (lastSensorDataValid) {
        doc["moisture"] = lastSensorData.moisturePercent;  // < 0 = moisture sensor switched off
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
    DynamicJsonDocument doc(JSON_ARRAY_SIZE(MAX_GROCERY_ITEMS) + 16);  // items by pointer, see saveStringList
    JsonArray arr = doc.to<JsonArray>();
    for (int i = 0; i < groceryCount; i++) arr.add(groceryItems[i].c_str());
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
    DynamicJsonDocument doc(JSON_ARRAY_SIZE(MAX_TODO_ITEMS) + 16);  // items by pointer, see saveStringList
    JsonArray arr = doc.to<JsonArray>();
    for (int i = 0; i < todoCount; i++) arr.add(todoItems[i].c_str());
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
    if (currentScreenIndex == SCREEN_MESSAGES) refreshMessages();
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
    <summary><span class="ico">&#9201;</span>Alarms<span class="count" id="alCount"></span></summary>
    <div class="fold">
      <input type="text" id="alLabel" placeholder="Label (optional)" maxlength="60">
      <div class="btn-row">
        <input type="time" id="alTime" style="flex:1;width:auto">
        <label style="display:flex;align-items:center;gap:6px"><input type="checkbox" id="alDaily" style="width:auto"> Every day</label>
        <button type="button" class="btn btn-primary" onclick="addAlarm()">Add</button>
      </div>
      <div class="list" id="al"></div>
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
    level('mo', v && d.moisture >= 0 ? d.moisture : null, 40, 20);
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

// ---- Alarms (id -> {time "HH:MM", daily, label}); ring on the screen with a Stop button ----
var alarmsById = {};
function drawAlarms() {
  var ids = Object.keys(alarmsById).sort(function(a, b) { return alarmsById[a].time < alarmsById[b].time ? -1 : 1; });
  fill('al', ids.map(function(k) {
    var a = alarmsById[k];
    return row(a.time + (a.label ? '  ' + a.label : ''), a.daily ? 'Every day' : 'Once',
               [{label: 'Delete', cls: 'btn-danger', fn: function() { delAlarm(k); }}]);
  }), 'No alarms');
  count('alCount', ids.length);
}
function loadAlarms() { q('/api/alarms').then(function(d) { alarmsById = d || {}; drawAlarms(); }); }
function addAlarm() {
  var t = el('alTime').value;
  if (!t) { el('alTime').focus(); return; }
  po('/api/alarms', {time: t, daily: el('alDaily').checked, label: el('alLabel').value.trim()}).then(function(d) {
    if (!d.success) alert(d.error || 'Could not set the alarm');
    else { el('alLabel').value = ''; }
    loadAlarms();
  }).catch(loadAlarms);
}
function delAlarm(id) {
  delete alarmsById[id]; drawAlarms();
  del('/api/alarms/' + encodeURIComponent(id)).then(function(d) { if (!d.success) loadAlarms(); });
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
loadAlarms();
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

    // Alarms: GET list, POST {"time":"07:30","daily":true,"label":"..."}, DELETE /api/alarms/<id>,
    // POST /api/alarms/stop silences a ringing one
    server.on("/api/alarms", HTTP_GET, []() {
        if (!isAuthenticated()) { server.send(401, "application/json", JSON_FAIL_UNAUTHORIZED); return; }
        server.send(200, "application/json", alarms.toJSON());
    });
    server.on("/api/alarms", HTTP_POST, []() {
        if (!isAuthenticated()) { server.send(401, "application/json", JSON_FAIL_UNAUTHORIZED); return; }
        DynamicJsonDocument doc(256);
        if (deserializeJson(doc, server.arg("plain"))) { server.send(400, "application/json", "{\"success\":false,\"error\":\"bad json\"}"); return; }
        String id = addAlarmFrom(doc["time"] | "", doc["daily"] | false, doc["label"] | "");
        if (id.length() == 0) { server.send(400, "application/json", "{\"success\":false,\"error\":\"time must be HH:MM (24 h), max " + String(MAX_ALARMS) + " alarms\"}"); return; }
        server.send(200, "application/json", "{\"success\":true,\"id\":\"" + id + "\"}");
    });
    server.on("/api/alarms/stop", HTTP_POST, []() {
        if (!isAuthenticated()) { server.send(401, "application/json", JSON_FAIL_UNAUTHORIZED); return; }
        stopAlarm();
        server.send(200, "application/json", JSON_SUCCESS);
    });
    
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
        if (uri.startsWith("/api/alarms/") && method == HTTP_DELETE) {
            if (!isAuthenticated()) { server.send(401, "application/json", JSON_FAIL_UNAUTHORIZED); return; }
            bool ok = alarms.remove(uri.substring(strlen("/api/alarms/")));
            if (ok) saveAlarms();
            server.send(ok ? 200 : 404, "application/json", ok ? JSON_SUCCESS : "{\"success\":false,\"error\":\"not found\"}");
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
        // Optional "volume" 0-100; keep the Settings slider in sync (handler runs on the LVGL loop task)
        if (doc.containsKey("volume")) {
            int v = constrain(doc["volume"].as<int>(), 0, 100);
            audioSvc.setVolume((float)v / 100.0f);
#if !defined(TZT_HEADLESS)
            if (volumeSlider) lv_slider_set_value(volumeSlider, v, LV_ANIM_OFF);
            if (settingsVolumeValueLabel) lv_label_set_text_fmt(settingsVolumeValueLabel, "%d%%", v);
#endif
        }
        String path = String(AUDIO_DIR) + "/" + file;
        bool ok = audioSvc.playFile(path);
        server.send(ok ? 200 : 500, "application/json", ok ? JSON_SUCCESS : "{\"error\":\"Playback failed\"}");
    });

    server.on("/api/audio/stop", HTTP_POST, []() {
        if (!isAuthenticated()) { server.send(401, "application/json", JSON_FAIL_UNAUTHORIZED); return; }
        audioSvc.stop();
        server.send(200, "application/json", JSON_SUCCESS);
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

    server.begin();
}

// Tell the Pi an upload is on the SD card so it isn't offered again (kind: "audio" or "images").
// Only while loop() handles a polled batch: it uses the poll's connection (see pollBackend).
static void markMediaDownloaded(const char* kind, const String& id) {
    if (pollBackend && id.length() > 0 &&
        !pollBackend->put("/" + String(kind) + "/" + id + "/downloaded.json", "true"))
        Logger::warn("Media", "Marking " + id + " downloaded failed: " + pollBackend->getLastError());
}

// Download a Pi upload over the poll's connection. Same rule as markMediaDownloaded().
static bool fetchFromPi(const String& piPath, Stream& out) {
    if (!pollBackend) return false;
    if (pollBackend->download(piPath, out)) return true;
    Logger::warn("Media", "Download " + piPath + " failed: " + pollBackend->getLastError());
    return false;
}

// ── Alarms ───────────────────────────────────────────────────────
// Ringing: alarm tune on repeat (at least ALARM_MIN_VOLUME), Main blinks the LED, and a full-screen
// card with a big Stop button. Stops on Stop, POST /api/alarms/stop, or after ALARM_RING_MS.
#if !defined(TZT_HEADLESS)
static lv_obj_t* alarmOverlay = nullptr;
static void alarmStopCb(lv_event_t* e);
#endif

static void stopAlarm() {
    if (!alarmRinging) return;
    alarmRinging = false;
    audioSvc.stop();
    audioSvc.setVolume(alarmSavedVolume);
#if !defined(TZT_HEADLESS)
    if (alarmOverlay) lv_obj_del_async(alarmOverlay);
    alarmOverlay = nullptr;
    lastDashboardTouchTime = millis();
#endif
    Logger::info("Alarm", "Stopped");
}

#if !defined(TZT_HEADLESS)
static void alarmStopCb(lv_event_t* e) {
    (void)e;
    stopAlarm();
}
#endif

static void startAlarm(const String& label) {
    if (alarmRinging) return;
    alarmRinging = true;
    alarmLabel = label;
    alarmStartedAt = millis();
    alarmLastLedAt = 0;
    alarmSavedVolume = audioSvc.getVolume();
    if (alarmSavedVolume < ALARM_MIN_VOLUME) audioSvc.setVolume(ALARM_MIN_VOLUME);
    audioSvc.playTune(TUNE_ALARM);
    wakeScreen();
    Logger::info("Alarm", "Ringing" + (label.length() ? ": " + label : String("")));
#if !defined(TZT_HEADLESS)
    if (!launcherScreen) return;  // still on the splash: sound + LED only
    closeRestartConfirm();
    alarmOverlay = lv_obj_create(lv_layer_top());
    lv_obj_remove_style_all(alarmOverlay);
    lv_obj_set_size(alarmOverlay, TZT_SCREEN_WIDTH, TZT_SCREEN_HEIGHT);
    lv_obj_set_style_bg_color(alarmOverlay, uiColor(UI_BACKGROUND), 0);
    lv_obj_set_style_bg_opa(alarmOverlay, LV_OPA_COVER, 0);
    lv_obj_add_flag(alarmOverlay, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_t* cap = makeLabel(alarmOverlay, "ALARM", &lv_font_montserrat_12, UI_ACCENT);
    lv_obj_set_style_text_letter_space(cap, 2, 0);
    lv_obj_align(cap, LV_ALIGN_TOP_MID, 0, 34);
    char hm[12] = "Alarm";
    time_t now = time(nullptr);
    struct tm t;
    localtime_r(&now, &t);
    if (t.tm_year > 100) strftime(hm, sizeof(hm), "%I:%M %p", &t);
    lv_obj_t* clock = makeLabel(alarmOverlay, hm[0] == '0' ? hm + 1 : hm, &lv_font_montserrat_32, UI_TEXT);
    lv_obj_align(clock, LV_ALIGN_TOP_MID, 0, 66);
    lv_obj_t* lbl = makeLabel(alarmOverlay, label.length() ? label.c_str() : "Alarm", &lv_font_montserrat_16, UI_TEXT_MUTED);
    lv_obj_set_width(lbl, TZT_SCREEN_WIDTH - 2 * UI_MARGIN);
    lv_obj_set_style_text_align(lbl, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_long_mode(lbl, LV_LABEL_LONG_DOT);
    lv_obj_align(lbl, LV_ALIGN_TOP_MID, 0, 112);
    lv_obj_t* stop = makeButton(alarmOverlay, "Stop", TZT_SCREEN_WIDTH - 2 * UI_MARGIN, 64, true, alarmStopCb);
    lv_obj_set_style_text_font(lv_obj_get_child(stop, 0), &lv_font_montserrat_18, 0);
    lv_obj_align(stop, LV_ALIGN_BOTTOM_MID, 0, -UI_MARGIN);
#endif
}

// Called every loop: due check every few seconds, and keep a ringing alarm going
static void alarmLoop() {
    static unsigned long lastCheck = 0;
    time_t now = time(nullptr);
    if (!alarmRinging && millis() - lastCheck >= 3000 && now > 1600000000) {  // clock synced (NTP)
        lastCheck = millis();
        String label;
        bool changed = false;
        if (alarms.checkDue(now, label, changed)) startAlarm(label);
        if (changed) saveAlarms();
    }
    if (!alarmRinging) return;
    if (millis() - alarmStartedAt > ALARM_RING_MS) { stopAlarm(); return; }
    if (!audioSvc.isPlaying()) audioSvc.playTune(TUNE_ALARM);
    if (!alarmLastLedAt || millis() - alarmLastLedAt >= ALARM_LED_EVERY_MS) {
        alarmLastLedAt = millis();
        enqueueSettingCommand(CMD_LED_NOTIFY, 4, 0);
    }
}

// "07:30" / "7:30" -> hour, minute
static bool parseAlarmTime(const String& hm, uint8_t& h, uint8_t& m) {
    int colon = hm.indexOf(':');
    if (colon < 1 || colon > 2 || hm.length() != (unsigned)(colon + 3)) return false;
    int hh = hm.substring(0, colon).toInt(), mm = hm.substring(colon + 1).toInt();
    if (hh < 0 || hh > 23 || mm < 0 || mm > 59) return false;
    h = (uint8_t)hh;
    m = (uint8_t)mm;
    return true;
}

// Shared by POST /api/alarms and the backend "set_alarm" command; returns the id or ""
static String addAlarmFrom(const String& hm, bool daily, const String& label) {
    uint8_t h, m;
    if (!parseAlarmTime(hm, h, m)) return "";
    String id = alarms.add(h, m, daily, label);
    if (id.length()) {
        saveAlarms();
        playChime(TUNE_ALARM_SET);
        Logger::info("Alarm", "Set " + hm + (daily ? " daily" : "") + (label.length() ? " - " + label : String("")));
    }
    return id;
}

// New-item alerts. Handling a batch of backend commands only records the newest arrival; the batch
// then presents it once: Main blinks the LED, the tile gets a NEW badge, and unless someone is using
// the screen right now, the item itself opens (message text, the picture, or Sounds with it on top).
static int pendingNewSection = -1;
static String pendingNewName;   // sound file name / picture path under IMAGES_DIR

static bool pendingNewPlay = false;  // a voice message: play it when it's presented

static void markNewItem(int section, const String& name, bool play = false) {
    pendingNewSection = section;
    pendingNewName = name;
    pendingNewPlay = play;
}

static void presentNewItem() {
    if (pendingNewSection < 0) return;
    int section = pendingNewSection;
    String name = pendingNewName;
    bool play = pendingNewPlay && section == SCREEN_SOUNDS && name.length() > 0 && !alarmRinging;
    wakeScreen();  // something new arrived: light up so it's noticed
    pendingNewSection = -1;
    pendingNewName = "";
    pendingNewPlay = false;
    enqueueSettingCommand(CMD_LED_NOTIFY, NEW_ITEM_LED_FLASHES, 0);
    // A voice message plays straight away (after the batch's downloads, so they can't stall it);
    // everything else gets its chime
    if (!play || !audioSvc.playFile(String(AUDIO_DIR) + "/" + name))
        playChime(section == SCREEN_MESSAGES ? TUNE_MESSAGE : section == SCREEN_SOUNDS ? TUNE_SOUND : TUNE_PICTURE);
    Logger::info("UI", "New " + String(section == SCREEN_MESSAGES ? "message" : section == SCREEN_SOUNDS ? "sound" : "picture")
                 + (name.length() ? ": " + name : String("")));
#if !defined(TZT_HEADLESS)
    if (section == SCREEN_SOUNDS) newSoundName = name;
    // Never switches screens: the NEW badges (Menu button + tile) say something arrived.
    // A section that's open right now just picks it up in place.
    setSectionNew(section, true);
    if (!launcherScreen) return;
    if (section == SCREEN_SOUNDS && lv_scr_act() == soundsScreen) refreshSounds(false);  // stays on their pick
    else if (section == SCREEN_MESSAGES && lv_scr_act() == messagesScreen && msgShown == 0) refreshMessages();
#endif
}

// Commands from the Pi arrive by long poll on a background task: the Pi holds the request until a
// command is queued, so a control sent from the web page lands in about a second, and the held
// request never freezes the screen. The task polls again only after loop() has handled the previous
// batch, and that poll carries the handled ids (?ack=) so the Pi deletes them first: a command can't
// be fetched twice, and loop() never blocks on a DELETE (each new TLS connection costs ~5 s).
#define COMMAND_POLL_WAIT_S   20     // the Pi holds an empty poll this long (nginx allows 60 s)
#define COMMAND_POLL_RETRY_MS 10000  // after a failed poll (Pi or WiFi down)
static DynamicJsonDocument polledCommands(2048);
static String polledBody;    // the poll's raw JSON: polledCommands points into it (zero-copy parse)
static bool polledOk = false;
static String polledAck;     // loop() -> task: ids handled from this batch, acked on the next poll
static String polledSkipId;  // a command the task couldn't read: loop() logs it, the next poll skips it
static String polledSkipWhy;
static SemaphoreHandle_t pollDone = nullptr;  // task -> loop(): polledCommands / polledOk are ready
static SemaphoreHandle_t pollNext = nullptr;  // loop() -> task: batch handled, poll again

static void commandPollTask(void*) {
    for (;;) {
        if (WiFi.status() != WL_CONNECTED) {
            vTaskDelay(pdMS_TO_TICKS(1000));
            continue;
        }
        // The Pi hands out one command per poll, with its id in X-Command-Id
        String commandId;
        pollStartedAt = millis();
        polledOk = pollBackend->pollCommandsRaw(polledBody, commandId);
        lastPollEndedAt = millis();
        pollStartedAt = 0;
        polledCommands.clear();
        polledSkipId = "";
        if (polledOk && polledBody.length() > 0 && polledBody != "null") {
            // Writable input = zero-copy parse: strings stay in polledBody, so even the longest
            // message (12.7 KB) fits this 2 KB document, with no second copy of it in RAM
            DeserializationError err = deserializeJson(polledCommands, polledBody.begin(), polledBody.length());
            if (err) {
                polledCommands.clear();
                polledSkipWhy = String("unreadable (") + err.c_str() + ", " + String(polledBody.length()) + " bytes)";
                if (commandId.length()) polledSkipId = commandId;  // skip it, so it can't jam the queue
                else polledOk = false;  // no id to skip by (older Pi): retry like a failed poll
            }
        } else if (polledOk && commandId.length()) {
            polledSkipId = commandId;  // a command came, but its body didn't fit in RAM
            polledSkipWhy = "too large to receive";
        }
        xSemaphoreGive(pollDone);
        xSemaphoreTake(pollNext, portMAX_DELAY);
        // The client keeps them until a poll succeeds (the server ignores ids it no longer has)
        if (polledAck.length()) {
            pollBackend->setPollAck(polledAck);
            polledAck = "";
        }
        if (polledSkipId.length()) pollBackend->setPollSkip(polledSkipId);
        if (!polledOk) vTaskDelay(pdMS_TO_TICKS(COMMAND_POLL_RETRY_MS));
    }
}

static void startCommandPolling() {
    pollBackend = new HttpBackendService(BACKEND_URL, BACKEND_TIMEOUT, BACKEND_API_KEY);
    pollBackend->setRetryPolicy(1, 0);  // the task's retry delay handles failures
    pollBackend->setDeviceIdentity("tzt-display", "TZT Display");  // Pi marks us seen on every poll
    pollBackend->setPollWait(COMMAND_POLL_WAIT_S);
    pollDone = xSemaphoreCreateBinary();
    pollNext = xSemaphoreCreateBinary();
    // Core 0 with the WiFi stack; LVGL and loop() stay on core 1. 10 KB covers the TLS handshake.
    xTaskCreatePinnedToCore(commandPollTask, "cmdPoll", 10240, nullptr, 1, nullptr, 0);
}

// Whether the print queue can take every print in the polled batch. Until it can, loop() leaves the
// batch unhandled (and so unacked, and the next poll waits), instead of dropping a message.
static bool polledPrintsFit() {
    size_t bytes = 0;
    uint8_t count = 0;
    for (JsonPair kv : polledCommands.as<JsonObject>()) {
        JsonObject c = kv.value();
        if (c["type"] == "print" && !(c["processed"] | false)) {
            bytes += strlen(c["data"] | "");
            count++;
        }
    }
    return count == 0 || printQueueHasRoom(bytes, count);
}

// Download any audio/image uploads the Pi still lists as not downloaded. Uploads normally arrive as
// download commands on the regular poll; this catches older ones and downloads that failed.
// Runs while loop() handles a polled batch, over the poll's connection (see pollBackend).
#define MEDIA_BACKLOG_SYNC_MS (30UL * 60UL * 1000UL)
#define MEDIA_SYNC_RETRY_MS   (2UL * 60UL * 1000UL)  // after a sync whose list request failed
// false = a list request failed (so retry soon rather than in 30 min)
static bool syncMediaBacklog() {
    struct { const char* kind; const char* dir; } media[] = { {"audio", AUDIO_DIR}, {"images", IMAGES_MEDIA_DIR} };
    bool ok = true;
    for (auto& m : media) {
        String resp;
        if (!pollBackend->get("/" + String(m.kind) + ".json?pending=1", resp)) {
            Logger::warn("Media", String("Sync: listing ") + m.kind + " failed: " + pollBackend->getLastError());
            ok = false;
            continue;
        }
        if (resp.length() <= 4 || resp == "null") continue;
        // In place (zero-copy): the document holds only pointers into resp, so 4 KB covers ~40 entries
        DynamicJsonDocument doc(4096);
        DeserializationError err = deserializeJson(doc, resp.begin(), resp.length());
        if (err) {
            Logger::warn("Media", String("Sync: ") + m.kind + " list unreadable (" + err.c_str() + ", " + String(resp.length()) + " bytes)");
            ok = false;
            continue;
        }
        for (JsonPair kv : doc.as<JsonObject>()) {
            JsonObject entry = kv.value();
            String url = entry["url"] | "";
            String name = entry["name"] | "";
            if (url.length() == 0 || name.length() == 0 || (entry["downloaded"] | false) || !isSafeFilename(name)) continue;
            if (sdCard.downloadToFile(url, String(m.dir) + "/" + name, fetchFromPi)) {
                markMediaDownloaded(m.kind, kv.key().c_str());
                bool audio = strcmp(m.kind, "audio") == 0;
                markNewItem(audio ? SCREEN_SOUNDS : SCREEN_PICTURES, audio ? name : String("media/") + name);
            }
            esp_task_wdt_reset();
        }
    }
    return ok;
}

void setup() {
    Serial.begin(115200);
    delay(1000);
    
    esp_task_wdt_init(60, true);   // 60s so setup (WiFi portal + backend + display) doesn't trigger
    esp_task_wdt_add(NULL);        // Current task (loop) is watched

    // SD card first (VSPI, independent of the display's HSPI): the startup image and the saved
    // reminders/groceries/todos below all read from it
    loadSensorPrefs();  // wanted IR/moisture sensor state, re-sent to Main whenever it differs
    loadDisplayPrefs(); // dim timeout
    if (sdCard.begin()) {
        sdCard.loadRecentMessages(MAX_CACHED_MESSAGES);
        String a;
        if (sdCard.readFile(ALARMS_FILE, a) && a.length()) alarms.fromJSON(a);
    } else {
        Logger::warn("SD", "Skipped: message history & media will use backend only");
    }
    audioSvc.begin();  // DAC output; chimes and alarms work without the SD card too

#if !defined(TZT_HEADLESS)
    // Display + startup image before WiFi, so the screen isn't black while WiFi connects
    tft.init();
    tft.setRotation(1);  // 2.4" 240x320 panel: rotation 1 = 320x240 landscape (matches TZT_SCREEN_*)
    tft.fillScreen(TFT_BLACK);
    // Backlight PWM for full brightness (digital HIGH can be dim on some boards)
    ledcSetup(TZT_TFT_BL_LEDC_CHANNEL, TZT_TFT_BL_LEDC_FREQ, TZT_TFT_BL_LEDC_RES);
    ledcAttachPin(TZT_TFT_BL_PIN, TZT_TFT_BL_LEDC_CHANNEL);
    ledcWrite(TZT_TFT_BL_LEDC_CHANNEL, TZT_TFT_BL_BRIGHTNESS);
    bool drewPhoto = drawSplashToTft();
    splashVisible = true;
    splashReadyAt = millis() + (drewPhoto ? SPLASH_PHOTO_MS : SPLASH_CARD_MS);
#endif

    // Initialize WiFi
    WiFiManager wifiManager;
    wifiManager.setDebugOutput(false);  // Suppress *wm: verbose logs
#if !defined(TZT_HEADLESS)
    wifiManager.setAPCallback([](WiFiManager*) { drawWifiSetupHelp(); });  // no saved WiFi: say how to set it up
#endif
    wifiManager.setAPStaticIPConfig(IPAddress(192, 168, 4, 1), 
                                     IPAddress(192, 168, 4, 1), 
                                     IPAddress(255, 255, 255, 0));
#if FORCE_WIFI_CONFIG_PORTAL
    Logger::info("WiFi", "FORCE_WIFI_CONFIG_PORTAL: erasing WiFi, opening config portal (AP: " TZT_AP_SSID ")");
    wifiManager.resetSettings();
    wifiManager.startConfigPortal(TZT_AP_SSID, TZT_AP_PASSWORD);
#else
    // The setup portal blocks this task for as long as someone is entering WiFi details, so unwatch it
    // (the 60 s WDT would reboot mid-setup); the portal timeout bounds the wait instead
    esp_task_wdt_delete(NULL);
    wifiManager.setConfigPortalTimeout(600);
    if (!wifiManager.autoConnect(TZT_AP_SSID, TZT_AP_PASSWORD)) {
        Logger::warn("WiFi", "config portal timed out or failed.");
    }
    esp_task_wdt_add(NULL);
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
    startCommandPolling();
    reminderService = new ReminderService(backend);
    // Load reminders from SD first (primary persistence)
    if (sdCard.isReady()) {
        String r;
        if (sdCard.readFile(DATA_DIR "/reminders.json", r) && r.length())
            reminderService->fromJSON(r);
    }
    loadGroceries();
    loadTodos();

    esp_task_wdt_reset();  // Feed WDT before display init
#if !defined(TZT_HEADLESS)
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
    {
        static unsigned long lastLoopAt = 0;
        unsigned long nowMs = millis();
        if (lastLoopAt && nowMs - lastLoopAt > maxLoopStallMs) maxLoopStallMs = nowMs - lastLoopAt;
        lastLoopAt = nowMs;
        // The poll task's timeouts should end every request; if one still hangs (a stalled TLS read
        // they miss), commands would silently stop while the screen looks fine. Restart instead.
        unsigned long started = pollStartedAt;
        // Signed: the task can stamp `started` just after nowMs was read (unsigned, that would wrap)
        if (started && (long)(nowMs - started) > (long)POLL_HANG_RESTART_MS) {
            Logger::error("Backend", "Command poll stuck for " + String((nowMs - started) / 1000) + " s, restarting");
            delay(200);  // let the log line go out
            ESP.restart();
        }
    }
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
            lv_scr_load(homeScreen);
            splashVisible = false;
            playChime(TUNE_STARTUP);
        }
        delay(2);
        return;
    }
    // Show test image if requested via /api/test/show-image
    if (pendingTestImagePath.length() > 0) {
        String path = pendingTestImagePath;
        pendingTestImagePath = "";
        openImageViewer(path);  // viewer redraw below keeps it on screen (LVGL repaints the black bg on load)
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

    // Handle a batch from the command poll task (it waits for pollNext before polling again)
    static unsigned long lastMediaSync = 0;
    static bool mediaSynced = false;
    static bool batchWaiting = false;  // a polled batch is in but waits for room in the print queue
    printQueueLoop();
    if (!batchWaiting && pollDone && xSemaphoreTake(pollDone, 0) == pdTRUE) {
        batchWaiting = true;
        if (!polledOk) Logger::warn("Backend", "Poll failed (check Pi URL/WiFi): " + pollBackend->getLastError());
        if (polledSkipId.length()) Logger::warn("Backend", "Skipping command " + polledSkipId + ": " + polledSkipWhy);
    }
    if (batchWaiting && (!polledOk || polledPrintsFit())) {
        batchWaiting = false;
        bool pollOk = polledOk;
        DynamicJsonDocument& cmds = polledCommands;
        // Backlog sync right after the first good poll, then every 30 min (piggybacks on a poll that worked).
        // Not while audio plays: the downloads block this loop, which also feeds the audio buffer.
        if (pollOk && sdCard.isReady() && !audioSvc.isPlaying() && (!mediaSynced || millis() - lastMediaSync >= MEDIA_BACKLOG_SYNC_MS)) {
            mediaSynced = true;
            lastMediaSync = millis();
            if (!syncMediaBacklog()) lastMediaSync -= MEDIA_BACKLOG_SYNC_MS - MEDIA_SYNC_RETRY_MS;
        }
        if (pollOk && cmds.size() > 0) {
            Logger::info("Backend", "Commands: " + String(cmds.size()));
            String ack;
            for (JsonPair kv : cmds.as<JsonObject>()) {
                JsonObject c = kv.value();
                if (ack.length()) ack += ",";
                ack += kv.key().c_str();  // acked even if skipped: a command left queued would end every long poll at once
                if (c["processed"] | false) continue;
                String typ = c["type"].as<String>(), data = c["data"].as<String>();
                // Commands: text → print + save to SD + display; audio → send/save + play; image → send/save + show
                if (typ == "print") {
                    Logger::info("Backend", "Print: \"" + data.substring(0, data.length() > 40 ? 40 : data.length()) + (data.length() > 40 ? "..." : "") + "\"");
                    String source = c["source"].as<String>();
                    // Full receipt (MESSAGE title, date, weather, levels) for everything from the Pi -
                    // Shortcuts don't always send "source". Opt out with "source":"plain".
                    bool messageOnly = source.equalsIgnoreCase("plain");
                    sendPrintChunked(data, messageOnly);
                    sdCard.saveMessage(data, source);
                    markNewItem(SCREEN_MESSAGES, "");
#if !defined(TZT_HEADLESS)
                    lastPrintMessage = data;
                    if (currentScreenIndex == SCREEN_MESSAGES) refreshMessages();
#endif
                }
                else if (typ == "download_audio") {
                    String url = c["url"] | "";
                    String name = c["name"] | "";
                    if (url.length() > 0 && name.length() > 0 && isSafeFilename(name) && sdCard.isReady()) {
                        String path = String(AUDIO_DIR) + "/" + name;
                        if (sdCard.downloadToFile(url, path, fetchFromPi)) {
                            markMediaDownloaded("audio", c["media_id"] | "");
                            // silent = just store it; otherwise it plays once the batch is done
                            markNewItem(SCREEN_SOUNDS, name, !(c["silent"] | false));
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
                        if (sdCard.downloadToFile(url, path, fetchFromPi)) {
                            markMediaDownloaded("images", c["media_id"] | "");
                            markNewItem(SCREEN_PICTURES, String("media/") + name);
#if !defined(TZT_HEADLESS)
                            // Not opened automatically: the NEW badge on Menu / Pictures shows it arrived
#endif
                        }
                    }
                }
                else if (typ == "set_alarm") {
                    // {"type":"set_alarm","time":"07:30","daily":true,"label":"Wake up"}
                    addAlarmFrom(c["time"] | "", c["daily"] | false, c["label"] | "");
                }
                else if (typ == "delete_alarm") {
                    if (alarms.remove(c["id"] | "")) saveAlarms();
                }
                else if (typ == "clear_alarms") {
                    alarms.clear();
                    saveAlarms();
                }
                else if (typ == "stop_alarm") {
                    stopAlarm();
                }
                else if (typ == "show_image" && data.length() > 0 && isSafeFilename(data)) {
#if !defined(TZT_HEADLESS)
                    String showPath = String(IMAGES_MEDIA_DIR) + "/" + data;
                    if (!SD.exists(showPath)) showPath = String(IMAGES_DIR) + "/" + data;
                    pendingTestImagePath = showPath;
#endif
                }
                esp_task_wdt_reset();  // a batch of downloads can take a while
            }
            polledAck = ack;
        }
        polledCommands.clear();
        polledBody = String();  // don't hold a long message in RAM through the next 20 s poll
        presentNewItem();  // newest arrival from this poll / backlog sync, if any
        xSemaphoreGive(pollNext);
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

    alarmLoop();
    audioSvc.loop();

#if !defined(TZT_HEADLESS)
    lv_timer_handler();
    // Draw the JPEG only after LVGL has painted over it (screen load, invalidation) or a new image was
    // picked - a static viewer costs nothing, instead of a full SD decode + push 5x per second.
    if (imageViewerNeedsDraw && imageViewerScreen && lv_scr_act() == imageViewerScreen && currentImagePath.length() > 0) {
        imageViewerNeedsDraw = false;
        displayFlushing = true;
        drawImageViewer();
        displayFlushing = false;
    }
    // Idle: untouched for 2 min anywhere but home -> back to home (not while a sound plays on Sounds)
    if (homeScreen && lv_scr_act() != homeScreen &&
        (unsigned long)(millis() - lastDashboardTouchTime) > DASHBOARD_IDLE_RETURN_MS &&
        !(lv_scr_act() == soundsScreen && audioSvc.isPlaying())) {
        lastDashboardTouchTime = millis();
        goToScreenIndex(SCREEN_HOME);
    }
    // Dim after a while untouched; never while an alarm rings, a sound plays or a restart is pending
    if (homeScreen && !screenDimmed) {
        if (alarmRinging || audioSvc.isPlaying() || restartConfirm || restartAt) {
            lastScreenActivity = millis();
        } else if (dimAfterMin && millis() - lastScreenActivity > dimAfterMin * 60000UL) {
            screenDimmed = true;
            setBacklight(DIM_BRIGHTNESS);
        }
    }
    if (toastHideAt && (long)(millis() - toastHideAt) >= 0) {
        toastHideAt = 0;
        if (toastLabel) lv_obj_add_flag(toastLabel, LV_OBJ_FLAG_HIDDEN);
    }
    // Dispense button rests for one dose; wake it when that's over
    if (dispenseLockUntil && !dispenseLocked()) updateHomeScreen();
    // Keep the Play/Stop button in sync when playback ends naturally
    if (soundsScreen && lv_scr_act() == soundsScreen) updateNowPlaying();
    // A touch while the new item is on screen means it's been seen: drop its launcher badge
    if (newItemShownSection >= 0 && lastUserTouchTime && (long)(lastUserTouchTime - newItemShownAt) > 0) {
        lv_obj_t* showing = (newItemShownSection == SCREEN_PICTURES) ? imageViewerScreen : screenByIndex(newItemShownSection);
        if (lv_scr_act() == showing) setSectionNew(newItemShownSection, false);
        newItemShownSection = -1;
    }
    // Settings > Restart: Main got its CMD_RESTART (queued with resends); now this board
    if (restartAt && (long)(millis() - restartAt) >= 0) {
        Logger::info("Boot", "Restart from Settings");
        delay(100);
        ESP.restart();
    }
#endif
    delay(0);  // Yield only — no fixed delay so LVGL gets more CPU for smoother GUI
}
