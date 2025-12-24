/*
 * ESP32 Printer Pot v2.0 - Romantic Message Dispenser
 * Industry-Standard Modular Architecture
 * 
 * Features:
 * - Modular service-oriented design
 * - Hardware abstraction layer
 * - Structured logging
 * - OTA updates
 * - Health monitoring
 * - API security
 */

#include <Arduino.h>
#include <WiFi.h>
#include <WiFiManager.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <WebServer.h>
#include <time.h>

// New modular components
#include "version.h"
#include "config.h"
#include "Logger.h"
#include "HardwareAbstraction.h"
#include "PrinterService.h"
#include "FirebaseService.h"
#include "ReminderService.h"
#include "OTAUpdateService.h"
#include "HealthMonitor.h"
#include "RequestQueue.h"

// Global service instances
HardwareAbstraction* hardware;
PrinterService* printerService;
FirebaseService* firebase;
ReminderService* reminderService;
OTAUpdateService* otaService;
HealthMonitor* healthMonitor;
RequestQueue* requestQueue;
WebServer server(8080);

// Global state
String deviceIP = "";
String currentWeather = "N/A";

// Web authentication
String webPassword = WEB_PASSWORD;
String authToken = "";  // Simple session token
unsigned long authTokenExpiry = 0;
const unsigned long AUTH_TOKEN_DURATION = 3600000;  // 1 hour

// Grocery list
#define MAX_GROCERY_ITEMS 50
String groceryItems[MAX_GROCERY_ITEMS];
int groceryCount = 0;

// Function declarations
void setupWiFi();
void setupTime();
void setupWebServer();
void getWeatherData();
void pollFirebaseCommands();
void updateFirebaseStatus();
void loadGroceries();
void saveGroceries();
void printGroceryList();
void processRequestQueue();  // Process queued requests asynchronously

// Web server handlers
void handleRoot();
void handleLogin();
void handleLogout();
bool isAuthenticated();
void handleGetStatus();
void handleResetSanitizer();
void handleAddReminder();
void handleGetReminders();
void handleDeleteReminder();
void handleAddGrocery();
void handleGetGroceries();
void handleClearGroceries();
void handlePrintGroceries();
void handleDeleteGrocery();
void handleFavicon();
void handleHealth();
void handleQueueStatus();
void handleTestPage();
void handleTestLED();
void handleTestPump();
void handleTestPrinter();
void handleTestSensors();

// Time configuration
const char* ntpServer = NTP_SERVER;
const long gmtOffset_sec = GMT_OFFSET_SEC;
const int daylightOffset_sec = DAYLIGHT_OFFSET_SEC;

void setup() {
    Serial.begin(115200);
    delay(1000);
    
    // Set up logging
    Logger::setLevel(LOG_LEVEL_INFO);
    Logger::info("Main", "========================================");
    Logger::info("Main", "ESP32 Printer Pot v" + String(FIRMWARE_VERSION));
    Logger::info("Main", "Build: " + String(BUILD_DATE) + " " + String(BUILD_TIME));
    Logger::info("Main", "========================================");
    
    // Initialize hardware abstraction layer
    hardware = new HardwareAbstraction();
    if (!hardware->initialize()) {
        Logger::error("Main", "Hardware initialization failed!");
        while(1) { delay(1000); }
    }
    
    // Initialize printer service
    printerService = new PrinterService(hardware);
    Logger::info("Main", "Printer service initialized");
    
    // Setup WiFi
    setupWiFi();
    
    // Setup time
    setupTime();
    
    // Initialize Firebase service
    firebase = new FirebaseService(FIREBASE_DATABASE_URL);
    firebase->setRetryPolicy(3, 1000);
    firebase->setRateLimit(60);  // 60 requests/minute with 2 second minimum window
    
    // Set authentication token if configured (optional)
    #ifdef FIREBASE_DATABASE_SECRET
    firebase->setAuthToken(FIREBASE_DATABASE_SECRET);
    #endif
    
    Logger::info("Main", "Firebase service initialized");
    
    // Initialize reminder service
    reminderService = new ReminderService(firebase);
    reminderService->load();
    Logger::info("Main", "Reminder service initialized");
    
    // Initialize OTA service
    otaService = new OTAUpdateService();
    #ifdef OTA_PASSWORD
    otaService->initialize("PrinterPot", OTA_PASSWORD);
    #else
    otaService->initialize("PrinterPot");
    #endif
    Logger::info("Main", "OTA update service initialized");
    
    // Initialize health monitor
    healthMonitor = new HealthMonitor();
    healthMonitor->setCheckInterval(60000);
    Logger::info("Main", "Health monitor initialized");
    
    // Initialize request queue
    requestQueue = new RequestQueue();
    requestQueue->setProcessInterval(2000);  // Process requests every 2 seconds
    Logger::info("Main", "Request queue initialized");
    
    // Load groceries and reminders (with delay to avoid rate limiting)
    delay(1000);  // Give Firebase service time to initialize
    loadGroceries();
    delay(500);
    reminderService->load();
    
    // Setup web server
    setupWebServer();
    
    Logger::info("Main", "Setup complete!");
    Logger::info("Main", "Starting main loop...");
}

void loop() {
    // Handle OTA updates (high priority)
    otaService->handle();
    
    // Handle web server requests
    server.handleClient();
    
    // Check for dispense timeout (safety feature)
    hardware->checkDispenseTimeout();
    
    // Update LED status
    hardware->setLED(WiFi.status() == WL_CONNECTED);
    
    // Check reminders every minute
    static unsigned long lastReminderCheck = 0;
    if (millis() - lastReminderCheck > 60000) {
        reminderService->checkReminders([](const Reminder& r) {
            Logger::info("Main", "⏰ Printing scheduled reminder: " + r.message);
            printerService->printReceipt(r.message, false, r.createdTime);
        });
        
        // Queue save after checking (in case any reminders were marked as printed or removed)
        // This ensures Firebase stays in sync
        String remindersJson = reminderService->toJSON();
        requestQueue->enqueue(REQUEST_FIREBASE_PUT, "/reminders.json", remindersJson);
        
        lastReminderCheck = millis();
    }
    
    // Update health monitor
    healthMonitor->update();
    
    // Process queued requests asynchronously (non-blocking)
    processRequestQueue();
    
    // Poll Firebase commands periodically (every 30 seconds)
    static unsigned long lastCommandPoll = 0;
    const unsigned long COMMAND_POLL_INTERVAL = 30000;  // 30 seconds
    if (millis() - lastCommandPoll > COMMAND_POLL_INTERVAL) {
        pollFirebaseCommands();
        lastCommandPoll = millis();
    }
    
    // Load reminders periodically (every 5 minutes)
    static unsigned long lastReminderLoad = 0;
    const unsigned long REMINDER_LOAD_INTERVAL = 300000;  // 5 minutes
    if (millis() - lastReminderLoad > REMINDER_LOAD_INTERVAL) {
        reminderService->load();
        lastReminderLoad = millis();
    }
    
    // Load groceries periodically (every 5 minutes) to stay in sync
    static unsigned long lastGroceryLoad = 0;
    const unsigned long GROCERY_LOAD_INTERVAL = 300000;  // 5 minutes
    if (millis() - lastGroceryLoad > GROCERY_LOAD_INTERVAL) {
        loadGroceries();
        lastGroceryLoad = millis();
    }
    
    // Update status periodically (every 5 minutes)
    static unsigned long lastStatusUpdate = 0;
    const unsigned long STATUS_UPDATE_INTERVAL = 300000;  // 5 minutes
    if (millis() - lastStatusUpdate > STATUS_UPDATE_INTERVAL) {
        // Read sensors
        hardware->readMoistureSensor();
        hardware->readIRSensor();
        
        // Queue status update (non-blocking)
        DynamicJsonDocument statusDoc(512);
        struct tm timeinfo;
        String timestamp = "N/A";
        if (getLocalTime(&timeinfo)) {
            char buffer[30];
            strftime(buffer, sizeof(buffer), "%Y-%m-%d %H:%M:%S", &timeinfo);
            timestamp = String(buffer);
        }
        
        statusDoc["timestamp"] = timestamp;
        statusDoc["wifi"] = true;
        statusDoc["irSensor"] = hardware->isIRDetected();
        statusDoc["dispensing"] = hardware->isDispensing();
        statusDoc["sanitizerLevel"] = hardware->getSanitizerLevel();
        statusDoc["moistureSensor"] = hardware->getMoisturePercent();
        statusDoc["weather"] = currentWeather;
        statusDoc["ip"] = deviceIP;
        statusDoc["status"] = "OK";
        statusDoc["firmware"] = FIRMWARE_VERSION;
        
        String statusJson;
        serializeJson(statusDoc, statusJson);
        requestQueue->enqueue(REQUEST_FIREBASE_PUT, "/status.json", statusJson);
        lastStatusUpdate = millis();
    }
    
    // Read sensors periodically (every 10 seconds)
    static unsigned long lastSensorCheck = 0;
    if (millis() - lastSensorCheck > SENSOR_CHECK_INTERVAL) {
        hardware->readMoistureSensor();
        hardware->readIRSensor();
        lastSensorCheck = millis();
    }
    
    delay(10);
}

void setupWiFi() {
    Logger::info("WiFi", "Setting up WiFi...");
    
    WiFiManager wm;
    WiFi.mode(WIFI_STA);
    
    bool res = wm.autoConnect(AP_SSID, AP_PASSWORD);
    
    if (!res) {
        Logger::error("WiFi", "Failed to connect");
    } else {
        deviceIP = WiFi.localIP().toString();
        Logger::info("WiFi", "Connected successfully!");
        Logger::info("WiFi", "IP Address: " + deviceIP);
    }
}

