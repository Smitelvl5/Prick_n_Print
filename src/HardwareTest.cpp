#include "HardwareTest.h"
#include "Logger.h"
#include "config.h"

const char* HardwareTest::TAG = "Test";

HardwareTest::HardwareTest(HardwareAbstraction* hw, PrinterService* printer)
    : hardware(hw), printerService(printer) {
}

void HardwareTest::printTestHeader(const char* testName) {
    Serial.println("\n========================================");
    Serial.println("TEST: " + String(testName));
    Serial.println("========================================");
}

void HardwareTest::printTestResult(const char* testName, bool passed, const String& details) {
    Serial.print("[" + String(testName) + "] ");
    if (passed) {
        Serial.print("✅ PASS");
    } else {
        Serial.print("❌ FAIL");
    }
    if (details.length() > 0) {
        Serial.println(" - " + details);
    } else {
        Serial.println();
    }
}

bool HardwareTest::testPump(unsigned long durationMs) {
    printTestHeader("PUMP TEST");
    
    if (!hardware) {
        printTestResult("Pump", false, "Hardware not initialized");
        return false;
    }
    
    Serial.println("Testing pump on GPIO " + String(SANITIZER_PUMP_PIN));
    Serial.println("Duration: " + String(durationMs) + "ms");
    Serial.println("\nStarting pump in 2 seconds...");
    delay(2000);
    
    // Check initial state
    bool initialState = hardware->isPumpRunning();
    Serial.println("Initial pump state: " + String(initialState ? "RUNNING" : "STOPPED"));
    
    // Start pump
    Serial.println("\n>>> Starting pump...");
    bool startResult = hardware->startPump();
    if (!startResult) {
        printTestResult("Pump Start", false, "Failed to start pump");
        return false;
    }
    
    // Verify pump is running
    delay(100);
    bool running = hardware->isPumpRunning();
    int pinState = digitalRead(SANITIZER_PUMP_PIN);
    Serial.println("Pump running: " + String(running ? "YES" : "NO"));
    Serial.println("GPIO " + String(SANITIZER_PUMP_PIN) + " state: " + String(pinState ? "HIGH" : "LOW"));
    
    if (!running || pinState != HIGH) {
        printTestResult("Pump Start", false, "Pump did not start correctly");
        hardware->stopPump();
        return false;
    }
    
    printTestResult("Pump Start", true, "Pump started successfully");
    
    // Run for specified duration
    Serial.println("\nPump running for " + String(durationMs) + "ms...");
    unsigned long startTime = millis();
    while (millis() - startTime < durationMs) {
        unsigned long elapsed = millis() - startTime;
        unsigned long remaining = durationMs - elapsed;
        Serial.print("\rElapsed: " + String(elapsed) + "ms | Remaining: " + String(remaining) + "ms");
        delay(100);
    }
    Serial.println();
    
    // Stop pump
    Serial.println("\n>>> Stopping pump...");
    bool stopResult = hardware->stopPump();
    delay(100);
    
    // Verify pump is stopped
    running = hardware->isPumpRunning();
    pinState = digitalRead(SANITIZER_PUMP_PIN);
    Serial.println("Pump running: " + String(running ? "YES" : "NO"));
    Serial.println("GPIO " + String(SANITIZER_PUMP_PIN) + " state: " + String(pinState ? "HIGH" : "LOW"));
    
    if (running || pinState != LOW) {
        printTestResult("Pump Stop", false, "Pump did not stop correctly");
        return false;
    }
    
    printTestResult("Pump Stop", true, "Pump stopped successfully");
    printTestResult("Pump", true, "Complete test passed");
    
    Serial.println("\nWaiting for cooldown period...");
    delay(DISPENSE_COOLDOWN_MS);
    Serial.println("Cooldown complete.");
    
    return true;
}

