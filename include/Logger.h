#ifndef LOGGER_H
#define LOGGER_H

#include <Arduino.h>
#include <WiFiUdp.h>

enum LogLevel {
    LOG_LEVEL_NONE = 0,
    LOG_LEVEL_ERROR = 1,
    LOG_LEVEL_WARN = 2,
    LOG_LEVEL_INFO = 3,
    LOG_LEVEL_DEBUG = 4,
    LOG_LEVEL_VERBOSE = 5
};

class Logger {
private:
    static LogLevel currentLevel;
    static const char* getLevelString(LogLevel level);
    static const char* getIcon(LogLevel level);

    // Network log broadcast (UDP) — off by default, opt in via enableNetworkLog()
    static WiFiUDP udp;
    static bool networkLogEnabled;
    static String deviceTag;
    static IPAddress broadcastAddr;
    static uint16_t broadcastPort;

public:
    static void setLevel(LogLevel level);
    static void log(LogLevel level, const String& tag, const String& message);

    // Broadcasts every subsequent log line as a UDP packet to 255.255.255.255:port,
    // in addition to Serial, so logs are visible without a USB connection (e.g. after OTA).
    // Call once after WiFi connects. deviceName tags each line so you can tell boards apart.
    static void enableNetworkLog(const String& deviceName, uint16_t port = 47269);

    // Convenience methods
    static void error(const String& tag, const String& message);
    static void warn(const String& tag, const String& message);
    static void info(const String& tag, const String& message);
    static void debug(const String& tag, const String& message);
    static void verbose(const String& tag, const String& message);
};

#endif // LOGGER_H
