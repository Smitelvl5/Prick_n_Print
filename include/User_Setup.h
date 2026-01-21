// User_Setup.h for TFT_eSPI library
// Configuration for 3.5" ILI9486 TFT LCD Display
// This file will be automatically used by TFT_eSPI

#define USER_SETUP_LOADED

// Driver
#define ILI9486_DRIVER

// ESP32 pins - configured to match your current GPIO assignments
#define TFT_MISO 23   // LCD_D1 / SPI MISO (changed from GPIO 12 to avoid boot failure)
#define TFT_MOSI 13   // LCD_D0 / SPI MOSI  
#define TFT_SCLK 14   // SD_SCK / SPI Clock
#define TFT_CS   22   // LCD_CS (Chip Select - moved from GPIO 15 to avoid strapping pin)
#define TFT_DC   18   // LCD_RS (Data/Command)
#define TFT_RST  19   // LCD_RST (Reset)

// Touch screen disabled to avoid TFT_eSPI parallel mode conflict and save memory
// Optional touch screen pins (if available) - commented out to fix build error
// #define TOUCH_CS 25   // Touch Controller Chip Select
// #define TOUCH_IRQ 4   // Touch Interrupt - moved from GPIO 26 to GPIO 4

// Display settings
#define TFT_WIDTH  320
#define TFT_HEIGHT 480

// SPI settings
#define SPI_FREQUENCY  20000000  // 20MHz
#define SPI_READ_FREQUENCY  20000000
#define SPI_TOUCH_FREQUENCY  2500000
// Memory optimization: Disable DMA to save RAM
#define ESP32_DMA_CHAN 0  // Disable DMA (0 = no DMA, saves ~50KB RAM)

// Optional features - Reduced font set to save flash memory (~15KB saved)
#define LOAD_GLCD   // Font 1. Original Adafruit 8 pixel font needs ~1820 bytes in FLASH
#define LOAD_FONT2  // Font 2. Small 16 pixel high font, needs ~3534 bytes in FLASH, 96 characters
#define LOAD_FONT4  // Font 4. Medium 26 pixel high font, needs ~5848 bytes in FLASH, 96 characters
// Removed FONT6, FONT7, FONT8, GFXFF, and SMOOTH_FONT to save ~15KB flash
// Uncomment if needed: #define LOAD_FONT6, #define LOAD_FONT7, #define LOAD_FONT8, #define LOAD_GFXFF, #define SMOOTH_FONT
