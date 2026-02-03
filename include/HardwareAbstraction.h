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
    float lightPercent;
    uint8_t ledBrightness;
    // Light calibration for auto-brightness: min = "turn off" level, max = "turn on" level (< 0 = not set)
    float lightCalibMinPercent;
    float lightCalibMaxPercent;
    bool autoBrightnessEnabled;  // Flag to enable/disable automatic brightness control
    
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
    float readLightSensor();
    float getMoisturePercent() const { return moisturePercent; }
    bool isIRDetected() const { return irDetected; }
    float getLightPercent() const { return lightPercent; }
    /** Set current light level as calibration min (dark / LED off) or max (bright / LED on). */
    void setLightCalibrationMin(float lightPercent);
    void setLightCalibrationMax(float lightPercent);
    
    // LED Brightness Control (inverse of light level)
    void updateLEDBrightness();
    void setLEDBrightness(uint8_t brightness);  // 0-255 (with soft ramp-up)
    void setAutoBrightness(bool enabled);  // Enable/disable automatic brightness control
    bool isAutoBrightnessEnabled() const { return autoBrightnessEnabled; }
    uint8_t getLEDBrightness() const { return ledBrightness; }
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
    
    // Diagnostic
    void printDiagnostics() const;
    String getStatusJSON() const;
};

#endif // HARDWARE_ABSTRACTION_H
