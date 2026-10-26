#ifndef TEST_SERVER_H
#define TEST_SERVER_H

#include <Arduino.h>
#include <WebServer.h>
#include "HardwareAbstraction.h"
#include "PrinterService.h"
#include "ApiSecurity.h"
#include "EspNowService.h"

// Bench-test HTTP server (port 8081) - exercise any hardware component from a
// browser/curl without the serial monitor. One handler class per component
// keeps validation/response shape consistent and main.cpp free of endpoint code.
//
// Response contract for every /printer, /pump, /led, /sensors route:
//   200 + {"ok":true, ...}              on success
//   503 + {"ok":false,"error":"..."}    dependency not initialized/ready
//   400 + {"ok":false,"error":"..."}    bad/out-of-range request argument
class TestServer {
public:
    // autoDispense points at main.cpp's autoDispenseEnabled flag - settings live here since this is
    // now the single place hardware config is read/changed (avoids the old TZT-dashboard/serial-menu split).
    // espNow (optional) feeds the link counters shown under System on the bench page.
    TestServer(HardwareAbstraction* hw, PrinterService* printer, bool* autoDispense, EspNowService* espNow = nullptr);

    void begin();
    void handleClient() { server_.handleClient(); }

    // Runs non-blocking pump/LED test pulses started via HTTP; call every loop().
    void update();

    // Cancels this server's in-flight pump/LED test pulse without touching the
    // actuator - call before another source (e.g. an ESP-NOW test command)
    // starts its own pulse, so only one timer is left owning the shutoff.
    void cancelActiveTest() { activeTest_ = 0; }

private:
    WebServer server_;
    HardwareAbstraction* hardware_;
    PrinterService* printerService_;
    bool* autoDispenseEnabled_;
    EspNowService* espNow_;
    ApiSecurity security_;

    // Non-blocking test-pulse state (mirrors ESP-NOW TEST_PUMP/TEST_LED behavior)
    uint8_t activeTest_ = 0;  // 0=none, 1=pump, 2=led
    unsigned long testStartMs_ = 0;
    unsigned long testDurationMs_ = 0;

    // True if the request's key (X-API-Key header or ?key= param) matches WEB_PASSWORD.
    // Checked at the top of every state-changing handler; "/" and "/status" stay open (read-only).
    bool isAuthorized();

    void sendOk(const String& extraJsonFields = "");
    void sendError(int httpStatus, const String& message);

    void handleRoot();
    void handleStatus();
    void handleSystem();  // firmware, uptime, memory, WiFi, ESP-NOW link (read-only, open like /status)

    void handlePrinterTest();
    void handlePrinterRaw();
    void handlePrinterReceipt();
    void handlePrinterFeed();

    void handlePump();
    void handleLed();
    void handleSensors();

    void handleGetSettings();
    void handleSetPumpDuration();
    void handleSetPumpCooldown();
    void handleSetAutoDispense();
    void handleSetLedSchedule();
    void handleSetLedScheduleTimes();
    void handleSetLedLevel();
};

#endif // TEST_SERVER_H
