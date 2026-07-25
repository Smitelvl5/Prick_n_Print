#ifndef HTTP_BACKEND_SERVICE_H
#define HTTP_BACKEND_SERVICE_H

#include <Arduino.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include "Logger.h"
#include "IBackendService.h"

/**
 * HTTP backend: implements IBackendService and talks to the web server.
 * Use BACKEND_URL in config (e.g. http://192.168.1.100:5000) and set USE_HTTP_BACKEND.
 */
class HttpBackendService : public IBackendService {
private:
    static const char* TAG;
    String baseUrl;  // e.g. http://192.168.1.100:5000 (no trailing slash)
    int timeout;
    int retryCount;
    int retryDelay;
    unsigned long lastRequest;
    unsigned long rateLimitWindow;
    int initialRequestsAllowed;

    bool isRateLimited();
    bool executeRequest(HTTPClient& http, const String& method, const String& payload = "");
    String pathToApiPath(const String& path, bool isGet);

public:
    HttpBackendService(const String& url, int timeoutMs = 10000);
    ~HttpBackendService() override;

    void setRetryPolicy(int count, int delayMs) override;
    void setRateLimit(int requestsPerMinute) override;

    bool get(const String& path, String& response) override;
    bool put(const String& path, const String& data) override;
    bool deleteData(const String& path) override;
    bool pollCommands(DynamicJsonDocument& commands) override;

    bool loadConfig(DynamicJsonDocument& doc) override;
    bool saveConfig(const DynamicJsonDocument& doc) override;
    bool isHealthy() override;
    bool sendHeartbeat(const String& deviceId, const String& name) override;
    String getLastError() const override { return lastError; }

private:
    String lastError;
};

#endif // HTTP_BACKEND_SERVICE_H
