#ifndef HARDWARE_ABSTRACTION_H
#define HARDWARE_ABSTRACTION_H

#include <Arduino.h>
#include <HardwareSerial.h>
#include <SPI.h>

// Configure TFT_eSPI before including it
#define USER_SETUP_LOADED
#define ILI9486_DRIVER
#define TFT_MISO 23   // LCD_D1 / SPI MISO (changed from GPIO 12 to avoid boot failure)
#define TFT_MOSI 13   // LCD_D0 / SPI MOSI  
#define TFT_SCLK 14   // SD_SCK / SPI Clock
#define TFT_CS   22   // LCD_CS (Chip Select - moved from GPIO 15 to avoid strapping pin)
#define TFT_DC   18   // LCD_RS (Data/Command)
#define TFT_RST  19   // LCD_RST (Reset)
// Touch screen disabled to avoid TFT_eSPI parallel mode conflict and save memory
// #define TOUCH_CS 25   // Touch Controller Chip Select
// #define TOUCH_IRQ 4   // Touch Interrupt - moved from GPIO 26 to GPIO 4
#define SPI_FREQUENCY  20000000
// Memory optimization: Disable DMA and frame buffers to save RAM
#define ESP32_DMA_CHAN 0  // Disable DMA (0 = no DMA)
#define ESP32_PARALLEL 0  // Not using parallel interface
// Reduced font set to save flash memory
#define LOAD_GLCD
#define LOAD_FONT2
#define LOAD_FONT4
// Removed FONT6, FONT7, FONT8, GFXFF, SMOOTH_FONT to save ~15KB flash

#include <TFT_eSPI.h>
#include "config.h"
#include "Logger.h"

// Hardware Abstraction Layer for Print_n_Prick
class HardwareAbstraction {
private:
    static const char* TAG;
    HardwareSerial* printerSerial;
    TFT_eSPI* tft;
    
    // Pin states
    bool ledState;
    bool pumpState;
    
    // Sensor readings
    float moisturePercent;
    bool irDetected;
    float lightPercent;
    uint8_t ledBrightness;
    bool autoBrightnessEnabled;  // Flag to enable/disable automatic brightness control
    
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
    bool initializeDisplay();
    
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
    float readLightSensor();
    float getMoisturePercent() const { return moisturePercent; }
    bool isIRDetected() const { return irDetected; }
    float getLightPercent() const { return lightPercent; }
    
    // LED Brightness Control (inverse of light level)
    void updateLEDBrightness();
    void setLEDBrightness(uint8_t brightness);  // 0-255
    void setAutoBrightness(bool enabled);  // Enable/disable automatic brightness control
    bool isAutoBrightnessEnabled() const { return autoBrightnessEnabled; }
    uint8_t getLEDBrightness() const { return ledBrightness; }
    
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
    
    // Display Operations
    TFT_eSPI* getDisplay() { return tft; }
    bool displayAvailable() const { return tft != nullptr; }
    void displayDebugText(const String& text, uint16_t color = 0xFFFF, uint8_t size = 2);
    void displayClear(uint16_t color = 0x0000);
    void displayTestPattern();
    void displayTestColors();
    void displayTestText();
    
    // Touch Screen Operations
    bool initializeTouch();
    bool isTouchPressed();
    bool readTouch(int16_t* x, int16_t* y);
    int getTouchIRQState() const;
    
    // Diagnostic
    void printDiagnostics() const;
    String getStatusJSON() const;
};

#endif // HARDWARE_ABSTRACTION_H
