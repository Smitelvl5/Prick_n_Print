/*
 * ESP-NOW Service
 * Handles ESP-NOW communication between Main ESP32 and TZT Display
 */

#include "EspNowService.h"
#include "EspNowProtocol.h"
#include "Logger.h"
#include "config.h"
#include <esp_now.h>
#include <esp_wifi.h>
#include <WiFi.h>
#include <string.h>

EspNowService* EspNowService::instance = nullptr;

// C-style callback wrappers for ESP-NOW
void onDataSentStatic(const uint8_t* mac_addr, esp_now_send_status_t status) {
    if (EspNowService::instance) {
        EspNowService::instance->onDataSent(mac_addr, status);
    }
}

void onDataRecvStatic(const uint8_t* mac_addr, const uint8_t* data, int len) {
    if (EspNowService::instance) {
        EspNowService::instance->onDataRecv(mac_addr, data, len);
    }
}

EspNowService::EspNowService() {
    instance = this;
    sequenceNumber = 0;
    lastSendTime = 0;
    sendPendingSince = 0;
    lastSendFailed = false;
    chunkAckQueueHead = 0;
    chunkAckQueueTail = 0;
    peerMacAddress[0] = 0;
    initialized = false;
}

bool EspNowService::initialize(const uint8_t* peerMac) {
    if (initialized) {
        return true;
    }
    
    // Reject all-zero MAC (TZT_DISPLAY_MAC_ADDRESS not set in config.h)
    bool macSet = false;
    for (int i = 0; i < 6; i++) { if (peerMac[i] != 0) { macSet = true; break; } }
    if (!macSet) {
        Logger::error("ESP-NOW", "TZT_DISPLAY_MAC_ADDRESS not set (all zeros in config.h). Set it to the TZT MAC from its serial output (e.g. MAC: 6C:C8:40:55:85:98 -> {0x6C,0xC8,0x40,0x55,0x85,0x98}).");
        return false;
    }
    
    // Copy peer MAC address
    memcpy(peerMacAddress, peerMac, 6);
    
    // Wait a bit for WiFi to fully stabilize before initializing ESP-NOW
    delay(500);
    
    // WiFi already in STA from setupWiFi(); do not call WiFi.mode(WIFI_STA) here (can glitch connection)
    
    // Initialize ESP-NOW
    esp_err_t initResult = esp_now_init();
    if (initResult != ESP_OK) {
        // If already initialized, deinit and try again
        if (initResult == ESP_ERR_ESPNOW_NOT_INIT) {
            // This shouldn't happen, but handle it
            Logger::warn("ESP-NOW", "ESP-NOW not initialized, attempting init");
        } else {
            Logger::error("ESP-NOW", "Failed to initialize ESP-NOW: " + String((int)initResult));
            return false;
        }
    }
    
    // Register send callback (C-style function)
    esp_now_register_send_cb(onDataSentStatic);
    
    // Register receive callback (C-style function)
    esp_now_register_recv_cb(onDataRecvStatic);
    
    // Remove peer if it already exists (in case of re-initialization)
    esp_now_del_peer(peerMac);
    delay(50);
    
    // Add peer: zero the struct so ifidx etc. are not garbage (fixes "Peer interface is invalid")
    esp_now_peer_info_t peerInfo;
    memset(&peerInfo, 0, sizeof(peerInfo));
    memcpy(peerInfo.peer_addr, peerMac, 6);
    peerInfo.ifidx = WIFI_IF_STA;
    // Use explicit WiFi channel (ESP-IDF example uses explicit channel, not 0)
    // Get current WiFi channel, fallback to ESP_NOW_CHANNEL if not connected
    uint8_t wifiChannel = (WiFi.status() == WL_CONNECTED && WiFi.channel() > 0) ? WiFi.channel() : (uint8_t)ESP_NOW_CHANNEL;
    peerInfo.channel = wifiChannel;
    peerInfo.encrypt = false;
    
    esp_err_t addPeerResult = esp_now_add_peer(&peerInfo);
    if (addPeerResult != ESP_OK) {
        String errStr = "";
        switch(addPeerResult) {
            case ESP_ERR_ESPNOW_NOT_INIT: errStr = "NOT_INIT"; break;
            case ESP_ERR_ESPNOW_ARG: errStr = "ARG"; break;
            case ESP_ERR_ESPNOW_FULL: errStr = "FULL (peer list full)"; break;
            case ESP_ERR_ESPNOW_NO_MEM: errStr = "NO_MEM"; break;
            case ESP_ERR_ESPNOW_EXIST: errStr = "EXIST (peer already added)"; break;
            default: errStr = "UNKNOWN(" + String((int)addPeerResult) + ")"; break;
        }
        Logger::error("ESP-NOW", "Failed to add peer: " + errStr + " (code=" + String((int)addPeerResult) + ")");
        return false;
    }
    
    // Small delay for peer to be fully registered
    delay(100);
    
    // Verify peer was added
    if (!esp_now_is_peer_exist(peerMac)) {
        Logger::error("ESP-NOW", "Peer added but esp_now_is_peer_exist() returns false!");
        return false;
    }
    
    initialized = true;
    Serial.println("ESP-NOW init OK. SSID: " + WiFi.SSID() + " WiFi.ch=" + String(WiFi.channel()) + " peer.ch=" + String(peerInfo.channel) + " (TZT must match)");
    Logger::info("ESP-NOW", "ESP-NOW initialized successfully");
    Logger::info("ESP-NOW", "Peer MAC: " + EspNowService::macToString(peerMac));
    uint8_t thisMac[6];
    WiFi.macAddress(thisMac);
    Logger::info("ESP-NOW", "This MAC: " + EspNowService::macToString(thisMac));
    
    return true;
}

