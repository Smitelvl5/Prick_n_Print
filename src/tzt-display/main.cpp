/*
 * TZT ESP32 LVGL Display - Web Server + Firebase + ESP-NOW
 * Hosts web UI, Firebase (commands, reminders, groceries), sends commands to Main via ESP-NOW.
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
#endif

#include "version.h"
#include "config_tzt.h"
#include "EspNowProtocol.h"
#include "FirebaseService.h"
#include "ReminderService.h"
#include "Logger.h"
#include <esp_now.h>
#include <esp_wifi.h>
#include <esp_task_wdt.h>
#include <string.h>

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

#if !defined(TZT_HEADLESS)
// Display and control panel (LVGL) - 3 screens with left/right nav
static TFT_eSPI tft;
// Display buffer: 320*20 = 6400 pixels * 2 bytes = 12.8KB per buffer. If memory constrained, use (TZT_SCREEN_WIDTH * 10).
#define TZT_DISP_BUF_PIXELS  (TZT_SCREEN_WIDTH * 20)
static lv_disp_draw_buf_t draw_buf;
static lv_color_t disp_buf1[TZT_DISP_BUF_PIXELS];
static lv_color_t disp_buf2[TZT_DISP_BUF_PIXELS];
static lv_obj_t* screen1 = nullptr;
static lv_obj_t* screen2 = nullptr;
static lv_obj_t* screen3 = nullptr;
static int currentScreenIndex = 0;
static lv_obj_t* labelMoisture = nullptr;
static lv_obj_t* labelSanitizer = nullptr;
static lv_obj_t* labelLED = nullptr;
static lv_obj_t* labelPump = nullptr;
static lv_obj_t* labelPumpDuration = nullptr;
static lv_obj_t* labelPumpCooldown = nullptr;
static lv_obj_t* slDuration = nullptr;
static lv_obj_t* slCooldown = nullptr;
static lv_obj_t* btnStart = nullptr;
static lv_obj_t* btnStop = nullptr;
static lv_obj_t* swDispense = nullptr;
static lv_obj_t* slLed = nullptr;
static lv_obj_t* btnTurnOff = nullptr;
static lv_obj_t* btnTurnOn = nullptr;
static lv_obj_t* swBright = nullptr;
static unsigned long lastPanelUpdate = 0;
static uint32_t lastSensorHash = 0;  // Only redraw panel when sensor data changes
static void createControlPanel();
static void updateControlPanelStatus();
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

// Cached sensor data from Main (received via ESP-NOW)
SensorDataPacket lastSensorData;
bool lastSensorDataValid = false;
unsigned long lastSensorDataReceiveTime = 0;  // Track when we last received data from Main
// Optimistic cache for settings not in sensor packet (for HTTP GET /api/status)
static int lastPumpDurationTenths = -1;   // 0-255 = 0-25.5s, -1 = not set
static int lastPumpCooldownTenths = -1;    // 0-255 = 0-25.5s, -1 = not set

// Settings persisted to Firebase config (loaded on boot, saved when changed)
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
#define PRINT_CHUNK_SIZE 199  // For CHUNK_ACK handler and sendPrintChunked
#define CHUNK_ACK_TIMEOUT_MS 15000
#define CHUNK_RESEND_INTERVAL_MS 2000  // Resend current chunk every 2s until ACK or timeout
bool sendCommandViaESPNow(uint8_t commandType, uint8_t param1 = 0, uint8_t param2 = 0, const char* message = "", bool isRetry = false, bool fromSettingsQueue = false);

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

// Firebase + Reminders
FirebaseService* firebase = nullptr;
ReminderService* reminderService = nullptr;
bool remindersNeedSave = false;  // Flag to save reminders in background

// Groceries (Firebase)
#define MAX_GROCERY_ITEMS 50
String groceryItems[MAX_GROCERY_ITEMS];
int groceryCount = 0;
bool groceriesNeedSave = false;  // Flag to save groceries in background
static unsigned long lastGrocerySaveTime = 0;
static const unsigned long SAVE_DEBOUNCE_MS = 5000;  // Max one save per 5s

// Todo list (Firebase)
#define MAX_TODO_ITEMS 50
String todoItems[MAX_TODO_ITEMS];
int todoCount = 0;
bool todosNeedSave = false;  // Flag to save todos in background

String deviceIP = "";
String webPassword = MAIN_MODULE_API_PASSWORD;
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
            chunkResendAt = millis() + 600;
            if (millis() - lastFailLog > 15000) {
                Serial.println("[ESP-NOW] send failed, resending chunk in 600ms");
            }
        } else if (pendingSettingRemainingSends == 0) {
            // Don't set retryPending when a setting is in flight — settings queue resends on ACK timeout
            bool isPumpCmd = (lastSentCommandType == CMD_DISPENSE_START || lastSentCommandType == CMD_DISPENSE_STOP);
            int maxRetries = isPumpCmd ? 2 : 1;  // Pump: up to 2 retries (3 attempts total)
            if (retryCount < maxRetries) {
                retryPending = true;
                retryAt = millis() + 600;
                retryCount++;
                if (millis() - lastFailLog > 15000) {
                    Serial.println("[ESP-NOW] send failed, retrying in 600ms");
                }
            }
        }
        if (millis() - lastFailLog > 10000) {
            lastFailLog = millis();
            Serial.println("[ESP-NOW] send failed (status=" + String((int)status) + ")");
            if (!esp_now_is_peer_exist(mac_addr)) {
                Serial.println("   Peer not in list, re-adding...");
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
            Serial.println("[TZT] Rcvd: sensor");
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
                    // If we're applying boot settings from Firebase, enqueue next
                    applyNextBootSetting();
                }
            }
        }
    } else if (len >= (int)sizeof(ChunkAckPacket) && data[0] == ESP_NOW_MSG_CHUNK_ACK) {
        ChunkAckPacket* cap = (ChunkAckPacket*)data;
        if (verifyChecksum((uint8_t*)cap, sizeof(ChunkAckPacket)) && pendingChunkTotal > 0 && cap->chunkIndex == pendingChunkIndex) {
            pendingChunkIndex++;
            if (pendingChunkIndex < pendingChunkTotal) {
                nextChunkSendAt = millis() + 80;  // Send next chunk from loop (not here) so we never block callback
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
// LVGL display flush callback: send buffer to TFT
static void lvgl_flush_cb(lv_disp_drv_t* drv, const lv_area_t* area, lv_color_t* color_map) {
    uint32_t w = (area->x2 - area->x1 + 1);
    uint32_t h = (area->y2 - area->y1 + 1);
    tft.startWrite();
    tft.setAddrWindow(area->x1, area->y1, w, h);
    tft.pushPixels((uint16_t*)color_map, w * h);
    tft.endWrite();
    lv_disp_flush_ready(drv);
}

// Nav: user_data = 0 prev, 1 next
static void navBtnCb(lv_event_t* e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    int dir = (int)(uintptr_t)lv_event_get_user_data(e);
    currentScreenIndex = (currentScreenIndex + (dir ? 1 : -1) + 3) % 3;
    lv_obj_t* scr = (currentScreenIndex == 0) ? screen1 : (currentScreenIndex == 1) ? screen2 : screen3;
    lv_scr_load(scr);
}

// Command button: user_data = (void*)(uintptr_t)command_type
static void cmdBtnCb(lv_event_t* e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    uintptr_t cmd = (uintptr_t)lv_event_get_user_data(e);
    if (cmd == CMD_DISPENSE_START) {
        lastSensorData.isDispensing = true;
        lastSensorDataValid = true;
        updateControlPanelStatus();
        sendPumpCommandRepeated(CMD_DISPENSE_START);
    } else if (cmd == CMD_DISPENSE_STOP) {
        lastSensorData.isDispensing = false;
        lastSensorDataValid = true;
        updateControlPanelStatus();
        sendPumpCommandRepeated(CMD_DISPENSE_STOP);
    } else if (cmd == CMD_RESET_SANITIZER) sendCommandViaESPNow(CMD_RESET_SANITIZER);
    else if (cmd == CMD_TEST_PRINTER) sendCommandViaESPNow(CMD_TEST_PRINTER);
}

// LED slider (screen 3): optimistic UI, then enqueue for reliable send
static void ledSliderCb(lv_event_t* e) {
    if (lv_event_get_code(e) != LV_EVENT_VALUE_CHANGED) return;
    lv_obj_t* slider = lv_event_get_target(e);
    int val = lv_slider_get_value(slider);
    lastSensorData.ledBrightness = (uint8_t)val;
    lastSensorDataValid = true;
    settingsNeedSave = true;
    updateControlPanelStatus();
    enqueueSettingCommand(CMD_SET_LED_BRIGHTNESS, (uint8_t)val, val > 0 ? 1 : 0, "");
}

// Pump duration slider: value 0-255 = 0-25.5s (x100ms), optimistic label + enqueue on release
static void pumpDurationCb(lv_event_t* e) {
    if (lv_event_get_code(e) != LV_EVENT_VALUE_CHANGED && lv_event_get_code(e) != LV_EVENT_RELEASED) return;
    lv_obj_t* slider = lv_event_get_target(e);
    int val = lv_slider_get_value(slider);
    if (labelPumpDuration) lv_label_set_text_fmt(labelPumpDuration, "%d.%ds", val / 10, val % 10);
    if (lv_event_get_code(e) == LV_EVENT_RELEASED) {
        lastPumpDurationTenths = val;
        settingsNeedSave = true;
        enqueueSettingCommand(CMD_SET_PUMP_DURATION, (uint8_t)val, 0, "");
    }
}

// Pump cooldown slider: value 0-255 = 0-25.5s, optimistic label + enqueue on release
static void pumpCooldownCb(lv_event_t* e) {
    if (lv_event_get_code(e) != LV_EVENT_VALUE_CHANGED && lv_event_get_code(e) != LV_EVENT_RELEASED) return;
    lv_obj_t* slider = lv_event_get_target(e);
    int val = lv_slider_get_value(slider);
    if (labelPumpCooldown) lv_label_set_text_fmt(labelPumpCooldown, "%d.%ds", val / 10, val % 10);
    if (lv_event_get_code(e) == LV_EVENT_RELEASED) {
        lastPumpCooldownTenths = val;
        settingsNeedSave = true;
        enqueueSettingCommand(CMD_SET_PUMP_COOLDOWN, (uint8_t)val, 0, "");
    }
}

// Auto dispense toggle (screen 2): optimistic UI, then enqueue
static void autoDispenseCb(lv_event_t* e) {
    if (lv_event_get_code(e) != LV_EVENT_VALUE_CHANGED) return;
    lv_obj_t* sw = lv_event_get_target(e);
    bool on = lv_obj_has_state(sw, LV_STATE_CHECKED);
    lastSensorData.autoDispense = on;
    lastSensorDataValid = true;
    settingsNeedSave = true;
    updateControlPanelStatus();
    enqueueSettingCommand(CMD_SET_AUTO_DISPENSE, on ? 1 : 0, 0, "");
}

// Calibrate buttons (screen 3): enqueue for reliable send (no UI state to update)
static void calibrateBtnCb(lv_event_t* e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    uintptr_t isMax = (uintptr_t)lv_event_get_user_data(e);
    enqueueSettingCommand(CMD_SET_LIGHT_CALIBRATION, (uint8_t)(isMax ? 1 : 0), 0, "");
}

// Auto brightness toggle (screen 3): optimistic UI, then enqueue
static void autoBrightnessCb(lv_event_t* e) {
    if (lv_event_get_code(e) != LV_EVENT_VALUE_CHANGED) return;
    lv_obj_t* sw = lv_event_get_target(e);
    bool on = lv_obj_has_state(sw, LV_STATE_CHECKED);
    lastSensorData.autoBrightness = on;
    lastSensorDataValid = true;
    settingsNeedSave = true;
    updateControlPanelStatus();
    enqueueSettingCommand(CMD_SET_AUTO_BRIGHTNESS, on ? 1 : 0, 0, "");
}

static void setScreenStyle(lv_obj_t* scr) {
    lv_obj_set_style_bg_color(scr, lv_color_hex(0x1a3d0e), 0);
    lv_obj_set_style_bg_grad_color(scr, lv_color_hex(0x4a7c2a), 0);
    lv_obj_set_style_bg_grad_dir(scr, LV_GRAD_DIR_VER, 0);
}

// Add nav buttons at bottom of a screen (compact)
static void addNavButtons(lv_obj_t* scr) {
    int btnW = 80;
    int btnH = 26;
    int y = TZT_SCREEN_HEIGHT - btnH - 4;
    lv_obj_t* left = lv_btn_create(scr);
    lv_obj_set_size(left, btnW, btnH);
    lv_obj_set_pos(left, 4, y);
    lv_obj_set_style_bg_color(left, lv_color_hex(0x2d5016), 0);
    lv_obj_t* lbl = lv_label_create(left);
    lv_label_set_text(lbl, LV_SYMBOL_LEFT " Prev");
    lv_obj_set_style_text_font(lbl, &lv_font_montserrat_12, 0);
    lv_obj_center(lbl);
    lv_obj_add_event_cb(left, navBtnCb, LV_EVENT_CLICKED, (void*)0);
    lv_obj_t* right = lv_btn_create(scr);
    lv_obj_set_size(right, btnW, btnH);
    lv_obj_set_pos(right, TZT_SCREEN_WIDTH - 4 - btnW, y);
    lv_obj_set_style_bg_color(right, lv_color_hex(0x2d5016), 0);
    lbl = lv_label_create(right);
    lv_label_set_text(lbl, "Next " LV_SYMBOL_RIGHT);
    lv_obj_set_style_text_font(lbl, &lv_font_montserrat_12, 0);
    lv_obj_center(lbl);
    lv_obj_add_event_cb(right, navBtnCb, LV_EVENT_CLICKED, (void*)1);
}

static void createControlPanel() {
    // Screen 1: 4 boxes (Moisture, Sanitizer, LED, Pump) + nav - tight layout, big value font
    screen1 = lv_obj_create(NULL);
    setScreenStyle(screen1);
    int pad = 4;
    int gap = 4;
    int boxW = (TZT_SCREEN_WIDTH - pad * 2 - gap) / 2;
    int boxH = (TZT_SCREEN_HEIGHT - pad * 2 - 26 - 4) / 2 - gap / 2;  // nav 26 + 4
    int y0 = pad;
    auto addBox = [&](int col, int row, const char* name, lv_obj_t*& valueLabel) {
        int x = pad + col * (boxW + gap);
        int y = y0 + row * (boxH + gap);
        lv_obj_t* box = lv_obj_create(screen1);
        lv_obj_set_size(box, boxW, boxH);
        lv_obj_set_pos(box, x, y);
        lv_obj_set_style_bg_color(box, lv_color_hex(0x2d5016), 0);
        lv_obj_set_style_border_color(box, lv_color_hex(0x4a7c2a), 0);
        lv_obj_set_style_pad_all(box, 2, 0);
        lv_obj_t* title = lv_label_create(box);
        lv_label_set_text(title, name);
        lv_obj_set_style_text_color(title, lv_color_hex(0xaaaaaa), 0);
        lv_obj_set_style_text_font(title, &lv_font_montserrat_12, 0);
        lv_obj_align(title, LV_ALIGN_TOP_LEFT, 2, 0);
        lv_obj_t* val = lv_label_create(box);
        lv_label_set_text(val, "--");
        lv_obj_set_style_text_color(val, lv_color_hex(0xffffff), 0);
        lv_obj_set_style_text_font(val, &lv_font_montserrat_16, 0);
        lv_obj_align(val, LV_ALIGN_BOTTOM_LEFT, 2, -1);
        valueLabel = val;
    };
    addBox(0, 0, "Moisture", labelMoisture);
    addBox(1, 0, "Sanitizer", labelSanitizer);
    addBox(0, 1, "LED", labelLED);
    addBox(1, 1, "Pump", labelPump);
    addNavButtons(screen1);

    // Screen 2: Pump - tight layout, big font
    screen2 = lv_obj_create(NULL);
    setScreenStyle(screen2);
    int sy = 2;
    lv_obj_t* h2 = lv_label_create(screen2);
    lv_label_set_text(h2, "Pump");
    lv_obj_set_style_text_color(h2, lv_color_hex(0xffffff), 0);
    lv_obj_set_style_text_font(h2, &lv_font_montserrat_16, 0);
    lv_obj_set_pos(h2, 4, sy);
    sy += 14;
    lv_obj_t* lDur = lv_label_create(screen2);
    lv_label_set_text(lDur, "Duration:");
    lv_obj_set_style_text_color(lDur, lv_color_hex(0xe0e0e0), 0);
    lv_obj_set_style_text_font(lDur, &lv_font_montserrat_12, 0);
    lv_obj_set_pos(lDur, 4, sy);
    labelPumpDuration = lv_label_create(screen2);
    lv_label_set_text(labelPumpDuration, "2.0s");
    lv_obj_set_style_text_color(labelPumpDuration, lv_color_hex(0xffffff), 0);
    lv_obj_set_style_text_font(labelPumpDuration, &lv_font_montserrat_14, 0);
    lv_obj_set_pos(labelPumpDuration, TZT_SCREEN_WIDTH - 52, sy);
    sy += 12;
    slDuration = lv_slider_create(screen2);
    lv_obj_set_size(slDuration, TZT_SCREEN_WIDTH - 16, 12);
    lv_obj_set_pos(slDuration, 4, sy);
    lv_slider_set_range(slDuration, 5, 255);
    lv_slider_set_value(slDuration, 20, LV_ANIM_OFF);
    lv_obj_set_style_bg_color(slDuration, lv_color_hex(0x2d5016), LV_PART_MAIN);
    lv_obj_set_style_bg_color(slDuration, lv_color_hex(0x6b9f3d), LV_PART_INDICATOR);
    lv_obj_set_style_opa(slDuration, LV_OPA_50, LV_PART_MAIN | LV_STATE_DISABLED);
    lv_obj_set_style_opa(slDuration, LV_OPA_50, LV_PART_INDICATOR | LV_STATE_DISABLED);
    lv_obj_add_event_cb(slDuration, pumpDurationCb, LV_EVENT_VALUE_CHANGED, nullptr);
    lv_obj_add_event_cb(slDuration, pumpDurationCb, LV_EVENT_RELEASED, nullptr);
    sy += 14;
    lv_obj_t* lCo = lv_label_create(screen2);
    lv_label_set_text(lCo, "Cooldown:");
    lv_obj_set_style_text_color(lCo, lv_color_hex(0xe0e0e0), 0);
    lv_obj_set_style_text_font(lCo, &lv_font_montserrat_12, 0);
    lv_obj_set_pos(lCo, 4, sy);
    labelPumpCooldown = lv_label_create(screen2);
    lv_label_set_text(labelPumpCooldown, "3.0s");
    lv_obj_set_style_text_color(labelPumpCooldown, lv_color_hex(0xffffff), 0);
    lv_obj_set_style_text_font(labelPumpCooldown, &lv_font_montserrat_14, 0);
    lv_obj_set_pos(labelPumpCooldown, TZT_SCREEN_WIDTH - 52, sy);
    sy += 12;
    slCooldown = lv_slider_create(screen2);
    lv_obj_set_size(slCooldown, TZT_SCREEN_WIDTH - 16, 12);
    lv_obj_set_pos(slCooldown, 4, sy);
    lv_slider_set_range(slCooldown, 10, 255);
    lv_slider_set_value(slCooldown, 30, LV_ANIM_OFF);
    lv_obj_set_style_bg_color(slCooldown, lv_color_hex(0x2d5016), LV_PART_MAIN);
    lv_obj_set_style_bg_color(slCooldown, lv_color_hex(0x6b9f3d), LV_PART_INDICATOR);
    lv_obj_set_style_opa(slCooldown, LV_OPA_50, LV_PART_MAIN | LV_STATE_DISABLED);
    lv_obj_set_style_opa(slCooldown, LV_OPA_50, LV_PART_INDICATOR | LV_STATE_DISABLED);
    lv_obj_add_event_cb(slCooldown, pumpCooldownCb, LV_EVENT_VALUE_CHANGED, nullptr);
    lv_obj_add_event_cb(slCooldown, pumpCooldownCb, LV_EVENT_RELEASED, nullptr);
    sy += 16;
    btnStart = lv_btn_create(screen2);
    lv_obj_set_size(btnStart, 76, 26);
    lv_obj_set_pos(btnStart, 4, sy);
    lv_obj_set_style_bg_color(btnStart, lv_color_hex(0x4a7c2a), 0);
    lv_obj_set_style_opa(btnStart, LV_OPA_50, LV_PART_MAIN | LV_STATE_DISABLED);
    lv_obj_t* lbl2 = lv_label_create(btnStart);
    lv_label_set_text(lbl2, "Start");
    lv_obj_set_style_text_font(lbl2, &lv_font_montserrat_14, 0);
    lv_obj_center(lbl2);
    lv_obj_add_event_cb(btnStart, cmdBtnCb, LV_EVENT_CLICKED, (void*)(uintptr_t)CMD_DISPENSE_START);
    btnStop = lv_btn_create(screen2);
    lv_obj_set_size(btnStop, 76, 26);
    lv_obj_set_pos(btnStop, 84, sy);
    lv_obj_set_style_bg_color(btnStop, lv_color_hex(0x8b4513), 0);
    lv_obj_set_style_opa(btnStop, LV_OPA_50, LV_PART_MAIN | LV_STATE_DISABLED);
    lv_obj_t* lbl3 = lv_label_create(btnStop);
    lv_label_set_text(lbl3, "Stop");
    lv_obj_set_style_text_font(lbl3, &lv_font_montserrat_14, 0);
    lv_obj_center(lbl3);
    lv_obj_add_event_cb(btnStop, cmdBtnCb, LV_EVENT_CLICKED, (void*)(uintptr_t)CMD_DISPENSE_STOP);
    sy += 28;
    lv_obj_t* swLabel = lv_label_create(screen2);
    lv_label_set_text(swLabel, "Sensor");
    lv_obj_set_style_text_color(swLabel, lv_color_hex(0xe0e0e0), 0);
    lv_obj_set_style_text_font(swLabel, &lv_font_montserrat_12, 0);
    lv_obj_set_pos(swLabel, 4, sy);
    swDispense = lv_switch_create(screen2);
    lv_obj_set_size(swDispense, 40, 20);
    lv_obj_set_pos(swDispense, TZT_SCREEN_WIDTH - 46, sy - 1);
    lv_obj_add_event_cb(swDispense, autoDispenseCb, LV_EVENT_VALUE_CHANGED, nullptr);
    addNavButtons(screen2);

    // Screen 3: LED - tight layout, big font
    screen3 = lv_obj_create(NULL);
    setScreenStyle(screen3);
    sy = 2;
    lv_obj_t* h3 = lv_label_create(screen3);
    lv_label_set_text(h3, "LED");
    lv_obj_set_style_text_color(h3, lv_color_hex(0xffffff), 0);
    lv_obj_set_style_text_font(h3, &lv_font_montserrat_16, 0);
    lv_obj_set_pos(h3, 4, sy);
    sy += 14;
    slLed = lv_slider_create(screen3);
    lv_obj_set_size(slLed, TZT_SCREEN_WIDTH - 16, 14);
    lv_obj_set_pos(slLed, 4, sy);
    lv_slider_set_range(slLed, 0, 255);
    lv_slider_set_value(slLed, 0, LV_ANIM_OFF);
    lv_obj_set_style_bg_color(slLed, lv_color_hex(0x2d5016), LV_PART_MAIN);
    lv_obj_set_style_bg_color(slLed, lv_color_hex(0x6b9f3d), LV_PART_INDICATOR);
    lv_obj_set_style_opa(slLed, LV_OPA_50, LV_PART_MAIN | LV_STATE_DISABLED);
    lv_obj_set_style_opa(slLed, LV_OPA_50, LV_PART_INDICATOR | LV_STATE_DISABLED);
    lv_obj_add_event_cb(slLed, ledSliderCb, LV_EVENT_VALUE_CHANGED, nullptr);
    sy += 18;
    lv_obj_t* calLabel = lv_label_create(screen3);
    lv_label_set_text(calLabel, "Calibrate");
    lv_obj_set_style_text_color(calLabel, lv_color_hex(0xe0e0e0), 0);
    lv_obj_set_style_text_font(calLabel, &lv_font_montserrat_12, 0);
    lv_obj_set_pos(calLabel, 4, sy);
    sy += 14;
    btnTurnOff = lv_btn_create(screen3);
    lv_obj_set_size(btnTurnOff, 100, 26);
    lv_obj_set_pos(btnTurnOff, 4, sy);
    lv_obj_set_style_bg_color(btnTurnOff, lv_color_hex(0x2d5016), 0);
    lv_obj_set_style_opa(btnTurnOff, LV_OPA_50, LV_PART_MAIN | LV_STATE_DISABLED);
    lv_obj_t* lblOff = lv_label_create(btnTurnOff);
    lv_label_set_text(lblOff, "Turn off");
    lv_obj_set_style_text_font(lblOff, &lv_font_montserrat_14, 0);
    lv_obj_center(lblOff);
    lv_obj_add_event_cb(btnTurnOff, calibrateBtnCb, LV_EVENT_CLICKED, (void*)0);
    btnTurnOn = lv_btn_create(screen3);
    lv_obj_set_size(btnTurnOn, 100, 26);
    lv_obj_set_pos(btnTurnOn, 112, sy);
    lv_obj_set_style_bg_color(btnTurnOn, lv_color_hex(0x4a7c2a), 0);
    lv_obj_set_style_opa(btnTurnOn, LV_OPA_50, LV_PART_MAIN | LV_STATE_DISABLED);
    lv_obj_t* lblOn = lv_label_create(btnTurnOn);
    lv_label_set_text(lblOn, "Turn on");
    lv_obj_set_style_text_font(lblOn, &lv_font_montserrat_14, 0);
    lv_obj_center(lblOn);
    lv_obj_add_event_cb(btnTurnOn, calibrateBtnCb, LV_EVENT_CLICKED, (void*)1);
    sy += 28;
    lv_obj_t* swLedLabel = lv_label_create(screen3);
    lv_label_set_text(swLedLabel, "Sensor");
    lv_obj_set_style_text_color(swLedLabel, lv_color_hex(0xe0e0e0), 0);
    lv_obj_set_style_text_font(swLedLabel, &lv_font_montserrat_12, 0);
    lv_obj_set_pos(swLedLabel, 4, sy);
    swBright = lv_switch_create(screen3);
    lv_obj_set_size(swBright, 40, 20);
    lv_obj_set_pos(swBright, TZT_SCREEN_WIDTH - 46, sy - 1);
    lv_obj_add_event_cb(swBright, autoBrightnessCb, LV_EVENT_VALUE_CHANGED, nullptr);
    addNavButtons(screen3);

    currentScreenIndex = 0;
    lv_scr_load(screen1);
}

static void updateControlPanelStatus() {
    if (!labelMoisture) return;
    if (!lastSensorDataValid) {
        lv_label_set_text(labelMoisture, "--");
        lv_label_set_text(labelSanitizer, "--");
        lv_label_set_text(labelLED, "--");
        lv_label_set_text(labelPump, "--");
        return;
    }
    lv_label_set_text_fmt(labelMoisture, "%.1f%%", (double)lastSensorData.moisturePercent);
    lv_label_set_text_fmt(labelSanitizer, "%.1f%%", (double)lastSensorData.sanitizerLevel);
    lv_label_set_text_fmt(labelLED, "%d%%", (lastSensorData.ledBrightness * 100) / 255);
    lv_label_set_text_fmt(labelPump, "%s", lastSensorData.isDispensing ? "ON" : "OFF");
    // Sync sensor toggles from Main (TZT and HTTP stay in sync)
    if (swDispense) {
        if (lastSensorData.autoDispense) lv_obj_add_state(swDispense, LV_STATE_CHECKED);
        else lv_obj_clear_state(swDispense, LV_STATE_CHECKED);
    }
    if (swBright) {
        if (lastSensorData.autoBrightness) lv_obj_add_state(swBright, LV_STATE_CHECKED);
        else lv_obj_clear_state(swBright, LV_STATE_CHECKED);
    }
    // Auto-dispense: when ON, pump sliders usable (control what auto uses); on/off disabled. When OFF, sliders disabled; on/off enabled.
    bool pumpButtonsDisabled = lastSensorData.autoDispense;
    bool pumpSlidersDisabled = !lastSensorData.autoDispense;
    if (slDuration) { if (pumpSlidersDisabled) lv_obj_add_flag(slDuration, LV_OBJ_FLAG_DISABLED); else lv_obj_clear_flag(slDuration, LV_OBJ_FLAG_DISABLED); }
    if (slCooldown) { if (pumpSlidersDisabled) lv_obj_add_flag(slCooldown, LV_OBJ_FLAG_DISABLED); else lv_obj_clear_flag(slCooldown, LV_OBJ_FLAG_DISABLED); }
    if (btnStart) { if (pumpButtonsDisabled) lv_obj_add_flag(btnStart, LV_OBJ_FLAG_DISABLED); else lv_obj_clear_flag(btnStart, LV_OBJ_FLAG_DISABLED); }
    if (btnStop) { if (pumpButtonsDisabled) lv_obj_add_flag(btnStop, LV_OBJ_FLAG_DISABLED); else lv_obj_clear_flag(btnStop, LV_OBJ_FLAG_DISABLED); }
    // Auto-brightness: when ON, only min/max (calibrate) buttons work; LED slider disabled. When OFF, slider works; min/max disabled.
    bool ledSliderDisabled = lastSensorData.autoBrightness;
    bool calButtonsEnabled = lastSensorData.autoBrightness;
    if (slLed) { if (ledSliderDisabled) lv_obj_add_flag(slLed, LV_OBJ_FLAG_DISABLED); else lv_obj_clear_flag(slLed, LV_OBJ_FLAG_DISABLED); }
    if (btnTurnOff) { if (!calButtonsEnabled) lv_obj_add_flag(btnTurnOff, LV_OBJ_FLAG_DISABLED); else lv_obj_clear_flag(btnTurnOff, LV_OBJ_FLAG_DISABLED); }
    if (btnTurnOn) { if (!calButtonsEnabled) lv_obj_add_flag(btnTurnOn, LV_OBJ_FLAG_DISABLED); else lv_obj_clear_flag(btnTurnOn, LV_OBJ_FLAG_DISABLED); }
}
#endif

void loadGroceries() {
    String response;
    if (!firebase || !firebase->get("/groceries.json", response)) { groceryCount = 0; return; }
    if (response == "null" || response.length() == 0 || response == "{}") { groceryCount = 0; return; }
    size_t capacity = response.length() + 200;
    DynamicJsonDocument doc(capacity);
    if (deserializeJson(doc, response)) { groceryCount = 0; return; }
    JsonArray arr = doc.as<JsonArray>();
    groceryCount = 0;
    for (size_t i = 0; i < arr.size() && groceryCount < MAX_GROCERY_ITEMS; i++)
        groceryItems[groceryCount++] = arr[i].as<String>();
}

void saveGroceries() {
    if (!firebase) return;
    DynamicJsonDocument doc(4096);
    JsonArray arr = doc.to<JsonArray>();
    for (int i = 0; i < groceryCount; i++) arr.add(groceryItems[i]);
    String json; serializeJson(doc, json);
    firebase->put("/groceries.json", json);
}

void loadSettingsFromFirebase() {
    if (!firebase) return;
    DynamicJsonDocument doc(512);
    if (!firebase->loadConfig(doc)) return;
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

void saveSettingsToFirebase() {
    if (!firebase) return;
    DynamicJsonDocument doc(512);
    if (firebase->loadConfig(doc)) { /* merge into existing */ }
    doc["ledBrightness"] = lastSensorDataValid ? (int)lastSensorData.ledBrightness : 0;
    doc["autoDispense"] = lastSensorDataValid ? lastSensorData.autoDispense : false;
    doc["autoBrightness"] = lastSensorDataValid ? lastSensorData.autoBrightness : false;
    if (lastPumpDurationTenths >= 0) doc["pumpDurationTenths"] = lastPumpDurationTenths;
    if (lastPumpCooldownTenths >= 0) doc["pumpCooldownTenths"] = lastPumpCooldownTenths;
    firebase->saveConfig(doc);
}