bool HardwareTest::testLED(uint8_t brightness, unsigned long durationMs) {
    printTestHeader("LED TEST");
    
    if (!hardware) {
        printTestResult("LED", false, "Hardware not initialized");
        return false;
    }
    
    Serial.println("Testing LED on GPIO " + String(LED_PWM_PIN));
    Serial.println("Target brightness: " + String(brightness) + "/255 (" + String((brightness * 100) / 255) + "%)");
    Serial.println("Duration: " + String(durationMs) + "ms");
    
    // Disable auto-brightness for manual test
    hardware->setAutoBrightness(false);
    Serial.println("Auto-brightness disabled for test");
    
    // Turn LED off first
    Serial.println("\n>>> Setting LED to 0 (OFF)...");
    hardware->setLEDBrightness(0);
    delay(500);
    uint8_t currentBrightness = hardware->getLEDBrightness();
    Serial.println("Current brightness: " + String(currentBrightness) + "/255");
    
    // Set to target brightness
    Serial.println("\n>>> Setting LED to " + String(brightness) + "/255...");
    hardware->setLEDBrightness(brightness);
    
    // Wait for ramp-up (if active)
    Serial.println("Waiting for LED ramp-up (if active)...");
    unsigned long rampStart = millis();
    while (hardware->getLEDBrightness() != brightness && (millis() - rampStart < 5000)) {
        delay(100);
        hardware->updateLEDRamp();
        uint8_t current = hardware->getLEDBrightness();
        Serial.print("\rCurrent brightness: " + String(current) + "/255");
    }
    Serial.println();
    
    delay(500);
    currentBrightness = hardware->getLEDBrightness();
    Serial.println("Final brightness: " + String(currentBrightness) + "/255");
    
    if (currentBrightness != brightness) {
        printTestResult("LED Set Brightness", false, "Brightness mismatch");
        hardware->setLEDBrightness(0);
        return false;
    }
    
    printTestResult("LED Set Brightness", true, "Brightness set correctly");
    
    // Hold brightness for duration
    Serial.println("\nLED at " + String(brightness) + "/255 for " + String(durationMs) + "ms...");
    unsigned long startTime = millis();
    while (millis() - startTime < durationMs) {
        hardware->updateLEDRamp();
        delay(50);
    }
    
    // Turn LED off
    Serial.println("\n>>> Setting LED to 0 (OFF)...");
    hardware->setLEDBrightness(0);
    delay(1000);
    currentBrightness = hardware->getLEDBrightness();
    
    if (currentBrightness != 0) {
        printTestResult("LED Turn Off", false, "LED did not turn off");
        return false;
    }
    
    printTestResult("LED Turn Off", true, "LED turned off successfully");
    printTestResult("LED", true, "Complete test passed");
    
    return true;
}

bool HardwareTest::testLEDRamp() {
    printTestHeader("LED RAMP TEST");
    
    if (!hardware) {
        printTestResult("LED Ramp", false, "Hardware not initialized");
        return false;
    }
    
    Serial.println("Testing LED ramp-up/down functionality");
    Serial.println("Ramp rate: 1V per second");
    
    hardware->setAutoBrightness(false);
    
    // Test ramp up
    Serial.println("\n>>> Testing ramp UP (0 -> 255)...");
    hardware->setLEDBrightness(0);
    delay(500);
    hardware->setLEDBrightness(255);
    
    unsigned long startTime = millis();
    uint8_t lastBrightness = 0;
    while (millis() - startTime < 15000) {  // Max 15 seconds for full ramp
        hardware->updateLEDRamp();
        uint8_t current = hardware->getLEDBrightness();
        if (current != lastBrightness) {
            Serial.println("Brightness: " + String(current) + "/255");
            lastBrightness = current;
        }
        if (current == 255) {
            Serial.println("✅ Ramp-up complete!");
            break;
        }
        delay(200);
    }
    
    delay(1000);
    
    // Test ramp down
    Serial.println("\n>>> Testing ramp DOWN (255 -> 0)...");
    hardware->setLEDBrightness(0);
    
    startTime = millis();
    lastBrightness = 255;
    while (millis() - startTime < 15000) {  // Max 15 seconds for full ramp
        hardware->updateLEDRamp();
        uint8_t current = hardware->getLEDBrightness();
        if (current != lastBrightness) {
            Serial.println("Brightness: " + String(current) + "/255");
            lastBrightness = current;
        }
        if (current == 0) {
            Serial.println("✅ Ramp-down complete!");
            break;
        }
        delay(200);
    }
    
    printTestResult("LED Ramp", true, "Ramp test complete");
    return true;
}

