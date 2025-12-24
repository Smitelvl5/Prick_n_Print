#ifndef FIREBASE_SERVICE_H
#define FIREBASE_SERVICE_H

#include <Arduino.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include "Logger.h"
#include "config.h"

class FirebaseService {
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
    void setRetryPolicy(int count, int delayMs);
    void setRateLimit(int requestsPerMinute);
    void setAuthToken(const String& token);  // Set authentication token (optional)
    
    // CRUD operations with error handling
    bool get(const String& path, String& response);
    bool put(const String& path, const String& data);
    bool post(const String& path, const String& data);
    bool deleteData(const String& path);
    
    // Specialized operations
    bool loadConfig(DynamicJsonDocument& doc);
    bool saveConfig(const DynamicJsonDocument& doc);
    bool updateStatus(const DynamicJsonDocument& status);
    bool pollCommands(DynamicJsonDocument& commands);
    
    // Health check
    bool isHealthy();
    String getLastError() const { return lastError; }
    
private:
    String lastError;
};

#endif // FIREBASE_SERVICE_H
