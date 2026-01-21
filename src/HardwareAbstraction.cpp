#include "HardwareAbstraction.h"
#include <ArduinoJson.h>

const char* HardwareAbstraction::TAG = "HAL";

HardwareAbstraction::HardwareAbstraction() 
    : printerSerial(nullptr), tft(nullptr), ledState(false), pumpState(false),
      moisturePercent(0.0), irDetected(false), lightPercent(0.0),
      ledBrightness(0), sanitizerLevel(0.0),
      totalDispenses(0), lastDispenseTime(0), dispenseStartTime(0),
      dispensing(false), autoBrightnessEnabled(true) {
}

HardwareAbstraction::~HardwareAbstraction() {
    if (printerSerial) {
        printerSerial->end();
        delete printerSerial;
    }
    if (tft) {
        delete tft;
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
    
    // Delay display initialization to allow other services to allocate memory first
    // This helps prevent heap exhaustion
    delay(500);
    
    if (!initializeDisplay()) {
        Logger::warn(TAG, "Failed to initialize display (continuing without display)");
        // Display is optional - system can function without it
    }
    
    // Initialize touch screen
    initializeTouch();
    
    // Initialize sanitizer level (will be calibrated later)
    sanitizerLevel = random(30, 95);
    Logger::info(TAG, "Sanitizer level initialized: " + String(sanitizerLevel, 1) + "%");
    
    // Read initial sensor values
    readMoistureSensor();
    readIRSensor();
    readLightSensor();
    updateLEDBrightness();  // Set initial LED brightness based on light level
    
    Logger::info(TAG, "Hardware initialization complete");
    printDiagnostics();
    
    return true;
}

bool HardwareAbstraction::initializePins() {
    Logger::debug(TAG, "Configuring GPIO pins...");
    
    // Configure output pins
    pinMode(LED_PIN, OUTPUT);
    // Extended delay for external power stabilization - GPIO pins need time to stabilize
    delay(100);
    
    pinMode(SANITIZER_PUMP_PIN, OUTPUT);  // GPIO 4 - MOSFET Gate for pump control
    pinMode(IR_SENSOR_PIN, INPUT_PULLUP);
    
    // Configure LED PWM (for 12V LED via MOSFET)
    pinMode(LED_PWM_PIN, OUTPUT);
    digitalWrite(LED_PWM_PIN, LOW);  // Start with LED off
    delay(50);
    ledcSetup(LED_PWM_CHANNEL, LED_PWM_FREQUENCY, LED_PWM_RESOLUTION);
    ledcAttachPin(LED_PWM_PIN, LED_PWM_CHANNEL);
    ledcWrite(LED_PWM_CHANNEL, 0);  // Start with LED off (PWM will override digital state)
    
    // Set initial states (all outputs LOW = OFF)
    digitalWrite(LED_PIN, LOW);
    digitalWrite(SANITIZER_PUMP_PIN, LOW);  // Pump OFF initially
    
    // Test LED briefly to verify it works with external power
    // This helps diagnose if the LED pin is functioning
    digitalWrite(LED_PIN, HIGH);
    delay(200);  // LED on for 200ms
    digitalWrite(LED_PIN, LOW);
    delay(100);
    digitalWrite(LED_PIN, HIGH);
    delay(200);  // Second blink
    digitalWrite(LED_PIN, LOW);
    
    ledState = false;
    pumpState = false;
    ledBrightness = 0;
    
    Logger::debug(TAG, "GPIO pins configured successfully");
    Logger::debug(TAG, "LED (GPIO " + String(LED_PIN) + ") tested with 2 blinks");
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
    // GPIO 16 (RX) and GPIO 17 (TX) can interfere with boot if printer is connected
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

bool HardwareAbstraction::initializeDisplay() {
    Logger::debug(TAG, "Initializing TFT display (TFT_eSPI)...");
    
    // Check available heap before initialization
    size_t freeHeap = ESP.getFreeHeap();
    Logger::debug(TAG, "Free heap before display init: " + String(freeHeap) + " bytes");
    
    if (freeHeap < 50000) {  // Need at least 50KB free
        Logger::warn(TAG, "Insufficient heap memory for display (" + String(freeHeap) + " bytes free)");
        return false;
    }
    
    // Ensure CS pin is HIGH before initialization (CS HIGH = display inactive)
    pinMode(LCD_CS_PIN, OUTPUT);
    digitalWrite(LCD_CS_PIN, HIGH);  // CS HIGH = display inactive
    delay(100);
    
    // Additional delay for external power stabilization before creating display object
    delay(200);
    
    // Manually reset the display for external power stability
    // This ensures the display starts from a known state
    pinMode(LCD_RST_PIN, OUTPUT);
    digitalWrite(LCD_RST_PIN, LOW);
    delay(50);
    digitalWrite(LCD_RST_PIN, HIGH);
    delay(200);  // Wait for display to reset and stabilize
    
    // Create TFT_eSPI display object
    // Note: TFT_eSPI will handle SPI.begin() internally
    // Pins are configured via defines in HardwareAbstraction.h
    tft = new TFT_eSPI();
    
    if (!tft) {
        Logger::error(TAG, "Failed to create display object!");
        return false;
    }
    
    // Initialize the display
    // SPI is already initialized, so TFT_eSPI won't try to init it again
    tft->init();
    // Extended delay for external power stabilization - display needs time to power up
    delay(500);  // Give display more time to initialize with external power
    
    // Verify display is working by checking dimensions
    uint16_t displayWidth = tft->width();
    uint16_t displayHeight = tft->height();
    
    if (displayWidth == 0 || displayHeight == 0) {
        Logger::error(TAG, "Display initialization failed - invalid dimensions");
        Logger::error(TAG, "Width: " + String(displayWidth) + ", Height: " + String(displayHeight));
        Logger::warn(TAG, "Display may not be connected. Continuing without display.");
        delete tft;
        tft = nullptr;
        return false;
    }
    
    Logger::debug(TAG, "Display initialized - Size: " + String(displayWidth) + "x" + String(displayHeight));
    
    tft->setRotation(1);  // Landscape orientation (320x480)
    delay(100);  // Extended delay for external power
    
    // Draw a very simple, highly visible test pattern
    // Start with full screen white to verify display is working
    Logger::info(TAG, "Drawing test pattern...");
    tft->fillScreen(TFT_WHITE);
    delay(300);  // Extended delay for external power
    
    int w = tft->width();
    int h = tft->height();
    
    // Draw large colored rectangles - very visible
    // Top half red
    tft->fillRect(0, 0, w, h/2, TFT_RED);
    delay(100);
    
    // Bottom half blue  
    tft->fillRect(0, h/2, w, h/2, TFT_BLUE);
    delay(100);
    
    // White circle in center
    int centerX = w / 2;
    int centerY = h / 2;
    tft->fillCircle(centerX, centerY, 80, TFT_WHITE);
    delay(100);
    
    // Green square in center of circle
    tft->fillRect(centerX - 40, centerY - 40, 80, 80, TFT_GREEN);
    delay(100);
    
    Logger::info(TAG, "TFT display initialized successfully");
    Logger::info(TAG, "Display resolution: " + String(tft->width()) + "x" + String(tft->height()));
    Logger::info(TAG, "Display test pattern: Red/Blue halves with white circle and green square");
    Logger::info(TAG, "If screen is blank, check: 1) Power to display 2) SPI connections 3) Backlight");
    return true;
}

void HardwareAbstraction::setLED(bool state) {
    if (ledState != state) {
        digitalWrite(LED_PIN, state ? HIGH : LOW);
        ledState = state;
        Logger::info(TAG, "LED (GPIO " + String(LED_PIN) + ") set to " + String(state ? "ON" : "OFF"));
    }
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
    delay(10);  // Small delay to ensure pin state settles
    
    // Verify the pin state multiple times to ensure it's actually HIGH
    int pinState1 = digitalRead(SANITIZER_PUMP_PIN);
    delay(5);
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
    delay(10);  // Small delay to ensure pin state settles
    
    // Verify the pin state multiple times to ensure it's actually LOW
    int pinState1 = digitalRead(SANITIZER_PUMP_PIN);
    delay(5);
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

float HardwareAbstraction::readLightSensor() {
    int rawValue = analogRead(LIGHT_SENSOR_PIN);
    // LM393 typically outputs higher values in darkness, lower in bright light
    // Convert to percentage: 0% = dark, 100% = bright
    lightPercent = ((float)rawValue * 100.0 / 4095.0);
    
    if (lightPercent < 0) lightPercent = 0;
    if (lightPercent > 100) lightPercent = 100;
    
    Logger::verbose(TAG, "Light sensor: " + String(lightPercent, 1) + "% (raw: " + String(rawValue) + ")");
    return lightPercent;
}

void HardwareAbstraction::updateLEDBrightness() {
    // Only update if auto-brightness is enabled
    if (!autoBrightnessEnabled) {
        Logger::verbose(TAG, "Auto-brightness disabled, skipping update");
        return;
    }
    
    // Read light sensor first
    readLightSensor();
    
    // Inverse relationship: dark = bright LED, bright = dim LED
    // lightPercent: 0% = dark, 100% = bright
    // LED brightness: 0 = off, 255 = full brightness
    float inverseLight = 100.0 - lightPercent;
    uint8_t brightness = (uint8_t)((inverseLight / 100.0) * 255.0);
    
    setLEDBrightness(brightness);
}

void HardwareAbstraction::setAutoBrightness(bool enabled) {
    autoBrightnessEnabled = enabled;
    Logger::info(TAG, "Auto-brightness " + String(enabled ? "enabled" : "disabled"));
}

void HardwareAbstraction::setLEDBrightness(uint8_t brightness) {
    // Clamp brightness to 0-255
    if (brightness > 255) brightness = 255;
    
    ledBrightness = brightness;
    ledcWrite(LED_PWM_CHANNEL, brightness);
    
    Logger::verbose(TAG, "LED brightness set to: " + String(brightness) + "/255 (" + String((brightness * 100) / 255) + "%)");
    
    // If manually set to 0, disable auto-brightness to prevent it from turning back on
    if (brightness == 0) {
        autoBrightnessEnabled = false;
        Logger::debug(TAG, "LED set to 0, auto-brightness disabled");
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
    Logger::info(TAG, "LED State: " + String(ledState ? "ON" : "OFF"));
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
    doc["led"] = ledState;
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

void HardwareAbstraction::displayDebugText(const String& text, uint16_t color, uint8_t size) {
    if (!tft || !displayAvailable()) return;
    
    tft->setTextColor(color);
    tft->setTextSize(size);
    tft->setCursor(10, 10);
    tft->print(text);
    Logger::debug(TAG, "Display debug: " + text);
}

void HardwareAbstraction::displayClear(uint16_t color) {
    if (!tft || !displayAvailable()) return;
    tft->fillScreen(color);
}

void HardwareAbstraction::displayTestPattern() {
    if (!tft || !displayAvailable()) return;
    
    // Fill screen with gradient pattern
    for (int y = 0; y < tft->height(); y++) {
        uint16_t color = ((y * 31) / tft->height()) << 11;  // Red gradient
        tft->drawFastHLine(0, y, tft->width(), color);
    }
    
    // Draw some shapes
    tft->fillRect(10, 10, 100, 100, 0x07E0);  // Green rectangle
    tft->fillCircle(200, 150, 50, 0x001F);    // Blue circle
    tft->drawRect(150, 200, 100, 80, 0xF800); // Red rectangle outline
    
    Logger::info(TAG, "Display test pattern drawn");
}

void HardwareAbstraction::displayTestColors() {
    if (!tft || !displayAvailable()) return;
    
    // Draw color bars
    int barWidth = tft->width() / 8;
    uint16_t colors[] = {
        0x0000,  // Black
        0xFFFF,  // White
        0xF800,  // Red
        0x07E0,  // Green
        0x001F,  // Blue
        0xFFE0,  // Yellow
        0xF81F,  // Magenta
        0x07FF   // Cyan
    };
    
    for (int i = 0; i < 8; i++) {
        tft->fillRect(i * barWidth, 0, barWidth, tft->height(), colors[i]);
    }
    
    Logger::info(TAG, "Display color test drawn");
}

void HardwareAbstraction::displayTestText() {
    if (!tft || !displayAvailable()) return;
    
    // Text rendering causes crashes with external power - use graphics instead
    // Draw colored rectangles to simulate text test
    tft->fillScreen(0x0000);  // Black background
    
    // Draw colored bars to simulate text lines
    tft->fillRect(10, 10, 200, 30, 0xFFFF);   // White bar (simulates "Text Test")
    tft->fillRect(10, 50, 250, 40, 0xF800);   // Red bar (simulates "Large Text")
    tft->fillRect(10, 100, 300, 20, 0x07E0);  // Green bar (simulates "Small Text")
    tft->fillRect(10, 150, 280, 30, 0x001F);  // Blue bar (simulates "Numbers")
    
    // Draw some shapes to show display is working
    tft->fillCircle(400, 50, 30, 0xFFE0);     // Yellow circle
    tft->fillCircle(400, 150, 30, 0xF81F);    // Magenta circle
    
    Logger::info(TAG, "Display text test (graphics mode - text rendering disabled for stability)");
}

bool HardwareAbstraction::initializeTouch() {
    // Touch screen initialization - using config.h pins (not TFT_eSPI defines)
    // TFT_eSPI touch support is disabled to avoid parallel mode conflict
    Logger::debug(TAG, "Initializing touch screen (hardware pins only)...");
    Logger::debug(TAG, "  CS Pin: " + String(TOUCH_CS_PIN));
    Logger::debug(TAG, "  IRQ Pin: " + String(TOUCH_IRQ_PIN));
    
    // Configure touch pins
    pinMode(TOUCH_CS_PIN, OUTPUT);
    digitalWrite(TOUCH_CS_PIN, HIGH);  // CS high = not selected
    
    pinMode(TOUCH_IRQ_PIN, INPUT_PULLUP);
    
    Logger::info(TAG, "Touch screen pins configured (TFT_eSPI touch disabled)");
    return true;
}

bool HardwareAbstraction::isTouchPressed() {
    // IRQ pin goes LOW when touch is detected (active low)
    return digitalRead(TOUCH_IRQ_PIN) == LOW;
}

bool HardwareAbstraction::readTouch(int16_t* x, int16_t* y) {
    if (!isTouchPressed()) {
        return false;
    }
    
    // Basic touch reading - for XPT2046 or similar controllers
    // This is a simplified version - full implementation would require SPI communication
    // For now, we'll return a basic reading
    
    // Note: Full touch controller implementation would require:
    // 1. SPI communication with touch controller
    // 2. Reading X and Y coordinates from touch controller registers
    // 3. Calibration and coordinate transformation
    
    // Placeholder - return center of screen if touch detected
    if (x) *x = tft ? tft->width() / 2 : 240;
    if (y) *y = tft ? tft->height() / 2 : 160;
    
    return true;
}

int HardwareAbstraction::getTouchIRQState() const {
    return digitalRead(TOUCH_IRQ_PIN);
}
