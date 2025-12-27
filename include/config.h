#ifndef CONFIG_H
#define CONFIG_H

// ============================================================================
// PIN DEFINITIONS
// ============================================================================

#define LED_PIN 2              // Built-in blue LED
#define SANITIZER_PUMP_PIN 4   // IRF520 MOSFET Gate (pump control via MOSFET)
#define THERMAL_TX_PIN 17      // TX2 - Thermal printer TX (ESP32 TX → Printer RX)
#define THERMAL_RX_PIN 16      // RX2 - Thermal printer RX (ESP32 RX → Printer TX)
#define IR_SENSOR_PIN 32       // Infrared sensor (motion detection)
#define MOISTURE_SENSOR_PIN 34 // Moisture sensor (analog input, input-only pin)

// ============================================================================
// THERMAL PRINTER CONFIGURATION
// ============================================================================

#define THERMAL_PRINTER_BAUD 9600  // Confirmed working for this printer

// ============================================================================
// TIMING CONFIGURATION
// ============================================================================

#define SENSOR_CHECK_INTERVAL 10000     // Check sensors every 10 seconds

// ============================================================================
// CONFIGURATION
// ============================================================================

// WiFi Settings
#define AP_SSID "Print_n_Prick"
#define AP_PASSWORD "08202022"

// Firebase Settings
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

// Sanitizer Dispensing Settings
#define DISPENSE_COOLDOWN_MS 3000        // Cooldown period: 3 seconds between dispenses (3000ms)
#define MAX_DISPENSE_DURATION_MS 2000   // Maximum dispense duration: 2 seconds (prevents continuous dumping)

// Web Server Authentication
#define WEB_PASSWORD "0820"              // Password to access web interface (change in secrets.h if needed)

#endif // CONFIG_H 