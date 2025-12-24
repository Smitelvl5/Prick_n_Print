#include "PrinterService.h"

const char* PrinterService::TAG = "Printer";

PrinterService::PrinterService(HardwareAbstraction* hw) 
    : hardware(hw), currentWeather("N/A") {
}

PrinterService::~PrinterService() {
}

void PrinterService::sendInitialize() {
    if (!hardware || !hardware->printerAvailable()) {
        return; // Safety check
    }
    
    hardware->printerWrite(27);  // ESC
    hardware->printerWrite('@'); // ESC @ - Printer reset (initializes printer)
    delay(50);
    
    // Set character code page to UTF-8/Unicode for better emoji support
    // Try GBK (255) or Unicode UCS-2 (253) for emoji support
    // Using CP437 (0) as default for better compatibility
    setCharacterCodePage(0); // CP437 - U.S.A., Standard Europe (default, most compatible)
    delay(10);
    
    // Set default line space
    setDefaultLineSpace();
    delay(10);
}

void PrinterService::sendCenterAlign() {
    hardware->printerWrite(27);  // ESC
    hardware->printerWrite('a');
    hardware->printerWrite(1);
    delay(10);
}

void PrinterService::sendLeftAlign() {
    hardware->printerWrite(27);  // ESC
    hardware->printerWrite('a');
    hardware->printerWrite(0);
    delay(10);
}

void PrinterService::sendDoubleHeight() {
    // ESC ! n - Set character printing method
    // Bit 4 = Double height (16)
    hardware->printerWrite(27);  // ESC
    hardware->printerWrite('!');
    hardware->printerWrite(16);   // Double height only (bit 4 = 1)
    delay(10);
}

void PrinterService::sendExtraLarge() {
    // ESC ! n - Set character printing method
    // Bit 4 = Double height (16) + Bit 5 = Double width (32) = 48
    hardware->printerWrite(27);  // ESC
    hardware->printerWrite('!');
    hardware->printerWrite(48);  // Double height + Double width (16 + 32)
    delay(10);
}

void PrinterService::sendNormalSize() {
    // ESC ! 0 - Normal size (all bits = 0)
    hardware->printerWrite(27);  // ESC
    hardware->printerWrite('!');
    hardware->printerWrite(0);
    delay(10);
}

void PrinterService::sendCutPaper() {
    // GS V n - Cut paper command
    // n = 0: Full cut, n = 1: Partial cut
    hardware->printerWrite(29);  // GS
    hardware->printerWrite(86);  // V
    hardware->printerWrite(0);    // Full cut
    delay(100);
}

void PrinterService::setCharacterCodePage(uint8_t page) {
    if (!hardware || !hardware->printerAvailable()) {
        return; // Safety check
    }
    // ESC t n - Select character code page
    // 255 = GBK (supports Chinese and extended characters)
    // 253 = UNICODE UCS-2
    // 0 = CP437 (U.S.A., Standard Europe) - default
    hardware->printerWrite(27);  // ESC
    hardware->printerWrite(116); // t
    hardware->printerWrite(page);
    delay(10);
}

void PrinterService::setDefaultLineSpace() {
    if (!hardware || !hardware->printerAvailable()) {
        return; // Safety check
    }
    // ESC 2 - Set line space to default (30 dots)
    hardware->printerWrite(27);  // ESC
    hardware->printerWrite(50);  // 2
    delay(10);
}

void PrinterService::setLineSpacing(uint8_t dots) {
    if (!hardware || !hardware->printerAvailable()) {
        return; // Safety check
    }
    // ESC 3 n - Set line spacing to n dots (0-255)
    hardware->printerWrite(27);  // ESC
    hardware->printerWrite(51);  // 3
    hardware->printerWrite(dots);
    delay(10);
}

void PrinterService::setBold(bool enable) {
    if (!hardware || !hardware->printerAvailable()) {
        return; // Safety check
    }
    // ESC E n - Turn emphasized mode on/off (bold)
    hardware->printerWrite(27);  // ESC
    hardware->printerWrite(69);  // E
    hardware->printerWrite(enable ? 1 : 0);
    delay(10);
}

