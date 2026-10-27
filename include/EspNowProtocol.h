#ifndef ESP_NOW_PROTOCOL_H
#define ESP_NOW_PROTOCOL_H

#include <Arduino.h>
#include <WiFi.h>
#include <esp_now.h>

// ============================================================================
// ESP-NOW PROTOCOL DEFINITIONS
// ============================================================================
// Data structures for communication between Main ESP32 and TZT Display

// Message types
#define ESP_NOW_MSG_SENSOR_DATA    0x01  // Main ESP32 → TZT Display
#define ESP_NOW_MSG_COMMAND         0x02  // TZT Display → Main ESP32
#define ESP_NOW_MSG_ACK             0x03  // Acknowledgment
#define ESP_NOW_MSG_STATUS_REQUEST  0x04  // TZT Display → Main ESP32 (request sensor data)
#define ESP_NOW_MSG_CHUNK_ACK       0x05  // Main ESP32 → TZT Display (ack chunk N received; TZT sends N+1)

// Command types (for ESP_NOW_MSG_COMMAND)
#define CMD_PRINT                   0x01
#define CMD_DISPENSE_START          0x02
#define CMD_DISPENSE_STOP           0x03
#define CMD_RESET_SANITIZER         0x04
#define CMD_SET_LED_BRIGHTNESS      0x05  // param1 = brightness level 1-100% (saved; applied now if LED is on)
#define CMD_SET_LED_STATE           0x06  // param1 = 1 on (at saved level), 0 off; holds until next schedule switch
#define CMD_SET_LED_SCHEDULE        0x07  // param1 = 1 on, 0 off (night schedule controls LED)
#define CMD_TEST_PRINTER            0x08
#define CMD_TEST_PUMP               0x09
#define CMD_TEST_LED                0x0A
#define CMD_SET_PUMP_DURATION       0x0B  // param1 = duration ms/100 (0-255 = 0-25.5s)
#define CMD_SET_PUMP_COOLDOWN       0x0C  // param1 = cooldown ms/100 (0-255 = 0-25.5s)
#define CMD_SET_AUTO_DISPENSE       0x0D  // param1 = 1 on, 0 off (sensor control for pump)
#define CMD_SET_LED_SCHEDULE_TIME   0x0E  // param1 = 0 on-time, 1 off-time; param2 = quarter-hour of day (0-95, e.g. 67 = 4:45 PM)
#define CMD_PRINT_CHUNK             0x0F  // param1 = chunk index (0-based), param2 = total chunks; message = chunk payload; Main reassembles and prints
#define CMD_PRINT_CHUNK_START       0x10  // param2 = total chunks; TZT sends this first so Main pauses sensor before chunk 0

// Sensor data packet (Main ESP32 → TZT Display)
// Total size: ~32 bytes
#pragma pack(push, 1)  // Pack struct to avoid padding bytes affecting checksum
struct SensorDataPacket {
    uint8_t msgType;           // ESP_NOW_MSG_SENSOR_DATA
    uint32_t timestamp;        // millis() from main ESP32
    float moisturePercent;     // 0-100
    float sanitizerLevel;      // 0-100
    bool irDetected;           // true/false
    uint16_t ledOnMinute;      // LED schedule on-time, minutes since local midnight
    uint16_t ledOffMinute;     // LED schedule off-time, minutes since local midnight
    uint8_t ledBrightness;     // 0-255 current PWM output (0 = off)
    uint8_t ledLevel;          // 1-100 saved brightness level used when the LED is on
    bool isDispensing;         // true/false
    bool pumpRunning;          // true/false
    uint8_t sequenceNumber;    // For detecting lost packets
    bool autoDispense;         // true = IR sensor controls pump
    bool ledSchedule;          // true = night schedule controls LED
    uint8_t checksum;          // Simple checksum for data integrity
};
#pragma pack(pop)

// Command packet (TZT Display → Main ESP32)
// Total size: ~220 bytes (max)
#pragma pack(push, 1)  // Pack struct to avoid padding bytes affecting checksum
struct CommandPacket {
    uint8_t msgType;           // ESP_NOW_MSG_COMMAND
    uint8_t commandType;       // CMD_* constant
    uint8_t param1;            // For brightness, state, etc.
    uint8_t param2;            // Reserved
    char message[200];         // For print commands
    uint8_t sequenceNumber;    // For tracking
    uint8_t checksum;          // Simple checksum
};
#pragma pack(pop)