bool HardwareTest::testIRSensor(unsigned long durationMs) {
    printTestHeader("IR SENSOR TEST");
    
    if (!hardware) {
        printTestResult("IR Sensor", false, "Hardware not initialized");
        return false;
    }
    
    Serial.println("Testing IR sensor on GPIO " + String(IR_SENSOR_PIN));
    Serial.println("Monitoring for " + String(durationMs) + "ms");
    Serial.println("IR sensor: LOW = DETECTED, HIGH = CLEAR");
    Serial.println("\n>>> Starting monitoring...");
    Serial.println("Wave your hand in front of the sensor to test detection");
    
    unsigned long startTime = millis();
    bool lastState = false;
    int detectionCount = 0;
    
    while (millis() - startTime < durationMs) {
        bool detected = hardware->readIRSensor();
        int rawPin = digitalRead(IR_SENSOR_PIN);
        
        if (detected != lastState) {
            if (detected) {
                Serial.println("🔴 DETECTED! (GPIO " + String(IR_SENSOR_PIN) + " = LOW)");
                detectionCount++;
            } else {
                Serial.println("⚪ CLEAR (GPIO " + String(IR_SENSOR_PIN) + " = HIGH)");
            }
            lastState = detected;
        }
        
        delay(100);
    }
    
    Serial.println("\n>>> Test complete");
    Serial.println("Total detections: " + String(detectionCount));
    Serial.println("Raw pin state: " + String(digitalRead(IR_SENSOR_PIN) ? "HIGH" : "LOW"));
    
    bool result = (detectionCount > 0 || lastState);
    printTestResult("IR Sensor", result, "Detections: " + String(detectionCount));
    
    return result;
}

bool HardwareTest::testMoistureSensor(unsigned long durationMs) {
    printTestHeader("MOISTURE SENSOR TEST");
    
    if (!hardware) {
        printTestResult("Moisture Sensor", false, "Hardware not initialized");
        return false;
    }
    
    Serial.println("Testing moisture sensor on GPIO " + String(MOISTURE_SENSOR_PIN));
    Serial.println("Monitoring for " + String(durationMs) + "ms");
    Serial.println("Moisture: 0% = dry, 100% = wet");
    Serial.println("\n>>> Starting monitoring...");
    Serial.println("Touch the sensor or add water to see readings change");
    
    unsigned long startTime = millis();
    float minMoisture = 100.0;
    float maxMoisture = 0.0;
    float sumMoisture = 0.0;
    int readingCount = 0;
    
    while (millis() - startTime < durationMs) {
        float moisture = hardware->readMoistureSensor();
        int rawValue = analogRead(MOISTURE_SENSOR_PIN);
        
        if (moisture < minMoisture) minMoisture = moisture;
        if (moisture > maxMoisture) maxMoisture = moisture;
        sumMoisture += moisture;
        readingCount++;
        
        Serial.println("Moisture: " + String(moisture, 1) + "% | Raw: " + String(rawValue) + "/4095");
        
        delay(500);
    }
    
    float avgMoisture = sumMoisture / readingCount;
    
    Serial.println("\n>>> Test complete");
    Serial.println("Readings: " + String(readingCount));
    Serial.println("Min: " + String(minMoisture, 1) + "%");
    Serial.println("Max: " + String(maxMoisture, 1) + "%");
    Serial.println("Avg: " + String(avgMoisture, 1) + "%");
    Serial.println("Current raw: " + String(analogRead(MOISTURE_SENSOR_PIN)) + "/4095");
    
    bool result = (readingCount > 0 && minMoisture >= 0 && maxMoisture <= 100);
    printTestResult("Moisture Sensor", result, "Range: " + String(minMoisture, 1) + "-" + String(maxMoisture, 1) + "%");
    
    return result;
}

