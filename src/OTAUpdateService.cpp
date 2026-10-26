/*
 * OTAUpdateService - Over-the-air firmware updates via ArduinoOTA
 */

#include "OTAUpdateService.h"
#include "Logger.h"
#include <ArduinoOTA.h>
#include <WiFi.h>
#include <esp_task_wdt.h>

const char* OTAUpdateService::TAG = "OTA";

OTAUpdateService::OTAUpdateService()
    : enabled(false), hostname(""), password("") {
}

OTAUpdateService::~OTAUpdateService() {
    ArduinoOTA.end();
}

bool OTAUpdateService::initialize(const String& deviceHostname, const String& otaPassword) {
    hostname = deviceHostname;
    password = otaPassword;
    enabled = true;

    ArduinoOTA.setHostname(hostname.c_str());
    if (password.length() > 0) {
        ArduinoOTA.setPassword(password.c_str());
    }

    ArduinoOTA.onStart(onStart);
    ArduinoOTA.onEnd(onEnd);
    ArduinoOTA.onProgress(onProgress);
    ArduinoOTA.onError(onError);

    // Both boards are mains-powered. Modem sleep (the ESP32 default) adds 100-300 ms per round trip,
    // which stretches a 1.6 MB upload past the loop watchdog and kills it partway.
    WiFi.setSleep(false);

    ArduinoOTA.begin();
    Logger::info(TAG, "OTA ready - hostname: " + hostname);
    return true;
}

void OTAUpdateService::setPassword(const String& pwd) {
    password = pwd;
    if (enabled && password.length() > 0) {
        ArduinoOTA.setPassword(password.c_str());
    }
}

void OTAUpdateService::enable(bool state) {
    enabled = state;
    if (!enabled) {
        ArduinoOTA.end();
    } else {
        ArduinoOTA.begin();
    }
}

void OTAUpdateService::handle() {
    if (enabled) {
        ArduinoOTA.handle();
    }
}

void OTAUpdateService::onStart() {
    Logger::info(TAG, "OTA update starting...");
}

void OTAUpdateService::onEnd() {
    Logger::info(TAG, "OTA update complete");
}

void OTAUpdateService::onProgress(unsigned int progress, unsigned int total) {
    // The whole upload runs inside ArduinoOTA.handle() on the loop task, which the task watchdog
    // watches (60 s on the TZT); feed it per chunk or a slow upload reboots the board mid-flash.
    esp_task_wdt_reset();
    static unsigned long lastLog = 0;
    if (millis() - lastLog > 500) {
        Logger::debug(TAG, "OTA progress: " + String((progress * 100) / total) + "%");
        lastLog = millis();
    }
}

void OTAUpdateService::onError(ota_error_t error) {
    const char* msg = "";
    switch (error) {
        case OTA_AUTH_ERROR:    msg = "Auth failed"; break;
        case OTA_BEGIN_ERROR:    msg = "Begin failed"; break;
        case OTA_CONNECT_ERROR:  msg = "Connect failed"; break;
        case OTA_RECEIVE_ERROR:  msg = "Receive failed"; break;
        case OTA_END_ERROR:      msg = "End failed"; break;
        default:                 msg = "Unknown error"; break;
    }
    Logger::error(TAG, "OTA error: " + String(msg));
}
