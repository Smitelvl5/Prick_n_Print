#include "HardwareAbstraction.h"
#include <ArduinoJson.h>
#include <Preferences.h>
#include <time.h>

const char* HardwareAbstraction::TAG = "HAL";

// NVS namespace/keys for sanitizer tracking
static const char* SANITIZER_NVS_NS = "sanitizer";
static const char* SANITIZER_NVS_LEVEL = "level";
static const char* SANITIZER_NVS_COUNT = "dispenses";

// NVS namespace/keys for the LED night schedule
static const char* LED_NVS_NS = "ledsched";
static const char* LED_NVS_ENABLED = "enabled";
static const char* LED_NVS_ON = "on";
static const char* LED_NVS_OFF = "off";
static const char* LED_NVS_LEVEL = "level";

HardwareAbstraction::HardwareAbstraction() 
    : printerSerial(nullptr), pumpState(false),
      moisturePercent(0.0), irDetected(false),
      ledBrightness(0), ledScheduleEnabled(true), ledOnMinute(LED_SCHEDULE_ON_MINUTE),
      ledOffMinute(LED_SCHEDULE_OFF_MINUTE), ledScheduleLastWindow(-1), ledLevelPct(LED_DEFAULT_LEVEL_PCT),
      sanitizerLevel(0.0), totalDispenses(0), lastDispenseTime(0), dispenseStartTime(0),
      dispensing(false),
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
    
    // Restore sanitizer level from NVS (100% on first boot)
    loadSanitizerState();
    Logger::info(TAG, "Sanitizer level restored: " + String(sanitizerLevel, 1) + "% (" + String(totalDispenses) + " dispenses)");
    
    // Restore LED schedule from NVS (config.h defaults on first boot)
    loadLedSchedule();
    Logger::info(TAG, "LED schedule: " + String(ledScheduleEnabled ? "on " : "off ") +
                 formatMinute(ledOnMinute) + " - " + formatMinute(ledOffMinute) + ", level " + String(ledLevelPct) + "%");
    
    // Read initial sensor values
    readMoistureSensor();
    readIRSensor();
    
    // Explicitly set LED to OFF on startup (schedule turns it on once NTP time is known)
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
    // Using GPIO 17 (TX) and GPIO 16 (RX)
    pinMode(THERMAL_RX_PIN, INPUT_PULLUP);  // Set RX as input with pull-up
    pinMode(THERMAL_TX_PIN, INPUT_PULLUP);  // Set TX as input with pull-up
    delay(200);  // Allow pins to stabilize
    
    printerSerial = new HardwareSerial(2);
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

    // Verify the pin state (quick double-read); retry once before giving up
    int pinState1 = digitalRead(SANITIZER_PUMP_PIN);
    delay(1);
    int pinState2 = digitalRead(SANITIZER_PUMP_PIN);

    if (pinState1 != HIGH || pinState2 != HIGH) {
        Logger::warn(TAG, "GPIO " + String(SANITIZER_PUMP_PIN) + " not HIGH after write, retrying");
        digitalWrite(SANITIZER_PUMP_PIN, HIGH);
        delay(2);
        pinState1 = digitalRead(SANITIZER_PUMP_PIN);
        delay(1);
        pinState2 = digitalRead(SANITIZER_PUMP_PIN);
    }

    if (pinState1 == HIGH && pinState2 == HIGH) {
        Logger::info(TAG, "✅ GPIO " + String(SANITIZER_PUMP_PIN) + " confirmed HIGH (MOSFET should be switching)");
    } else {
        Logger::error(TAG, "❌ GPIO " + String(SANITIZER_PUMP_PIN) + " state inconsistent after retry! Read: " + String(pinState1) + ", " + String(pinState2) + " - pump not started");
        return false;
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

    // Verify the pin state (quick double-read); retry once before giving up.
    // A pump that fails to turn off is a safety issue, so we keep retrying the
    // write rather than optimistically reporting success.
    int pinState1 = digitalRead(SANITIZER_PUMP_PIN);
    delay(1);
    int pinState2 = digitalRead(SANITIZER_PUMP_PIN);

    if (pinState1 != LOW || pinState2 != LOW) {
        Logger::warn(TAG, "GPIO " + String(SANITIZER_PUMP_PIN) + " not LOW after write, retrying");
        digitalWrite(SANITIZER_PUMP_PIN, LOW);
        delay(2);
        pinState1 = digitalRead(SANITIZER_PUMP_PIN);
        delay(1);
        pinState2 = digitalRead(SANITIZER_PUMP_PIN);
    }

    bool verifiedOff = (pinState1 == LOW && pinState2 == LOW);
    if (verifiedOff) {
        Logger::info(TAG, "✅ GPIO " + String(SANITIZER_PUMP_PIN) + " confirmed LOW (MOSFET should be off)");
    } else {
        Logger::error(TAG, "❌ GPIO " + String(SANITIZER_PUMP_PIN) + " state inconsistent after retry! Read: " + String(pinState1) + ", " + String(pinState2) + " - pump may still be running");
    }

    // Reflect the verified pin state, not just intent: if the pin is still
    // HIGH, dispensing must stay true so checkDispenseTimeout() keeps retrying
    // and callers don't believe a stuck-on pump has stopped.
    pumpState = verifiedOff ? false : (pinState2 == HIGH);
    dispensing = !verifiedOff;
    lastDispenseTime = millis();

    if (verifiedOff) {
        totalDispenses++;
        unsigned long duration = lastDispenseTime - dispenseStartTime;
        float usedMl = (duration / 1000.0f) * PUMP_FLOW_ML_PER_SEC;
        updateSanitizerLevel(usedMl / SANITIZER_CAPACITY_ML * 100.0f);
        saveSanitizerState();
        Logger::info(TAG, "✅ Pump stopped (duration: " + String(duration) + "ms, used: " + String(usedMl, 1) + "ml, level: " + String(sanitizerLevel, 1) + "%, total dispenses: " + String(totalDispenses) + ")");
    }

    return verifiedOff;
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

String HardwareAbstraction::formatMinute(uint16_t minute) {
    int h = (minute / 60) % 24, m = minute % 60;
    int h12 = h % 12 == 0 ? 12 : h % 12;
    char buf[12];
    snprintf(buf, sizeof(buf), "%d:%02d %s", h12, m, h < 12 ? "AM" : "PM");
    return String(buf);
}

void HardwareAbstraction::updateLEDSchedule() {
    if (!ledScheduleEnabled) return;
    struct tm t;
    if (!getLocalTime(&t, 0)) return;  // no NTP time yet - leave LED as is
    uint16_t now = t.tm_hour * 60 + t.tm_min;
    // Window wraps midnight when on > off (e.g. 4:45 PM -> 7:15 AM)
    bool inWindow = (ledOnMinute <= ledOffMinute)
        ? (now >= ledOnMinute && now < ledOffMinute)
        : (now >= ledOnMinute || now < ledOffMinute);
    // Edge-triggered: only switch when the window changes, so a manual on/off holds until the next switch time
    if ((int8_t)inWindow == ledScheduleLastWindow) return;
    ledScheduleLastWindow = inWindow;
    Logger::info(TAG, String("LED schedule: ") + (inWindow ? "on" : "off") + " at " + formatMinute(now));
    setLEDBrightness(inWindow ? getLedOnPwm() : 0);
}

uint8_t HardwareAbstraction::getLedOnPwm() const {
    // Eyes perceive brightness roughly as the square of duty cycle, so 50% on the slider looks about half as bright
    float f = ledLevelPct / 100.0f;
    int pwm = (int)(f * f * 255.0f + 0.5f);
    return (uint8_t)(pwm < 1 ? 1 : pwm);
}

void HardwareAbstraction::setLedLevel(uint8_t pct) {
    if (pct < 1) pct = 1;
    if (pct > 100) pct = 100;
    ledLevelPct = pct;
    saveLedSchedule();
    Logger::info(TAG, "LED level set to " + String(pct) + "%");
    if (ledBrightness > 0) setLEDBrightness(getLedOnPwm());  // live update while on
}

void HardwareAbstraction::reapplyLEDSchedule() {
    ledScheduleLastWindow = -1;
    updateLEDSchedule();
}

void HardwareAbstraction::setLedScheduleEnabled(bool enabled) {
    ledScheduleEnabled = enabled;
    saveLedSchedule();
    Logger::info(TAG, "LED schedule " + String(enabled ? "enabled" : "disabled"));
    if (enabled) reapplyLEDSchedule();
}

bool HardwareAbstraction::setLedScheduleTimes(uint16_t onMinute, uint16_t offMinute) {
    if (onMinute >= 1440 || offMinute >= 1440 || onMinute == offMinute) return false;
    ledOnMinute = onMinute;
    ledOffMinute = offMinute;
    saveLedSchedule();
    Logger::info(TAG, "LED schedule times: " + formatMinute(onMinute) + " - " + formatMinute(offMinute));
    reapplyLEDSchedule();
    return true;
}

void HardwareAbstraction::loadLedSchedule() {
    Preferences prefs;
    prefs.begin(LED_NVS_NS, true);
    ledScheduleEnabled = prefs.getBool(LED_NVS_ENABLED, true);
    ledOnMinute = prefs.getUShort(LED_NVS_ON, LED_SCHEDULE_ON_MINUTE);
    ledOffMinute = prefs.getUShort(LED_NVS_OFF, LED_SCHEDULE_OFF_MINUTE);
    ledLevelPct = prefs.getUChar(LED_NVS_LEVEL, LED_DEFAULT_LEVEL_PCT);
    if (ledLevelPct < 1 || ledLevelPct > 100) ledLevelPct = LED_DEFAULT_LEVEL_PCT;
    prefs.end();
}

void HardwareAbstraction::saveLedSchedule() {
    Preferences prefs;
    prefs.begin(LED_NVS_NS, false);
    prefs.putBool(LED_NVS_ENABLED, ledScheduleEnabled);
    prefs.putUShort(LED_NVS_ON, ledOnMinute);
    prefs.putUShort(LED_NVS_OFF, ledOffMinute);
    prefs.putUChar(LED_NVS_LEVEL, ledLevelPct);
    prefs.end();
}

void HardwareAbstraction::setLEDBrightness(uint8_t brightness) {
    // Clamp brightness to 0-255
    if (brightness > 255) brightness = 255;
    
    // Off = 0V immediately (no ramp). On = 6.5V–12V with optional ramp.
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
        uint8_t newPWM = (uint8_t)((currentVoltage / LED_MAX_VOLTAGE) * 255.0);
        if (newPWM > 255) newPWM = 255;

        ledcWrite(LED_PWM_CHANNEL, newPWM);
        currentPWM = newPWM;  // Update tracked value
    } else {
        // Ramping down
        float currentVoltage = startVoltage - voltageChange;
        if (currentVoltage <= targetVoltage) {
            // Ramp complete
            currentVoltage = targetVoltage;
            rampActive = false;
        }

        // Convert voltage to PWM
        uint8_t newPWM = (uint8_t)((currentVoltage / LED_MAX_VOLTAGE) * 255.0);
        if (newPWM > 255) newPWM = 255;

        ledcWrite(LED_PWM_CHANNEL, newPWM);
        currentPWM = newPWM;  // Update tracked value
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
    saveSanitizerState();
    Logger::info(TAG, "Sanitizer reset to 100%");
}

void HardwareAbstraction::loadSanitizerState() {
    Preferences prefs;
    prefs.begin(SANITIZER_NVS_NS, true);
    sanitizerLevel = prefs.getFloat(SANITIZER_NVS_LEVEL, 100.0f);
    totalDispenses = prefs.getInt(SANITIZER_NVS_COUNT, 0);
    prefs.end();
}

void HardwareAbstraction::saveSanitizerState() {
    Preferences prefs;
    prefs.begin(SANITIZER_NVS_NS, false);
    prefs.putFloat(SANITIZER_NVS_LEVEL, sanitizerLevel);
    prefs.putInt(SANITIZER_NVS_COUNT, totalDispenses);
    prefs.end();
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

bool HardwareAbstraction::printerFeed(uint8_t lines) {
    if (!printerSerial) return false;
    printerSerial->write(27);      // ESC
    printerSerial->write('d');     // ESC d n - print buffer and feed n lines
    printerSerial->write(lines);
    return true;
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
    Logger::info(TAG, "LED Schedule: " + String(ledScheduleEnabled ? "on " : "off ") +
                 formatMinute(ledOnMinute) + " - " + formatMinute(ledOffMinute));
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
    doc["ledLevel"] = ledLevelPct;
    doc["ledSchedule"] = ledScheduleEnabled;
    doc["ledOn"] = formatMinute(ledOnMinute);
    doc["ledOff"] = formatMinute(ledOffMinute);
    doc["sanitizer"] = String(sanitizerLevel, 1);
    doc["dispenses"] = totalDispenses;
    doc["printer"] = printerAvailable();
    
    String json;
    serializeJson(doc, json);
    return json;
}

