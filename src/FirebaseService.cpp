#include "FirebaseService.h"

const char* FirebaseService::TAG = "Firebase";

FirebaseService::FirebaseService(const String& url, int timeoutMs) 
    : databaseUrl(url), authToken(""), timeout(timeoutMs), retryCount(3), retryDelay(1000),
      lastRequest(0), requestCount(0), rateLimitWindow(60000), initialRequestsAllowed(5) {
}

FirebaseService::~FirebaseService() {
}

void FirebaseService::setRetryPolicy(int count, int delayMs) {
    retryCount = count;
    retryDelay = delayMs;
    Logger::debug(TAG, "Retry policy set: " + String(count) + " retries, " + String(delayMs) + "ms delay");
}

void FirebaseService::setRateLimit(int requestsPerMinute) {
    // Use a more lenient window - allow requests every 2 seconds minimum
    unsigned long calculatedWindow = (unsigned long)(60000 / requestsPerMinute);
    rateLimitWindow = (calculatedWindow < 2000) ? 2000 : calculatedWindow;
    Logger::debug(TAG, "Rate limit set: " + String(requestsPerMinute) + " requests/minute (window: " + String(rateLimitWindow) + "ms)");
}

void FirebaseService::setAuthToken(const String& token) {
    authToken = token;
    if (token.length() > 0) {
        Logger::info(TAG, "Authentication token configured");
    } else {
        Logger::info(TAG, "Authentication token cleared (using public access)");
    }
}

bool FirebaseService::isRateLimited() {
    // Allow first few requests during boot to bypass rate limiting
    if (initialRequestsAllowed > 0) {
        initialRequestsAllowed--;
        Logger::debug(TAG, "Initial request allowed (" + String(initialRequestsAllowed) + " remaining)");
        return false;
    }
    
    unsigned long now = millis();
    unsigned long timeSinceLastRequest = now - lastRequest;
    
    if (timeSinceLastRequest < rateLimitWindow) {
        unsigned long waitTime = rateLimitWindow - timeSinceLastRequest;
        Logger::debug(TAG, "Rate limit: waiting " + String(waitTime) + "ms (last request was " + String(timeSinceLastRequest) + "ms ago)");
        delay(waitTime);  // Wait instead of blocking
        return false;  // After delay, allow the request
    }
    return false;
}

bool FirebaseService::executeRequest(HTTPClient& http, const String& method, const String& payload) {
    int httpCode = -1;
    
    for (int attempt = 0; attempt < retryCount; attempt++) {
        if (attempt > 0) {
            Logger::info(TAG, "Retry attempt " + String(attempt + 1) + "/" + String(retryCount));
            delay(retryDelay);
        }
        
        if (method == "GET") {
            httpCode = http.GET();
        } else if (method == "PUT") {
            httpCode = http.PUT(payload);
        } else if (method == "POST") {
            httpCode = http.POST(payload);
        } else if (method == "DELETE") {
            httpCode = http.sendRequest("DELETE");
        }
        
        if (httpCode == HTTP_CODE_OK || httpCode == HTTP_CODE_CREATED) {
            lastRequest = millis();
            return true;
        }
        
        if (httpCode == 401) {
            lastError = "Unauthorized - Check Firebase security rules or authentication token";
            Logger::error(TAG, lastError);
            Logger::error(TAG, "   Solution: Update Firebase Realtime Database security rules");
            Logger::error(TAG, "   See FIREBASE_SETUP.md for instructions");
            return false;  // Don't retry on auth errors
        } else if (httpCode == 429) {
            lastError = "Rate limit exceeded";
            Logger::warn(TAG, lastError);
            delay(retryDelay * 2);  // Longer delay for rate limits
        } else if (httpCode == 403) {
            lastError = "Permission denied / Quota exceeded";
            Logger::error(TAG, lastError);
            Logger::error(TAG, "   Check Firebase quota: https://console.firebase.google.com/");
            return false;  // Don't retry on permission errors
        }
    }
    
    lastError = "Request failed after " + String(retryCount) + " attempts. HTTP code: " + String(httpCode);
    Logger::error(TAG, lastError);
    return false;
}