void setupTime() {
    configTime(gmtOffset_sec, daylightOffset_sec, ntpServer);
    Logger::info("Time", "NTP time server configured");
}


void getWeatherData() {
    Logger::info("Weather", "Getting weather data...");
    
    HTTPClient http;
    String url = "http://api.openweathermap.org/data/2.5/weather?lat=" + 
                 String(WEATHER_LATITUDE, 6) + "&lon=" + 
                 String(WEATHER_LONGITUDE, 6) + 
                 "&appid=" + String(WEATHER_API_KEY) + 
                 "&units=imperial";
    
    http.begin(url);
    http.setTimeout(10000);
    int httpCode = http.GET();
    
    if (httpCode == HTTP_CODE_OK) {
        String payload = http.getString();
        DynamicJsonDocument doc(512);
        deserializeJson(doc, payload);
        
        if (doc.containsKey("main") && doc["main"].containsKey("temp")) {
            float temp = doc["main"]["temp"];
            String description = doc["weather"][0]["description"];
            currentWeather = String(temp, 1) + "°F, " + description;
            printerService->setWeather(currentWeather);
            Logger::info("Weather", currentWeather);
        } else {
            Logger::warn("Weather", "Parsing error");
            currentWeather = "Unable to fetch";
        }
    } else {
        Logger::error("Weather", "API error: " + String(httpCode));
        currentWeather = "API Error";
    }
    
    http.end();
}

void pollFirebaseCommands() {
    Logger::info("Firebase", "📡 Polling commands...");
    
    DynamicJsonDocument commands(2048);
    if (!firebase->pollCommands(commands)) {
        Logger::warn("Firebase", "Failed to poll commands");
        return;
    }
    
    if (commands.size() == 0) {
        Logger::debug("Firebase", "No commands available");
        return;
    }
    
    Logger::info("Firebase", "Processing " + String(commands.size()) + " command(s)");
    
    for (JsonPair kv : commands.as<JsonObject>()) {
        String commandKey = kv.key().c_str();
        JsonObject command = kv.value();
        
        bool processed = command["processed"] | false;
        
        if (!processed) {
            String commandType = command["type"].as<String>();
            String commandData = command["data"].as<String>();
            
            Logger::info("Firebase", "✅ Command: " + commandType + " = " + commandData);
            
            // Process commands
            if (commandType == "dispense_start" || commandType == "water_start") {
                // Queue pump start (non-blocking)
                requestQueue->enqueue(REQUEST_DISPENSE_START);
            }
            else if (commandType == "dispense_stop" || commandType == "water_stop") {
                // Queue pump stop (non-blocking)
                requestQueue->enqueue(REQUEST_DISPENSE_STOP);
            }
            else if (commandType == "weather") {
                // Queue weather request (non-blocking)
                requestQueue->enqueue(REQUEST_WEATHER);
            }
            else if (commandType == "print") {
                Logger::info("Firebase", "🖨️ Queuing print command: " + commandData);
                // Queue print operation (non-blocking)
                requestQueue->enqueue(REQUEST_PRINT, "", commandData);
            }
            else if (commandType == "test_print") {
                Logger::info("Firebase", "🧪 Test print");
                printerService->printTest();
            }
            else if (commandType == "gpio_status" || commandType == "status") {
                hardware->printDiagnostics();
            }
            else {
                Logger::warn("Firebase", "⚠️ Unknown command: " + commandType);
            }
            
            // Mark as processed and delete
            String deleteUrl = "/commands/" + commandKey + ".json";
            firebase->deleteData(deleteUrl);
        }
    }
}

void updateFirebaseStatus() {
    Logger::debug("Firebase", "📊 Updating status...");
    
    DynamicJsonDocument doc(512);
    struct tm timeinfo;
    String timestamp = "N/A";
    if (getLocalTime(&timeinfo)) {
        char buffer[30];
        strftime(buffer, sizeof(buffer), "%Y-%m-%d %H:%M:%S", &timeinfo);
        timestamp = String(buffer);
    }
    
    doc["timestamp"] = timestamp;
    doc["wifi"] = true;
    doc["irSensor"] = hardware->isIRDetected();
    doc["dispensing"] = hardware->isDispensing();
    doc["sanitizerLevel"] = hardware->getSanitizerLevel();
    doc["moistureSensor"] = hardware->getMoisturePercent();
    doc["weather"] = currentWeather;
    doc["ip"] = deviceIP;
    doc["status"] = "OK";
    doc["firmware"] = FIRMWARE_VERSION;
    
    if (firebase->updateStatus(doc)) {
        Logger::debug("Firebase", "Status updated");
    }
}

void loadGroceries() {
    String response;
    if (!firebase->get("/groceries.json", response)) {
        Logger::warn("Groceries", "Failed to load from Firebase (may not exist yet)");
        groceryCount = 0;
        return;
    }
    
    if (response == "null" || response.length() == 0 || response == "{}") {
        Logger::info("Groceries", "No groceries in Firebase (empty list)");
        groceryCount = 0;
        return;
    }
    
    DynamicJsonDocument doc(4096);
    DeserializationError error = deserializeJson(doc, response);
    
    if (error) {
        Logger::error("Groceries", "Failed to parse JSON: " + String(error.c_str()));
        groceryCount = 0;
        return;
    }
    
    groceryCount = 0;
    
    // Try array format first (preferred)
    if (doc.is<JsonArray>()) {
        JsonArray array = doc.as<JsonArray>();
        for (JsonVariant item : array) {
            if (groceryCount >= MAX_GROCERY_ITEMS) break;
            groceryItems[groceryCount] = item.as<String>();
            groceryCount++;
        }
        Logger::info("Groceries", "Loaded " + String(groceryCount) + " items (array format)");
    }
    // Fallback to object format (for backwards compatibility)
    else if (doc.is<JsonObject>()) {
        for (JsonPair kv : doc.as<JsonObject>()) {
            if (groceryCount >= MAX_GROCERY_ITEMS) break;
            groceryItems[groceryCount] = kv.value().as<String>();
            groceryCount++;
        }
        Logger::info("Groceries", "Loaded " + String(groceryCount) + " items (object format)");
    } else {
        Logger::warn("Groceries", "Unknown JSON format");
        groceryCount = 0;
    }
}

void saveGroceries() {
    DynamicJsonDocument doc(4096);
    JsonArray array = doc.to<JsonArray>();
    
    for (int i = 0; i < groceryCount; i++) {
        array.add(groceryItems[i]);
    }
    
    String json;
    serializeJson(doc, json);
    
    // Queue the Firebase save operation (non-blocking)
    requestQueue->enqueue(REQUEST_FIREBASE_PUT, "/groceries.json", json);
    Logger::info("Groceries", "Queued save to Firebase (" + String(groceryCount) + " items)");
}

void printGroceryList() {
    if (groceryCount == 0) {
        Logger::warn("Groceries", "List is empty");
        return;
    }
    
    Logger::info("Groceries", "🛒 Printing list (" + String(groceryCount) + " items)");
    printerService->printGroceryList(groceryItems, groceryCount);
}

bool isAuthenticated() {
    // Check if token exists and hasn't expired
    if (authToken.length() == 0) {
        Logger::debug("WebServer", "Auth check: No token set");
        return false;
    }
    
    if (millis() > authTokenExpiry) {
        authToken = "";  // Token expired
        Logger::debug("WebServer", "Auth check: Token expired");
        return false;
    }
    
    // Check if request has valid token
    String cookie = server.header("Cookie");
    Logger::debug("WebServer", "Auth check from " + server.client().remoteIP().toString());
    
    if (cookie.length() > 0) {
        // Parse cookie string - look for "auth=TOKEN" or "auth=TOKEN;"
        String cookieLower = cookie;
        cookieLower.toLowerCase(); // Convert to lowercase for case-insensitive search
        int authIndex = cookieLower.indexOf("auth=");
        if (authIndex >= 0) {
            // Use original cookie (case-sensitive) for token extraction
            int tokenStart = authIndex + 5; // "auth=" is 5 chars
            int tokenEnd = cookie.indexOf(";", tokenStart);
            if (tokenEnd < 0) {
                tokenEnd = cookie.length(); // End of string if no semicolon
            }
            String cookieToken = cookie.substring(tokenStart, tokenEnd);
            cookieToken.trim(); // Remove any whitespace
            
            // Compare tokens (exact match, case-sensitive)
            if (cookieToken.length() == authToken.length() && cookieToken == authToken) {
                Logger::debug("WebServer", "✅ Valid cookie found - authenticated");
                return true;
            }
        }
    }
    
    // Also check URL parameter (for API calls)
    String tokenParam = server.arg("token");
    if (tokenParam.length() > 0 && tokenParam == authToken) {
        Logger::debug("WebServer", "✅ Valid token parameter - authenticated");
        return true;
    }
    
    Logger::debug("WebServer", "❌ No valid authentication");
    return false;
}

