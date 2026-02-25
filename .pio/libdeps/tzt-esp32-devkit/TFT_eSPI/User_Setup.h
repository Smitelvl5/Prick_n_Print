// TFT_eSPI User_Setup for TZT ESP32 LVGL 2.4" 240x320 - MUST be in include/ so library uses it
// Based on ESP32-Cheap-Yellow-Display (CYD) DisplayConfig/User_Setup.h
// https://github.com/witnessmenow/ESP32-Cheap-Yellow-Display
// TZT differs: TFT_BL 27 (CYD uses 21), same display pins (DC 2, CS 15, MOSI 13, MISO 12, SCLK 14)

#define USER_SETUP_LOADED 1

// CYD uses ILI9341_2_DRIVER (alternative ILI9341 driver - Bodmer TFT_eSPI issue #1172)
#define ILI9341_2_DRIVER
// #define ILI9341_DRIVER
// #define ST7789_DRIVER

#define TFT_WIDTH  240
#define TFT_HEIGHT 320

// CYD leaves these commented; uncomment if colours are wrong
// #define TFT_RGB_ORDER TFT_BGR
#define TFT_INVERSION_ON   // Fix black/white swapped on this panel

#define TFT_MOSI 13
#define TFT_MISO 12
#define TFT_SCLK 14
#define TFT_CS   15
#define TFT_DC   2
#define TFT_RST  -1

#define TFT_BL   27
#define TFT_BACKLIGHT_ON HIGH

#define TOUCH_CS 33

#define USE_HSPI_PORT

#define LOAD_GLCD  1
#define LOAD_FONT2 1
#define LOAD_GFXFF 0
#define SMOOTH_FONT 0

#define SPI_FREQUENCY  40000000
#define SPI_READ_FREQUENCY  20000000
#define SPI_TOUCH_FREQUENCY  2500000
