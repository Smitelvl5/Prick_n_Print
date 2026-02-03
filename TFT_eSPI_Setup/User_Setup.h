// TFT_eSPI User_Setup for TZT ESP32 LVGL 2.4" 240x320 integrated module (no pins)
// Board: TZT ESP32 LVGL WIFI&Bluetooth Development Board 2.4 inch LCD TFT Module
//       240*320 Smart Display Screen With Touch WROOM (fixed pinout on PCB)

#define USER_SETUP_LOADED 1
#define ILI9341_DRIVER 1

// Native panel resolution (portrait); use setRotation(1) for 320x240 landscape
#define TFT_WIDTH  240
#define TFT_HEIGHT 320

// Integrated module pinout (ESP32-2432S024 / CYD-style; display is on PCB)
#define TFT_MOSI 13
#define TFT_MISO 12
#define TFT_SCLK 14
#define TFT_CS   15
#define TFT_DC   2
#define TFT_RST  -1

// Backlight (TZT uses GPIO 27; turn on with HIGH)
#define TFT_BL   27
#define TFT_BACKLIGHT_ON HIGH

// Touch (XPT2046 resistive; shared SPI with display)
#define TOUCH_CS 33

#define LOAD_GLCD  1
#define LOAD_FONT2 1
#define LOAD_GFXFF 0
#define SMOOTH_FONT 0

#define SPI_FREQUENCY  40000000
#define SPI_READ_FREQUENCY  20000000
#define SPI_TOUCH_FREQUENCY  2500000
