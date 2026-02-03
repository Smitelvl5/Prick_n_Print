#ifndef HARDWARE_TEST_H
#define HARDWARE_TEST_H

#include <Arduino.h>
#include "HardwareAbstraction.h"
#include "PrinterService.h"

// Hardware Test Suite for Print-n-Prick
// Allows individual testing of each hardware component

class HardwareTest {
private:
    HardwareAbstraction* hardware;
    PrinterService* printerService;
    static const char* TAG;
    
public:
    HardwareTest(HardwareAbstraction* hw, PrinterService* printer);
    
    // Individual hardware tests
    bool testPump(unsigned long durationMs = 1000);
    bool testLED(uint8_t brightness = 255, unsigned long durationMs = 2000);
    bool testLEDRamp();
    bool testIRSensor(unsigned long durationMs = 5000);
    bool testMoistureSensor(unsigned long durationMs = 5000);
    bool testLightSensor(unsigned long durationMs = 5000);
    bool testLightSensorContinuous();  // Continuous monitoring until user stops
    bool testPrinter();
    bool testAllSensors();
    
    // Interactive test menu
    void printTestMenu();
    bool runInteractiveTests();  // Returns false if user wants to exit
    
    // Helper functions
    void printTestHeader(const char* testName);
    void printTestResult(const char* testName, bool passed, const String& details = "");
};

#endif // HARDWARE_TEST_H
