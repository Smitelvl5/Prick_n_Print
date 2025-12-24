#include "OTAUpdateService.h"

const char* OTAUpdateService::TAG = "OTA";

OTAUpdateService::OTAUpdateService() 
    : enabled(false) {
}

OTAUpdateService::~OTAUpdateService() {
}

bool OTAUpdateService::initialize(const String& deviceHostname, const String& otaPassword) {
    hostname = deviceHostname;
    password = otaPassword;
    
    ArduinoOTA.setHostname(hostname.c_str());
    
    if (password.length() > 0) {
        ArduinoOTA.setPassword(password.c_str());
        Logger::info(TAG, "OTA password protection enabled");
    }
    
    ArduinoOTA.onStart(onStart);
    ArduinoOTA.onEnd(onEnd);
    ArduinoOTA.onProgress(onProgress);
    ArduinoOTA.onError(onError);
    
    ArduinoOTA.begin();
    enabled = true;
    
    Logger::info(TAG, "OTA updates enabled");
    Logger::info(TAG, "  Hostname: " + hostname);
    Logger::info(TAG, "  Version: " + String(FIRMWARE_VERSION));
    
    return true;
}

void OTAUpdateService::setPassword(const String& pwd) {
    password = pwd;
    ArduinoOTA.setPassword(pwd.c_str());
    Logger::info(TAG, "OTA password updated");
}

void OTAUpdateService::enable(bool state) {
    enabled = state;
    Logger::info(TAG, "OTA " + String(state ? "enabled" : "disabled"));
}

void OTAUpdateService::handle() {
    if (enabled) {
        ArduinoOTA.handle();
    }
}

void OTAUpdateService::onStart() {
    String type;
    if (ArduinoOTA.getCommand() == U_FLASH) {
        type = "sketch";
    } else {
        type = "filesystem";
    }
    Logger::info(TAG, "Starting OTA update: " + type);
}

void OTAUpdateService::onEnd() {
    Logger::info(TAG, "OTA update completed");
}

void OTAUpdateService::onProgress(unsigned int progress, unsigned int total) {
    static int lastPercent = -1;
    int percent = (progress * 100) / total;
    
    if (percent != lastPercent && percent % 10 == 0) {
        Logger::info(TAG, "OTA Progress: " + String(percent) + "%");
        lastPercent = percent;
    }
}

void OTAUpdateService::onError(ota_error_t error) {
    String errorMsg;
    switch(error) {
        case OTA_AUTH_ERROR: errorMsg = "Auth Failed"; break;
        case OTA_BEGIN_ERROR: errorMsg = "Begin Failed"; break;
        case OTA_CONNECT_ERROR: errorMsg = "Connect Failed"; break;
        case OTA_RECEIVE_ERROR: errorMsg = "Receive Failed"; break;
        case OTA_END_ERROR: errorMsg = "End Failed"; break;
        default: errorMsg = "Unknown Error"; break;
    }
    Logger::error(TAG, "OTA Error: " + errorMsg);
}
