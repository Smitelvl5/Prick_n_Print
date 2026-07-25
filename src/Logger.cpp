#include "Logger.h"
#include <WiFi.h>

LogLevel Logger::currentLevel = LOG_LEVEL_INFO;
WiFiUDP Logger::udp;
bool Logger::networkLogEnabled = false;
String Logger::deviceTag = "";
IPAddress Logger::broadcastAddr(255, 255, 255, 255);
uint16_t Logger::broadcastPort = 47269;

const char* Logger::getLevelString(LogLevel level) {
    switch(level) {
        case LOG_LEVEL_ERROR: return "ERROR";
        case LOG_LEVEL_WARN: return "WARN";
        case LOG_LEVEL_INFO: return "INFO";
        case LOG_LEVEL_DEBUG: return "DEBUG";
        case LOG_LEVEL_VERBOSE: return "VERBOSE";
        default: return "UNKNOWN";
    }
}

const char* Logger::getIcon(LogLevel level) {
    switch(level) {
        case LOG_LEVEL_ERROR: return "❌";
        case LOG_LEVEL_WARN: return "⚠️";
        case LOG_LEVEL_INFO: return "ℹ️";
        case LOG_LEVEL_DEBUG: return "🔍";
        case LOG_LEVEL_VERBOSE: return "📝";
        default: return "  ";
    }
}

void Logger::setLevel(LogLevel level) {
    currentLevel = level;
}

void Logger::enableNetworkLog(const String& deviceName, uint16_t port) {
    deviceTag = deviceName;
    broadcastPort = port;
    networkLogEnabled = true;
    udp.begin(0);  // Ephemeral local port; we only ever send.
}

void Logger::log(LogLevel level, const String& tag, const String& message) {
    if (level > currentLevel) return;

    char timestamp[9];
    unsigned long ms = millis();
    snprintf(timestamp, sizeof(timestamp), "%02lu:%02lu:%02lu",
             (ms / 3600000) % 24,
             (ms / 60000) % 60,
             (ms / 1000) % 60);

    Serial.printf("[%s] %s %s: %s\n",
                  timestamp,
                  getIcon(level),
                  tag.c_str(),
                  message.c_str());

    if (networkLogEnabled && WiFi.status() == WL_CONNECTED) {
        String line = "[" + String(deviceTag) + "][" + String(timestamp) + "] " +
                      getLevelString(level) + " " + tag + ": " + message;
        udp.beginPacket(broadcastAddr, broadcastPort);
        udp.write((const uint8_t*)line.c_str(), line.length());
        udp.endPacket();
    }
}

void Logger::error(const String& tag, const String& message) {
    log(LOG_LEVEL_ERROR, tag, message);
}

void Logger::warn(const String& tag, const String& message) {
    log(LOG_LEVEL_WARN, tag, message);
}

void Logger::info(const String& tag, const String& message) {
    log(LOG_LEVEL_INFO, tag, message);
}

void Logger::debug(const String& tag, const String& message) {
    log(LOG_LEVEL_DEBUG, tag, message);
}

void Logger::verbose(const String& tag, const String& message) {
    log(LOG_LEVEL_VERBOSE, tag, message);
}
