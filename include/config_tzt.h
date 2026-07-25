#ifndef CONFIG_TZT_H
#define CONFIG_TZT_H

// ============================================================================
// TZT ESP32 LVGL SCREEN CONFIGURATION
// ============================================================================
// This file contains configuration specific to the TZT ESP32 LVGL display module

// ============================================================================
// DISPLAY CONFIGURATION
// ============================================================================

// Screen dimensions (adjust based on your TZT screen model)
#define TZT_SCREEN_WIDTH 320
#define TZT_SCREEN_HEIGHT 240

// Display interface type (SPI, I2C, etc.)
// #define TZT_DISPLAY_SPI
// #define TZT_DISPLAY_I2C

// ============================================================================
// PIN DEFINITIONS (TZT ESP32 LVGL 2.4" 240x320 integrated module)
// ============================================================================
// Fixed by PCB (no breakout pins). Pins are in include/User_Setup.h (copied to TFT_eSPI by script):
//   TFT: DC 2, CS 15, SCLK 14, MOSI 13, MISO 12, RST -1, BL 27
//   Touch: TOUCH_CS 33 (XPT2046, shared SPI) — must match User_Setup.h
#ifndef TZT_TOUCH_CS
#define TZT_TOUCH_CS 33
#endif
// Touch calibration: affine transform coefficients (from test/calibration tool)
// screenX = TOUCH_AX*rawX + TOUCH_BX*rawY + TOUCH_CX
// screenY = TOUCH_AY*rawX + TOUCH_BY*rawY + TOUCH_CY
#define TOUCH_AX   0.098444f
#define TOUCH_BX  -0.000961f
#define TOUCH_CX -34.178f
#define TOUCH_AY   0.000785f
#define TOUCH_BY   0.073316f
#define TOUCH_CY -35.711f
#define TOUCH_Z_PRESSED 800   // rawZ above this = finger on screen (idle ~725)

// Backlight pin: set explicitly in code so display is visible even if TFT_eSPI
// was compiled with default User_Setup (which may not define TFT_BL).
#ifndef TZT_TFT_BL_PIN
#define TZT_TFT_BL_PIN 27
#endif
// Backlight PWM: full brightness = 255; use LEDC channel 1 (main board uses 0 for 12V LED)
#define TZT_TFT_BL_LEDC_CHANNEL  1
#define TZT_TFT_BL_LEDC_FREQ     5000
#define TZT_TFT_BL_LEDC_RES      8
#define TZT_TFT_BL_BRIGHTNESS    255   // 0-255, 255 = full

// ============================================================================
// LVGL CONFIGURATION
// ============================================================================

// LVGL memory pool size
#define LVGL_MEMORY_SIZE (32 * 1024)  // 32KB for LVGL

// Color depth (16 or 32)
#define LVGL_COLOR_DEPTH 16

// ============================================================================
// WIFI CONFIGURATION
// ============================================================================

// TZT_AP_SSID / TZT_AP_PASSWORD (config-portal credentials) now come from secrets.h

// 0 = use saved WiFi; config portal only when no credentials (ask once, then never). 1 = erase and open portal every boot (for re-config only).
#ifndef FORCE_WIFI_CONFIG_PORTAL
#define FORCE_WIFI_CONFIG_PORTAL 0
#endif

// ============================================================================
// ESP-NOW CONFIGURATION (TZT Display)
// ============================================================================
// ESP-NOW communication with Main ESP32

// Main ESP32 MAC — set from Main board serial at boot ("MAC: xx:xx:xx:xx:xx:xx"). Wrong = no sensor data.
// Format: {0xAA, 0xBB, 0xCC, 0xDD, 0xEE, 0xFF}
#define MAIN_ESP32_MAC_ADDRESS {0x6C, 0xC8, 0x40, 0x55, 0x85, 0x98}

// Onboard blue LED (GPIO 2 on most ESP32 DevKit). Set to -1 to disable.
#define BOARD_LED_PIN 2
// 1 = active LOW (LOW=on, most DevKits). 0 = active HIGH (HIGH=on); use if LED turns on then off.
#define BOARD_LED_ACTIVE_LOW 0

// ESP-NOW channel: fallback when WiFi.channel() is 0. Set to your AP's channel (TZT showed 6 for Kanta).
#define ESP_NOW_CHANNEL 6

// ============================================================================
// BACKEND: HTTP Web Server (TZT Display)
// ============================================================================
// Pi server URL (no trailing slash). TZT polls /api/commands, /api/audio, /api/images.
// Same LAN: use Pi's local IP e.g. "http://192.168.1.146:5000" (Flask) or "http://192.168.1.146" (nginx)
// Include :5000 if running Flask directly; omit if nginx proxies port 80.
#ifndef BACKEND_URL
#define BACKEND_URL "http://192.168.1.146:5000"
#endif
#define BACKEND_TIMEOUT 10000

// Time Settings (Central Time Zone - Tennessee)
#define NTP_SERVER "pool.ntp.org"
#define GMT_OFFSET_SEC -21600        // UTC-6 (Central Standard Time)
#define DAYLIGHT_OFFSET_SEC 3600     // +1 hour during Daylight Saving Time

// Weather API Settings (using coordinates)
// WEATHER_API_KEY now comes from secrets.h
#define WEATHER_LATITUDE 35.074824
#define WEATHER_LONGITUDE -89.796545

// Web Server Authentication
// WEB_PASSWORD now comes from secrets.h

// ============================================================================
// MAIN ESP32 + WEB AUTH
// ============================================================================
// MAIN_ESP32_MAC_ADDRESS (above) is used for ESP-NOW. No HTTP to Main.
// TZT web UI password: uses WEB_PASSWORD from secrets.h directly (see tzt-display/main.cpp)

#endif // CONFIG_TZT_H
