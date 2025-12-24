#ifndef PRINTER_SERVICE_H
#define PRINTER_SERVICE_H

#include <Arduino.h>
#include "HardwareAbstraction.h"
#include "Logger.h"

class PrinterService {
private:
    static const char* TAG;
    HardwareAbstraction* hardware;
    String currentWeather;
    
    // Printer commands
    void sendInitialize();
    void sendCenterAlign();
    void sendLeftAlign();
    void sendDoubleHeight();
    void sendExtraLarge();
    void sendNormalSize();
    void sendCutPaper();
    void setCharacterCodePage(uint8_t page);
    void setDefaultLineSpace();
    void setLineSpacing(uint8_t dots);
    void setBold(bool enable);
    void setUnderline(uint8_t mode);
    void setInverse(bool enable);
    
    // Text sanitization for printer compatibility
    String sanitizeForPrinter(const String& text);
    
public:
    PrinterService(HardwareAbstraction* hw);
    ~PrinterService();
    
    // Main printing functions
    bool printReceipt(const String& message, bool includeWeatherAndSanitizer, time_t createdTime = 0);
    bool printGroceryList(const String* items, int itemCount);
    bool printBitmap(const uint8_t* bitmap, uint16_t width, uint16_t height);
    
    // Weather management
    void setWeather(const String& weather) { currentWeather = weather; }
    String getWeather() const { return currentWeather; }
    
    // Test functions
    bool printTest();
    bool isReady() const;
};

#endif // PRINTER_SERVICE_H