void setupWebServer() {
    // Login endpoints (no auth required)
    server.on("/login", HTTP_GET, handleLogin);
    server.on("/login", HTTP_POST, handleLogin);
    server.on("/logout", HTTP_GET, handleLogout);
    
    // Main page (requires auth)
    server.on("/", handleRoot);
    
    // API endpoints (require auth - check in handlers)
    server.on("/api/status", HTTP_GET, handleGetStatus);
    server.on("/api/health", HTTP_GET, handleHealth);
    server.on("/api/queue", HTTP_GET, handleQueueStatus);
    server.on("/api/reset-sanitizer", HTTP_POST, handleResetSanitizer);
    
    // Hardware test endpoints
    server.on("/test", HTTP_GET, handleTestPage);
    server.on("/api/test/led", HTTP_POST, handleTestLED);
    server.on("/api/test/pump", HTTP_POST, handleTestPump);
    server.on("/api/test/printer", HTTP_POST, handleTestPrinter);
    server.on("/api/test/sensors", HTTP_GET, handleTestSensors);
    
    // Reminder endpoints
    server.on("/api/reminders", HTTP_GET, handleGetReminders);
    server.on("/api/reminders", HTTP_POST, handleAddReminder);
    
    // Grocery endpoints
    server.on("/api/groceries", HTTP_GET, handleGetGroceries);
    server.on("/api/groceries", HTTP_POST, handleAddGrocery);
    server.on("/api/groceries", HTTP_DELETE, handleClearGroceries);
    server.on("/api/groceries/print", HTTP_POST, handlePrintGroceries);
    
    server.on("/favicon.ico", handleFavicon);
    
    // Handle DELETE for reminders and groceries (dynamic routes)
    server.onNotFound([]() {
        String uri = server.uri();
        HTTPMethod method = server.method();
        
        // Handle dynamic DELETE routes
        if (uri.startsWith("/api/reminders/") && method == HTTP_DELETE) {
            handleDeleteReminder();
            return;
        }
        if (uri.startsWith("/api/groceries/") && method == HTTP_DELETE) {
            handleDeleteGrocery();
            return;
        }
        
        // Handle static files
        if (uri == "/favicon.ico" || uri == "/robots.txt") {
            server.send(204);
            return;
        }
        
        // Unknown route
        server.send(404, "application/json", "{\"success\":false,\"message\":\"Not Found\"}");
    });
    
    server.begin();
    Logger::info("WebServer", "🌐 HTTP Server started on http://" + deviceIP + ":" + String(8080));
    Logger::info("WebServer", "   Access at: http://" + deviceIP + ":8080");
}

void handleHealth() {
    if (!isAuthenticated()) {
        server.send(401, "application/json", "{\"error\":\"Unauthorized\"}");
        return;
    }
    Logger::debug("WebServer", "Health check from " + server.client().remoteIP().toString());
    server.send(200, "application/json", healthMonitor->getHealthJSON());
}

void handleQueueStatus() {
    if (!isAuthenticated()) {
        server.send(401, "application/json", "{\"error\":\"Unauthorized\"}");
        return;
    }
    DynamicJsonDocument doc(256);
    doc["size"] = requestQueue->getSize();
    doc["maxSize"] = 20;
    doc["isEmpty"] = requestQueue->isEmpty();
    doc["isFull"] = requestQueue->isFull();
    
    String response;
    serializeJson(doc, response);
    server.send(200, "application/json", response);
}

void handleGetStatus() {
    if (!isAuthenticated()) {
        server.send(401, "application/json", "{\"error\":\"Unauthorized\"}");
        return;
    }
    Logger::debug("WebServer", "Status request from " + server.client().remoteIP().toString());
    
    DynamicJsonDocument doc(256);
    doc["moisture"] = String(hardware->getMoisturePercent(), 1);
    doc["sanitizer"] = String(hardware->getSanitizerLevel(), 1);
    
    String response;
    serializeJson(doc, response);
    server.send(200, "application/json", response);
}

void handleResetSanitizer() {
    if (!isAuthenticated()) {
        server.send(401, "application/json", "{\"error\":\"Unauthorized\"}");
        return;
    }
    
    Logger::info("WebServer", "🔄 Reset sanitizer from " + server.client().remoteIP().toString());
    
    if (server.method() != HTTP_POST) {
        server.send(405, "text/plain", "Method Not Allowed");
        return;
    }
    
    hardware->resetSanitizer();
    
    DynamicJsonDocument doc(128);
    doc["success"] = true;
    doc["message"] = "Sanitizer reset to 100%";
    
    String response;
    serializeJson(doc, response);
    server.send(200, "application/json", response);
}

void handleAddReminder() {
    String body = server.arg("plain");
    DynamicJsonDocument doc(512);
    DeserializationError error = deserializeJson(doc, body);
    
    if (error || !doc.containsKey("message") || !doc.containsKey("scheduledTime")) {
        server.send(400, "text/plain", "Invalid request");
        return;
    }
    
    String message = doc["message"].as<String>();
    time_t scheduledTime = doc["scheduledTime"].as<time_t>();
    
    // Add reminder locally (fast - immediate response)
    String id = reminderService->addReminder(message, scheduledTime);
    if (id.length() > 0) {
        Logger::info("WebServer", "📝 Reminder added: " + message);
        
        // Queue Firebase save (non-blocking)
        String remindersJson = reminderService->toJSON();
        requestQueue->enqueue(REQUEST_FIREBASE_PUT, "/reminders.json", remindersJson);
        
        // Send immediate response - Firebase save happens in background
        DynamicJsonDocument responseDoc(128);
        responseDoc["success"] = true;
        responseDoc["id"] = id;
        responseDoc["message"] = "Reminder added (saving to Firebase in background)";
        responseDoc["queueSize"] = requestQueue->getSize();
        
        String response;
        serializeJson(responseDoc, response);
        server.send(200, "application/json", response);
    } else {
        server.send(400, "text/plain", "Failed to add reminder");
    }
}

void handleGetReminders() {
    if (!isAuthenticated()) {
        server.send(401, "application/json", "{\"error\":\"Unauthorized\"}");
        return;
    }
    DynamicJsonDocument doc(4096);
    JsonArray array = doc.to<JsonArray>();
    
    time_t currentTime = time(nullptr);
    
    for (int i = 0; i < reminderService->getReminderCount(); i++) {
        const Reminder* r = reminderService->getReminder(i);
        // Only include active reminders that haven't passed yet
        if (r && r->active && r->scheduledTime > currentTime) {
            JsonObject reminder = array.createNestedObject();
            reminder["id"] = r->id;
            reminder["message"] = r->message;
            reminder["scheduledTime"] = r->scheduledTime;
            reminder["printed"] = r->printed;
        }
    }
    
    String response;
    serializeJson(doc, response);
    server.send(200, "application/json", response);
}

void handleDeleteReminder() {
    if (!isAuthenticated()) {
        server.send(401, "application/json", "{\"error\":\"Unauthorized\"}");
        return;
    }
    String uri = server.uri();
    int lastSlash = uri.lastIndexOf('/');
    String id = uri.substring(lastSlash + 1);
    
    // Delete locally (fast)
    if (reminderService->deleteReminder(id)) {
        Logger::info("WebServer", "🗑️ Reminder deleted: " + id);
        
        // Queue Firebase save (non-blocking)
        String remindersJson = reminderService->toJSON();
        requestQueue->enqueue(REQUEST_FIREBASE_PUT, "/reminders.json", remindersJson);
        
        // Send immediate response
        DynamicJsonDocument responseDoc(128);
        responseDoc["success"] = true;
        responseDoc["message"] = "Reminder deleted (saving to Firebase in background)";
        
        String response;
        serializeJson(responseDoc, response);
        server.send(200, "application/json", response);
    } else {
        DynamicJsonDocument errorDoc(128);
        errorDoc["success"] = false;
        errorDoc["message"] = "Reminder not found";
        String errorResponse;
        serializeJson(errorDoc, errorResponse);
        server.send(404, "application/json", errorResponse);
    }
}

void handleAddGrocery() {
    String body = server.arg("plain");
    DynamicJsonDocument doc(256);
    DeserializationError error = deserializeJson(doc, body);
    
    if (error || !doc.containsKey("item")) {
        server.send(400, "text/plain", "Invalid request");
        return;
    }
    
    if (groceryCount >= MAX_GROCERY_ITEMS) {
        server.send(507, "text/plain", "Too many items");
        return;
    }
    
    String item = doc["item"].as<String>();
    item.trim();
    
    if (item.length() == 0) {
        server.send(400, "text/plain", "Item cannot be empty");
        return;
    }
    
    // Add to local array immediately (fast response)
    groceryItems[groceryCount] = item;
    groceryCount++;
    
    // Queue Firebase save (non-blocking - responds immediately)
    saveGroceries();
    
    Logger::info("WebServer", "🛒 Grocery added: " + item);
    
    // Send immediate response - Firebase save happens in background
    DynamicJsonDocument responseDoc(128);
    responseDoc["success"] = true;
    responseDoc["message"] = "Item added (saving to Firebase in background)";
    responseDoc["queueSize"] = requestQueue->getSize();
    
    String response;
    serializeJson(responseDoc, response);
    server.send(200, "application/json", response);
}

void handleGetGroceries() {
    if (!isAuthenticated()) {
        server.send(401, "application/json", "{\"error\":\"Unauthorized\"}");
        return;
    }
    DynamicJsonDocument doc(4096);
    JsonArray array = doc.to<JsonArray>();
    
    for (int i = 0; i < groceryCount; i++) {
        array.add(groceryItems[i]);
    }
    
    String response;
    serializeJson(doc, response);
    server.send(200, "application/json", response);
}

void handleDeleteGrocery() {
    String uri = server.uri();
    // Extract index from /api/groceries/{index}
    int lastSlash = uri.lastIndexOf('/');
    String indexStr = uri.substring(lastSlash + 1);
    int index = indexStr.toInt();
    
    if (index < 0 || index >= groceryCount) {
        DynamicJsonDocument errorDoc(128);
        errorDoc["success"] = false;
        errorDoc["message"] = "Item not found";
        String errorResponse;
        serializeJson(errorDoc, errorResponse);
        server.send(404, "application/json", errorResponse);
        return;
    }
    
    // Remove item by shifting array
    for (int i = index; i < groceryCount - 1; i++) {
        groceryItems[i] = groceryItems[i + 1];
    }
    groceryCount--;
    
    // Queue Firebase save (non-blocking)
    saveGroceries();
    
    Logger::info("WebServer", "🗑️ Grocery item deleted at index " + String(index));
    
    DynamicJsonDocument responseDoc(128);
    responseDoc["success"] = true;
    responseDoc["message"] = "Item deleted";
    
    String response;
    serializeJson(responseDoc, response);
    server.send(200, "application/json", response);
}

