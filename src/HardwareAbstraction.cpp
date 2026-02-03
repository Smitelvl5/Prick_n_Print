#include "HardwareAbstraction.h"
#include <ArduinoJson.h>

const char* HardwareAbstraction::TAG = "HAL";

// Sentinel for "calibration not set" (valid calibration is 0-100)
static const float LIGHT_CALIB_NOT_SET = -1.0f;

HardwareAbstraction::HardwareAbstraction() 
    : printerSerial(nullptr), pumpState(false),
      moisturePercent(0.0), irDetected(false), lightPercent(0.0),
      ledBrightness(0), lightCalibMinPercent(LIGHT_CALIB_NOT_SET), lightCalibMaxPercent(LIGHT_CALIB_NOT_SET),
      sanitizerLevel(0.0), totalDispenses(0), lastDispenseTime(0), dispenseStartTime(0),
      dispensing(false), autoBrightnessEnabled(true),
      maxDispenseDurationMs_(0), dispenseCooldownMs_(0),
      rampActive(false), rampStartPWM(0), rampTargetPWM(0), currentPWM(0), rampStartTime(0) {
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
    readLightSensor();
    
    // Explicitly set LED to OFF on startup (before any auto-brightness)
    ledcWrite(LED_PWM_CHANNEL, 0);
    currentPWM = 0;
    ledBrightness = 0;
    rampActive = false;  // Ensure no ramp is active
    
    Logger::info(TAG, "Hardware initialization complete");
    printDiagnostics();
    
    return true;
}

bool HardwareAbstraction::initializePins() {
    Logger::debug(TAG, "Configuring GPIO pins...");
    
    pinMode(SANITIZER_PUMP_PIN, OUTPUT);  // GPIO 26 - MOSFET Gate for pump control
    pinMode(IR_SENSOR_PIN, INPUT_PULLUP);
    
    // Configure LED PWM (for 12V LED via MOSFET)
    pinMode(LED_PWM_PIN, OUTPUT);
    digitalWrite(LED_PWM_PIN, LOW);  // Start with LED off
    delay(50);
    ledcSetup(LED_PWM_CHANNEL, LED_PWM_FREQUENCY, LED_PWM_RESOLUTION);
    ledcAttachPin(LED_PWM_PIN, LED_PWM_CHANNEL);
    ledcWrite(LED_PWM_CHANNEL, 0);  // Start with LED off (PWM will override digital state)
    
    // Set initial states (all outputs LOW = OFF)
    digitalWrite(SANITIZER_PUMP_PIN, LOW);  // Pump OFF initially
    
    pumpState = false;
    ledBrightness = 0;
    currentPWM = 0;  // Initialize current PWM value
    rampActive = false;  // Ensure ramp is disabled on startup
    rampStartPWM = 0;
    rampTargetPWM = 0;
    rampStartTime = 0;
    
    Logger::debug(TAG, "GPIO pins configured successfully");
    Logger::debug(TAG, "Pump control pin (GPIO " + String(SANITIZER_PUMP_PIN) + ") set to OUTPUT mode");
    Logger::debug(TAG, "LED PWM initialized on pin " + String(LED_PWM_PIN) + " (channel " + String(LED_PWM_CHANNEL) + ")");
    return true;
}

bool HardwareAbstraction::initializePrinter() {
    Logger::debug(TAG, "Initializing thermal printer...");
    Logger::debug(TAG, "  RX Pin: " + String(THERMAL_RX_PIN));
    Logger::debug(TAG, "  TX Pin: " + String(THERMAL_TX_PIN));
    Logger::debug(TAG, "  Baud Rate: " + String(THERMAL_PRINTER_BAUD));
    
    // Ensure UART pins are in safe state before initializing serial
    // Using GPIO 4 (TX) and GPIO 33 (RX) - safe pins that don't interfere with boot
    // Previous GPIO 16/17 caused boot issues due to flash memory interface
    pinMode(THERMAL_RX_PIN, INPUT_PULLUP);  // Set RX as input with pull-up
    pinMode(THERMAL_TX_PIN, INPUT_PULLUP);  // Set TX as input with pull-up
    delay(200);  // Allow pins to stabilize
    
    printerSerial = new HardwareSerial(2);
    // Try inverted serial logic - common issue with thermal printers
    printerSerial->begin(THERMAL_PRINTER_BAUD, SERIAL_8N1, THERMAL_RX_PIN, THERMAL_TX_PIN, true);
    // Extended delay for external power stabilization
    delay(300);  // Increased delay for external power
    
    // Test printer connection
    printerSerial->write(27);  // ESC
    printerSerial->write('@');  // Initialize (fixed: was string, should be char)
    // Extended delay for external power stabilization
    delay(200);
    
    Logger::info(TAG, "Thermal printer initialized");
    return true;
}


