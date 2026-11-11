#ifndef HTTP_BACKEND_SERVICE_H
#define HTTP_BACKEND_SERVICE_H

#include <Arduino.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include "Logger.h"
#include "IBackendService.h"

/**
 * HTTP backend: implements IBackendService and talks to the web server.
 * Use BACKEND_URL in config (e.g. http://192.168.1.100:5000) and set USE_HTTP_BACKEND.
 */
class HttpBackendService : public IBackendService {
private:
    static const char* TAG;
    String baseUrl;  // e.g. http://192.168.1.100:5000 (no trailing slash)
    String apiKey;   // sent as X-API-Key on every request; must match server's PRICK_N_PRINT_API_KEY
    int timeout;
    int retryCount;
    int retryDelay;
    unsigned long lastRequest;
    String deviceId;
    String deviceName;
    int pollWaitS = 0;  // > 0: pollCommands() long-polls (server holds the request until a command arrives)
    String pollAck;     // handled command ids, sent with the next poll (the server deletes them)
    String pollSkip;    // received but unreadable command ids (the server deletes them, marks them failed)
    WiFiClient* pollConn = nullptr;  // kept alive between long polls: one TLS handshake, not one per poll
    HTTPClient* pollHttp = nullptr;  // long-lived too: ~HTTPClient() closes its connection

    bool beginRequest(HTTPClient& http, const String& url, WiFiClient* conn = nullptr);
    void ensureKeptAlive();   // creates pollHttp/pollConn
    void dropKeptAlive();     // after a failed request: the next one reconnects
    // The HTTPClient to use for a request: the kept-alive one in long-poll mode, else `fresh`
    HTTPClient& clientFor(HTTPClient& fresh, const String& url, bool& ok);
    bool executeRequest(HTTPClient& http, const String& method, const String& payload = "");
    String failureText(int httpCode);  // error text for a failed request, with TLS + RAM detail
    String pathToApiPath(const String& path, bool isGet);

public:
    HttpBackendService(const String& url, int timeoutMs = 10000, const String& apiKey = "");
    ~HttpBackendService() override;

    void setRetryPolicy(int count, int delayMs) override;
    void setDeviceIdentity(const String& id, const String& name) override;
    // Long poll: blocks up to waitS seconds, so only call pollCommands() off the UI loop when set
    void setPollWait(int waitS) { pollWaitS = waitS; }
    // Comma-separated ids handled since the last poll; replaces DELETE /commands/<id> per command
    void setPollAck(const String& ids) { pollAck = ids; }
    void setPollSkip(const String& ids) { pollSkip = ids; }
    // pollCommands() without the parse: the body as received, and the Pi's X-Command-Id header (the
    // command's id even when the body can't be parsed, so it can be skipped instead of jamming the queue)
    bool pollCommandsRaw(String& body, String& commandId);
    // GET baseUrl + path into `out` (e.g. a file on the SD card); false unless the whole body arrived.
    // In long-poll mode get(), put() and download() reuse the poll's open connection: the ESP32 has
    // no RAM for a second TLS session while it's open, and reusing it skips a ~6 s handshake. So only
    // call them while pollCommands() isn't running (for the TZT: between polls, while loop() has the batch).
    bool download(const String& path, Stream& out);

    bool get(const String& path, String& response) override;
    bool put(const String& path, const String& data) override;
    bool deleteData(const String& path) override;
    bool pollCommands(DynamicJsonDocument& commands) override;

    bool loadConfig(DynamicJsonDocument& doc) override;
    bool saveConfig(const DynamicJsonDocument& doc) override;
    bool isHealthy() override;
    bool sendHeartbeat(const String& deviceId, const String& name) override;
    String getLastError() const override { return lastError; }

private:
    String lastError;
};

#endif // HTTP_BACKEND_SERVICE_H