void handleClearGroceries() {
    if (server.method() != HTTP_DELETE) {
        server.send(405, "text/plain", "Method Not Allowed");
        return;
    }
    
    // Clear locally (fast)
    groceryCount = 0;
    
    // Queue Firebase save (non-blocking)
    saveGroceries();
    
    Logger::info("WebServer", "🗑️ Groceries cleared");
    
    // Send immediate response
    DynamicJsonDocument responseDoc(128);
    responseDoc["success"] = true;
    responseDoc["message"] = "Groceries cleared (saving to Firebase in background)";
    responseDoc["queueSize"] = requestQueue->getSize();
    
    String response;
    serializeJson(responseDoc, response);
    server.send(200, "application/json", response);
}

void handlePrintGroceries() {
    if (!isAuthenticated()) {
        server.send(401, "application/json", "{\"error\":\"Unauthorized\"}");
        return;
    }
    if (server.method() != HTTP_POST) {
        server.send(405, "text/plain", "Method Not Allowed");
        return;
    }
    
    if (groceryCount == 0) {
        DynamicJsonDocument doc(128);
        doc["success"] = false;
        doc["message"] = "Grocery list is empty";
        String response;
        serializeJson(doc, response);
        server.send(400, "application/json", response);
        return;
    }
    
    printGroceryList();
    
    DynamicJsonDocument doc(128);
    doc["success"] = true;
    doc["message"] = "Printing grocery list";
    String response;
    serializeJson(doc, response);
    server.send(200, "application/json", response);
}

void handleFavicon() {
    server.send(204);
}

