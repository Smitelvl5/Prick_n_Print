#include "Logger.h"

LogLevel Logger::currentLevel = LOG_LEVEL_INFO;

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