void EspNowService::onDataSent(const uint8_t* mac_addr, esp_now_send_status_t status) {
    if (status == ESP_NOW_SEND_SUCCESS) {
        lastSendFailed = false;
        sendPendingSince = 0;
    } else {
        lastSendFailed = true;
    }
}

void EspNowService::onDataRecv(const uint8_t* mac_addr, const uint8_t* data, int len) {
    if (len < 1) return;
    
    uint8_t msgType = data[0];
    
    switch (msgType) {
        case ESP_NOW_MSG_SENSOR_DATA:
            // Main sends this to TZT; ignore if we somehow receive it (loopback/stray)
            break;
        case ESP_NOW_MSG_COMMAND: {
            // ESP-NOW can receive from devices not in peer list, but for reliable communication,
            // we should ensure the sender is in our peer list (especially for ACKs/responses)
            // Check if sender is in peer list, if not, add it
            if (!esp_now_is_peer_exist(mac_addr)) {
                esp_now_peer_info_t peerInfo;
                memset(&peerInfo, 0, sizeof(peerInfo));
                memcpy(peerInfo.peer_addr, mac_addr, 6);
                peerInfo.ifidx = WIFI_IF_STA;
                uint8_t wifiChannel = (WiFi.status() == WL_CONNECTED && WiFi.channel() > 0) ? WiFi.channel() : (uint8_t)ESP_NOW_CHANNEL;
                peerInfo.channel = wifiChannel;
                peerInfo.encrypt = false;
                
                if (esp_now_add_peer(&peerInfo) != ESP_OK) {
                    Serial.println("⚠️ ESP-NOW: Failed to add command sender to peer list");
                }
            }
            
            if (len >= sizeof(CommandPacket)) {
                CommandPacket* cmd = (CommandPacket*)data;
                if (verifyChecksum((uint8_t*)cmd, sizeof(CommandPacket))) {
                    if (cmd->commandType == CMD_PRINT_CHUNK) {
                        // Never send CHUNK_ACK from callback (avoids NO_MEM). Queue and send from loop only.
                        uint8_t nextTail = (chunkAckQueueTail + 1) % CHUNK_ACK_QUEUE_SIZE;
                        if (nextTail != chunkAckQueueHead) {
                            chunkAckQueue[chunkAckQueueTail].chunkIndex = cmd->param1;
                            memcpy(chunkAckQueue[chunkAckQueueTail].mac, mac_addr, 6);
                            chunkAckQueueTail = nextTail;
                        }
                    }
                    handleCommand(cmd);
                    if (cmd->commandType != CMD_PRINT_CHUNK) {
                        AckPacket ack;
                        memset(&ack, 0, sizeof(ack));
                        ack.msgType = ESP_NOW_MSG_ACK;
                        ack.ackForMsgType = ESP_NOW_MSG_COMMAND;
                        ack.sequenceNumber = cmd->sequenceNumber;
                        ack.success = true;
                        ack.checksum = calculateChecksum((uint8_t*)&ack, sizeof(AckPacket));
                        esp_now_send(mac_addr, (uint8_t*)&ack, sizeof(AckPacket));
                    }
                } else {
                    Logger::warn("ESP-NOW", "Invalid checksum in command packet");
                    Serial.println("⚠️ ESP-NOW: Command packet checksum failed");
                }
            } else {
                Serial.println("⚠️ ESP-NOW: Command packet too short: " + String(len) + " bytes (expected " + String(sizeof(CommandPacket)) + ")");
            }
            break;
        }
        case ESP_NOW_MSG_STATUS_REQUEST: {
            // TZT Display is requesting sensor data - send it immediately
            sendSensorData();
            break;
        }
        default:
            Logger::warn("ESP-NOW", "Unknown message type: " + String(msgType));
            break;
    }
}

