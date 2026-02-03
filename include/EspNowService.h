#ifndef ESP_NOW_SERVICE_H
#define ESP_NOW_SERVICE_H

#include <Arduino.h>
#include <functional>
#include <esp_now.h>
#include "EspNowProtocol.h"

// Forward declaration
struct CommandPacket;

// C-style callback functions (required by ESP-NOW)
void onDataSentStatic(const uint8_t* mac_addr, esp_now_send_status_t status);
void onDataRecvStatic(const uint8_t* mac_addr, const uint8_t* data, int len);

class EspNowService {
public:
    static EspNowService* instance;

    EspNowService();
    bool initialize(const uint8_t* peerMac);

    // Sensor data management
    bool sendSensorData();
    void updateSensorData(float moisture, float sanitizer, bool ir, float light,
                         uint8_t led, bool dispensing, bool pump, bool autoDispense, bool autoBrightness);

    // Command handling
    inline void setCommandCallback(std::function<void(CommandPacket*)> callback) {
        commandCallback = callback;
    }

    // ESP-NOW callbacks (called from static functions)
    void onDataSent(const uint8_t* mac_addr, esp_now_send_status_t status);
    void onDataRecv(const uint8_t* mac_addr, const uint8_t* data, int len);

    // Utility
    static String macToString(const uint8_t* mac);

    // State queries (inline for performance)
    inline bool isInitialized() const { return initialized; }
    inline bool shouldRetrySend() const {
        const unsigned long RETRY_DELAY_MS = 400;
        return lastSendFailed && (sendPendingSince != 0) && (millis() - sendPendingSince >= RETRY_DELAY_MS);
    }
    inline bool hasSendInFlight() const {
        return sendPendingSince != 0 && (millis() - sendPendingSince < 3000);
    }
    inline bool hasPendingChunkAck() const {
        return chunkAckQueueHead != chunkAckQueueTail;
    }

    // State management
    void clearRetryState();
    void sendPendingChunkAck();  // Call from main loop; sends one queued ACK per call

private:
    void handleCommand(CommandPacket* cmd);

    // Configuration
    uint8_t peerMacAddress[6];
    bool initialized;

    // Send state tracking
    uint8_t sequenceNumber;
    unsigned long lastSendTime;
    unsigned long sendPendingSince;    // When current send was queued (0 = none pending)
    bool lastSendFailed : 1;            // Bitfield: set by onDataSent(FAIL), cleared on SUCCESS or clearRetryState

    // CHUNK_ACK queue: never send from callback; enqueue and drain from loop to avoid NO_MEM
    static constexpr int CHUNK_ACK_QUEUE_SIZE = 8;
    struct ChunkAckSlot {
        uint8_t chunkIndex;
        uint8_t mac[6];
    } __attribute__((packed));

    ChunkAckSlot chunkAckQueue[CHUNK_ACK_QUEUE_SIZE];
    uint8_t chunkAckQueueHead;
    uint8_t chunkAckQueueTail;

    // Sensor data cache (packed + bitfields to reduce memory)
    struct SensorDataCache {
        float moisturePercent;
        float sanitizerLevel;
        float lightPercent;
        uint8_t ledBrightness;
        bool irDetected : 1;
        bool isDispensing : 1;
        bool pumpRunning : 1;
        bool autoDispense : 1;
        bool autoBrightness : 1;
    } __attribute__((packed)) lastSensorData;

    std::function<void(CommandPacket*)> commandCallback;
};

#endif // ESP_NOW_SERVICE_H