void handleLogin() {
    // If already authenticated, redirect to main page
    if (isAuthenticated() && server.method() == HTTP_GET) {
        server.sendHeader("Location", "/");
        server.send(302, "text/plain", "");
        return;
    }
    
    if (server.method() == HTTP_POST) {
        // Try to get password from form data
        String password = server.arg("password");
        
        // Fallback: if arg() doesn't work, try parsing from body
        if (password.length() == 0) {
            String body = server.arg("plain");
            int passwordIndex = body.indexOf("password=");
            if (passwordIndex >= 0) {
                int passwordStart = passwordIndex + 9; // "password=" is 9 chars
                int passwordEnd = body.indexOf("&", passwordStart);
                if (passwordEnd < 0) {
                    passwordEnd = body.length();
                }
                password = body.substring(passwordStart, passwordEnd);
                // URL decode if needed (simple version)
                password.replace("+", " ");
            }
        }
        
        password.trim();  // Remove any leading/trailing whitespace
        
        #ifdef WEB_PASSWORD_OVERRIDE
        String correctPassword = String(WEB_PASSWORD_OVERRIDE);
        #else
        String correctPassword = String(WEB_PASSWORD);
        #endif
        correctPassword.trim();  // Ensure no whitespace in expected password
        
        // More robust password comparison (case-sensitive, exact match)
        bool passwordMatch = (password.length() == correctPassword.length()) && 
                            (password.equals(correctPassword));
        
        if (passwordMatch) {
            // Generate simple token (timestamp + random)
            authToken = String(millis()) + String(random(1000, 9999));
            authTokenExpiry = millis() + AUTH_TOKEN_DURATION;
            
            Logger::info("WebServer", "✅ Login successful from " + server.client().remoteIP().toString());
            
            // Set cookie with proper attributes for better browser compatibility
            String cookieHeader = "auth=" + authToken + "; Path=/; Max-Age=3600; SameSite=Lax";
            server.sendHeader("Set-Cookie", cookieHeader);
            
            // Use redirect with token in URL parameter as primary method (more reliable than cookies)
            // Also set cookie as backup
            String redirectUrl = "/?token=" + authToken;
            String redirectPage = "<!DOCTYPE html><html><head><meta charset=\"UTF-8\"><script>"
                                 "document.cookie=\"auth=" + authToken + "; Path=/; Max-Age=3600; SameSite=Lax\";"
                                 "setTimeout(function(){window.location.href=\"" + redirectUrl + "\";},50);"
                                 "</script></head><body><p>Login successful! Redirecting...</p><p>If you are not redirected, <a href=\"" + redirectUrl + "\">click here</a>.</p></body></html>";
            
            server.send(200, "text/html", redirectPage);
        } else {
            Logger::warn("WebServer", "❌ Failed login attempt from " + server.client().remoteIP().toString());
            delay(1000);  // Small delay to prevent brute force
            server.sendHeader("Location", "/login?error=1");
            server.send(302, "text/plain", "");
        }
        return;
    }
    
    // Show login page
    String errorMsg = server.hasArg("error") ? "<p style='color:#8b4513; text-align:center; margin-top:10px;'>❌ Incorrect password</p>" : "";
    
    String loginPage = R"HTML(
<!DOCTYPE html>
<html>
<head>
    <meta charset="UTF-8">
    <meta name="viewport" content="width=device-width, initial-scale=1.0">
    <title>Login - Printer Pot</title>
    <style>
        * { margin: 0; padding: 0; box-sizing: border-box; }
        body {
            font-family: -apple-system, BlinkMacSystemFont, 'Segoe UI', Roboto, sans-serif;
            background: linear-gradient(135deg, #2d5016 0%, #4a7c2a 50%, #6b9f3d 100%);
            min-height: 100vh;
            display: flex;
            align-items: center;
            justify-content: center;
            padding: 20px;
        }
        .login-container {
            background: #f5f5f0;
            border-radius: 20px;
            padding: 40px;
            box-shadow: 0 20px 60px rgba(45, 80, 22, 0.4);
            border: 2px solid #4a7c2a;
            max-width: 400px;
            width: 100%;
        }
        h1 {
            color: #2d5016;
            text-align: center;
            margin-bottom: 30px;
            font-size: 32px;
        }
        input[type="password"] {
            width: 100%;
            padding: 16px;
            border: 2px solid #4a7c2a;
            border-radius: 10px;
            font-size: 18px;
            margin-bottom: 20px;
            font-family: inherit;
        }
        button {
            width: 100%;
            padding: 16px;
            background: linear-gradient(135deg, #2d5016 0%, #4a7c2a 100%);
            color: white;
            border: none;
            border-radius: 10px;
            font-size: 18px;
            font-weight: 600;
            cursor: pointer;
            transition: transform 0.2s;
        }
        button:active {
            transform: scale(0.98);
        }
    </style>
</head>
<body>
    <div class="login-container">
        <h1>🌵💌 Printer Pot</h1>
        <form method="POST" action="/login" enctype="application/x-www-form-urlencoded">
            <input type="password" name="password" placeholder="Enter password..." required autofocus autocomplete="current-password">
            <button type="submit">Login</button>
        </form>
        )HTML" + errorMsg + R"HTML(
    </div>
</body>
</html>
)HTML";
    
    server.send(200, "text/html", loginPage);
}

void handleLogout() {
    authToken = "";
    authTokenExpiry = 0;
    server.sendHeader("Set-Cookie", "auth=; Path=/; Max-Age=0");
    server.sendHeader("Location", "/login");
    server.send(302, "text/plain", "");
}

void handleRoot() {
    // Check authentication
    if (!isAuthenticated()) {
        server.sendHeader("Location", "/login");
        server.send(302, "text/plain", "");
        return;
    }
    
    String html = R"HTML(
<!DOCTYPE html>
<html>
<head>
    <meta charset="UTF-8">
    <meta name="viewport" content="width=device-width, initial-scale=1.0, maximum-scale=1.0, user-scalable=no, viewport-fit=cover">
    <meta name="apple-mobile-web-app-capable" content="yes">
    <title>Printer Pot v2.0</title>
    <style>
        * { margin: 0; padding: 0; box-sizing: border-box; -webkit-tap-highlight-color: transparent; }
        body {
            font-family: -apple-system, BlinkMacSystemFont, 'Segoe UI', Roboto, sans-serif;
            background: linear-gradient(135deg, #2d5016 0%, #4a7c2a 50%, #6b9f3d 100%);
            min-height: 100vh;
            padding: 10px;
            padding-bottom: env(safe-area-inset-bottom);
        }
        .container {
            max-width: 600px;
            margin: 0 auto;
            background: #f5f5f0;
            border-radius: 20px;
            padding: 20px;
            box-shadow: 0 20px 60px rgba(45, 80, 22, 0.4);
            border: 2px solid #4a7c2a;
        }
        h1 {
            color: #2d5016;
            margin-bottom: 10px;
            text-align: center;
            font-size: 28px;
        }
        .version {
            text-align: center;
            color: #999;
            font-size: 12px;
            margin-bottom: 20px;
        }
        .status-section {
            background: #f5f5f0;
            padding: 20px;
            border-radius: 12px;
            margin-bottom: 20px;
            border: 1px solid #4a7c2a;
        }
        .status-grid {
            display: grid;
            grid-template-columns: 1fr 1fr;
            gap: 15px;
            margin-bottom: 15px;
        }
        .status-card {
            background: white;
            padding: 15px;
            border-radius: 10px;
            text-align: center;
        }
        .status-label {
            font-size: 14px;
            color: #666;
            margin-bottom: 8px;
        }
        .status-value {
            font-size: 24px;
            font-weight: 700;
            color: #2d5016;
        }
        button {
            width: 100%;
            padding: 16px;
            background: linear-gradient(135deg, #2d5016 0%, #4a7c2a 100%);
            color: white;
            border: none;
            border-radius: 10px;
            font-size: 17px;
            font-weight: 600;
            cursor: pointer;
            transition: transform 0.2s;
        }
        button:active {
            transform: scale(0.98);
        }
        .tabs {
            display: flex;
            gap: 10px;
            margin-bottom: 20px;
            border-bottom: 2px solid #e0e0e0;
        }
        .tab-btn {
            flex: 1;
            padding: 12px;
            background: transparent;
            border: none;
            border-bottom: 3px solid transparent;
            color: #666;
            font-size: 16px;
            font-weight: 600;
            cursor: pointer;
        }
        .tab-btn.active {
            color: #2d5016;
            border-bottom-color: #2d5016;
        }
        .tab-content {
            display: none;
        }
        .tab-content.active {
            display: block;
        }
        textarea {
            width: 100%;
            padding: 14px;
            border: 2px solid #e0e0e0;
            border-radius: 10px;
            font-size: 16px;
            min-height: 120px;
            font-family: inherit;
            margin-bottom: 15px;
        }
        input {
            width: 100%;
            padding: 14px;
            border: 2px solid #e0e0e0;
            border-radius: 10px;
            font-size: 16px;
            margin-bottom: 15px;
        }
        .reminder-item, .grocery-item {
            background: #f5f5f0;
            padding: 12px;
            border-radius: 8px;
            margin-bottom: 8px;
            display: flex;
            justify-content: space-between;
            align-items: center;
            border-left: 3px solid #4a7c2a;
        }
        .list-container {
            max-height: 400px;
            overflow-y: auto;
            overflow-x: hidden;
            margin-top: 15px;
            padding-right: 5px;
        }
        .list-container::-webkit-scrollbar {
            width: 8px;
        }
        .list-container::-webkit-scrollbar-track {
            background: #f1f1f1;
            border-radius: 10px;
        }
        .list-container::-webkit-scrollbar-thumb {
            background: #4a7c2a;
            border-radius: 10px;
        }
        .list-container::-webkit-scrollbar-thumb:hover {
            background: #2d5016;
        }
        .item-content {
            flex: 1;
        }
        .item-actions {
            display: flex;
            gap: 8px;
        }
        .btn-small {
            padding: 6px 12px;
            font-size: 14px;
            border-radius: 6px;
            cursor: pointer;
            border: none;
            font-weight: 600;
        }
        .btn-delete {
            background: #8b4513;
            color: white;
        }
        .btn-edit {
            background: #4a7c2a;
            color: white;
        }
        .btn-delete:hover, .btn-edit:hover {
            opacity: 0.9;
        }
    </style>
</head>
<body>
    <div class="container">
        <h1>🌵💌 Printer Pot</h1>
        <div style="text-align: center; margin-bottom: 15px;">
            <a href="#" onclick="window.location.href=addAuthToken('/test'); return false;" style="color: #4a7c2a; text-decoration: none; font-weight: 600; font-size: 14px;">🔧 Hardware Test</a>
        </div>
        
        <div class="status-section">
            <h2 style="margin-bottom: 15px;">System Status</h2>
            <div class="status-grid">
                <div class="status-card">
                    <div class="status-label">Moisture</div>
                    <div class="status-value" id="moisture">--</div>
                </div>
                <div class="status-card">
                    <div class="status-label">Sanitizer</div>
                    <div class="status-value" id="sanitizer">--</div>
                </div>
            </div>
            <button onclick="resetSanitizer()">Reset Sanitizer</button>
        </div>
        
        <div class="tabs">
            <button class="tab-btn active" onclick="showTab('reminders')">Reminders</button>
            <button class="tab-btn" onclick="showTab('groceries')">Groceries</button>
        </div>
        
        <div id="reminders-tab" class="tab-content active">
            <h2>Add Reminder</h2>
            <textarea id="reminder-msg" placeholder="Enter message..." maxlength="200" oninput="updateCharCount()"></textarea>
            <div style="text-align: right; margin-bottom: 15px; color: #666; font-size: 14px;">
                <span id="char-count">0</span>/200 characters
            </div>
            <label style="display: block; margin-bottom: 8px; font-weight: 600; color: #333;">Schedule Time:</label>
            <select id="reminder-time-type" style="width: 100%; padding: 14px; border: 2px solid #e0e0e0; border-radius: 10px; font-size: 16px; margin-bottom: 15px;" onchange="updateTimeInput()">
                <option value="quick">Quick Options</option>
                <option value="custom">Custom Date/Time</option>
            </select>
            <div id="quick-time-options" style="display: grid; grid-template-columns: 1fr 1fr; gap: 10px; margin-bottom: 15px;">
                <button type="button" onclick="setQuickTime(1)" style="background: #4a7c2a;">1 min</button>
                <button type="button" onclick="setQuickTime(30)" style="background: #4a7c2a;">30 mins</button>
                <button type="button" onclick="setQuickTime(60)" style="background: #4a7c2a;">1 hour</button>
                <button type="button" onclick="setQuickTime(720)" style="background: #4a7c2a;">12 hours</button>
                <button type="button" onclick="setQuickTime(1440)" style="background: #4a7c2a;">1 day</button>
                <button type="button" onclick="setQuickTime(10080)" style="background: #4a7c2a;">1 week</button>
            </div>
            <input type="datetime-local" id="reminder-time" style="display: none;">
            <div id="selected-time" style="padding: 12px; background: #e8f5e9; border-radius: 8px; margin-bottom: 15px; display: none; border-left: 3px solid #4a7c2a;">
                <strong>Selected:</strong> <span id="time-display">Not set</span>
            </div>
            <button onclick="addReminder()">Add Reminder</button>
            <div class="list-container">
                <div id="reminders-list"></div>
            </div>
        </div>
        
        <div id="groceries-tab" class="tab-content">
            <h2>Grocery List</h2>
            <input type="text" id="grocery-item" placeholder="Add item...">
            <button onclick="addGrocery()">Add Item</button>
            <div class="list-container">
                <div id="grocery-list"></div>
            </div>
            <button onclick="printGroceries()" style="margin-top:10px;">Print List</button>
            <button onclick="clearGroceries()" style="margin-top:10px;background:#8b4513;">Clear List</button>
        </div>
    </div>
    
    <script>
        // Check for token in URL and store it
        (function() {
            const urlParams = new URLSearchParams(window.location.search);
            const token = urlParams.get('token');
            if (token) {
                // Store token in sessionStorage for future requests
                sessionStorage.setItem('authToken', token);
                // Set cookie as backup
                document.cookie = 'auth=' + token + '; Path=/; Max-Age=3600; SameSite=Lax';
                // Remove token from URL for security
                window.history.replaceState({}, document.title, window.location.pathname);
            }
        })();
        
        // Helper function to add auth token to URL
        function addAuthToken(url) {
            const token = sessionStorage.getItem('authToken');
            if (token) {
                const separator = url.includes('?') ? '&' : '?';
                return url + separator + 'token=' + encodeURIComponent(token);
            }
            return url;
        }
        
        function loadStatus() {
            fetch(addAuthToken('/api/status'))
                .then(r => {
                    if (!r.ok) {
                        throw new Error('HTTP error: ' + r.status);
                    }
                    const contentType = r.headers.get('content-type');
                    if (contentType && contentType.includes('application/json')) {
                        return r.json();
                    } else {
                        return r.text().then(text => {
                            console.error('Non-JSON response:', text);
                            return {moisture: '--', sanitizer: '--'};
                        });
                    }
                })
                .then(data => {
                    if (data) {
                        document.getElementById('moisture').textContent = (data.moisture || '--') + '%';
                        document.getElementById('sanitizer').textContent = (data.sanitizer || '--') + '%';
                    }
                })
                .catch(err => {
                    console.error('Status load error:', err);
                });
        }
        
        function resetSanitizer() {
            if (confirm('Reset sanitizer to 100%?')) {
                fetch(addAuthToken('/api/reset-sanitizer'), {method: 'POST'})
                    .then(r => {
                        if (!r.ok) {
                            throw new Error('HTTP error: ' + r.status);
                        }
                        const contentType = r.headers.get('content-type');
                        if (contentType && contentType.includes('application/json')) {
                            return r.json();
                        } else {
                            return r.text().then(text => ({success: false, message: text || 'Unknown error'}));
                        }
                    })
                    .then(data => {
                        if (data.success) {
                            alert('✅ Sanitizer reset to 100%!');
                            loadStatus();
                        } else {
                            alert('❌ Error resetting sanitizer');
                        }
                    })
                    .catch(err => {
                        console.error('Reset sanitizer error:', err);
                        alert('❌ Error: ' + err.message);
                    });
            }
        }
        
        function showTab(tab) {
            document.querySelectorAll('.tab-btn').forEach(b => b.classList.remove('active'));
            document.querySelectorAll('.tab-content').forEach(c => c.classList.remove('active'));
            event.target.classList.add('active');
            document.getElementById(tab + '-tab').classList.add('active');
            if (tab === 'groceries') loadGroceries();
            if (tab === 'reminders') loadReminders();
        }
        
        let selectedScheduledTime = null;
        
        function updateCharCount() {
            const msg = document.getElementById('reminder-msg').value;
            const count = msg.length;
            const max = 200;
            const counter = document.getElementById('char-count');
            counter.textContent = count;
            if (count > max * 0.9) {
                counter.style.color = '#8b4513';
            } else if (count > max * 0.7) {
                counter.style.color = '#6b9f3d';
            } else {
                counter.style.color = '#4a7c2a';
            }
        }
        
        function updateTimeInput() {
            const type = document.getElementById('reminder-time-type').value;
            const quickOptions = document.getElementById('quick-time-options');
            const customInput = document.getElementById('reminder-time');
            const selectedTime = document.getElementById('selected-time');
            
            if (type === 'quick') {
                quickOptions.style.display = 'grid';
                customInput.style.display = 'none';
                selectedTime.style.display = selectedScheduledTime ? 'block' : 'none';
            } else {
                quickOptions.style.display = 'none';
                customInput.style.display = 'block';
                selectedTime.style.display = 'none';
                selectedScheduledTime = null;
            }
        }
        
        function setQuickTime(minutes) {
            const now = new Date();
            const scheduled = new Date(now.getTime() + minutes * 60000);
            selectedScheduledTime = Math.floor(scheduled.getTime() / 1000);
            
            const timeDisplay = document.getElementById('time-display');
            const hours = Math.floor(minutes / 60);
            const mins = minutes % 60;
            let timeStr = '';
            if (hours > 0) {
                timeStr = hours + (hours === 1 ? ' hour' : ' hours');
                if (mins > 0) timeStr += ' ' + mins + ' mins';
            } else {
                timeStr = mins + ' mins';
            }
            timeStr += ' from now (' + scheduled.toLocaleString() + ')';
            timeDisplay.textContent = timeStr;
            document.getElementById('selected-time').style.display = 'block';
        }
        
        function addReminder() {
            const msg = document.getElementById('reminder-msg').value.trim();
            if (!msg) return alert('Please enter a message');
            
            let scheduledTime;
            const type = document.getElementById('reminder-time-type').value;
            
            if (type === 'quick') {
                if (!selectedScheduledTime) return alert('Please select a time');
                scheduledTime = selectedScheduledTime;
            } else {
                const time = document.getElementById('reminder-time').value;
                if (!time) return alert('Please select a date and time');
                scheduledTime = Math.floor(new Date(time).getTime() / 1000);
            }
            
            if (scheduledTime <= Math.floor(Date.now() / 1000)) {
                return alert('Please select a time in the future');
            }
            
            fetch(addAuthToken('/api/reminders'), {
                method: 'POST',
                headers: {'Content-Type': 'application/json'},
                body: JSON.stringify({message: msg, scheduledTime: scheduledTime})
            })
            .then(r => {
                const contentType = r.headers.get('content-type');
                if (contentType && contentType.includes('application/json')) {
                    return r.json();
                } else {
                    return r.text().then(text => ({success: false, message: text || 'Unknown error'}));
                }
            })
            .then(data => {
                if (data.success) {
                    alert('✅ Reminder added!');
                    document.getElementById('reminder-msg').value = '';
                    document.getElementById('selected-time').style.display = 'none';
                    selectedScheduledTime = null;
                    updateCharCount();
                    loadReminders();
                } else {
                    alert('❌ Error: ' + (data.message || 'Failed to add reminder'));
                }
            })
            .catch(err => {
                alert('❌ Error: ' + err.message);
            });
        }
        
        function loadReminders() {
            fetch(addAuthToken('/api/reminders'))
                .then(r => {
                    if (!r.ok) {
                        throw new Error('HTTP error: ' + r.status);
                    }
                    const contentType = r.headers.get('content-type');
                    if (contentType && contentType.includes('application/json')) {
                        return r.json();
                    } else {
                        return r.text().then(text => {
                            console.error('Non-JSON response:', text);
                            return [];
                        });
                    }
                })
                .then(data => {
                    const list = document.getElementById('reminders-list');
                    list.innerHTML = '<h3 style="margin-top:0; margin-bottom:15px;">Scheduled Reminders</h3>';
                    
                    // Filter out past reminders (client-side backup filter)
                    const currentTime = Math.floor(Date.now() / 1000);
                    const futureReminders = data.filter(r => r.scheduledTime > currentTime);
                    
                    if (futureReminders.length === 0) {
                        list.innerHTML += '<p style="color:#999; text-align:center; padding:20px;">No reminders scheduled</p>';
                        return;
                    }
                    futureReminders.forEach(r => {
                        const scheduledDate = new Date(r.scheduledTime * 1000);
                        const timeStr = scheduledDate.toLocaleString();
                        const status = r.printed ? '✅ Printed' : '⏰ Pending';
                        // Escape message for HTML and JavaScript
                        const escapedMsg = r.message.replace(/\\/g, '\\\\').replace(/'/g, "\\'").replace(/"/g, '&quot;').replace(/\n/g, '\\n');
                        list.innerHTML += `
                            <div class="reminder-item">
                                <div class="item-content">
                                    <div style="font-weight:600; margin-bottom:4px;">${r.message.replace(/</g, '&lt;').replace(/>/g, '&gt;')}</div>
                                    <div style="font-size:12px; color:#666;">
                                        ${timeStr} - ${status}
                                    </div>
                                </div>
                                <div class="item-actions">
                                    <button class="btn-small btn-edit" onclick="editReminder('${r.id}', '${escapedMsg}', ${r.scheduledTime})">Edit</button>
                                    <button class="btn-small btn-delete" onclick="deleteReminder('${r.id}')">✕</button>
                                </div>
                            </div>
                        `;
                    });
                });
        }
        
        function deleteReminder(id) {
            if (confirm('Delete this reminder?')) {
                fetch(addAuthToken('/api/reminders/' + id), {method: 'DELETE'})
                    .then(r => {
                        const contentType = r.headers.get('content-type');
                        if (contentType && contentType.includes('application/json')) {
                            return r.json();
                        } else {
                            return r.text().then(text => ({success: false, message: text || 'Unknown error'}));
                        }
                    })
                    .then(data => {
                        if (data.success) {
                            alert('✅ Reminder deleted!');
                            loadReminders();
                        } else {
                            alert('❌ Error: ' + (data.message || 'Failed to delete'));
                        }
                    })
                    .catch(err => {
                        alert('❌ Error: ' + err.message);
                    });
            }
        }
        
        let editingReminderId = null;
        
        function editReminder(id, message, scheduledTime) {
            editingReminderId = id;
            document.getElementById('reminder-msg').value = message;
            updateCharCount();
            
            // Set the scheduled time
            const date = new Date(scheduledTime * 1000);
            const dateStr = date.toISOString().slice(0, 16);
            document.getElementById('reminder-time').value = dateStr;
            document.getElementById('reminder-time-type').value = 'custom';
            updateTimeInput();
            
            // Scroll to top
            document.getElementById('reminder-msg').scrollIntoView({behavior: 'smooth'});
            
            // Change add button to update button
            const addBtn = document.querySelector('#reminders-tab button[onclick="addReminder()"]');
            if (addBtn) {
                addBtn.textContent = 'Update Reminder';
                addBtn.onclick = function() { updateReminder(); };
            }
        }
        
        function updateReminder() {
            if (!editingReminderId) {
                addReminder();
                return;
            }
            
            const msg = document.getElementById('reminder-msg').value.trim();
            if (!msg) return alert('Please enter a message');
            
            const time = document.getElementById('reminder-time').value;
            if (!time) return alert('Please select a date and time');
            
            const scheduledTime = Math.floor(new Date(time).getTime() / 1000);
            if (scheduledTime <= Math.floor(Date.now() / 1000)) {
                return alert('Please select a time in the future');
            }
            
                    // Delete old reminder first
                    fetch(addAuthToken('/api/reminders/' + editingReminderId), {method: 'DELETE'})
                        .then(() => {
                            // Add new reminder
                            return fetch(addAuthToken('/api/reminders'), {
                                method: 'POST',
                                headers: {'Content-Type': 'application/json'},
                                body: JSON.stringify({message: msg, scheduledTime: scheduledTime})
                            });
                        })
                        .then(r => {
                            const contentType = r.headers.get('content-type');
                            if (contentType && contentType.includes('application/json')) {
                                return r.json();
                            } else {
                                return r.text().then(text => ({success: false, message: text || 'Unknown error'}));
                            }
                        })
                .then(data => {
                    if (data.success) {
                        alert('✅ Reminder updated!');
                        editingReminderId = null;
                        document.getElementById('reminder-msg').value = '';
                        document.getElementById('selected-time').style.display = 'none';
                        updateCharCount();
                        
                        // Reset button
                        const addBtn = document.querySelector('#reminders-tab button[onclick="updateReminder()"]');
                        if (addBtn) {
                            addBtn.textContent = 'Add Reminder';
                            addBtn.onclick = function() { addReminder(); };
                        }
                        
                        loadReminders();
                    } else {
                        alert('❌ Error: ' + (data.message || 'Failed to update reminder'));
                    }
                })
                .catch(err => {
                    alert('❌ Error: ' + err.message);
                });
        }
        
        function addGrocery() {
            const item = document.getElementById('grocery-item').value;
            if (!item) return;
            
            fetch(addAuthToken('/api/groceries'), {
                method: 'POST',
                headers: {'Content-Type': 'application/json'},
                body: JSON.stringify({item: item})
            })
            .then(r => {
                if (!r.ok) {
                    throw new Error('HTTP error: ' + r.status);
                }
                const contentType = r.headers.get('content-type');
                if (contentType && contentType.includes('application/json')) {
                    return r.json();
                } else {
                    return r.text().then(text => ({success: false, message: text || 'Unknown error'}));
                }
            })
            .then(data => {
                if (data && data.success !== false) {
                    document.getElementById('grocery-item').value = '';
                    loadGroceries();
                } else {
                    alert('❌ Error: ' + (data.message || 'Failed to add item'));
                }
            })
            .catch(err => {
                alert('❌ Error: ' + err.message);
            });
        }
        
        function loadGroceries() {
            fetch(addAuthToken('/api/groceries'))
                .then(r => {
                    if (!r.ok) {
                        throw new Error('HTTP error: ' + r.status);
                    }
                    const contentType = r.headers.get('content-type');
                    if (contentType && contentType.includes('application/json')) {
                        return r.json();
                    } else {
                        return r.text().then(text => {
                            console.error('Non-JSON response:', text);
                            return [];
                        });
                    }
                })
                .then(data => {
                    const list = document.getElementById('grocery-list');
                    list.innerHTML = '';
                    if (data.length === 0) {
                        list.innerHTML = '<p style="color:#999; text-align:center; padding:20px;">No items in list</p>';
                        return;
                    }
                    data.forEach((item, i) => {
                        const escapedItem = item.replace(/</g, '&lt;').replace(/>/g, '&gt;');
                        list.innerHTML += `
                            <div class="grocery-item">
                                <div class="item-content">${i+1}. ${escapedItem}</div>
                                <div class="item-actions">
                                    <button class="btn-small btn-delete" onclick="deleteGrocery(${i})">✕</button>
                                </div>
                            </div>
                        `;
                    });
                });
        }
        
        function deleteGrocery(index) {
            fetch(addAuthToken('/api/groceries/' + index), {method: 'DELETE'})
                .then(r => {
                    const contentType = r.headers.get('content-type');
                    if (contentType && contentType.includes('application/json')) {
                        return r.json();
                    } else {
                        return r.text().then(text => ({success: false, message: text || 'Unknown error'}));
                    }
                })
                .then(data => {
                    if (data.success) {
                        loadGroceries();
                    } else {
                        alert('❌ Error: ' + (data.message || 'Failed to delete'));
                    }
                })
                .catch(err => {
                    alert('❌ Error: ' + err.message);
                });
        }
        
        function printGroceries() {
            fetch(addAuthToken('/api/groceries/print'), {method: 'POST'})
                .then(r => {
                    if (!r.ok) {
                        throw new Error('HTTP error: ' + r.status);
                    }
                    const contentType = r.headers.get('content-type');
                    if (contentType && contentType.includes('application/json')) {
                        return r.json();
                    } else {
                        return r.text().then(text => ({success: false, message: text || 'Unknown error'}));
                    }
                })
                .then(data => {
                    if (data.success) {
                        alert('✅ Printing grocery list!');
                    } else {
                        alert('❌ Error: Failed to print');
                    }
                })
                .catch(err => {
                    console.error('Print groceries error:', err);
                    alert('❌ Error: ' + err.message);
                });
        }
        
        function clearGroceries() {
            if (confirm('Clear all items?')) {
                fetch(addAuthToken('/api/groceries'), {method: 'DELETE'})
                    .then(r => {
                        if (!r.ok) {
                            throw new Error('HTTP error: ' + r.status);
                        }
                        const contentType = r.headers.get('content-type');
                        if (contentType && contentType.includes('application/json')) {
                            return r.json();
                        } else {
                            return r.text().then(text => ({success: false, message: text || 'Unknown error'}));
                        }
                    })
                    .then(data => {
                        if (data.success) {
                            alert('✅ Groceries cleared!');
                            loadGroceries();
                        } else {
                            alert('❌ Error: ' + (data.message || 'Failed to clear'));
                        }
                    })
                    .catch(err => {
                        console.error('Clear groceries error:', err);
                        alert('❌ Error: ' + err.message);
                    });
            }
        }
        
        loadStatus();
        setInterval(loadStatus, 30000);
        updateCharCount(); // Initialize character counter
    </script>
</body>
</html>
)HTML";
    
    server.send(200, "text/html", html);
}

void processRequestQueue() {
    if (!requestQueue || requestQueue->isEmpty()) {
        return;
    }
    
    if (!requestQueue->shouldProcess()) {
        return;  // Wait for interval
    }
    
    QueuedRequest request;
    if (!requestQueue->dequeue(request)) {
        return;
    }
    
    Logger::debug("Queue", "Processing queued request (type: " + String(request.type) + ", queue: " + String(requestQueue->getSize()) + " remaining)");
    
    bool success = false;
    
    switch (request.type) {
        case REQUEST_FIREBASE_GET: {
            String response;
            success = firebase->get(request.path, response);
            break;
        }
        case REQUEST_FIREBASE_PUT: {
            success = firebase->put(request.path, request.data);
            if (success) {
                Logger::info("Queue", "✅ Firebase PUT successful: " + request.path);
                if (request.path == "/groceries.json") {
                    Logger::info("Groceries", "✅ Groceries saved to Firebase successfully");
                }
            } else {
                Logger::error("Queue", "❌ Firebase PUT failed: " + request.path);
                if (request.path == "/groceries.json") {
                    Logger::error("Groceries", "❌ Failed to save groceries - check Firebase rules");
                }
            }
            break;
        }
        case REQUEST_FIREBASE_POST: {
            success = firebase->post(request.path, request.data);
            break;
        }
        case REQUEST_FIREBASE_DELETE: {
            success = firebase->deleteData(request.path);
            break;
        }
        case REQUEST_WEATHER: {
            getWeatherData();
            success = true;  // Weather is best-effort
            break;
        }
        case REQUEST_PRINT: {
            Logger::info("Queue", "🖨️ Printing queued message: " + request.data.substring(0, 30) + "...");
            if (currentWeather == "N/A") {
                getWeatherData();
                delay(500);  // Wait for weather
            }
            success = printerService->printReceipt(request.data, true);
            if (success) {
                Logger::info("Queue", "✅ Print completed successfully");
            }
            break;
        }
        case REQUEST_DISPENSE_START: {
            success = hardware->startPump();
            break;
        }
        case REQUEST_DISPENSE_STOP: {
            success = hardware->stopPump();
            break;
        }
        default:
            Logger::warn("Queue", "Unknown request type: " + String(request.type));
            success = false;
            break;
    }
    
    if (success) {
        Logger::debug("Queue", "✅ Request processed successfully");
    } else {
        Logger::warn("Queue", "⚠️ Request failed (retry: " + String(request.retryCount) + "/3)");
        // Re-queue if retry count is low
        if (request.retryCount < 3) {
            request.retryCount++;
            Logger::debug("Queue", "Re-queuing request for retry");
            requestQueue->enqueue(request.type, request.path, request.data);
        } else {
            Logger::error("Queue", "❌ Request failed after max retries, dropping");
        }
    }
    
    requestQueue->markProcessed();
}

// Hardware Test Handlers
void handleTestPage() {
    // Check authentication - allow token parameter
    if (!isAuthenticated()) {
        // Try to get token from URL and redirect with it
        String token = server.arg("token");
        if (token.length() > 0) {
            // Token in URL, check if it matches
            if (token == authToken && authToken.length() > 0) {
                // Valid token, continue to show page
            } else {
                server.sendHeader("Location", "/login");
                server.send(302, "text/plain", "");
                return;
            }
        } else {
            server.sendHeader("Location", "/login");
            server.send(302, "text/plain", "");
            return;
        }
    }
    
    String html = R"HTML(
<!DOCTYPE html>
<html>
<head>
    <meta charset="UTF-8">
    <meta name="viewport" content="width=device-width, initial-scale=1.0">
    <title>Hardware Test - Printer Pot</title>
    <style>
        * { margin: 0; padding: 0; box-sizing: border-box; }
        body {
            font-family: -apple-system, BlinkMacSystemFont, 'Segoe UI', Roboto, sans-serif;
            background: linear-gradient(135deg, #2d5016 0%, #4a7c2a 50%, #6b9f3d 100%);
            min-height: 100vh;
            padding: 20px;
        }
        .container {
            max-width: 800px;
            margin: 0 auto;
            background: #f5f5f0;
            border-radius: 20px;
            padding: 30px;
            box-shadow: 0 20px 60px rgba(45, 80, 22, 0.4);
            border: 2px solid #4a7c2a;
        }
        h1 {
            color: #2d5016;
            margin-bottom: 20px;
            text-align: center;
        }
        .test-section {
            background: white;
            padding: 20px;
            border-radius: 12px;
            margin-bottom: 20px;
            border: 2px solid #4a7c2a;
        }
        .test-section h2 {
            color: #2d5016;
            margin-bottom: 15px;
            font-size: 20px;
        }
        button {
            padding: 12px 24px;
            background: linear-gradient(135deg, #2d5016 0%, #4a7c2a 100%);
            color: white;
            border: none;
            border-radius: 8px;
            font-size: 16px;
            font-weight: 600;
            cursor: pointer;
            margin: 5px;
            transition: transform 0.2s;
        }
        button:active {
            transform: scale(0.98);
        }
        button.danger {
            background: linear-gradient(135deg, #8b4513 0%, #a0522d 100%);
        }
        .status {
            margin-top: 10px;
            padding: 10px;
            border-radius: 8px;
            font-size: 14px;
        }
        .status.success {
            background: #d4edda;
            color: #155724;
            border: 1px solid #c3e6cb;
        }
        .status.error {
            background: #f8d7da;
            color: #721c24;
            border: 1px solid #f5c6cb;
        }
        .status.info {
            background: #d1ecf1;
            color: #0c5460;
            border: 1px solid #bee5eb;
        }
        .sensor-value {
            font-size: 24px;
            font-weight: 700;
            color: #2d5016;
            margin-top: 10px;
        }
        .back-link {
            display: inline-block;
            margin-top: 20px;
            color: #4a7c2a;
            text-decoration: none;
            font-weight: 600;
        }
        .back-link:hover {
            text-decoration: underline;
        }
    </style>
</head>
<body>
    <div class="container">
        <h1>🔧 Hardware Test</h1>
        
        <div class="test-section">
            <h2>💡 LED Test (GPIO 2)</h2>
            <button onclick="testLED(true)">Turn LED ON</button>
            <button onclick="testLED(false)" class="danger">Turn LED OFF</button>
            <div id="led-status"></div>
        </div>
        
        <div class="test-section">
            <h2>💧 Pump Test (GPIO 4)</h2>
            <p style="color: #666; margin-bottom: 10px;">⚠️ Pump will run for 1 second maximum</p>
            <button onclick="testPump()">Start Pump (1 sec)</button>
            <div id="pump-status"></div>
        </div>
        
        <div class="test-section">
            <h2>🖨️ Printer Test (9600 baud)</h2>
            <button onclick="testPrinter()">Print Test Page</button>
            <div id="printer-status"></div>
        </div>
        
        <div class="test-section">
            <h2>📡 Sensor Tests</h2>
            <button onclick="testSensors()">Read All Sensors</button>
            <div id="sensor-status"></div>
            <div id="sensor-values"></div>
        </div>
        
        <a href="#" onclick="window.location.href=addAuthToken('/'); return false;" class="back-link">← Back to Main</a>
    </div>
    
    <script>
        // Check for token in URL and store it
        (function() {
            const urlParams = new URLSearchParams(window.location.search);
            const token = urlParams.get('token');
            if (token) {
                // Store token in sessionStorage for future requests
                sessionStorage.setItem('authToken', token);
                // Set cookie as backup
                document.cookie = 'auth=' + token + '; Path=/; Max-Age=3600; SameSite=Lax';
                // Remove token from URL for security
                window.history.replaceState({}, document.title, window.location.pathname);
            }
        })();
        
        function addAuthToken(url) {
            const token = sessionStorage.getItem('authToken');
            if (token) {
                const separator = url.includes('?') ? '&' : '?';
                return url + separator + 'token=' + encodeURIComponent(token);
            }
            return url;
        }
        
        function testLED(state) {
            const statusDiv = document.getElementById('led-status');
            statusDiv.innerHTML = '<div class="status info">Testing LED...</div>';
            
            fetch(addAuthToken('/api/test/led'), {
                method: 'POST',
                headers: {'Content-Type': 'application/json'},
                body: JSON.stringify({state: state})
            })
            .then(r => r.json())
            .then(data => {
                if (data.success) {
                    statusDiv.innerHTML = '<div class="status success">✅ LED ' + (state ? 'ON' : 'OFF') + ' - Test successful!</div>';
                } else {
                    statusDiv.innerHTML = '<div class="status error">❌ Error: ' + (data.message || 'Failed') + '</div>';
                }
            })
            .catch(err => {
                statusDiv.innerHTML = '<div class="status error">❌ Error: ' + err.message + '</div>';
            });
        }
        
        function testPump() {
            const statusDiv = document.getElementById('pump-status');
            statusDiv.innerHTML = '<div class="status info">Starting pump test...</div>';
            
            fetch(addAuthToken('/api/test/pump'), {
                method: 'POST',
                headers: {'Content-Type': 'application/json'}
            })
            .then(r => r.json())
            .then(data => {
                if (data.success) {
                    statusDiv.innerHTML = '<div class="status success">✅ Pump started for ' + (data.duration || 1) + ' second(s)</div>';
                } else {
                    statusDiv.innerHTML = '<div class="status error">❌ Error: ' + (data.message || 'Failed') + '</div>';
                }
            })
            .catch(err => {
                statusDiv.innerHTML = '<div class="status error">❌ Error: ' + err.message + '</div>';
            });
        }
        
        function testPrinter() {
            const statusDiv = document.getElementById('printer-status');
            statusDiv.innerHTML = '<div class="status info">Sending test print...</div>';
            
            fetch(addAuthToken('/api/test/printer'), {
                method: 'POST',
                headers: {'Content-Type': 'application/json'}
            })
            .then(r => r.json())
            .then(data => {
                if (data.success) {
                    statusDiv.innerHTML = '<div class="status success">✅ Test print sent! Check printer.</div>';
                } else {
                    statusDiv.innerHTML = '<div class="status error">❌ Error: ' + (data.message || 'Failed') + '</div>';
                }
            })
            .catch(err => {
                statusDiv.innerHTML = '<div class="status error">❌ Error: ' + err.message + '</div>';
            });
        }
        
        function testBaudRate(baud) {
            const statusDiv = document.getElementById('printer-status');
            statusDiv.innerHTML = '<div class="status info">🔧 Changing to ' + baud + ' baud and printing test...<br>Check your printer output!</div>';
            
            fetch(addAuthToken('/api/printer/baudrate?baud=' + baud), {
                method: 'POST',
                headers: {'Content-Type': 'application/json'}
            })
            .then(r => r.json())
            .then(data => {
                if (data.success) {
                    statusDiv.innerHTML = '<div class="status success">✅ Test printed at ' + baud + ' baud!<br><br>' +
                        '<strong>Check your printer:</strong><br>' +
                        'If you see clean readable text like "TEST 1234567890 ABC...", you found it!<br><br>' +
                        '<strong>To make it permanent:</strong><br>' +
                        'Edit config.h line 22:<br>' +
                        '<code style="background: #333; color: #0f0; padding: 2px 6px; border-radius: 4px;">#define THERMAL_PRINTER_BAUD ' + baud + '</code><br>' +
                        'Then recompile and upload.</div>';
                } else {
                    statusDiv.innerHTML = '<div class="status error">❌ Error: ' + (data.message || 'Failed') + '</div>';
                }
            })
            .catch(err => {
                statusDiv.innerHTML = '<div class="status error">❌ Error: ' + err.message + '</div>';
            });
        }
        
        
        function testSensors() {
            const statusDiv = document.getElementById('sensor-status');
            const valuesDiv = document.getElementById('sensor-values');
            statusDiv.innerHTML = '<div class="status info">Reading sensors...</div>';
            
            fetch(addAuthToken('/api/test/sensors'))
            .then(r => r.json())
            .then(data => {
                if (data.success) {
                    statusDiv.innerHTML = '<div class="status success">✅ Sensors read successfully!</div>';
                    valuesDiv.innerHTML = 
                        '<div style="margin-top: 15px;">' +
                        '<div><strong>Moisture Sensor (GPIO 34):</strong> <span class="sensor-value">' + data.moisture + '%</span></div>' +
                        '<div style="margin-top: 10px;"><strong>IR Sensor (GPIO 32):</strong> <span class="sensor-value">' + (data.irDetected ? 'DETECTED' : 'CLEAR') + '</span></div>' +
                        '<div style="margin-top: 5px; font-size: 14px; color: #666;">Raw Pin Value: ' + (data.irRaw !== undefined ? data.irRaw : 'N/A') + ' (0=LOW, 1=HIGH)</div>' +
                        '</div>';
                } else {
                    statusDiv.innerHTML = '<div class="status error">❌ Error: ' + (data.message || 'Failed') + '</div>';
                }
            })
            .catch(err => {
                statusDiv.innerHTML = '<div class="status error">❌ Error: ' + err.message + '</div>';
            });
        }
    </script>
</body>
</html>
)HTML";
    
    server.send(200, "text/html", html);
}

void handleTestLED() {
    if (!isAuthenticated()) {
        server.send(401, "application/json", "{\"error\":\"Unauthorized\"}");
        return;
    }
    
    String body = server.arg("plain");
    DynamicJsonDocument doc(128);
    DeserializationError error = deserializeJson(doc, body);
    
    if (error || !doc.containsKey("state")) {
        server.send(400, "application/json", "{\"success\":false,\"message\":\"Invalid request\"}");
        return;
    }
    
    bool state = doc["state"].as<bool>();
    hardware->setLED(state);
    
    Logger::info("WebServer", "🧪 LED test: " + String(state ? "ON" : "OFF"));
    
    DynamicJsonDocument response(128);
    response["success"] = true;
    response["message"] = "LED set to " + String(state ? "ON" : "OFF");
    
    String responseStr;
    serializeJson(response, responseStr);
    server.send(200, "application/json", responseStr);
}

void handleTestPump() {
    if (!isAuthenticated()) {
        server.send(401, "application/json", "{\"error\":\"Unauthorized\"}");
        return;
    }
    
    Logger::info("WebServer", "🧪 Pump test: Starting pump for 1 second");
    
    bool started = hardware->startPump();
    if (!started) {
        DynamicJsonDocument response(128);
        response["success"] = false;
        response["message"] = "Pump could not start (may be in cooldown)";
        String responseStr;
        serializeJson(response, responseStr);
        server.send(200, "application/json", responseStr);
        return;
    }
    
    delay(1000);  // Run for 1 second
    hardware->stopPump();
    
    DynamicJsonDocument response(128);
    response["success"] = true;
    response["message"] = "Pump test completed";
    response["duration"] = 1;
    
    String responseStr;
    serializeJson(response, responseStr);
    server.send(200, "application/json", responseStr);
}

void handleTestPrinter() {
    if (!isAuthenticated()) {
        server.send(401, "application/json", "{\"error\":\"Unauthorized\"}");
        return;
    }
    
    Logger::info("WebServer", "🧪 Printer test: Sending test print");
    
    bool success = printerService->printTest();
    
    DynamicJsonDocument response(128);
    response["success"] = success;
    response["message"] = success ? "Test print sent" : "Printer test failed";
    
    String responseStr;
    serializeJson(response, responseStr);
    server.send(200, "application/json", responseStr);
}

void handleTestSensors() {
    if (!isAuthenticated()) {
        server.send(401, "application/json", "{\"error\":\"Unauthorized\"}");
        return;
    }
    
    Logger::info("WebServer", "🧪 Sensor test: Reading sensors");
    
    // Read sensors
    float moisture = hardware->readMoistureSensor();
    bool irDetected = hardware->readIRSensor();
    
    // Read raw IR sensor pin value for debugging
    int rawIRValue = digitalRead(IR_SENSOR_PIN);
    
    DynamicJsonDocument response(256);
    response["success"] = true;
    response["moisture"] = String(moisture, 1);
    response["irDetected"] = irDetected;
    response["irRaw"] = rawIRValue;  // Add raw pin value for debugging
    response["message"] = "Sensors read successfully";
    
    String responseStr;
    serializeJson(response, responseStr);
    server.send(200, "application/json", responseStr);
}
