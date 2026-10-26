#ifndef HARDWARE_ABSTRACTION_H
#define HARDWARE_ABSTRACTION_H

#include <Arduino.h>
#include <HardwareSerial.h>
#include "config.h"
#include "Logger.h"

// Hardware Abstraction Layer for Print-n-Prick
class HardwareAbstraction {
private:
    static const char* TAG;
    HardwareSerial* printerSerial;
    
    // Pin states
    bool pumpState;
    
    // Sensor readings
    float moisturePercent;
    bool irDetected;
    uint8_t ledBrightness;
    // LED night schedule: on at ledOnMinute, off at ledOffMinute (minutes since local midnight)
    bool ledScheduleEnabled;
    uint16_t ledOnMinute;
    uint16_t ledOffMinute;
    int8_t ledScheduleLastWindow;  // -1 = not yet applied; 0/1 = last in-window state acted on
    uint8_t ledLevelPct;           // brightness used whenever the LED turns on (1-100%)
    
    // PWM ramp-up state (1V per second)
    bool rampActive;
    uint8_t rampStartPWM;
    uint8_t rampTargetPWM;
    uint8_t currentPWM;  // Track current PWM value
    unsigned long rampStartTime;
    
    // Sanitizer tracking
    float sanitizerLevel;
    int totalDispenses;
    unsigned long lastDispenseTime;
    unsigned long dispenseStartTime;
    bool dispensing;
    // Runtime config (0 = use config.h defaults)
    unsigned long maxDispenseDurationMs_;
    unsigned long dispenseCooldownMs_;

    // Persist sanitizer level + dispense count to NVS so they survive reboots
    void loadSanitizerState();
    void saveSanitizerState();
    // Persist LED schedule (enabled + on/off times) to NVS
    void loadLedSchedule();
    void saveLedSchedule();

public:
    HardwareAbstraction();
    ~HardwareAbstraction();
    
    // Initialization
    bool initialize();
    bool initializePins();
    bool initializePrinter();
    
    // Pump Control
    bool startPump();
    bool stopPump();
    bool isPumpRunning() const { return pumpState; }
    bool isDispensing() const { return dispensing; }
    unsigned long getDispenseDuration() const;
    bool checkDispenseTimeout();
    bool checkCooldown() const;
    void setMaxDispenseDurationMs(unsigned long ms);  // 0 = use MAX_DISPENSE_DURATION_MS
    void setDispenseCooldownMs(unsigned long ms);    // 0 = use DISPENSE_COOLDOWN_MS
    unsigned long getMaxDispenseDurationMs() const { return maxDispenseDurationMs_; }
    unsigned long getDispenseCooldownMs() const { return dispenseCooldownMs_; }
    
    // Sensor Reading
    float readMoistureSensor();
    bool readIRSensor();
    float getMoisturePercent() const { return moisturePercent; }
    bool isIRDetected() const { return irDetected; }
    
    // LED Control: night schedule switches the LED at the on/off times; manual changes
    // hold until the next scheduled switch
    void setLEDBrightness(uint8_t brightness);  // 0-255 (with soft ramp-up)
    uint8_t getLEDBrightness() const { return ledBrightness; }
    void updateLEDSchedule();                    // call from loop; acts only when the window changes
    void reapplyLEDSchedule();                   // force LED to match the schedule now (e.g. after a test pulse)
    void setLedScheduleEnabled(bool enabled);
    bool isLedScheduleEnabled() const { return ledScheduleEnabled; }
    bool setLedScheduleTimes(uint16_t onMinute, uint16_t offMinute);  // false if out of range
    uint16_t getLedOnMinute() const { return ledOnMinute; }
    uint16_t getLedOffMinute() const { return ledOffMinute; }
    static String formatMinute(uint16_t minute);  // e.g. 1005 -> "4:45 PM"
    // Brightness level (1-100%) used by the schedule and manual "on"; applied immediately if the LED is on
    void setLedLevel(uint8_t pct);
    uint8_t getLedLevel() const { return ledLevelPct; }
    uint8_t getLedOnPwm() const;  // ledLevelPct -> 0-255 with perceptual (gamma 2) curve
    void updateLEDRamp();  // Update PWM ramp-up (call in main loop)
    
    // Sanitizer Management
    float getSanitizerLevel() const { return sanitizerLevel; }
    void setSanitizerLevel(float level);
    void updateSanitizerLevel(float amount);
    int getTotalDispenses() const { return totalDispenses; }
    void resetSanitizer();
    
    // Printer Operations
    bool printerWrite(const uint8_t* data, size_t length);
    bool printerWrite(uint8_t data);
    bool printerWriteString(const String& str);
    bool printerPrintln(const String& str);
    bool printerAvailable() const;
    bool printerFeed(uint8_t lines);  // ESC d n - feed n lines, same as holding the printer's FEED button
    
    // Diagnostic
    void printDiagnostics() const;
    String getStatusJSON() const;
};

#endif // HARDWARE_ABSTRACTION_H
