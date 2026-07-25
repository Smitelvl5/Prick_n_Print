#ifndef I_BACKEND_SERVICE_H
#define I_BACKEND_SERVICE_H

#include <Arduino.h>
#include <ArduinoJson.h>

/**
 * Backend interface for commands, audio, images, reminders.
 * Implemented by HttpBackendService (HTTP server).
 */
class IBackendService {
public:
    virtual ~IBackendService() = default;

    virtual bool get(const String& path, String& response) = 0;
    virtual bool put(const String& path, const String& data) = 0;
    virtual bool deleteData(const String& path) = 0;
    virtual bool pollCommands(DynamicJsonDocument& commands) = 0;

    virtual void setRetryPolicy(int count, int delayMs) { (void)count; (void)delayMs; }
    virtual void setRateLimit(int requestsPerMinute) { (void)requestsPerMinute; }
    virtual bool loadConfig(DynamicJsonDocument& doc) { (void)doc; return false; }
    virtual bool saveConfig(const DynamicJsonDocument& doc) { (void)doc; return false; }
    virtual bool isHealthy() = 0;
    virtual String getLastError() const = 0;
    /** Optional: POST to /api/devices/heartbeat. Default no-op returns false. */
    virtual bool sendHeartbeat(const String& deviceId, const String& name) { (void)deviceId; (void)name; return false; }
};

#endif // I_BACKEND_SERVICE_H