// Acknowledgment packet
#pragma pack(push, 1)  // Pack struct to avoid padding bytes affecting checksum
struct AckPacket {
    uint8_t msgType;           // ESP_NOW_MSG_ACK
    uint8_t ackForMsgType;     // Which message this ACKs
    uint8_t sequenceNumber;    // Sequence number being ACKed
    bool success;              // true if command succeeded
    uint8_t checksum;
};
#pragma pack(pop)

// Chunk ACK packet (Main → TZT): "chunk N received, send next"
#pragma pack(push, 1)
struct ChunkAckPacket {
    uint8_t msgType;    // ESP_NOW_MSG_CHUNK_ACK
    uint8_t chunkIndex; // 0-based index of chunk that was received
    uint8_t checksum;
};
#pragma pack(pop)

// Calculate simple checksum (inline to avoid multiple-definition when header is included in several .cpp files)
inline uint8_t calculateChecksum(uint8_t* data, size_t len) {
    uint8_t checksum = 0;
    for (size_t i = 0; i < len - 1; i++) {  // Exclude checksum byte
        checksum ^= data[i];
    }
    return checksum;
}

// Verify checksum
inline bool verifyChecksum(uint8_t* data, size_t len) {
    uint8_t calculated = calculateChecksum(data, len);
    return calculated == data[len - 1];
}

// ---- Peer helpers (shared by Main and TZT) ----

// False for an all-zero MAC (address not configured yet in config.h / config_tzt.h)
inline bool espNowMacIsSet(const uint8_t* mac) {
    for (int i = 0; i < 6; i++) if (mac[i] != 0) return true;
    return false;
}

// Channel a peer must use: the STA's current WiFi channel, or `fallback` (ESP_NOW_CHANNEL) when offline
inline uint8_t espNowPeerChannel(uint8_t fallback) {
    return (WiFi.status() == WL_CONNECTED && WiFi.channel() > 0) ? (uint8_t)WiFi.channel() : fallback;
}

// Make sure `mac` is registered as an unencrypted STA peer on `channel`: no-op when it already is,
// otherwise (missing, wrong channel or interface) it is (re)added. The struct is zeroed so ifidx etc.
// are not garbage (fixes "Peer interface is invalid").
inline esp_err_t espNowEnsurePeer(const uint8_t* mac, uint8_t channel) {
    esp_now_peer_info_t peer;
    if (esp_now_get_peer(mac, &peer) == ESP_OK) {
        if (peer.channel == channel && peer.ifidx == WIFI_IF_STA) return ESP_OK;
        esp_now_del_peer(mac);
    }
    memset(&peer, 0, sizeof(peer));
    memcpy(peer.peer_addr, mac, 6);
    peer.ifidx = WIFI_IF_STA;
    peer.channel = channel;
    peer.encrypt = false;
    return esp_now_add_peer(&peer);
}

// Human-readable name for a CMD_* constant, for logging (shared by Main and TZT
// to avoid the two copies drifting out of sync when a command is added/renamed).
inline const char* espNowCommandName(uint8_t cmd) {
    switch (cmd) {
        case CMD_PRINT: return "PRINT";
        case CMD_PRINT_CHUNK: return "PRINT_CHUNK";
        case CMD_PRINT_CHUNK_START: return "PRINT_CHUNK_START";
        case CMD_DISPENSE_START: return "DISPENSE_START";
        case CMD_DISPENSE_STOP: return "DISPENSE_STOP";
        case CMD_RESET_SANITIZER: return "RESET_SANITIZER";
        case CMD_SET_LED_BRIGHTNESS: return "SET_LED_BRIGHTNESS";
        case CMD_SET_LED_STATE: return "SET_LED_STATE";
        case CMD_SET_LED_SCHEDULE: return "SET_LED_SCHEDULE";
        case CMD_TEST_PRINTER: return "TEST_PRINTER";
        case CMD_TEST_PUMP: return "TEST_PUMP";
        case CMD_TEST_LED: return "TEST_LED";
        case CMD_SET_PUMP_DURATION: return "SET_PUMP_DURATION";
        case CMD_SET_PUMP_COOLDOWN: return "SET_PUMP_COOLDOWN";
        case CMD_SET_AUTO_DISPENSE: return "SET_AUTO_DISPENSE";
        case CMD_SET_LED_SCHEDULE_TIME: return "SET_LED_SCHEDULE_TIME";
        default: return "CMD_?";
    }
}

#endif // ESP_NOW_PROTOCOL_H