bool HardwareTest::testLightSensor(unsigned long durationMs) {
    printTestHeader("LIGHT SENSOR TEST");
    
    if (!hardware) {
        printTestResult("Light Sensor", false, "Hardware not initialized");
        return false;
    }
    
    Serial.println("Testing light sensor on GPIO " + String(LIGHT_SENSOR_PIN));
    Serial.println("Monitoring for " + String(durationMs) + "ms");
    Serial.println("Light: 0% = dark, 100% = bright");
    Serial.println("\n>>> Starting monitoring...");
    Serial.println("Cover/uncover the sensor or change lighting to see readings change");
    
    // Initial diagnostic reading
    int initialRaw = analogRead(LIGHT_SENSOR_PIN);
    Serial.println("\n--- Initial Reading ---");
    Serial.println("Raw ADC: " + String(initialRaw) + "/4095");
    Serial.println("Voltage: ~" + String((initialRaw * 3.3) / 4095.0, 3) + "V");
    
    if (initialRaw == 0) {
        Serial.println("⚠️  WARNING: Raw value is 0 - sensor may not be connected!");
    } else if (initialRaw >= 4090) {
        Serial.println("⚠️  WARNING: Raw value is near maximum - check wiring!");
    }
    
    Serial.println("\n--- Continuous Monitoring ---");
    
    unsigned long startTime = millis();
    float minLight = 100.0;
    float maxLight = 0.0;
    float sumLight = 0.0;
    int readingCount = 0;
    int minRaw = 4095;
    int maxRaw = 0;
    int lastRaw = initialRaw;
    int unchangedCount = 0;
    
    while (millis() - startTime < durationMs) {
        int rawValue = analogRead(LIGHT_SENSOR_PIN);
        float light = hardware->readLightSensor();
        
        // Track raw value changes
        if (rawValue < minRaw) minRaw = rawValue;
        if (rawValue > maxRaw) maxRaw = rawValue;
        
        // Check if value is changing
        if (rawValue == lastRaw) {
            unchangedCount++;
        } else {
            unchangedCount = 0;
        }
        
        if (light < minLight) minLight = light;
        if (light > maxLight) maxLight = light;
        sumLight += light;
        readingCount++;
        
        // Show detailed reading
        float voltage = (rawValue * 3.3) / 4095.0;
        String changeIndicator = (rawValue != lastRaw) ? " ⚡" : "";
        Serial.println("[" + String(readingCount) + "] Light: " + String(light, 1) + "% | " +
                      "Raw: " + String(rawValue) + "/4095 | " +
                      "Voltage: " + String(voltage, 3) + "V" + changeIndicator);
        
        lastRaw = rawValue;
        delay(500);
    }
    
    float avgLight = sumLight / readingCount;
    int rawRange = maxRaw - minRaw;
    
    Serial.println("\n>>> Test complete");
    Serial.println("Readings: " + String(readingCount));
    Serial.println("--- Processed Values ---");
    Serial.println("Min: " + String(minLight, 1) + "%");
    Serial.println("Max: " + String(maxLight, 1) + "%");
    Serial.println("Avg: " + String(avgLight, 1) + "%");
    Serial.println("--- Raw ADC Values ---");
    Serial.println("Min Raw: " + String(minRaw) + "/4095");
    Serial.println("Max Raw: " + String(maxRaw) + "/4095");
    Serial.println("Raw Range: " + String(rawRange) + " (should be > 0 if sensor is working)");
    Serial.println("Current raw: " + String(analogRead(LIGHT_SENSOR_PIN)) + "/4095");
    
    // Diagnostic feedback
    Serial.println("\n--- Diagnostics ---");
    if (rawRange == 0) {
        Serial.println("❌ PROBLEM: Raw value did NOT change during test!");
        Serial.println("   Possible issues:");
        Serial.println("   1. Sensor not connected to GPIO " + String(LIGHT_SENSOR_PIN));
        Serial.println("   2. Sensor not powered (check VCC and GND)");
        Serial.println("   3. Sensor AO pin not connected to GPIO " + String(LIGHT_SENSOR_PIN));
        Serial.println("   4. Sensor may be faulty");
        Serial.println("   5. Check if sensor needs pull-up/pull-down resistor");
    } else if (rawRange < 100) {
        Serial.println("⚠️  WARNING: Raw value changed very little (" + String(rawRange) + " counts)");
        Serial.println("   Sensor is working but sensitivity is LOW");
        Serial.println("\n   🔧 POTENTIOMETER ADJUSTMENT:");
        Serial.println("   ⚠️  IMPORTANT: Try BOTH directions!");
        Serial.println("   - If turned RIGHT (clockwise) doesn't help, try LEFT (counter-clockwise)");
        Serial.println("   - The 'best' direction depends on your specific module");
        Serial.println("   - While test is running, slowly turn the potentiometer and watch values");
        Serial.println("   - Find the position where you get MAXIMUM range between dark and bright");
        Serial.println("\n   📋 WIRING CHECK:");
        Serial.println("   ✅ VCC → 3.3V or 5V (try 5V if using 3.3V - some modules work better)");
        Serial.println("   ✅ GND → GND");
        Serial.println("   ✅ AO (Analog Out) → GPIO " + String(LIGHT_SENSOR_PIN) + " ← You have this!");
        Serial.println("   ❌ DO (Digital Out) → NOT USED (we use analog)");
        Serial.println("\n   🧪 TEST PROCEDURE:");
        Serial.println("   1. Run test again: type 'test' → '6'");
        Serial.println("   2. Cover sensor COMPLETELY (dark) - should read Raw ~3500-4000");
        Serial.println("   3. Shine BRIGHT light directly on sensor - should read Raw ~500-1500");
        Serial.println("   4. While test runs, slowly adjust potentiometer and watch the range");
        Serial.println("   5. Find position where range is MAXIMUM (ideally 2000+ counts)");
        Serial.println("\n   📊 YOUR CURRENT VALUES:");
        Serial.println("   - Min Raw: " + String(minRaw) + " (should be ~3500-4000 when dark)");
        Serial.println("   - Max Raw: " + String(maxRaw) + " (should be ~500-1500 when bright)");
        Serial.println("   - Range: " + String(rawRange) + " counts (need 1000+ for good sensitivity)");
        Serial.println("\n   💡 TIPS:");
        Serial.println("   - If values are stuck around 2900-3000, potentiometer may be at wrong end");
        Serial.println("   - Try turning it ALL THE WAY LEFT, then test again");
        Serial.println("   - Some modules work better with 5V instead of 3.3V");
        Serial.println("   - Make sure you're testing in a room with variable lighting");
    } else if (rawRange < 500) {
        Serial.println("⚠️  Sensor working but sensitivity could be better");
        Serial.println("   Range: " + String(rawRange) + " counts");
        Serial.println("   Try adjusting LM393 potentiometer for better sensitivity");
    } else {
        Serial.println("✅ Raw value is changing significantly - sensor working well!");
        Serial.println("   Range: " + String(rawRange) + " counts");
    }
    
    if (unchangedCount > (readingCount * 0.8)) {
        Serial.println("⚠️  WARNING: Value was unchanged for " + String(unchangedCount) + " readings");
        Serial.println("   Sensor may not be responding to light changes");
    }
    
    bool result = (readingCount > 0 && minLight >= 0 && maxLight <= 100 && rawRange > 0);
    String details = "Range: " + String(minLight, 1) + "-" + String(maxLight, 1) + "%";
    if (rawRange == 0) {
        details += " (RAW NOT CHANGING!)";
    }
    printTestResult("Light Sensor", result, details);
    
    return result;
}