bool EspNowService::sendSensorData() {
    if (!initialized) return false;
    
    // Verify peer exists before sending
    if (!esp_now_is_peer_exist(peerMacAddress)) {
        static unsigned long lastPeerCheckFail = 0;
        if (millis() - lastPeerCheckFail > 10000) {  // Log every 10s max
            Logger::warn("ESP-NOW", "Peer not found, attempting to re-add...");
            lastPeerCheckFail = millis();
            
            // Try to re-add peer
            esp_now_peer_info_t peerInfo;
            memset(&peerInfo, 0, sizeof(peerInfo));
            memcpy(peerInfo.peer_addr, peerMacAddress, 6);
            peerInfo.ifidx = WIFI_IF_STA;
            uint8_t wifiChannel = (WiFi.status() == WL_CONNECTED && WiFi.channel() > 0) ? WiFi.channel() : (uint8_t)ESP_NOW_CHANNEL;
            peerInfo.channel = wifiChannel;
            peerInfo.encrypt = false;
            
            if (esp_now_add_peer(&peerInfo) == ESP_OK) {
                delay(50);
                if (esp_now_is_peer_exist(peerMacAddress)) {
                    Logger::info("ESP-NOW", "Peer re-added successfully");
                }
            }
        }
        return false;
    }
    
    SensorDataPacket packet;
    memset(&packet, 0, sizeof(packet));  // Zero-initialize to avoid garbage in padding/checksum
    packet.msgType = ESP_NOW_MSG_SENSOR_DATA;
    packet.timestamp = millis();
    
    // Get sensor data from hardware (will be set by main.cpp)
    packet.moisturePercent = lastSensorData.moisturePercent;
    packet.sanitizerLevel = lastSensorData.sanitizerLevel;
    packet.irDetected = lastSensorData.irDetected;
    packet.lightPercent = lastSensorData.lightPercent;
    packet.ledBrightness = lastSensorData.ledBrightness;
    packet.isDispensing = lastSensorData.isDispensing;
    packet.pumpRunning = lastSensorData.pumpRunning;
    packet.autoDispense = lastSensorData.autoDispense;
    packet.autoBrightness = lastSensorData.autoBrightness;
    packet.sequenceNumber = sequenceNumber++;
    packet.checksum = calculateChecksum((uint8_t*)&packet, sizeof(SensorDataPacket));
    
    esp_err_t result = esp_now_send(peerMacAddress, (uint8_t*)&packet, sizeof(SensorDataPacket));
    
    if (result == ESP_OK) {
        Serial.println("[Main] Sent: sensor");
        sendPendingSince = millis();
        return true;
    } else {
        // Decode error code for better debugging
        String errStr = "";
        switch(result) {
            case ESP_ERR_ESPNOW_NOT_INIT: errStr = "NOT_INIT"; break;
            case ESP_ERR_ESPNOW_ARG: errStr = "ARG"; break;
            case ESP_ERR_ESPNOW_INTERNAL: errStr = "INTERNAL"; break;
            case ESP_ERR_ESPNOW_NO_MEM: errStr = "NO_MEM"; break;
            case ESP_ERR_ESPNOW_NOT_FOUND: errStr = "NOT_FOUND (peer not added)"; break;
            case ESP_ERR_ESPNOW_IF: errStr = "IF (interface mismatch)"; break;
            // ESP_ERR_ESPNOW_CHAN may not be available in all Arduino ESP32 versions
            default: errStr = "UNKNOWN(code=" + String((int)result) + ")"; break;
        }
        Logger::warn("ESP-NOW", "Failed to send sensor data: " + errStr + " (code=" + String((int)result) + ")");
        return false;
    }
}

void EspNowService::updateSensorData(float moisture, float sanitizer, bool ir, float light,
                                     uint8_t led, bool dispensing, bool pump, bool autoDispense, bool autoBrightness) {
    lastSensorData.moisturePercent = moisture;
    lastSensorData.sanitizerLevel = sanitizer;
    lastSensorData.irDetected = ir;
    lastSensorData.lightPercent = light;
    lastSensorData.ledBrightness = led;
    lastSensorData.isDispensing = dispensing;
    lastSensorData.pumpRunning = pump;
    lastSensorData.autoDispense = autoDispense;
    lastSensorData.autoBrightness = autoBrightness;
}

void EspNowService::handleCommand(CommandPacket* cmd) {
    if (commandCallback) {
        commandCallback(cmd);  // Main logs "Command XXX received from TZT, executing..." and "Executed: XXX"
    }
}

void EspNowService::clearRetryState() {
    sendPendingSince = 0;
    lastSendFailed = false;
}

void EspNowService::sendPendingChunkAck() {
    if (!initialized || chunkAckQueueHead == chunkAckQueueTail) return;
    ChunkAckSlot& slot = chunkAckQueue[chunkAckQueueHead];
    ChunkAckPacket chunkAck;
    chunkAck.msgType = ESP_NOW_MSG_CHUNK_ACK;
    chunkAck.chunkIndex = slot.chunkIndex;
    chunkAck.checksum = calculateChecksum((uint8_t*)&chunkAck, sizeof(ChunkAckPacket));
    esp_err_t result = esp_now_send(slot.mac, (uint8_t*)&chunkAck, sizeof(ChunkAckPacket));
    if (result == ESP_OK) {
        chunkAckQueueHead = (chunkAckQueueHead + 1) % CHUNK_ACK_QUEUE_SIZE;
    }
    // If failed, leave in queue; loop will retry next time (no backoff so ACKs drain quickly)
}

String EspNowService::macToString(const uint8_t* mac) {
    char macStr[18];
    snprintf(macStr, sizeof(macStr), "%02X:%02X:%02X:%02X:%02X:%02X",
             mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    return String(macStr);
}