bool FirebaseService::get(const String& path, String& response) {
    if (isRateLimited()) return false;
    
    HTTPClient http;
    String url = databaseUrl + path;
    
    // Add authentication token if configured
    if (authToken.length() > 0) {
        url += "?auth=" + authToken;
    }
    
    http.begin(url);
    http.setTimeout(timeout);
    
    Logger::debug(TAG, "GET " + path);
    
    if (executeRequest(http, "GET")) {
        response = http.getString();
        http.end();
        Logger::debug(TAG, "GET success (" + String(response.length()) + " bytes)");
        return true;
    }
    
    http.end();
    return false;
}

bool FirebaseService::put(const String& path, const String& data) {
    if (isRateLimited()) return false;
    
    HTTPClient http;
    String url = databaseUrl + path;
    
    // Add authentication token if configured
    if (authToken.length() > 0) {
        url += "?auth=" + authToken;
    }
    
    http.begin(url);
    http.addHeader("Content-Type", "application/json");
    http.setTimeout(timeout);
    
    Logger::debug(TAG, "PUT " + path);
    
    bool success = executeRequest(http, "PUT", data);
    http.end();
    
    if (success) {
        Logger::debug(TAG, "PUT success");
    }
    
    return success;
}

bool FirebaseService::post(const String& path, const String& data) {
    if (isRateLimited()) return false;
    
    HTTPClient http;
    String url = databaseUrl + path;
    
    // Add authentication token if configured
    if (authToken.length() > 0) {
        url += "?auth=" + authToken;
    }
    
    http.begin(url);
    http.addHeader("Content-Type", "application/json");
    http.setTimeout(timeout);
    
    Logger::debug(TAG, "POST " + path);
    
    bool success = executeRequest(http, "POST", data);
    http.end();
    
    if (success) {
        Logger::debug(TAG, "POST success");
    }
    
    return success;
}

bool FirebaseService::deleteData(const String& path) {
    if (isRateLimited()) return false;
    
    HTTPClient http;
    String url = databaseUrl + path;
    
    // Add authentication token if configured
    if (authToken.length() > 0) {
        url += "?auth=" + authToken;
    }
    
    http.begin(url);
    http.setTimeout(timeout);
    
    Logger::debug(TAG, "DELETE " + path);
    
    bool success = executeRequest(http, "DELETE");
    http.end();
    
    if (success) {
        Logger::debug(TAG, "DELETE success");
    }
    
    return success;
}

bool FirebaseService::loadConfig(DynamicJsonDocument& doc) {
    String response;
    if (!get("/config.json", response)) {
        return false;
    }
    
    if (response == "null" || response.length() == 0) {
        Logger::info(TAG, "No config found, using defaults");
        return false;
    }
    
    DeserializationError error = deserializeJson(doc, response);
    if (error) {
        lastError = "Failed to parse config: " + String(error.c_str());
        Logger::error(TAG, lastError);
        return false;
    }
    
    Logger::info(TAG, "Config loaded successfully");
    return true;
}

bool FirebaseService::saveConfig(const DynamicJsonDocument& doc) {
    String data;
    serializeJson(doc, data);
    
    if (put("/config.json", data)) {
        Logger::info(TAG, "Config saved successfully");
        return true;
    }
    return false;
}

bool FirebaseService::updateStatus(const DynamicJsonDocument& status) {
    String data;
    serializeJson(status, data);
    
    if (put("/status.json", data)) {
        Logger::debug(TAG, "Status updated");
        return true;
    }
    return false;
}

bool FirebaseService::pollCommands(DynamicJsonDocument& commands) {
    String response;
    if (!get("/commands.json", response)) {
        return false;
    }
    
    if (response == "null" || response.length() == 0) {
        Logger::debug(TAG, "No commands available");
        return true;  // Not an error, just no commands
    }
    
    DeserializationError error = deserializeJson(commands, response);
    if (error) {
        lastError = "Failed to parse commands: " + String(error.c_str());
        Logger::error(TAG, lastError);
        return false;
    }
    
    Logger::debug(TAG, "Commands retrieved: " + String(commands.size()) + " items");
    return true;
}

bool FirebaseService::isHealthy() {
    String response;
    return get("/.json", response);
}
