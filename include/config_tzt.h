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
// Fixed by PCB (no breakout pins). Actual pins are in TFT_eSPI_Setup/User_Setup.h:
//   TFT: DC 2, CS 15, SCLK 14, MOSI 13, MISO 12, RST -1, BL 27
//   Touch: TOUCH_CS 33 (XPT2046, shared SPI)

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

#define TZT_AP_SSID "TZT_Display"
#define TZT_AP_PASSWORD "08202022"

// 0 = use saved WiFi; config portal only when no credentials (ask once, then never). 1 = erase and open portal every boot (for re-config only).
#ifndef FORCE_WIFI_CONFIG_PORTAL
#define FORCE_WIFI_CONFIG_PORTAL 0
#endif

// ============================================================================
// ESP-NOW CONFIGURATION (TZT Display)
// ============================================================================
// ESP-NOW communication with Main ESP32

// Main ESP32 MAC address (from Main serial: "MAC: 6C:C8:40:4E:E6:24")
// Format: {0xAA, 0xBB, 0xCC, 0xDD, 0xEE, 0xFF}
#define MAIN_ESP32_MAC_ADDRESS {0x6C, 0xC8, 0x40, 0x4E, 0xE6, 0x24}

// Onboard blue LED (GPIO 2 on most ESP32 DevKit). Set to -1 to disable.
#define BOARD_LED_PIN 2
// 1 = active LOW (LOW=on, most DevKits). 0 = active HIGH (HIGH=on); use if LED turns on then off.
#define BOARD_LED_ACTIVE_LOW 0

// ESP-NOW channel: fallback when WiFi.channel() is 0. Set to your AP's channel (TZT showed 6 for Kanta).
#define ESP_NOW_CHANNEL 6

// ============================================================================
// FIREBASE CONFIGURATION (TZT Display)
// ============================================================================
// TZT Display now handles all Firebase communication

#define FIREBASE_DATABASE_URL "https://printerpot-d96f8-default-rtdb.firebaseio.com"
#define FIREBASE_TIMEOUT 10000          // 10 seconds timeout for Firebase operations

// Time Settings (Central Time Zone - Tennessee)
#define NTP_SERVER "pool.ntp.org"
#define GMT_OFFSET_SEC -21600        // UTC-6 (Central Standard Time)
#define DAYLIGHT_OFFSET_SEC 3600     // +1 hour during Daylight Saving Time

// Weather API Settings (using coordinates)
#define WEATHER_API_KEY "bc3d9c1e4453f3e2b887c817006021ea"
#define WEATHER_LATITUDE 35.074824
#define WEATHER_LONGITUDE -89.796545

// Web Server Authentication
#define WEB_PASSWORD "0820"              // Password to access web interface

// ============================================================================
// MAIN ESP32 + WEB AUTH
// ============================================================================
// MAIN_ESP32_MAC_ADDRESS (above) is used for ESP-NOW. No HTTP to Main.
#define MAIN_MODULE_API_PASSWORD "0820"  // Web UI password (= WEB_PASSWORD)

#endif // CONFIG_TZT_H
