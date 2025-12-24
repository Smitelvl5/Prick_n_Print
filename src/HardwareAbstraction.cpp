#include "HardwareAbstraction.h"
#include <ArduinoJson.h>

const char* HardwareAbstraction::TAG = "HAL";

HardwareAbstraction::HardwareAbstraction() 
    : printerSerial(nullptr), ledState(false), pumpState(false),
      moisturePercent(0.0), irDetected(false), sanitizerLevel(0.0),
      totalDispenses(0), lastDispenseTime(0), dispenseStartTime(0),
      dispensing(false) {
}

HardwareAbstraction::~HardwareAbstraction() {
    if (printerSerial) {
        printerSerial->end();
        delete printerSerial;
    }
}

bool HardwareAbstraction::initialize() {
    Logger::info(TAG, "Initializing hardware abstraction layer...");
    
    if (!initializePins()) {
        Logger::error(TAG, "Failed to initialize pins");
        return false;
    }
    
    if (!initializePrinter()) {
        Logger::error(TAG, "Failed to initialize printer");
        return false;
    }
    
    // Initialize sanitizer level (will be calibrated later)
    sanitizerLevel = random(30, 95);
    Logger::info(TAG, "Sanitizer level initialized: " + String(sanitizerLevel, 1) + "%");
    
    // Read initial sensor values
    readMoistureSensor();
    readIRSensor();
    
    Logger::info(TAG, "Hardware initialization complete");
    printDiagnostics();
    
    return true;
}

bool HardwareAbstraction::initializePins() {
    Logger::debug(TAG, "Configuring GPIO pins...");
    
    // Configure output pins
    pinMode(LED_PIN, OUTPUT);
    pinMode(SANITIZER_PUMP_PIN, OUTPUT);
    pinMode(IR_SENSOR_PIN, INPUT_PULLUP);
    
    // Set initial states
    digitalWrite(LED_PIN, LOW);
    digitalWrite(SANITIZER_PUMP_PIN, LOW);
    
    ledState = false;
    pumpState = false;
    
    Logger::debug(TAG, "GPIO pins configured successfully");
    return true;
}

bool HardwareAbstraction::initializePrinter() {
    Logger::debug(TAG, "Initializing thermal printer...");
    Logger::debug(TAG, "  RX Pin: " + String(THERMAL_RX_PIN));
    Logger::debug(TAG, "  TX Pin: " + String(THERMAL_TX_PIN));
    Logger::debug(TAG, "  Baud Rate: " + String(THERMAL_PRINTER_BAUD));
    
    printerSerial = new HardwareSerial(2);
    // Try inverted serial logic - common issue with thermal printers
    printerSerial->begin(THERMAL_PRINTER_BAUD, SERIAL_8N1, THERMAL_RX_PIN, THERMAL_TX_PIN, true);
    delay(100);
    
    // Test printer connection
    printerSerial->write(27);  // ESC
    printerSerial->write('@');  // Initialize (fixed: was string, should be char)
    delay(100);
    
    Logger::info(TAG, "Thermal printer initialized");
    return true;
}

void HardwareAbstraction::setLED(bool state) {
    if (ledState != state) {
        digitalWrite(LED_PIN, state ? HIGH : LOW);
        ledState = state;
        Logger::verbose(TAG, "LED set to " + String(state ? "ON" : "OFF"));
    }
}

bool HardwareAbstraction::startPump() {
    if (dispensing) {
        Logger::warn(TAG, "Pump already running");
        return false;
    }
    
    if (!checkCooldown()) {
        unsigned long remainingCooldown = DISPENSE_COOLDOWN_MS - (millis() - lastDispenseTime);
        Logger::warn(TAG, "Cooldown active: " + String(remainingCooldown) + "ms remaining");
        return false;
    }
    
    digitalWrite(SANITIZER_PUMP_PIN, HIGH);
    pumpState = true;
    dispensing = true;
    dispenseStartTime = millis();
    
    Logger::info(TAG, "Pump started");
    return true;
}

bool HardwareAbstraction::stopPump() {
    if (!dispensing) {
        Logger::debug(TAG, "Pump already stopped");
        return false;
    }
    
    digitalWrite(SANITIZER_PUMP_PIN, LOW);
    pumpState = false;
    dispensing = false;
    lastDispenseTime = millis();
    totalDispenses++;
    
    unsigned long duration = lastDispenseTime - dispenseStartTime;
    Logger::info(TAG, "Pump stopped (duration: " + String(duration) + "ms, total dispenses: " + String(totalDispenses) + ")");
    
    return true;
}

