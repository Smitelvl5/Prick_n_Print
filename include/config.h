#ifndef CONFIG_H
#define CONFIG_H

// ============================================================================
// PIN DEFINITIONS
// ============================================================================

#define LED_PIN 21             // Built-in blue LED (moved from GPIO 2 to avoid strapping pin)
#define SANITIZER_PUMP_PIN 26  // IRF520 MOSFET Driver Module Gate (pump control via MOSFET module) - moved from GPIO 4
#define THERMAL_TX_PIN 17      // TX2 - Thermal printer TX (ESP32 TX → Printer RX)
#define THERMAL_RX_PIN 16      // RX2 - Thermal printer RX (ESP32 RX → Printer TX)
#define IR_SENSOR_PIN 32       // Infrared sensor (motion detection)
#define MOISTURE_SENSOR_PIN 34 // Moisture sensor (analog input, input-only pin)
#define LIGHT_SENSOR_PIN 35    // LM393 Light Sensor Module (analog input, input-only pin)
#define LED_PWM_PIN 27         // 12V LED PWM control via MOSFET (moved from GPIO 5 to avoid strapping pin)

// ============================================================================
// DISPLAY CONFIGURATION
// ============================================================================
// TFT Pin Mapping (SPI Mode):
// LCD_CS    -> GPIO 22 (Chip Select - moved from GPIO 15 to avoid strapping pin)
// LCD_RS    -> GPIO 18 (Data/Command - DC pin)
// LCD_SCK   -> GPIO 14 (SPI Clock - LCD_WR in parallel mode)
// LCD_MOSI  -> GPIO 13 (SPI MOSI - LCD_D0 in parallel mode)
// LCD_MISO  -> GPIO 23 (SPI MISO - Changed from GPIO 12 to avoid boot issues)
// LCD_RST   -> GPIO 19 (Reset)

#define LCD_CS_PIN 22          // LCD_CS (Chip Select - moved from GPIO 15 to avoid strapping pin)
#define LCD_RS_PIN 18          // LCD_RS (Data/Command - DC pin)
#define LCD_SCK_PIN 14         // LCD_WR / SPI Clock
#define LCD_MOSI_PIN 13        // LCD_D0 / SPI MOSI
#define LCD_MISO_PIN 23        // LCD_D1 / SPI MISO (changed from GPIO 12 to avoid boot failure)
#define LCD_RST_PIN 19         // LCD_RST (Reset)

// Touch Screen Configuration
#define TOUCH_CS_PIN 25         // Touch Controller Chip Select
#define TOUCH_IRQ_PIN 4         // Touch Interrupt (optional but recommended) - moved from GPIO 26 to GPIO 4

// ============================================================================
// THERMAL PRINTER CONFIGURATION
// ============================================================================

#define THERMAL_PRINTER_BAUD 9600  // Confirmed working for this printer

// ============================================================================
// TIMING CONFIGURATION
// ============================================================================

#define SENSOR_CHECK_INTERVAL 10000     // Check sensors every 10 seconds

// ============================================================================
// LED PWM CONFIGURATION
// ============================================================================

#define LED_PWM_CHANNEL 0               // LEDC channel for LED PWM (0-15)
#define LED_PWM_FREQUENCY 5000          // PWM frequency in Hz (5kHz)
#define LED_PWM_RESOLUTION 8            // PWM resolution in bits (8-bit = 0-255)

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