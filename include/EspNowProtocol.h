#ifndef ESP_NOW_PROTOCOL_H
#define ESP_NOW_PROTOCOL_H

#include <Arduino.h>

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
#define CMD_SET_LED_BRIGHTNESS      0x05
#define CMD_SET_LED_STATE           0x06
#define CMD_SET_AUTO_BRIGHTNESS     0x07
#define CMD_TEST_PRINTER            0x08
#define CMD_TEST_PUMP               0x09
#define CMD_TEST_LED                0x0A
#define CMD_SET_PUMP_DURATION       0x0B  // param1 = duration ms/100 (0-255 = 0-25.5s)
#define CMD_SET_PUMP_COOLDOWN       0x0C  // param1 = cooldown ms/100 (0-255 = 0-25.5s)
#define CMD_SET_AUTO_DISPENSE       0x0D  // param1 = 1 on, 0 off (sensor control for pump)
#define CMD_SET_LIGHT_CALIBRATION   0x0E  // param1 = 0 record current light as min (turn off), 1 as max (turn on); Main reads sensor
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
    float lightPercent;        // 0-100
    uint8_t ledBrightness;     // 0-255
    bool isDispensing;         // true/false
    bool pumpRunning;          // true/false
    uint8_t sequenceNumber;    // For detecting lost packets
    bool autoDispense;         // true = IR sensor controls pump
    bool autoBrightness;       // true = light sensor controls LED
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

#endif // ESP_NOW_PROTOCOL_H
