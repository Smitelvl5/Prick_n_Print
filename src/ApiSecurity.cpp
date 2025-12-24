#include "ApiSecurity.h"

const char* ApiSecurity::TAG = "Security";

ApiSecurity::ApiSecurity() 
    : authEnabled(false), maxRequestsPerWindow(60), 
      windowDuration(60000), whitelistEnabled(false) {
}

ApiSecurity::~ApiSecurity() {
}

void ApiSecurity::setApiKey(const String& key) {
    apiKey = key;
    Logger::info(TAG, "API key configured");
}

void ApiSecurity::enableAuth(bool enable) {
    authEnabled = enable;
    Logger::info(TAG, "Authentication " + String(enable ? "enabled" : "disabled"));
}

void ApiSecurity::setRateLimit(int requestsPerMinute) {
    maxRequestsPerWindow = requestsPerMinute;
    Logger::info(TAG, "Rate limit set to " + String(requestsPerMinute) + " requests/minute");
}

void ApiSecurity::addToWhitelist(const String& ip) {
    whitelist.push_back(ip);
    Logger::info(TAG, "Added to whitelist: " + ip);
}

void ApiSecurity::enableWhitelist(bool enable) {
    whitelistEnabled = enable;
    Logger::info(TAG, "IP whitelist " + String(enable ? "enabled" : "disabled"));
}

bool ApiSecurity::validateApiKey(const String& key) const {
    if (!authEnabled) return true;
    return key == apiKey;
}

bool ApiSecurity::checkRateLimit(const String& clientIP) {
    unsigned long now = millis();
    
    auto it = clientLimits.find(clientIP);
    if (it == clientLimits.end()) {
        // First request from this IP
        RateLimitInfo info;
        info.windowStart = now;
        info.requestCount = 1;
        info.blocked = false;
        clientLimits[clientIP] = info;
        return true;
    }
    
    RateLimitInfo& info = it->second;
    
    // Check if window has expired
    if (now - info.windowStart > windowDuration) {
        info.windowStart = now;
        info.requestCount = 1;
        info.blocked = false;
        return true;
    }
    
    // Within window - check limit
    info.requestCount++;
    if (info.requestCount > maxRequestsPerWindow) {
        if (!info.blocked) {
            Logger::warn(TAG, "Rate limit exceeded for IP: " + clientIP);
            info.blocked = true;
        }
        return false;
    }
    
    return true;
}

bool ApiSecurity::isWhitelisted(const String& ip) const {
    if (!whitelistEnabled) return true;
    
    for (const String& whitelistedIP : whitelist) {
        if (whitelistedIP == ip) {
            return true;
        }
    }
    return false;
}

bool ApiSecurity::isAuthorized(const String& clientIP, const String& providedKey) {
    // Check whitelist first
    if (!isWhitelisted(clientIP)) {
        Logger::warn(TAG, "IP not whitelisted: " + clientIP);
        return false;
    }
    
    // Check rate limit
    if (!checkRateLimit(clientIP)) {
        return false;
    }
    
    // Check API key
    if (!validateApiKey(providedKey)) {
        Logger::warn(TAG, "Invalid API key from: " + clientIP);
        return false;
    }
    
    return true;
}

void ApiSecurity::resetRateLimit(const String& clientIP) {
    auto it = clientLimits.find(clientIP);
    if (it != clientLimits.end()) {
        clientLimits.erase(it);
        Logger::info(TAG, "Rate limit reset for: " + clientIP);
    }
}

void ApiSecurity::clearRateLimits() {
    clientLimits.clear();
    Logger::info(TAG, "All rate limits cleared");
}

String ApiSecurity::getSecurityReport() const {
    String report = "Security Status:\n";
    report += "  Auth: " + String(authEnabled ? "ENABLED" : "DISABLED") + "\n";
    report += "  Rate Limit: " + String(maxRequestsPerWindow) + " req/min\n";
    report += "  Whitelist: " + String(whitelistEnabled ? "ENABLED" : "DISABLED") + "\n";
    report += "  Whitelisted IPs: " + String(whitelist.size()) + "\n";
    report += "  Tracked Clients: " + String(clientLimits.size()) + "\n";
    return report;
}
