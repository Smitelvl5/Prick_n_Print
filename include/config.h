#ifndef CONFIG_H
#define CONFIG_H

// ============================================================================
// PIN DEFINITIONS
// ============================================================================

#define SANITIZER_PUMP_PIN 26  // IRF520 MOSFET Driver Module Gate (pump control via MOSFET module) - moved from GPIO 4
#define THERMAL_TX_PIN 17      // TX2 - Thermal printer TX (ESP32 TX → Printer RX)
#define THERMAL_RX_PIN 16      // RX2 - Thermal printer RX (ESP32 RX → Printer TX)
#define IR_SENSOR_PIN 32       // Infrared sensor (motion detection)
#define MOISTURE_SENSOR_PIN 34 // Moisture sensor (analog input, input-only pin)
#define LED_PWM_PIN 27         // 12V LED PWM control via MOSFET (moved from GPIO 5 to avoid strapping pin)

// ============================================================================
// THERMAL PRINTER CONFIGURATION
// ============================================================================

#define THERMAL_PRINTER_BAUD 9600  // Confirmed working for this printer

// ============================================================================
// TIMING CONFIGURATION
// ============================================================================

#define SENSOR_CHECK_INTERVAL 2000     // Check sensors every 2 seconds (for status/ESP-NOW; HTTP doesn't need to be fast)
#define LED_SCHEDULE_CHECK_INTERVAL_MS 1000  // How often the LED night schedule checks the clock
#define WIFI_STATUS_CHECK_INTERVAL_MS 5000  // Onboard LED / WiFi status (non-actuator; can be slower to save loop work)

// ============================================================================
// LED PWM CONFIGURATION
// ============================================================================

#define LED_PWM_CHANNEL 0               // LEDC channel for LED PWM (0-15)
#define LED_PWM_FREQUENCY 5000          // PWM frequency in Hz (5kHz)
#define LED_PWM_RESOLUTION 8            // PWM resolution in bits (8-bit = 0-255)

// LED Voltage Range: 6.5V (minimum) to 12V (maximum)
// PWM mapping: 0-255 input maps to 6.5V-12V output
// Minimum PWM value for 6.5V: (6.5V / 12V) * 255 = 138
#define LED_MIN_VOLTAGE 0.0             // Min voltage when on (V); off = 0V
#define LED_MAX_VOLTAGE 12.0            // Maximum operating voltage (V)
#define LED_MIN_PWM_VALUE 0             // PWM for 6.5V (min brightness when on)
#define LED_MAX_PWM_VALUE 255           // PWM for 12V (full duty cycle)

// LED night schedule (minutes since local midnight). Defaults cover Memphis winter darkness:
// earliest sunset ~4:48 PM (early Dec), latest sunrise ~7:09 AM (early Jan).
// Changeable at runtime from the test server (:8081) and saved to NVS.
#define LED_SCHEDULE_ON_MINUTE  (16 * 60 + 45)  // 4:45 PM
#define LED_SCHEDULE_OFF_MINUTE (7 * 60 + 15)   // 7:15 AM
#define LED_DEFAULT_LEVEL_PCT 10                // Brightness (1-100%) used when the LED turns on; saved to NVS

// ============================================================================
// CONFIGURATION
// ============================================================================

// WiFi Settings
// AP_SSID / AP_PASSWORD (config-portal credentials) now come from secrets.h
// 0 = use saved WiFi; config portal only when no credentials (ask once, then never). 1 = erase and open portal every boot (for re-config only).
#ifndef FORCE_WIFI_CONFIG_PORTAL
#define FORCE_WIFI_CONFIG_PORTAL 0
#endif

// Time Settings (Central Time Zone - Tennessee)
#define NTP_SERVER "pool.ntp.org"
// POSIX TZ: CST (UTC-6), CDT from 2nd Sun of March to 1st Sun of November - switches DST automatically
#define TIMEZONE_POSIX "CST6CDT,M3.2.0,M11.1.0"

// Weather API Settings (using coordinates)
// WEATHER_API_KEY, WEATHER_LATITUDE, WEATHER_LONGITUDE now come from secrets.h

// Onboard blue LED (GPIO 2 on most ESP32 DevKit). Set to -1 to disable.
#define BOARD_LED_PIN 2
// 1 = active LOW (LOW=on, most DevKits). 0 = active HIGH (HIGH=on); use if LED turns on then off.
#define BOARD_LED_ACTIVE_LOW 0

// Sanitizer Dispensing Settings
#define DISPENSE_COOLDOWN_MS 3000        // Cooldown period: 3 seconds between dispenses (3000ms)
#define MAX_DISPENSE_DURATION_MS 2000   // Maximum dispense duration: 2 seconds (prevents continuous dumping)

// Sanitizer level tracking: level drops by (pump run time x flow rate) / capacity.
// Calibrate flow: run the pump for 10 s into a measuring cup, divide ml by 10.
#define SANITIZER_CAPACITY_ML 250.0f    // Full reservoir volume (ml)
#define PUMP_FLOW_ML_PER_SEC 1.0f       // Pump output (ml/s) - measure and update

// Web Server Authentication
// WEB_PASSWORD now comes from secrets.h

// ============================================================================
// ESP-NOW CONFIGURATION (Main ESP32)
// ============================================================================
// ESP-NOW is the link between the web UI (on TZT) and hardware (on Main).
// Flow: Browser -> TZT HTTP server -> TZT sends ESP-NOW command -> Main receives -> hardware runs.
// If web buttons (pump, printer, LED) do nothing: (1) Both devices same WiFi/SSID and channel,
// (2) MACs correct: Main uses TZT's MAC here; TZT uses Main's MAC in config_tzt.h (MAIN_ESP32_MAC_ADDRESS).

// TZT Display MAC address — MUST match TZT serial "This MAC: xx:xx:xx:xx:xx:xx"
// Format: {0xAA, 0xBB, 0xCC, 0xDD, 0xEE, 0xFF}
#define TZT_DISPLAY_MAC_ADDRESS {0xF4, 0x2D, 0xC9, 0x6D, 0x6E, 0x60}

// ESP-NOW channel: fallback when WiFi.channel() is 0. Set to your AP's channel (TZT showed 6 for Kanta).
#define ESP_NOW_CHANNEL 6

// ESP-NOW transmission interval (milliseconds)
#define ESP_NOW_SEND_INTERVAL 10000  // Heartbeat: full sensor packet every 10 s even when nothing changed
#define ESP_NOW_MIN_SEND_GAP_MS 250  // State changes (LED, pump, IR, levels) go out immediately, but no closer together than this

#endif // CONFIG_H 