void PrinterService::setUnderline(uint8_t mode) {
    if (!hardware || !hardware->printerAvailable()) {
        return; // Safety check
    }
    // ESC - n - Set underline mode
    // 0 = off, 1 = 1 dot thick, 2 = 2 dots thick
    hardware->printerWrite(27);  // ESC
    hardware->printerWrite(45);  // -
    hardware->printerWrite(mode);
    delay(10);
}

void PrinterService::setInverse(bool enable) {
    if (!hardware || !hardware->printerAvailable()) {
        return; // Safety check
    }
    // GS B n - Turn white/black reverse printing mode on/off
    hardware->printerWrite(29);  // GS
    hardware->printerWrite(66);  // B
    hardware->printerWrite(enable ? 1 : 0);
    delay(10);
}

String PrinterService::sanitizeForPrinter(const String& text) {
    // Replace common emojis with text equivalents that thermal printers can handle
    String result = text;
    
    // Love/Heart emojis
    result.replace("💌", "[LOVE LETTER]");
    result.replace("💕", "<3");
    result.replace("❤️", "<3");
    result.replace("❤", "<3");
    result.replace("💖", "<3");
    result.replace("💝", "[GIFT]");
    result.replace("💗", "<3");
    result.replace("💓", "<3");
    result.replace("💞", "<3");
    result.replace("💟", "<3");
    result.replace("💋", "[KISS]");
    
    // Face emojis
    result.replace("😊", ":)");
    result.replace("😍", ":)");
    result.replace("😘", ":*");
    result.replace("🥰", ":)");
    result.replace("😻", ":)");
    
    // Nature emojis
    result.replace("🌵", "[CACTUS]");
    result.replace("🌹", "[ROSE]");
    result.replace("🌸", "[FLOWER]");
    result.replace("🌺", "[FLOWER]");
    result.replace("🌻", "[FLOWER]");
    result.replace("🌷", "[FLOWER]");
    result.replace("💐", "[FLOWERS]");
    
    // Symbols
    result.replace("⭐", "*");
    result.replace("✨", "*");
    result.replace("💫", "*");
    result.replace("🌟", "*");
    result.replace("🎉", "[PARTY]");
    result.replace("🎊", "[PARTY]");
    result.replace("🎈", "[BALLOON]");
    result.replace("🎁", "[GIFT]");
    
    // Utility symbols
    result.replace("⏰", "[ALARM]");
    result.replace("📝", "[NOTE]");
    result.replace("🛒", "[CART]");
    result.replace("✅", "[OK]");
    result.replace("❌", "[X]");
    result.replace("⚠️", "[!]");
    result.replace("⚠", "[!]");
    result.replace("🔧", "[TOOL]");
    result.replace("📡", "[SIGNAL]");
    result.replace("🖨️", "[PRINTER]");
    result.replace("🖨", "[PRINTER]");
    result.replace("💧", "[DROP]");
    
    // Remove any remaining multi-byte UTF-8 characters (emojis we didn't catch)
    String cleaned = "";
    for (unsigned int i = 0; i < result.length(); i++) {
        char c = result.charAt(i);
        // Keep only printable ASCII characters (32-126) plus newline/tab
        if ((c >= 32 && c <= 126) || c == '\n' || c == '\r' || c == '\t') {
            cleaned += c;
        }
        // Skip multi-byte UTF-8 sequences
        else if ((c & 0x80) != 0) {
            // Skip continuation bytes
            while (i + 1 < result.length() && (result.charAt(i + 1) & 0xC0) == 0x80) {
                i++;
            }
        }
    }
    
    return cleaned;
}

bool PrinterService::isReady() const {
    return hardware && hardware->printerAvailable();
}