bool HardwareAbstraction::startPump() {
    Logger::debug(TAG, "startPump() called - Current state: dispensing=" + String(dispensing) + ", pumpState=" + String(pumpState));
    
    if (dispensing) {
        Logger::warn(TAG, "Pump already running");
        return false;
    }
    
    if (!checkCooldown()) {
        unsigned long remainingCooldown = DISPENSE_COOLDOWN_MS - (millis() - lastDispenseTime);
        Logger::warn(TAG, "Cooldown active: " + String(remainingCooldown) + "ms remaining");
        return false;
    }
    
    Logger::debug(TAG, "Setting GPIO " + String(SANITIZER_PUMP_PIN) + " to HIGH");
    digitalWrite(SANITIZER_PUMP_PIN, HIGH);
    delay(2);  // Minimal settle for GPIO/MOSFET (sensor→actuator priority)
    
    // Verify the pin state (quick double-read)
    int pinState1 = digitalRead(SANITIZER_PUMP_PIN);
    delay(1);
    int pinState2 = digitalRead(SANITIZER_PUMP_PIN);
    
    if (pinState1 == HIGH && pinState2 == HIGH) {
        Logger::info(TAG, "✅ GPIO " + String(SANITIZER_PUMP_PIN) + " confirmed HIGH (MOSFET should be switching)");
    } else {
        Logger::error(TAG, "❌ GPIO " + String(SANITIZER_PUMP_PIN) + " state inconsistent! Read: " + String(pinState1) + ", " + String(pinState2));
    }
    
    pumpState = true;
    dispensing = true;
    dispenseStartTime = millis();
    
    Logger::info(TAG, "✅ Pump started - GPIO " + String(SANITIZER_PUMP_PIN) + " = HIGH");
    return true;
}

bool HardwareAbstraction::stopPump() {
    Logger::debug(TAG, "stopPump() called - Current state: dispensing=" + String(dispensing) + ", pumpState=" + String(pumpState));
    
    if (!dispensing) {
        Logger::debug(TAG, "Pump already stopped");
        // Still ensure pin is LOW even if we think it's stopped
        digitalWrite(SANITIZER_PUMP_PIN, LOW);
        pumpState = false;
        return false;
    }
    
    Logger::debug(TAG, "Setting GPIO " + String(SANITIZER_PUMP_PIN) + " to LOW");
    digitalWrite(SANITIZER_PUMP_PIN, LOW);
    delay(2);  // Minimal settle for GPIO/MOSFET (sensor→actuator priority)
    
    // Verify the pin state (quick double-read)
    int pinState1 = digitalRead(SANITIZER_PUMP_PIN);
    delay(1);
    int pinState2 = digitalRead(SANITIZER_PUMP_PIN);
    
    if (pinState1 == LOW && pinState2 == LOW) {
        Logger::info(TAG, "✅ GPIO " + String(SANITIZER_PUMP_PIN) + " confirmed LOW (MOSFET should be off)");
    } else {
        Logger::error(TAG, "❌ GPIO " + String(SANITIZER_PUMP_PIN) + " state inconsistent! Read: " + String(pinState1) + ", " + String(pinState2));
    }
    
    pumpState = false;
    dispensing = false;
    lastDispenseTime = millis();
    totalDispenses++;
    
    unsigned long duration = lastDispenseTime - dispenseStartTime;
    Logger::info(TAG, "✅ Pump stopped (duration: " + String(duration) + "ms, total dispenses: " + String(totalDispenses) + ")");
    
    return true;
}

unsigned long HardwareAbstraction::getDispenseDuration() const {
    if (!dispensing) return 0;
    return millis() - dispenseStartTime;
}

bool HardwareAbstraction::checkDispenseTimeout() {
    if (!dispensing) return false;
    unsigned long limit = (maxDispenseDurationMs_ > 0) ? maxDispenseDurationMs_ : MAX_DISPENSE_DURATION_MS;
    if (getDispenseDuration() >= limit) {
        stopPump();
        return true;
    }
    return false;
}

bool HardwareAbstraction::checkCooldown() const {
    if (lastDispenseTime == 0) return true;
    unsigned long cooldown = (dispenseCooldownMs_ > 0) ? dispenseCooldownMs_ : DISPENSE_COOLDOWN_MS;
    return (millis() - lastDispenseTime) >= cooldown;
}

void HardwareAbstraction::setMaxDispenseDurationMs(unsigned long ms) {
    maxDispenseDurationMs_ = ms;
}

