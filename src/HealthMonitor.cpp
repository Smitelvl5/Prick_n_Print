#include "HealthMonitor.h"
#include <WiFi.h>
#include <ArduinoJson.h>
#include "version.h"

const char* HealthMonitor::TAG = "Health";

HealthMonitor::HealthMonitor() 
    : lastHealthCheck(0), healthCheckInterval(60000) {
    health.wifiConnected = false;
    health.firebaseHealthy = false;
    health.printerReady = false;
    health.uptime = 0;
    health.freeHeap = 0;
    health.minFreeHeap = 0;
    health.cpuFreq = 0;
    health.wifiRSSI = 0;
    health.firmwareVersion = FIRMWARE_VERSION;
    health.lastCheck = 0;
}

HealthMonitor::~HealthMonitor() {
}

void HealthMonitor::setCheckInterval(unsigned long intervalMs) {
    healthCheckInterval = intervalMs;
}

void HealthMonitor::update() {
    unsigned long now = millis();
    if (now - lastHealthCheck < healthCheckInterval) {
        return;
    }
    
    checkWiFi();
    checkMemory();
    checkSystem();
    
    health.lastCheck = time(nullptr);
    lastHealthCheck = now;
    
    Logger::debug(TAG, "Health check completed");
}

bool HealthMonitor::isHealthy() const {
    return health.wifiConnected && 
           health.freeHeap > 50000 &&  // At least 50KB free
           health.printerReady;
}

void HealthMonitor::checkWiFi() {
    health.wifiConnected = (WiFi.status() == WL_CONNECTED);
    
    if (health.wifiConnected) {
        health.wifiRSSI = WiFi.RSSI();
        health.ipAddress = WiFi.localIP().toString();
    } else {
        health.wifiRSSI = 0;
        health.ipAddress = "N/A";
    }
}

void HealthMonitor::checkMemory() {
    health.freeHeap = ESP.getFreeHeap();
    health.minFreeHeap = ESP.getMinFreeHeap();
    health.cpuFreq = ESP.getCpuFreqMHz();
}

void HealthMonitor::checkSystem() {
    health.uptime = millis();
}

String HealthMonitor::getHealthReport() const {
    String report;
    report += "========================================\n";
    report += "SYSTEM HEALTH REPORT\n";
    report += "========================================\n";
    report += "Firmware: v" + health.firmwareVersion + "\n";
    report += "Uptime: " + String(getUptimeHours(), 1) + " hours\n";
    report += "WiFi: " + String(health.wifiConnected ? "CONNECTED" : "DISCONNECTED") + "\n";
    report += "  IP: " + health.ipAddress + "\n";
    report += "  RSSI: " + String(health.wifiRSSI) + " dBm\n";
    report += "Firebase: " + String(health.firebaseHealthy ? "HEALTHY" : "UNHEALTHY") + "\n";
    report += "Printer: " + String(health.printerReady ? "READY" : "NOT READY") + "\n";
    report += "Memory:\n";
    report += "  Free Heap: " + String(health.freeHeap) + " bytes\n";
    report += "  Min Free: " + String(health.minFreeHeap) + " bytes\n";
    report += "  Usage: " + String(getMemoryUsagePercent()) + "%\n";
    report += "CPU Frequency: " + String(health.cpuFreq) + " MHz\n";
    report += "Status: " + String(isHealthy() ? "✅ HEALTHY" : "⚠️ UNHEALTHY") + "\n";
    report += "========================================\n";
    return report;
}

String HealthMonitor::getHealthJSON() const {
    DynamicJsonDocument doc(1024);
    
    doc["firmware"] = health.firmwareVersion;
    doc["uptime"] = health.uptime;
    doc["uptimeHours"] = getUptimeHours();
    doc["wifi"]["connected"] = health.wifiConnected;
    doc["wifi"]["ip"] = health.ipAddress;
    doc["wifi"]["rssi"] = health.wifiRSSI;
    doc["firebase"]["healthy"] = health.firebaseHealthy;
    doc["printer"]["ready"] = health.printerReady;
    doc["memory"]["freeHeap"] = health.freeHeap;
    doc["memory"]["minFreeHeap"] = health.minFreeHeap;
    doc["memory"]["usagePercent"] = getMemoryUsagePercent();
    doc["cpu"]["frequencyMHz"] = health.cpuFreq;
    doc["healthy"] = isHealthy();
    doc["lastCheck"] = health.lastCheck;
    
    String json;
    serializeJson(doc, json);
    return json;
}

void HealthMonitor::printHealthReport() const {
    Logger::info(TAG, getHealthReport());
}

float HealthMonitor::getUptimeHours() const {
    return health.uptime / 3600000.0;
}

int HealthMonitor::getMemoryUsagePercent() const {
    // Estimate total heap (ESP32 typically has ~320KB)
    const uint32_t totalHeap = 320000;
    return ((totalHeap - health.freeHeap) * 100) / totalHeap;
}