bool PrinterService::printTest() {
    if (!isReady()) {
        Logger::error(TAG, "Printer not ready");
        return false;
    }
    
    Logger::info(TAG, "Printing minimal test (raw text only)...");
    
    // No initialization - just send raw text to test serial communication
    hardware->printerPrintln("TEST");
    hardware->printerPrintln("1234567890");
    hardware->printerPrintln("ABCDEFGHIJKLMNOPQRSTUVWXYZ");
    hardware->printerPrintln("abcdefghijklmnopqrstuvwxyz");
    hardware->printerPrintln("");
    hardware->printerPrintln("");
    hardware->printerPrintln("");
    delay(100);
    
    Logger::info(TAG, "Test print complete");
    return true;
}

bool PrinterService::printReceipt(const String& message, bool includeWeatherAndSanitizer, time_t createdTime) {
    if (!isReady()) {
        Logger::error(TAG, "Printer not ready");
        return false;
    }
    
    // Sanitize message for printer compatibility
    String cleanMessage = sanitizeForPrinter(message);
    
    Logger::info(TAG, "Printing receipt: \"" + cleanMessage.substring(0, 30) + "...\"");
    
    // Initialize printer
    sendInitialize();
    sendCenterAlign();
    
    // Header
    hardware->printerPrintln("================================");
    setBold(true);
    if (includeWeatherAndSanitizer) {
        hardware->printerPrintln("SMIT'S MESSAGE");
    } else {
        hardware->printerPrintln("REMINDER");
    }
    setBold(false);
    hardware->printerPrintln("================================");
    delay(50);
    
    // Date/Time
    if (includeWeatherAndSanitizer) {
        sendLeftAlign();
        struct tm timeinfo;
        if (getLocalTime(&timeinfo)) {
            char buffer[30];
            strftime(buffer, sizeof(buffer), "%b %d, %Y %I:%M %p", &timeinfo);
            hardware->printerPrintln("Date: " + String(buffer));
        }
        hardware->printerPrintln("");
        delay(50);
    } else if (createdTime > 0) {
        sendLeftAlign();
        struct tm* createdInfo = localtime(&createdTime);
        if (createdInfo) {
            char buffer[30];
            strftime(buffer, sizeof(buffer), "%b %d, %Y %I:%M %p", createdInfo);
            hardware->printerPrintln("Set on: " + String(buffer));
        }
        hardware->printerPrintln("");
        delay(50);
    }
    
    // Message (Normal size - matches grocery list style)
    sendCenterAlign();
    hardware->printerPrintln(cleanMessage);
    delay(50);
    
    // Weather and Sanitizer info (for user messages only)
    if (includeWeatherAndSanitizer) {
        hardware->printerPrintln("--------------------------------");
        delay(50);
        
        sendLeftAlign();
        hardware->printerPrintln("Today's Weather:");
        hardware->printerPrintln("  " + sanitizeForPrinter(currentWeather));
        delay(50);
        
        hardware->printerWriteString("Moisture: ");
        hardware->printerWriteString(String(hardware->getMoisturePercent(), 1));
        hardware->printerWriteString("%  Sanitizer: ");
        hardware->printerWriteString(String(hardware->getSanitizerLevel(), 1));
        hardware->printerPrintln("%");
        delay(50);
    }
    
    // Footer
    hardware->printerPrintln("================================");
    delay(50);
    
    sendCutPaper();
    
    Logger::info(TAG, "Receipt printed successfully");
    return true;
}

bool PrinterService::printGroceryList(const String* items, int itemCount) {
    if (!isReady()) {
        Logger::error(TAG, "Printer not ready");
        return false;
    }
    
    if (itemCount == 0) {
        Logger::warn(TAG, "No items to print");
        return false;
    }
    
    Logger::info(TAG, "Printing grocery list (" + String(itemCount) + " items)");
    
    // Initialize printer
    sendInitialize();
    sendCenterAlign();
    
    // Header
    hardware->printerPrintln("================================");
    setBold(true);
    hardware->printerPrintln("GROCERY LIST");
    setBold(false);
    hardware->printerPrintln("================================");
    delay(50);
    
    // Date
    sendLeftAlign();
    struct tm timeinfo;
    if (getLocalTime(&timeinfo)) {
        char buffer[30];
        strftime(buffer, sizeof(buffer), "%b %d, %Y %I:%M %p", &timeinfo);
        hardware->printerPrintln("Date: " + String(buffer));
    }
    hardware->printerPrintln("");
    delay(50);
    
    // Items (sanitize each item for printer compatibility)
    for (int i = 0; i < itemCount; i++) {
        hardware->printerWriteString(String(i + 1) + ". ");
        hardware->printerPrintln(sanitizeForPrinter(items[i]));
        delay(20);
    }
    
    hardware->printerPrintln("================================");
    delay(50);
    
    sendCutPaper();
    
    Logger::info(TAG, "Grocery list printed successfully");
    return true;
}

