#ifndef HARDWARE_ABSTRACTION_H
#define HARDWARE_ABSTRACTION_H

#include <Arduino.h>
#include <HardwareSerial.h>
#include "config.h"
#include "Logger.h"

// Hardware Abstraction Layer for Print_n_Prick
class HardwareAbstraction {
private:
    static const char* TAG;
    HardwareSerial* printerSerial;
    
    // Pin states
    bool ledState;
    bool pumpState;
    
    // Sensor readings
    float moisturePercent;
    bool irDetected;
    
    // Sanitizer tracking
    float sanitizerLevel;
    int totalDispenses;
    unsigned long lastDispenseTime;
    unsigned long dispenseStartTime;
    bool dispensing;
    
public:
    HardwareAbstraction();
    ~HardwareAbstraction();
    
    // Initialization
    bool initialize();
    bool initializePins();
    bool initializePrinter();
    
    // LED Control
    void setLED(bool state);
    bool getLEDState() const { return ledState; }
    
    // Pump Control
    bool startPump();
    bool stopPump();
    bool isPumpRunning() const { return pumpState; }
    bool isDispensing() const { return dispensing; }
    unsigned long getDispenseDuration() const;
    bool checkDispenseTimeout();
    bool checkCooldown() const;
    
    // Sensor Reading
    float readMoistureSensor();
    bool readIRSensor();
    float getMoisturePercent() const { return moisturePercent; }
    bool isIRDetected() const { return irDetected; }
    
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
