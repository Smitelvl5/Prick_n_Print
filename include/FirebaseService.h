#ifndef FIREBASE_SERVICE_H
#define FIREBASE_SERVICE_H

#include <Arduino.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include "Logger.h"
#include "IBackendService.h"

class FirebaseService : public IBackendService {
private:
    static const char* TAG;
    String databaseUrl;
    String authToken;  // Optional authentication token
    int timeout;
    int retryCount;
    int retryDelay;
    
    // Rate limiting
    unsigned long lastRequest;
    int requestCount;
    unsigned long rateLimitWindow;
    int initialRequestsAllowed;  // Allow first N requests to bypass rate limit during boot
    
    bool isRateLimited();
    bool executeRequest(HTTPClient& http, const String& method, const String& payload = "");
    
public:
    FirebaseService(const String& url, int timeoutMs = 10000);
    ~FirebaseService();
    
    // Configuration
    void setRetryPolicy(int count, int delayMs) override;
    void setRateLimit(int requestsPerMinute) override;
    void setAuthToken(const String& token);  // Set authentication token (optional)
    
    // CRUD operations with error handling
    bool get(const String& path, String& response) override;
    bool put(const String& path, const String& data) override;
    bool deleteData(const String& path) override;
    bool post(const String& path, const String& data);
    
    // Specialized operations (config = settings only; no sensor/status data written to Firebase)
    bool loadConfig(DynamicJsonDocument& doc) override;
    bool saveConfig(const DynamicJsonDocument& doc) override;
    bool pollCommands(DynamicJsonDocument& commands) override;
    
    // Health check
    bool isHealthy() override;
    String getLastError() const override { return lastError; }
    
private:
    String lastError;
};

#endif // FIREBASE_SERVICE_H
