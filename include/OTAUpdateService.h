#ifndef OTA_UPDATE_SERVICE_H
#define OTA_UPDATE_SERVICE_H

#include <Arduino.h>
#include <ArduinoOTA.h>
#include "Logger.h"
#include "version.h"

class OTAUpdateService {
private:
    static const char* TAG;
    bool enabled;
    String hostname;
    String password;
    
    static void onStart();
    static void onEnd();
    static void onProgress(unsigned int progress, unsigned int total);
    static void onError(ota_error_t error);
    
public:
    OTAUpdateService();
    ~OTAUpdateService();
    
    // Setup
    bool initialize(const String& deviceHostname, const String& otaPassword = "");
    void setPassword(const String& pwd);
    void enable(bool state);
    
    // Runtime
    void handle();
    bool isEnabled() const { return enabled; }
    String getHostname() const { return hostname; }
    String getFirmwareVersion() const { return String(FIRMWARE_VERSION); }
};

#endif // OTA_UPDATE_SERVICE_H