bool HardwareTest::testLightSensorContinuous() {
    printTestHeader("LIGHT SENSOR CONTINUOUS MONITORING");
    
    if (!hardware) {
        printTestResult("Light Sensor", false, "Hardware not initialized");
        return false;
    }
    
    Serial.println("Testing light sensor on GPIO " + String(LIGHT_SENSOR_PIN));
    Serial.println("Continuous monitoring mode - runs until you stop it");
    Serial.println("Light: 0% = dark, 100% = bright");
    Serial.println("\n>>> Starting continuous monitoring...");
    Serial.println(">>> Type 'stop' or 's' in Serial Monitor to stop");
    Serial.println(">>> Adjust potentiometer while monitoring to find best sensitivity");
    Serial.println(">>> Cover/uncover sensor to see readings change\n");
    
    // Initial diagnostic reading
    int initialRaw = analogRead(LIGHT_SENSOR_PIN);
    Serial.println("--- Initial Reading ---");
    Serial.println("Raw ADC: " + String(initialRaw) + "/4095");
    Serial.println("Voltage: ~" + String((initialRaw * 3.3) / 4095.0, 3) + "V");
    Serial.println("\n--- Continuous Monitoring (Type 'stop' to end) ---\n");
    
    int readingCount = 0;
    int minRaw = 4095;
    int maxRaw = 0;
    float minLight = 100.0;
    float maxLight = 0.0;
    int lastRaw = initialRaw;
    unsigned long lastPrint = 0;
    const unsigned long PRINT_INTERVAL = 500;  // Print every 500ms
    
    while (true) {
        // Check for stop command (non-blocking)
        if (Serial.available() > 0) {
            String input = Serial.readStringUntil('\n');
            input.trim();
            input.toLowerCase();
            if (input == "stop" || input == "s" || input == "exit") {
                Serial.println("\n>>> Stopping continuous monitoring...");
                break;
            }
        }
        
        // Read sensor
        int rawValue = analogRead(LIGHT_SENSOR_PIN);
        float light = hardware->readLightSensor();
        
        // Track min/max
        if (rawValue < minRaw) minRaw = rawValue;
        if (rawValue > maxRaw) maxRaw = rawValue;
        if (light < minLight) minLight = light;
        if (light > maxLight) maxLight = light;
        
        // Print at regular intervals
        unsigned long now = millis();
        if (now - lastPrint >= PRINT_INTERVAL) {
            float voltage = (rawValue * 3.3) / 4095.0;
            int rawRange = maxRaw - minRaw;
            String changeIndicator = (rawValue != lastRaw) ? " ⚡" : "";
            
            Serial.println("[" + String(++readingCount) + "] Light: " + String(light, 1) + "% | " +
                          "Raw: " + String(rawValue) + "/4095 | " +
                          "Voltage: " + String(voltage, 3) + "V | " +
                          "Range: " + String(rawRange) + changeIndicator);
            
            lastPrint = now;
            lastRaw = rawValue;
        }
        
        delay(50);  // Small delay to prevent tight loop
    }
    
    int rawRange = maxRaw - minRaw;
    float avgLight = (minLight + maxLight) / 2.0;
    
    Serial.println("\n>>> Monitoring stopped");
    Serial.println("Total readings: " + String(readingCount));
    Serial.println("--- Final Statistics ---");
    Serial.println("--- Processed Values ---");
    Serial.println("Min: " + String(minLight, 1) + "%");
    Serial.println("Max: " + String(maxLight, 1) + "%");
    Serial.println("Avg: " + String(avgLight, 1) + "%");
    Serial.println("--- Raw ADC Values ---");
    Serial.println("Min Raw: " + String(minRaw) + "/4095");
    Serial.println("Max Raw: " + String(maxRaw) + "/4095");
    Serial.println("Raw Range: " + String(rawRange) + " counts");
    Serial.println("Current raw: " + String(analogRead(LIGHT_SENSOR_PIN)) + "/4095");
    
    // Diagnostic feedback
    Serial.println("\n--- Diagnostics ---");
    if (rawRange == 0) {
        Serial.println("❌ PROBLEM: Raw value did NOT change during monitoring!");
    } else if (rawRange < 100) {
        Serial.println("⚠️  WARNING: Low sensitivity - Range only " + String(rawRange) + " counts");
        Serial.println("   Try adjusting potentiometer to increase range");
    } else if (rawRange < 500) {
        Serial.println("⚠️  Sensor working but sensitivity could be better");
        Serial.println("   Range: " + String(rawRange) + " counts");
    } else {
        Serial.println("✅ Good sensitivity! Range: " + String(rawRange) + " counts");
    }
    
    bool result = (readingCount > 0 && minLight >= 0 && maxLight <= 100 && rawRange > 0);
    printTestResult("Light Sensor", result, "Range: " + String(minLight, 1) + "-" + String(maxLight, 1) + "%");
    
    return result;
}

