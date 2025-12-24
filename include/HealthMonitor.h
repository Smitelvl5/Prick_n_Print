#ifndef HEALTH_MONITOR_H
#define HEALTH_MONITOR_H

#include <Arduino.h>
#include "Logger.h"

struct SystemHealth {
    bool wifiConnected;
    bool firebaseHealthy;
    bool printerReady;
    unsigned long uptime;
    uint32_t freeHeap;
    uint32_t minFreeHeap;
    float cpuFreq;
    int wifiRSSI;
    String ipAddress;
    String firmwareVersion;
    time_t lastCheck;
};

class HealthMonitor {
private:
    static const char* TAG;
    SystemHealth health;
    unsigned long lastHealthCheck;
    unsigned long healthCheckInterval;
    
public:
    HealthMonitor();
    ~HealthMonitor();
    
    // Configuration
    void setCheckInterval(unsigned long intervalMs);
    
    // Health checks
    void update();
    bool isHealthy() const;
    const SystemHealth& getHealth() const { return health; }
    
    // Individual checks
    void checkWiFi();
    void checkMemory();
    void checkSystem();
    
    // Reporting
    String getHealthReport() const;
    String getHealthJSON() const;
    void printHealthReport() const;
    
    // Metrics
    float getUptimeHours() const;
    int getMemoryUsagePercent() const;
};

#endif // HEALTH_MONITOR_H
