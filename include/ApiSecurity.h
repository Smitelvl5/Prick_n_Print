#ifndef API_SECURITY_H
#define API_SECURITY_H

#include <Arduino.h>
#include <map>
#include <vector>
#include "Logger.h"

class ApiSecurity {
private:
    static const char* TAG;
    String apiKey;
    bool authEnabled;
    
    // Rate limiting
    struct RateLimitInfo {
        unsigned long windowStart;
        int requestCount;
        bool blocked;
    };
    std::map<String, RateLimitInfo> clientLimits;
    int maxRequestsPerWindow;
    unsigned long windowDuration;
    
    // IP Whitelist (optional)
    std::vector<String> whitelist;
    bool whitelistEnabled;
    
public:
    ApiSecurity();
    ~ApiSecurity();
    
    // Configuration
    void setApiKey(const String& key);
    void enableAuth(bool enable);
    void setRateLimit(int requestsPerMinute);
    void addToWhitelist(const String& ip);
    void enableWhitelist(bool enable);
    
    // Validation
    bool validateApiKey(const String& key) const;
    bool checkRateLimit(const String& clientIP);
    bool isWhitelisted(const String& ip) const;
    bool isAuthorized(const String& clientIP, const String& providedKey);
    
    // Management
    void resetRateLimit(const String& clientIP);
    void clearRateLimits();
    String getSecurityReport() const;
};

#endif // API_SECURITY_H