bool PrinterService::printBitmap(const uint8_t* bitmap, uint16_t width, uint16_t height) {
    if (!isReady()) {
        Logger::error(TAG, "Printer not ready");
        return false;
    }
    
    if (!bitmap || width == 0 || height == 0) {
        Logger::error(TAG, "Invalid bitmap parameters");
        return false;
    }
    
    // Width must be a multiple of 8 for bitmap mode
    if (width % 8 != 0) {
        Logger::error(TAG, "Bitmap width must be multiple of 8");
        return false;
    }
    
    Logger::info(TAG, "Printing bitmap " + String(width) + "x" + String(height));
    
    // Calculate bytes per line
    uint16_t bytesPerLine = width / 8;
    
    // GS * x y d1...dk - Print downloaded bit image
    // x = width in bytes (low byte)
    // y = width in bytes (high byte)
    hardware->printerWrite(29);  // GS
    hardware->printerWrite(42);  // *
    hardware->printerWrite(bytesPerLine & 0xFF);  // xL (width low byte)
    hardware->printerWrite((bytesPerLine >> 8) & 0xFF);  // xH (width high byte)
    
    // Send bitmap data
    for (uint16_t row = 0; row < height; row++) {
        for (uint16_t col = 0; col < bytesPerLine; col++) {
            uint16_t index = row * bytesPerLine + col;
            hardware->printerWrite(bitmap[index]);
        }
        delay(10);  // Small delay between rows
    }
    
    // Print the bitmap
    hardware->printerWrite(29);  // GS
    hardware->printerWrite(47);  // /
    hardware->printerWrite(48);  // 0 (print normal density)
    
    hardware->printerPrintln("");  // Line feed after bitmap
    delay(100);
    
    Logger::info(TAG, "Bitmap printed successfully");
    return true;
}

/* 
 * Example: How to use printBitmap()
 * 
 * 1. Create a bitmap array (width must be multiple of 8)
 * 2. Each byte represents 8 horizontal pixels (MSB = leftmost pixel)
 * 3. 1 = black dot, 0 = white/no dot
 * 
 * Example - Simple 16x16 heart icon:
 * 
 * const uint8_t heartBitmap[32] = {
 *     0x00,0x00,  // Row 1:  ................
 *     0x0C,0x30,  // Row 2:  ....##....##....
 *     0x1E,0x78,  // Row 3:  ...####..####...
 *     0x3F,0xFC,  // Row 4:  ..############..
 *     0x7F,0xFE,  // Row 5:  .##############.
 *     0xFF,0xFF,  // Row 6:  ################
 *     0xFF,0xFF,  // Row 7:  ################
 *     0xFF,0xFF,  // Row 8:  ################
 *     0x7F,0xFE,  // Row 9:  .##############.
 *     0x3F,0xFC,  // Row 10: ..############..
 *     0x1F,0xF8,  // Row 11: ...##########...
 *     0x0F,0xF0,  // Row 12: ....########....
 *     0x07,0xE0,  // Row 13: .....######.....
 *     0x03,0xC0,  // Row 14: ......####......
 *     0x01,0x80,  // Row 15: .......##.......
 *     0x00,0x00   // Row 16: ................
 * };
 * 
 * Usage:
 * printerService.printBitmap(heartBitmap, 16, 16);
 * 
 * For custom images:
 * - Use image editing software to create monochrome bitmap
 * - Convert to byte array using online tools or scripts
 * - Ensure width is multiple of 8 (pad if necessary)
 */
