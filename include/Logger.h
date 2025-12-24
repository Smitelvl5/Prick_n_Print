#ifndef LOGGER_H
#define LOGGER_H

#include <Arduino.h>

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
    
public:
    static void setLevel(LogLevel level);
    static void log(LogLevel level, const String& tag, const String& message);
    
    // Convenience methods
    static void error(const String& tag, const String& message);
    static void warn(const String& tag, const String& message);
    static void info(const String& tag, const String& message);
    static void debug(const String& tag, const String& message);
    static void verbose(const String& tag, const String& message);
};

#endif // LOGGER_H
