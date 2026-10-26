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
    void updateSensorData(float moisture, float sanitizer, bool ir, uint16_t ledOnMinute, uint16_t ledOffMinute,
                         uint8_t led, uint8_t ledLevel, bool dispensing, bool pump, bool autoDispense, bool ledSchedule);

    // True when the cached state differs from what the display last received (see updateSensorData);
    // main.cpp sends right away instead of waiting for the heartbeat
    inline bool hasUnsentChange() const { return dataDirty; }

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

    // Link health counters for the bench page (written from the WiFi-task callbacks; read-only elsewhere)
    struct LinkStats {
        uint32_t sentOk;
        uint32_t sentFail;
        uint32_t commandsReceived;
        unsigned long lastSendOkMs;   // 0 = never
        unsigned long lastCommandMs;  // 0 = never
    };
    inline LinkStats getLinkStats() const {
        return { statSentOk, statSentFail, statCommandsReceived, statLastSendOkMs, statLastCommandMs };
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

    volatile uint32_t statSentOk = 0;
    volatile uint32_t statSentFail = 0;
    volatile uint32_t statCommandsReceived = 0;
    volatile unsigned long statLastSendOkMs = 0;
    volatile unsigned long statLastCommandMs = 0;

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
        uint16_t ledOnMinute;
        uint16_t ledOffMinute;
        uint8_t ledBrightness;
        uint8_t ledLevel;
        bool irDetected : 1;
        bool isDispensing : 1;
        bool pumpRunning : 1;
        bool autoDispense : 1;
        bool ledSchedule : 1;
    } __attribute__((packed)) lastSensorData;

    SensorDataCache lastSentData;   // what the display last received; compared in updateSensorData
    bool hasSentData = false;
    bool dataDirty = false;
    bool differsFromLastSent() const;

    // Duplicate-command filter: the same sequence number from the same sender within a few seconds
    // is a resend whose ACK got lost - ACK it again, but don't run it twice
    uint8_t lastCmdMac[6] = {0};
    uint8_t lastCmdSeq = 0;
    unsigned long lastCmdSeqMs = 0;

    std::function<void(CommandPacket*)> commandCallback;
};

#endif // ESP_NOW_SERVICE_H