bool HardwareTest::testPrinter() {
    printTestHeader("PRINTER TEST");
    
    if (!hardware || !printerService) {
        printTestResult("Printer", false, "Hardware or printer service not initialized");
        return false;
    }
    
    if (!hardware->printerAvailable()) {
        printTestResult("Printer", false, "Printer not available");
        return false;
    }
    
    Serial.println("Testing thermal printer");
    Serial.println("TX Pin: GPIO " + String(THERMAL_TX_PIN));
    Serial.println("RX Pin: GPIO " + String(THERMAL_RX_PIN));
    Serial.println("Baud Rate: " + String(THERMAL_PRINTER_BAUD));
    
    Serial.println("\n>>> Sending test print...");
    
    // Test basic write
    bool writeResult = hardware->printerPrintln("=== PRINTER TEST ===");
    if (!writeResult) {
        printTestResult("Printer Write", false, "Failed to write to printer");
        return false;
    }
    delay(500);
    
    // Test print service
    bool printResult = printerService->printTest();
    delay(1000);
    
    if (!printResult) {
        printTestResult("Printer Service", false, "Print service test failed");
        return false;
    }
    
    printTestResult("Printer", true, "Test print sent successfully");
    Serial.println("Check printer output for test pattern");
    
    return true;
}

bool HardwareTest::testAllSensors() {
    printTestHeader("ALL SENSORS TEST");
    
    Serial.println("Reading all sensors simultaneously...");
    Serial.println("\n>>> Starting 10-second sensor read test...");
    
    unsigned long startTime = millis();
    int readingCount = 0;
    
    while (millis() - startTime < 10000) {
        float moisture = hardware->readMoistureSensor();
        bool ir = hardware->readIRSensor();
        float light = hardware->readLightSensor();
        
        Serial.println("[" + String(readingCount) + "] " +
                      "Moisture: " + String(moisture, 1) + "% | " +
                      "IR: " + String(ir ? "DETECTED" : "CLEAR") + " | " +
                      "Light: " + String(light, 1) + "%");
        
        readingCount++;
        delay(1000);
    }
    
    Serial.println("\n>>> Test complete");
    Serial.println("Total readings: " + String(readingCount));
    
    printTestResult("All Sensors", true, "Read " + String(readingCount) + " samples");
    return true;
}