void loadTodos() {
    String response;
    if (!firebase || !firebase->get("/todos.json", response)) { todoCount = 0; return; }
    if (response == "null" || response.length() == 0 || response == "{}") { todoCount = 0; return; }
    size_t capacity = response.length() + 200;
    DynamicJsonDocument doc(capacity);
    if (deserializeJson(doc, response)) { todoCount = 0; return; }
    JsonArray arr = doc.as<JsonArray>();
    todoCount = 0;
    for (size_t i = 0; i < arr.size() && todoCount < MAX_TODO_ITEMS; i++)
        todoItems[todoCount++] = arr[i].as<String>();
}

void saveTodos() {
    if (!firebase) return;
    DynamicJsonDocument doc(4096);
    JsonArray arr = doc.to<JsonArray>();
    for (int i = 0; i < todoCount; i++) arr.add(todoItems[i]);
    String json; serializeJson(doc, json);
    firebase->put("/todos.json", json);
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
<html>
<head>
    <meta charset="UTF-8">
    <meta name="viewport" content="width=device-width, initial-scale=1.0, maximum-scale=1.0, user-scalable=no">
    <title>Login - Print-n-Prick</title>
    <style>
        * { margin: 0; padding: 0; box-sizing: border-box; -webkit-tap-highlight-color: rgba(74, 124, 42, 0.2); }
        body {
            font-family: -apple-system, BlinkMacSystemFont, 'Segoe UI', Roboto, sans-serif;
            background: linear-gradient(135deg, #1a120c 0%, #1a3d0e 25%, #2d5016 50%, #4a7c2a 75%, #6b9f3d 100%);
            min-height: 100vh;
            margin: 0;
            padding: 12px;
            color: #333;
            position: relative;
            -webkit-font-smoothing: antialiased;
            -moz-osx-font-smoothing: grayscale;
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
            padding: 18px;
            font-size: 17px;
            border: none;
            border-radius: 12px;
            background: linear-gradient(135deg, #2c1810 0%, #2d5016 50%, #4a7c2a 100%);
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

void handleFavicon() {
    server.send(204);
}

void handleRoot() {
    if (!isAuthenticated()) { server.sendHeader("Location", "/login"); server.send(302, "text/plain", ""); return; }
    // Optional: if client sends Accept-Encoding: gzip, serve pre-compressed .gz from SPIFFS/LittleFS for large HTML
    const char* html = R"HTML(
<!DOCTYPE html>
<html>
<head>
    <meta charset="UTF-8">
    <meta name="viewport" content="width=device-width, initial-scale=1.0, maximum-scale=1.0, user-scalable=no">
    <title>Print-n-Prick - Dashboard</title>
    <style>
        * { margin: 0; padding: 0; box-sizing: border-box; -webkit-tap-highlight-color: rgba(74, 124, 42, 0.2); }
        body {
            font-family: -apple-system, BlinkMacSystemFont, 'Segoe UI', Roboto, sans-serif;
            background: linear-gradient(135deg, #1a120c 0%, #1a3d0e 25%, #2d5016 50%, #4a7c2a 75%, #6b9f3d 100%);
            min-height: 100vh;
            margin: 0;
            padding: 12px;
            color: #333;
            position: relative;
            -webkit-font-smoothing: antialiased;
            -moz-osx-font-smoothing: grayscale;
        }
        body::before {
            content: '🌵';
            position: fixed;
            font-size: 150px;
            opacity: 0.03;
            top: -50px;
            left: -50px;
            z-index: 0;
        }
        body::after {
            content: '🖨️';
            position: fixed;
            font-size: 120px;
            opacity: 0.03;
            bottom: -40px;
            right: -40px;
            z-index: 0;
        }
        @media (min-width: 768px) {
            body::before { font-size: 300px; top: -100px; left: -100px; }
            body::after { font-size: 250px; bottom: -80px; right: -80px; }
        }
        .container {
            max-width: 600px;
            margin: 0 auto;
            background: linear-gradient(135deg, #f5f5f0 0%, #ffffff 100%);
            border-radius: 20px;
            padding: 20px;
            box-shadow: 0 12px 40px rgba(26, 18, 12, 0.5), 0 0 0 3px rgba(74, 124, 42, 0.2);
            border: 3px solid #4a7c2a;
            position: relative;
            z-index: 1;
        }
        @media (min-width: 768px) {
            .container { border-radius: 24px; padding: 24px; }
        }
        .header {
            text-align: center;
            margin-bottom: 20px;
            padding-bottom: 16px;
            border-bottom: 3px solid #4a7c2a;
        }
        @media (min-width: 768px) {
            .header { margin-bottom: 24px; padding-bottom: 20px; }
        }
        .header-icon { font-size: 42px; margin-bottom: 8px; display: block; }
        @media (min-width: 768px) { .header-icon { font-size: 48px; } }
        h1 {
            color: #2d5016;
            font-size: 28px;
            font-weight: 700;
            margin-bottom: 4px;
            text-shadow: 2px 2px 4px rgba(0,0,0,0.1);
        }
        @media (min-width: 768px) { h1 { font-size: 32px; } }
        .tagline { color: #666; font-size: 12px; font-style: italic; }
        @media (min-width: 768px) { .tagline { font-size: 13px; } }
        .sensor-grid {
            display: grid;
            grid-template-columns: 1fr;
            gap: 12px;
            margin: 20px 0;
        }
        @media (min-width: 480px) {
            .sensor-grid { grid-template-columns: repeat(2, 1fr); }
        }
        .sensor-card {
            background: linear-gradient(135deg, #ffffff 0%, #f8f9fa 100%);
            padding: 16px;
            border-radius: 16px;
            text-align: center;
            border: 2px solid #4a7c2a;
            box-shadow: 0 4px 8px rgba(0,0,0,0.1);
            transition: transform 0.2s;
            -webkit-touch-callout: none;
            user-select: none;
        }
        .sensor-card:active { transform: scale(0.98); }
        @media (hover: hover) {
            .sensor-card:hover {
                transform: translateY(-2px);
                box-shadow: 0 6px 12px rgba(0,0,0,0.15);
            }
        }
        .sensor-icon { font-size: 28px; margin-bottom: 8px; }
        @media (min-width: 768px) { .sensor-icon { font-size: 32px; } }
        .sensor-label {
            font-size: 11px;
            color: #666;
            margin-bottom: 6px;
            font-weight: 600;
            text-transform: uppercase;
            letter-spacing: 0.5px;
        }
        @media (min-width: 768px) { .sensor-label { font-size: 12px; } }
        .sensor-value { font-size: 24px; font-weight: 700; color: #2d5016; }
        @media (min-width: 768px) { .sensor-value { font-size: 28px; } }
        .btn-group {
            display: flex;
            gap: 10px;
            margin: 20px 0;
            flex-wrap: wrap;
        }
        button, .btn {
            flex: 1;
            min-width: 120px;
            padding: 16px 20px;
            border: none;
            border-radius: 12px;
            background: linear-gradient(135deg, #2c1810 0%, #2d5016 50%, #4a7c2a 100%);
            color: white;
            font-weight: 600;
            cursor: pointer;
            font-size: 15px;
            transition: all 0.3s;
            box-shadow: 0 4px 8px rgba(26, 18, 12, 0.3);
            text-decoration: none;
            display: inline-block;
            text-align: center;
            -webkit-touch-callout: none;
            user-select: none;
        }
        @media (min-width: 768px) {
            button, .btn { padding: 14px 20px; }
        }
        button:active, .btn:active {
            transform: scale(0.98);
            box-shadow: 0 2px 6px rgba(26, 18, 12, 0.3);
        }
        @media (hover: hover) {
            button:hover, .btn:hover {
                transform: translateY(-2px);
                box-shadow: 0 6px 12px rgba(45, 80, 22, 0.4);
            }
            button:active, .btn:active { transform: translateY(0) scale(0.98); }
        }
        .btn-danger { background: linear-gradient(135deg, #8b4513 0%, #a0522d 100%); }
        .btn-secondary { background: linear-gradient(135deg, #3e2723 0%, #5d4037 100%); }
        .section {
            margin: 20px 0;
            padding: 16px;
            background: linear-gradient(135deg, #ffffff 0%, #f8f9fa 100%);
            border-radius: 16px;
            border: 2px solid #4a7c2a;
            border-left: 4px solid #3e2723;
        }
        @media (min-width: 768px) {
            .section { margin: 24px 0; padding: 20px; }
        }
        .section-title {
            font-size: 18px;
            font-weight: 700;
            color: #2d5016;
            margin-bottom: 14px;
            display: flex;
            align-items: center;
            gap: 8px;
        }
        @media (min-width: 768px) {
            .section-title { font-size: 20px; margin-bottom: 16px; }
        }
        .input-group {
            display: flex;
            gap: 8px;
            margin: 12px 0;
            flex-wrap: wrap;
        }
        input[type="text"], input[type="datetime-local"] {
            flex: 1;
            min-width: 150px;
            padding: 12px 16px;
            border: 2px solid #4a7c2a;
            border-bottom: 2px solid #3e2723;
            border-radius: 10px;
            font-size: 16px;
            font-family: inherit;
            background: #fff;
        }
        @media (min-width: 768px) {
            input[type="text"], input[type="datetime-local"] { font-size: 14px; }
        }
        input[type="text"]:focus, input[type="datetime-local"]:focus {
            outline: none;
            border-color: #6b9f3d;
            border-bottom-color: #8b4513;
            box-shadow: 0 0 0 3px rgba(74, 124, 42, 0.2);
        }
        .list-container {
            max-height: 200px;
            overflow-y: auto;
            -webkit-overflow-scrolling: touch;
            background: #fff;
            padding: 12px;
            border-radius: 10px;
            margin: 12px 0;
            border: 1px solid #3e2723;
            min-height: 60px;
        }
        .section.reminders-section .list-container,
        .section.grocery-section .list-container {
            max-height: 240px;
            min-height: 100px;
        }
        @media (min-width: 768px) {
            .section.reminders-section .list-container,
            .section.grocery-section .list-container {
                max-height: 280px;
                min-height: 120px;
            }
        }
        .list-item {
            padding: 12px 10px;
            margin: 6px 0;
            background: #f8f9fa;
            border-radius: 8px;
            border-left: 4px solid #4a7c2a;
            display: flex;
            justify-content: space-between;
            align-items: center;
            gap: 8px;
            font-size: 14px;
            min-height: 48px;
        }
        @media (min-width: 768px) {
            .list-item { padding: 10px; min-height: auto; }
        }
        .list-item:nth-child(odd) { border-left-color: #8b4513; }
        .list-item small { color: #5d4037; font-size: 11px; }
        .list-item button {
            padding: 8px 14px;
            font-size: 13px;
            min-width: auto;
            flex: 0 0 auto;
        }
        @media (min-width: 768px) {
            .list-item button { padding: 6px 12px; font-size: 12px; }
        }
        .empty-state { text-align: center; color: #999; font-style: italic; padding: 20px; }
        .control-grid {
            display: grid;
            grid-template-columns: 1fr;
            gap: 12px;
            margin: 20px 0;
            min-width: 0;
        }
        @media (min-width: 640px) {
            .control-grid { grid-template-columns: 1fr 1fr; }
        }
        .control-half {
            background: linear-gradient(135deg, #ffffff 0%, #f8f9fa 100%);
            border: 2px solid #4a7c2a;
            border-left: 3px solid #8b4513;
            border-radius: 12px;
            padding: 14px;
            min-width: 0;
            display: flex;
            flex-direction: column;
        }
        .control-half .ctrl-title {
            font-size: 17px;
            font-weight: 700;
            color: #2d5016;
            margin-bottom: 10px;
            flex-shrink: 0;
        }
        @media (min-width: 768px) { .control-half .ctrl-title { font-size: 18px; } }
        .slider-row {
            display: flex;
            justify-content: space-between;
            align-items: center;
            margin-bottom: 8px;
            font-size: 14px;
            color: #5d4037;
            font-weight: 600;
            gap: 8px;
            min-width: 0;
        }
        .slider-row span:first-child { flex-shrink: 0; white-space: nowrap; }
        .slider-row .val { color: #2d5016; min-width: 36px; text-align: right; flex-shrink: 0; }
        .ctrl-slider {
            width: 100%;
            min-width: 0;
            height: 10px;
            border-radius: 5px;
            margin-bottom: 10px;
            -webkit-appearance: none;
            background: linear-gradient(90deg, #1a3d0e 0%, #2d5016 100%);
            box-sizing: border-box;
        }
        .ctrl-slider:disabled {
            opacity: 0.45;
            cursor: not-allowed;
            pointer-events: none;
        }
        .ctrl-slider:disabled::-webkit-slider-thumb { cursor: not-allowed; }
        .ctrl-slider:disabled::-moz-range-thumb { cursor: not-allowed; }
        @media (min-width: 768px) {
            .ctrl-slider { height: 8px; border-radius: 4px; }
        }
        .ctrl-slider::-webkit-slider-thumb {
            -webkit-appearance: none;
            width: 24px;
            height: 24px;
            border-radius: 50%;
            background: #6b9f3d;
            border: 2px solid #8b4513;
            cursor: pointer;
        }
        .ctrl-slider::-moz-range-thumb {
            width: 24px;
            height: 24px;
            border-radius: 50%;
            background: #6b9f3d;
            border: 2px solid #8b4513;
            cursor: pointer;
        }
        @media (min-width: 768px) {
            .ctrl-slider::-webkit-slider-thumb { width: 20px; height: 20px; }
            .ctrl-slider::-moz-range-thumb { width: 20px; height: 20px; }
        }
        .cal-btns { display: flex; gap: 8px; margin-top: auto; padding-top: 12px; flex-shrink: 0; }
        .cal-btns button { flex: 1; min-width: 0; padding: 12px 10px; border: none; border-radius: 8px; font-size: 14px; font-weight: 600; cursor: pointer; }
        .cal-btns button:disabled { opacity: 0.45; cursor: not-allowed; }
        @media (min-width: 768px) { .cal-btns button { padding: 10px; } }
        .cal-btns .cal-off { background: #3e2723; color: #e8ecd8; }
        .cal-btns .cal-on { background: #4a7c2a; color: #fff; }
        .pump-btns { display: flex; gap: 8px; margin-top: auto; padding-top: 12px; flex-shrink: 0; }
        .pump-btns button { flex: 1; min-width: 0; padding: 12px 10px; font-size: 14px; }
        .pump-btns button:disabled { opacity: 0.45; cursor: not-allowed; }
        @media (min-width: 768px) { .pump-btns button { padding: 10px; } }
        .sensor-toggle-row {
            display: grid;
            grid-template-columns: 1fr;
            gap: 12px;
            margin-top: 12px;
            padding-top: 12px;
            border-top: 1px solid #3e2723;
        }
        @media (min-width: 480px) {
            .sensor-toggle-row { grid-template-columns: 1fr 1fr; }
        }
        .sensor-toggle-item {
            display: flex;
            justify-content: space-between;
            align-items: center;
            font-size: 14px;
            color: #5d4037;
            font-weight: 600;
            min-height: 44px;
        }
        .tog {
            width: 50px;
            height: 28px;
            border-radius: 14px;
            background: #3e2723;
            cursor: pointer;
            position: relative;
            flex-shrink: 0;
            transition: background 0.3s;
        }
        @media (min-width: 768px) {
            .tog { width: 44px; height: 24px; border-radius: 12px; }
        }
        .tog.on { background: #4a7c2a; }
        .tog::after {
            content: '';
            position: absolute;
            width: 24px;
            height: 24px;
            border-radius: 50%;
            background: #fff;
            top: 2px;
            left: 2px;
            transition: left 0.2s;
            box-shadow: 0 2px 4px rgba(0,0,0,0.2);
        }
        @media (min-width: 768px) {
            .tog::after { width: 20px; height: 20px; }
        }
        .tog.on::after { left: 24px; }
        @media (min-width: 768px) { .tog.on::after { left: 22px; } }
        textarea.reminder-msg {
            width: 100%;
            min-height: 120px;
            padding: 12px 16px;
            border: 2px solid #4a7c2a;
            border-bottom: 2px solid #3e2723;
            border-radius: 10px;
            font-size: 16px;
            font-family: inherit;
            background: #fff;
            resize: vertical;
            box-sizing: border-box;
        }
        @media (min-width: 768px) { textarea.reminder-msg { font-size: 14px; min-height: 130px; } }
        textarea.reminder-msg:focus {
            outline: none;
            border-color: #6b9f3d;
            border-bottom-color: #8b4513;
            box-shadow: 0 0 0 3px rgba(74, 124, 42, 0.2);
        }
        textarea.todo-msg {
            width: 100%;
            padding: 12px 16px;
            border: 2px solid #4a7c2a;
            border-bottom: 2px solid #3e2723;
            border-radius: 10px;
            font-size: 16px;
            font-family: inherit;
            background: #fff;
            resize: vertical;
            box-sizing: border-box;
        }
        @media (min-width: 768px) { textarea.todo-msg { font-size: 14px; } }
        textarea.todo-msg:focus {
            outline: none;
            border-color: #6b9f3d;
            border-bottom-color: #8b4513;
            box-shadow: 0 0 0 3px rgba(74, 124, 42, 0.2);
        }
        button, .btn, .sensor-card, .tog, .list-item button {
            -webkit-user-select: none;
            user-select: none;
        }
        @supports (padding: env(safe-area-inset-bottom)) {
            body {
                padding-top: max(12px, env(safe-area-inset-top));
                padding-bottom: max(12px, env(safe-area-inset-bottom));
                padding-left: max(12px, env(safe-area-inset-left));
                padding-right: max(12px, env(safe-area-inset-right));
            }
        }
    </style>
</head>
<body>
    <div class="container">
        <div class="header">
            <span class="header-icon">🌵💌</span>
            <h1>Print-n-Prick</h1>
            <p class="tagline">Smart Cactus Care & Thermal Printing</p>
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
        
        <div class="section reminders-section">
            <div class="section-title">⏰ Reminders</div>
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
        
        <div class="section grocery-section">
            <div class="section-title">🛒 Grocery List</div>
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
        
        <div class="section grocery-section">
            <div class="section-title">✅ Todo List</div>
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
        
        <div class="section">
            <div class="section-title">💧 Pump & LED</div>
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
        
        loadReminders();
        loadGroceries();
        loadTodos();
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
    
    server.begin();
    
    Serial.println("🌐 TZT Display Web Server started on http://" + deviceIP + ":8080");
    Serial.println("   • Firebase + ESP-NOW to Main ESP32 (no HTTP proxy)");
    Serial.println("   💡 Try: http://" + deviceIP + ":8080/login");
}

void setup() {
    Serial.begin(115200);
    delay(1000);
    
    esp_task_wdt_init(30, true);   // 30s watchdog, panic on timeout
    esp_task_wdt_add(NULL);        // Current task (loop) is watched
    
    Serial.println("========================================");
    Serial.println("TZT ESP32 LVGL Display Module");
    Serial.println("Version: " + String(FIRMWARE_VERSION));
    Serial.println("========================================");
    
    // Initialize WiFi
    WiFiManager wifiManager;
    wifiManager.setAPStaticIPConfig(IPAddress(192, 168, 4, 1), 
                                     IPAddress(192, 168, 4, 1), 
                                     IPAddress(255, 255, 255, 0));
#if FORCE_WIFI_CONFIG_PORTAL
    Serial.println("FORCE_WIFI_CONFIG_PORTAL: erasing WiFi, opening config portal (AP: " TZT_AP_SSID ")");
    wifiManager.resetSettings();
    wifiManager.startConfigPortal(TZT_AP_SSID, TZT_AP_PASSWORD);
#else
    if (!wifiManager.autoConnect(TZT_AP_SSID, TZT_AP_PASSWORD)) {
        Serial.println("WiFi config portal timed out or failed.");
    }
#endif
    
    if (WiFi.status() == WL_CONNECTED) {
        deviceIP = WiFi.localIP().toString();
        Serial.println("WiFi connected! IP: " + deviceIP);
    } else {
        deviceIP = "192.168.4.1";
        Serial.println("WiFi failed. Running in AP mode.");
    }
    Serial.println("MAC: " + WiFi.macAddress());

#if (BOARD_LED_PIN >= 0)
    pinMode(BOARD_LED_PIN, OUTPUT);
    digitalWrite(BOARD_LED_PIN, BOARD_LED_ACTIVE_LOW ? HIGH : LOW);  // Off until WiFi connected
#endif

    configTime(GMT_OFFSET_SEC, DAYLIGHT_OFFSET_SEC, NTP_SERVER);

    Logger::setLevel(LOG_LEVEL_WARN);
    firebase = new FirebaseService(FIREBASE_DATABASE_URL, FIREBASE_TIMEOUT);
    firebase->setRetryPolicy(3, 1000);
    firebase->setRateLimit(60);
    reminderService = new ReminderService(firebase);
    reminderService->load();
    loadGroceries();
    loadTodos();
    loadSettingsFromFirebase();  // Restore LED, autoDispense, autoBrightness, pump duration/cooldown; send to Main
    Serial.println("Firebase + Reminders + Groceries + Todos + Settings initialized");

#if !defined(TZT_HEADLESS)
    Serial.println("Initializing display and LVGL...");
    tft.init();
    tft.setRotation(1);  // 2.4" 240x320 panel: rotation 1 = 320x240 landscape (matches TZT_SCREEN_*)
    tft.fillScreen(TFT_BLACK);
    lv_init();
    lv_disp_draw_buf_init(&draw_buf, disp_buf1, disp_buf2, TZT_DISP_BUF_PIXELS);
    lv_disp_drv_init(&disp_drv);
    disp_drv.hor_res = TZT_SCREEN_WIDTH;
    disp_drv.ver_res = TZT_SCREEN_HEIGHT;
    disp_drv.draw_buf = &draw_buf;
    disp_drv.flush_cb = lvgl_flush_cb;
    lv_disp_drv_register(&disp_drv);
    createControlPanel();
    updateControlPanelStatus();
    Serial.println("Control panel on display ready.");
#else
    Serial.println("TZT_HEADLESS: running without display (LVGL skipped)");
#endif
    
    // Initialize ESP-NOW for sending commands to Main ESP32
    Serial.println("Initializing ESP-NOW...");
    
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
                    Serial.println("✅ ESP-NOW initialized successfully");
                    // Send saved settings to Main so it restores LED, toggles, pump duration/cooldown
                    if (bootSettingsCount > 0 && bootSettingsIndex < bootSettingsCount)
                        applyNextBootSetting();
                    Serial.print("   Main ESP32 MAC: ");
                    for (int i = 0; i < 6; i++) {
                        if (i > 0) Serial.print(":");
                        Serial.print(mainESP32Mac[i], HEX);
                    }
                    Serial.println();
                    Serial.println("   This MAC: " + WiFi.macAddress());
                    Serial.println("   SSID: " + WiFi.SSID() + " WiFi.ch=" + String(WiFi.channel()) + " peer.ch=" + String(peerInfo.channel) + " (Main must match)");
                } else {
                    Serial.println("⚠️ ESP-NOW: Peer added but verification failed");
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
    
    // Setup web server
    setupWebServer();
    
    Serial.println("Setup complete!");
}

void loop() {
    esp_task_wdt_reset();
    server.handleClient();

#if !defined(TZT_HEADLESS)
    if (labelMoisture && (millis() - lastPanelUpdate >= 500)) {
        lastPanelUpdate = millis();
        uint32_t currentHash = calculateSensorHash();
        if (currentHash != lastSensorHash) {
            lastSensorHash = currentHash;
            updateControlPanelStatus();
        }
    }
#endif

    // Timeout: if chunked send in progress but no CHUNK_ACK for 15s, abort so next print can start
    if (pendingChunkTotal > 0 && pendingChunkSendTime > 0 && (millis() - pendingChunkSendTime) > CHUNK_ACK_TIMEOUT_MS) {
        pendingChunkTotal = 0;
        pendingChunkSendTime = 0;
        nextChunkSendAt = 0;
        chunkResendAt = 0;
    }
    // Send next chunk from loop (after CHUNK_ACK received) — never send from callback
    if (espNowInitialized && pendingChunkTotal > 0 && nextChunkSendAt > 0 && millis() >= nextChunkSendAt) {
        nextChunkSendAt = 0;
        pendingChunkSendTime = millis();
        String chunk = pendingChunkMessage.substring(pendingChunkIndex * PRINT_CHUNK_SIZE, (pendingChunkIndex + 1) * PRINT_CHUNK_SIZE);
        sendCommandViaESPNow(CMD_PRINT_CHUNK, (uint8_t)pendingChunkIndex, pendingChunkTotal, chunk.c_str());
    }
    // Resend current chunk after send failed (chunk-level retry so prints never fail)
    if (espNowInitialized && pendingChunkTotal > 0 && chunkResendAt > 0 && millis() >= chunkResendAt) {
        chunkResendAt = 0;
        pendingChunkSendTime = millis();
        String chunk = pendingChunkMessage.substring(pendingChunkIndex * PRINT_CHUNK_SIZE, (pendingChunkIndex + 1) * PRINT_CHUNK_SIZE);
        sendCommandViaESPNow(CMD_PRINT_CHUNK, (uint8_t)pendingChunkIndex, pendingChunkTotal, chunk.c_str(), true);
    }
    // Periodic resend: if no CHUNK_ACK for 2s, resend current chunk (handshake never stalls)
    if (espNowInitialized && pendingChunkTotal > 0 && pendingChunkSendTime > 0 && (millis() - pendingChunkSendTime) > CHUNK_RESEND_INTERVAL_MS) {
        pendingChunkSendTime = millis();
        String chunk = pendingChunkMessage.substring(pendingChunkIndex * PRINT_CHUNK_SIZE, (pendingChunkIndex + 1) * PRINT_CHUNK_SIZE);
        sendCommandViaESPNow(CMD_PRINT_CHUNK, (uint8_t)pendingChunkIndex, pendingChunkTotal, chunk.c_str(), true);
    }
    // Reliable settings: send once, wait for ACK; resend every 400ms if no ACK (handshake like printer)
    if (espNowInitialized && pendingChunkTotal == 0 && pendingSettingRemainingSends > 0 && millis() >= pendingSettingNextSendAt) {
        sendingFromSettingsQueue = true;
        sendCommandViaESPNow(pendingSettingCommandType, pendingSettingParam1, pendingSettingParam2, pendingSettingMessage, false, true);
        sendingFromSettingsQueue = false;
        pendingSettingRemainingSends--;
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

    // Save groceries to Firebase in background (debounced: max once per 5s)
    if (groceriesNeedSave && firebase) {
        unsigned long now = millis();
        if (now - lastGrocerySaveTime >= SAVE_DEBOUNCE_MS) {
            groceriesNeedSave = false;
            lastGrocerySaveTime = now;
            saveGroceries();
        }
    }

    // Save settings to Firebase (debounced: max once per 5s) so restart remembers LED, toggles, pump duration/cooldown
    if (settingsNeedSave && firebase) {
        unsigned long now = millis();
        if (now - lastSettingsSaveTime >= SAVE_DEBOUNCE_MS) {
            settingsNeedSave = false;
            lastSettingsSaveTime = now;
            saveSettingsToFirebase();
        }
    }
    
    // Save todos to Firebase in background (non-blocking, after HTTP response sent)
    if (todosNeedSave && firebase) {
        todosNeedSave = false;
        saveTodos();
    }
    
    // Save reminders to Firebase in background (non-blocking, after HTTP response sent)
    if (remindersNeedSave && reminderService) {
        remindersNeedSave = false;  // Clear flag before save (prevents re-trigger if save fails)
        reminderService->save();  // Save in background (doesn't block HTTP response)
    }

    // OPTIMIZED: Poll Firebase /commands with adaptive interval
    // Start with 30s, reduce to 10s if commands found (more responsive), back to 30s if idle
    static unsigned long lastCmd = 0;
    static unsigned long firebasePollInterval = 30000;  // Start with 30 seconds
    static unsigned long lastCommandFound = 0;
    
    // If we found a command recently, poll more frequently (10s) for next 2 minutes
    if (millis() - lastCommandFound < 120000) {
        firebasePollInterval = 10000;  // Poll every 10s when active
    } else {
        firebasePollInterval = 30000;  // Back to 30s when idle
    }
    
    if (firebase && (millis() - lastCmd >= firebasePollInterval)) {
        lastCmd = millis() + (unsigned long)random(0, 2000);  // Jitter 0–2s to avoid thundering herd
        DynamicJsonDocument cmds(2048);
        if (firebase->pollCommands(cmds) && cmds.size() > 0) {
            lastCommandFound = millis();  // Track when we found commands
            for (JsonPair kv : cmds.as<JsonObject>()) {
                JsonObject c = kv.value();
                if (c["processed"] | false) continue;
                String typ = c["type"].as<String>(), data = c["data"].as<String>();
                if (typ == "print") {
                    // source "web" = message only (no title/date/weather); "shortcut" or missing = full receipt
                    String source = c["source"].as<String>();
                    bool messageOnly = source.equalsIgnoreCase("web");
                    sendPrintChunked(data, messageOnly);
                }
                else if (typ == "dispense_start" || typ == "water_start") {
                    lastSensorData.isDispensing = true;
                    lastSensorDataValid = true;
#if !defined(TZT_HEADLESS)
                    updateControlPanelStatus();
#endif
                    sendPumpCommandRepeated(CMD_DISPENSE_START);
                }
                else if (typ == "dispense_stop" || typ == "water_stop") {
                    lastSensorData.isDispensing = false;
                    lastSensorDataValid = true;
#if !defined(TZT_HEADLESS)
                    updateControlPanelStatus();
#endif
                    sendPumpCommandRepeated(CMD_DISPENSE_STOP);
                }
                firebase->deleteData("/commands/" + String(kv.key().c_str()) + ".json");
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

#if !defined(TZT_HEADLESS)
    lv_timer_handler();
#endif
    delay(5);
}