unsigned long HardwareAbstraction::getDispenseDuration() const {
    if (!dispensing) return 0;
    return millis() - dispenseStartTime;
}

bool HardwareAbstraction::checkDispenseTimeout() {
    if (!dispensing) return false;
    
    if (getDispenseDuration() >= MAX_DISPENSE_DURATION_MS) {
        Logger::warn(TAG, "Dispense timeout reached, stopping pump");
        stopPump();
        return true;
    }
    return false;
}

bool HardwareAbstraction::checkCooldown() const {
    if (lastDispenseTime == 0) return true;
    return (millis() - lastDispenseTime) >= DISPENSE_COOLDOWN_MS;
}

float HardwareAbstraction::readMoistureSensor() {
    int rawValue = analogRead(MOISTURE_SENSOR_PIN);
    moisturePercent = 100.0 - ((float)rawValue * 100.0 / 4095.0);
    
    if (moisturePercent < 0) moisturePercent = 0;
    if (moisturePercent > 100) moisturePercent = 100;
    
    Logger::verbose(TAG, "Moisture sensor: " + String(moisturePercent, 1) + "% (raw: " + String(rawValue) + ")");
    return moisturePercent;
}

bool HardwareAbstraction::readIRSensor() {
    int rawState = digitalRead(IR_SENSOR_PIN);
    irDetected = (rawState == LOW);
    
    Logger::verbose(TAG, "IR sensor: " + String(irDetected ? "DETECTED" : "CLEAR"));
    return irDetected;
}

void HardwareAbstraction::setSanitizerLevel(float level) {
    if (level < 0) level = 0;
    if (level > 100) level = 100;
    sanitizerLevel = level;
    Logger::debug(TAG, "Sanitizer level set to: " + String(level, 1) + "%");
}

void HardwareAbstraction::updateSanitizerLevel(float amount) {
    sanitizerLevel -= amount;
    if (sanitizerLevel < 0) sanitizerLevel = 0;
    Logger::debug(TAG, "Sanitizer level updated: " + String(sanitizerLevel, 1) + "%");
}

void HardwareAbstraction::resetSanitizer() {
    sanitizerLevel = 100.0;
    totalDispenses = 0;
    Logger::info(TAG, "Sanitizer reset to 100%");
}

bool HardwareAbstraction::printerWrite(const uint8_t* data, size_t length) {
    if (!printerSerial) return false;
    size_t written = printerSerial->write(data, length);
    return written == length;
}

bool HardwareAbstraction::printerWrite(uint8_t data) {
    if (!printerSerial) return false;
    return printerSerial->write(data) == 1;
}

bool HardwareAbstraction::printerWriteString(const String& str) {
    if (!printerSerial) return false;
    return printerSerial->print(str) == str.length();
}

bool HardwareAbstraction::printerPrintln(const String& str) {
    if (!printerSerial) return false;
    return printerSerial->println(str) > 0;
}

bool HardwareAbstraction::printerAvailable() const {
    return printerSerial != nullptr;
}

void HardwareAbstraction::printDiagnostics() const {
    Logger::info(TAG, "========================================");
    Logger::info(TAG, "HARDWARE DIAGNOSTICS");
    Logger::info(TAG, "========================================");
    Logger::info(TAG, "LED State: " + String(ledState ? "ON" : "OFF"));
    Logger::info(TAG, "Pump State: " + String(pumpState ? "ON" : "OFF"));
    Logger::info(TAG, "Dispensing: " + String(dispensing ? "YES" : "NO"));
    Logger::info(TAG, "IR Sensor: " + String(irDetected ? "DETECTED" : "CLEAR"));
    Logger::info(TAG, "Moisture: " + String(moisturePercent, 1) + "%");
    Logger::info(TAG, "Sanitizer Level: " + String(sanitizerLevel, 1) + "%");
    Logger::info(TAG, "Total Dispenses: " + String(totalDispenses));
    Logger::info(TAG, "Printer: " + String(printerAvailable() ? "READY" : "NOT AVAILABLE"));
    Logger::info(TAG, "========================================");
}

String HardwareAbstraction::getStatusJSON() const {
    DynamicJsonDocument doc(512);
    doc["led"] = ledState;
    doc["pump"] = pumpState;
    doc["dispensing"] = dispensing;
    doc["irSensor"] = irDetected;
    doc["moisture"] = String(moisturePercent, 1);
    doc["sanitizer"] = String(sanitizerLevel, 1);
    doc["dispenses"] = totalDispenses;
    doc["printer"] = printerAvailable();
    
    String json;
    serializeJson(doc, json);
    return json;
}