void HardwareTest::printTestMenu() {
    Serial.println("\n");
    Serial.println("╔════════════════════════════════════════╗");
    Serial.println("║     HARDWARE TEST MENU                 ║");
    Serial.println("╠════════════════════════════════════════╣");
    Serial.println("║  1 - Test Pump                         ║");
    Serial.println("║  2 - Test LED                          ║");
    Serial.println("║  3 - Test LED Ramp                     ║");
    Serial.println("║  4 - Test IR Sensor                    ║");
    Serial.println("║  5 - Test Moisture Sensor              ║");
    Serial.println("║  6 - Test Light Sensor (Continuous)    ║");
    Serial.println("║  7 - Test Printer                      ║");
    Serial.println("║  8 - Test All Sensors                  ║");
    Serial.println("║  9 - Run All Tests                     ║");
    Serial.println("║  0 - Show Menu                         ║");
    Serial.println("║                                        ║");
    Serial.println("║  SD Card + Speaker: use test/ project  ║");
    Serial.println("║  (cd test && pio run -e tzt-sd-speaker ║");
    Serial.println("║   -t upload -t monitor)                ║");
    Serial.println("╚════════════════════════════════════════╝");
    Serial.println("\nEnter test number (1-9, 0 for menu): ");
}

bool HardwareTest::runInteractiveTests() {
    // Wait for user input
    while (Serial.available() == 0) {
        delay(100);
    }
    
    String input = Serial.readStringUntil('\n');
    input.trim();
    input.toLowerCase();
    
    // Check for exit command
    if (input == "exit" || input == "e") {
        return false;  // Signal to exit
    }
    
    if (input.length() == 0) {
        printTestMenu();
        return true;  // Continue in test mode
    }
    
    char choice = input.charAt(0);
    
    switch (choice) {
        case '1':
            testPump(1000);
            break;
        case '2':
            testLED(255, 2000);
            break;
        case '3':
            testLEDRamp();
            break;
        case '4':
            testIRSensor(5000);
            break;
        case '5':
            testMoistureSensor(5000);
            break;
        case '6':
            testLightSensorContinuous();
            break;
        case '7':
            testPrinter();
            break;
        case '8':
            testAllSensors();
            break;
        case '9': {
            Serial.println("\n>>> Running all tests...");
            testPump(1000);
            delay(1000);
            testLED(128, 2000);
            delay(1000);
            testIRSensor(3000);
            delay(1000);
            testMoistureSensor(3000);
            delay(1000);
            testLightSensor(3000);
            delay(1000);
            testPrinter();
            Serial.println("\n>>> All tests complete!");
            break;
        }
        case '0':
            printTestMenu();
            return true;  // Continue in test mode
        default:
            Serial.println("Invalid choice. Enter 0-9 or 'exit' to quit");
            printTestMenu();
            return true;  // Continue in test mode
    }
    
    Serial.println("\nTest complete. Press Enter to return to menu...");
    // Wait for Enter key
    while (Serial.available() == 0) {
        delay(100);
    }
    Serial.readStringUntil('\n');  // Clear buffer
    printTestMenu();
    return true;  // Continue in test mode
}