void HardwareAbstraction::setDispenseCooldownMs(unsigned long ms) {
    dispenseCooldownMs_ = ms;
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

float HardwareAbstraction::readLightSensor() {
    int rawValue = analogRead(LIGHT_SENSOR_PIN);
    // LM393 AO: higher raw in darkness, lower in bright light
    // Invert so 0% = dark, 100% = bright (matches display/auto-brightness logic)
    lightPercent = 100.0 - ((float)rawValue * 100.0 / 4095.0);
    
    if (lightPercent < 0) lightPercent = 0;
    if (lightPercent > 100) lightPercent = 100;
    
    Logger::verbose(TAG, "Light sensor: " + String(lightPercent, 1) + "% (raw: " + String(rawValue) + ")");
    return lightPercent;
}

void HardwareAbstraction::setLightCalibrationMin(float pct) {
    if (pct < 0) pct = 0;
    if (pct > 100) pct = 100;
    lightCalibMinPercent = pct;
    Logger::info(TAG, "Light calibration MIN set to " + String(pct, 1) + "%");
}

void HardwareAbstraction::setLightCalibrationMax(float pct) {
    if (pct < 0) pct = 0;
    if (pct > 100) pct = 100;
    lightCalibMaxPercent = pct;
    Logger::info(TAG, "Light calibration MAX set to " + String(pct, 1) + "%");
}

void HardwareAbstraction::updateLEDBrightness() {
    // Only update if auto-brightness is enabled
    if (!autoBrightnessEnabled) {
        Logger::verbose(TAG, "Auto-brightness disabled, skipping update");
        return;
    }
    
    // Use cached light sensor value (already read by main loop)
    // This ensures LED brightness matches the light sensor reading sent to TZT
    // DO NOT call readLightSensor() here - use the cached value instead
    
    // Map light to 0-100% using calibrated min/max if both set; otherwise use raw 0-100
    float lightNorm = lightPercent;
    bool useCalib = (lightCalibMinPercent >= 0 && lightCalibMaxPercent >= 0 && lightCalibMaxPercent > lightCalibMinPercent);
    if (useCalib) {
        float range = lightCalibMaxPercent - lightCalibMinPercent;
        lightNorm = (lightPercent - lightCalibMinPercent) / range * 100.0f;
        if (lightNorm < 0) lightNorm = 0;
        if (lightNorm > 100) lightNorm = 100;
    }
    
    // Inverse relationship: dark = bright LED, bright = dim LED
    // lightNorm: 0% = dark, 100% = bright
    // LED brightness: 0 = off, 1-255 = 6.5V-12V range (mapped in setLEDBrightness)
    float inverseLight = 100.0f - lightNorm;
    uint8_t brightness = (uint8_t)((inverseLight / 100.0f) * 255.0f);
    
    // Ensure minimum brightness is 1 (not 0) so LED actually turns on at lowest setting
    if (brightness == 0 && inverseLight > 0) {
        brightness = 1;
    }
    
    setLEDBrightness(brightness);
}

void HardwareAbstraction::setAutoBrightness(bool enabled) {
    autoBrightnessEnabled = enabled;
    Logger::info(TAG, "Auto-brightness " + String(enabled ? "enabled" : "disabled"));
}

void HardwareAbstraction::setLEDBrightness(uint8_t brightness) {
    // Clamp brightness to 0-255
    if (brightness > 255) brightness = 255;
    
    // Off = 0V immediately (no ramp). On = 6.5V–12V with optional ramp.
    // Do NOT disable auto-brightness here — that would stop LED from responding when room gets dark again.
    // Call setAutoBrightness(false) from command handler when user explicitly turns LED off.
    if (brightness == 0) {
        ledcWrite(LED_PWM_CHANNEL, 0);
        currentPWM = 0;
        rampActive = false;
        ledBrightness = 0;
        Logger::debug(TAG, "LED set to 0 (0V)");
        return;
    }
    
    // Map 1-255 to 6.5V–12V (LED_MIN_PWM_VALUE to 255)
    uint8_t targetPWM = (uint8_t)(((float)brightness / 255.0) * (LED_MAX_PWM_VALUE - LED_MIN_PWM_VALUE) + LED_MIN_PWM_VALUE);
    if (targetPWM > 255) targetPWM = 255;
    
    // If target is same as current, no ramp needed
    if (targetPWM == currentPWM) {
        ledBrightness = brightness;
        rampActive = false;
        return;
    }
    
    // Start ramp: 1V per second
    rampActive = true;
    rampStartPWM = currentPWM;
    rampTargetPWM = targetPWM;
    rampStartTime = millis();
    ledBrightness = brightness;
    
    Logger::debug(TAG, "LED ramp started: " + String(currentPWM) + " -> " + String(targetPWM) + " PWM (1V/s)");
}

void HardwareAbstraction::updateLEDRamp() {
    if (!rampActive) return;
    
    // Calculate voltage difference
    float startVoltage = (rampStartPWM / 255.0) * LED_MAX_VOLTAGE;
    float targetVoltage = (rampTargetPWM / 255.0) * LED_MAX_VOLTAGE;
    float voltageDiff = targetVoltage - startVoltage;
    
    // If no voltage change needed, skip ramp
    if (abs(voltageDiff) < 0.01) {
        rampActive = false;
        ledcWrite(LED_PWM_CHANNEL, rampTargetPWM);
        currentPWM = rampTargetPWM;  // Update tracked value
        return;
    }
    
    // Calculate elapsed time in seconds
    unsigned long elapsedMs = millis() - rampStartTime;
    float elapsedSeconds = elapsedMs / 1000.0;
    
    // Ramp rate: 1V per second
    const float RAMP_RATE_V_PER_S = 1.0;
    float voltageChange = elapsedSeconds * RAMP_RATE_V_PER_S;
    
    // Determine direction (ramp up or down)
    if (voltageDiff > 0) {
        // Ramping up
        float currentVoltage = startVoltage + voltageChange;
        if (currentVoltage >= targetVoltage) {
            // Ramp complete
            currentVoltage = targetVoltage;
            rampActive = false;
        }
        
        // Convert voltage to PWM
        uint8_t currentPWM = (uint8_t)((currentVoltage / LED_MAX_VOLTAGE) * 255.0);
        if (currentPWM > 255) currentPWM = 255;
        
        ledcWrite(LED_PWM_CHANNEL, currentPWM);
        this->currentPWM = currentPWM;  // Update tracked value
    } else {
        // Ramping down
        float currentVoltage = startVoltage - voltageChange;
        if (currentVoltage <= targetVoltage) {
            // Ramp complete
            currentVoltage = targetVoltage;
            rampActive = false;
        }
        
        // Convert voltage to PWM
        uint8_t currentPWM = (uint8_t)((currentVoltage / LED_MAX_VOLTAGE) * 255.0);
        if (currentPWM > 255) currentPWM = 255;
        
        ledcWrite(LED_PWM_CHANNEL, currentPWM);
        this->currentPWM = currentPWM;  // Update tracked value
    }
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
    Logger::info(TAG, "LED Brightness: " + String(ledBrightness) + "/255 (" + String((ledBrightness * 100) / 255) + "%)");
    
    // Detailed pump diagnostics
    int pumpPinState = digitalRead(SANITIZER_PUMP_PIN);
    Logger::info(TAG, "Pump State: " + String(pumpState ? "ON" : "OFF"));
    Logger::info(TAG, "Pump GPIO " + String(SANITIZER_PUMP_PIN) + " Pin State: " + String(pumpPinState ? "HIGH" : "LOW"));
    Logger::info(TAG, "Dispensing: " + String(dispensing ? "YES" : "NO"));
    if (dispensing) {
        unsigned long duration = millis() - dispenseStartTime;
        Logger::info(TAG, "Dispense Duration: " + String(duration) + "ms");
    }
    if (lastDispenseTime > 0) {
        unsigned long timeSinceLast = millis() - lastDispenseTime;
        Logger::info(TAG, "Time Since Last Dispense: " + String(timeSinceLast) + "ms");
        Logger::info(TAG, "Cooldown Status: " + String(checkCooldown() ? "READY" : "ACTIVE"));
    }
    
    Logger::info(TAG, "IR Sensor: " + String(irDetected ? "DETECTED" : "CLEAR"));
    Logger::info(TAG, "Moisture: " + String(moisturePercent, 1) + "%");
    Logger::info(TAG, "Light Level: " + String(lightPercent, 1) + "%");
    Logger::info(TAG, "Sanitizer Level: " + String(sanitizerLevel, 1) + "%");
    Logger::info(TAG, "Total Dispenses: " + String(totalDispenses));
    Logger::info(TAG, "Printer: " + String(printerAvailable() ? "READY" : "NOT AVAILABLE"));
    Logger::info(TAG, "========================================");
}

String HardwareAbstraction::getStatusJSON() const {
    DynamicJsonDocument doc(512);
    doc["ledBrightness"] = ledBrightness;
    doc["pump"] = pumpState;
    doc["dispensing"] = dispensing;
    doc["irSensor"] = irDetected;
    doc["moisture"] = String(moisturePercent, 1);
    doc["light"] = String(lightPercent, 1);
    doc["sanitizer"] = String(sanitizerLevel, 1);
    doc["dispenses"] = totalDispenses;
    doc["printer"] = printerAvailable();
    
    String json;
    serializeJson(doc, json);
    return json;
}

