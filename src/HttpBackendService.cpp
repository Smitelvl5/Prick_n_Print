#include "HttpBackendService.h"
#include "BackendTls.h"
#include <WiFiClientSecure.h>

const char* HttpBackendService::TAG = "HttpBackend";

HttpBackendService::HttpBackendService(const String& url, int timeoutMs, const String& key)
    : baseUrl(url), apiKey(key), timeout(timeoutMs), retryCount(1), retryDelay(500),
      lastRequest(0) {
    // Ensure baseUrl has no trailing slash
    while (baseUrl.length() > 0 && baseUrl.endsWith("/"))
        baseUrl.remove(baseUrl.length() - 1);
}

HttpBackendService::~HttpBackendService() {
    delete pollHttp;
    delete pollConn;
}

void HttpBackendService::setRetryPolicy(int count, int delayMs) {
    retryCount = count;
    retryDelay = delayMs;
}

void HttpBackendService::setDeviceIdentity(const String& id, const String& name) {
    deviceId = id;
    deviceName = name;
}

// Common setup for every request. Connect timeout matters most: with the Pi down, the default 5 s
// connect wait per attempt is what stalls the TZT's LVGL/touch loop.
// conn: a caller-owned connection to reuse (keep-alive); otherwise HTTPClient opens its own.
bool HttpBackendService::beginRequest(HTTPClient& http, const String& url, WiFiClient* conn) {
    if (conn ? !http.begin(*conn, url) : !beginBackendHttp(http, url)) {
        lastError = "Connection failed";
        return false;
    }
    http.setConnectTimeout(timeout);
    http.setTimeout(timeout);
    http.addHeader("X-API-Key", apiKey);
    // Lets the server mark this device as seen on every poll (no separate heartbeat request)
    if (deviceId.length() > 0) {
        http.addHeader("X-Device-Id", deviceId);
        http.addHeader("X-Device-Name", deviceName);
    }
    if (firmwareVersion.length() > 0) http.addHeader("X-Firmware", firmwareVersion);
    return true;
}

bool HttpBackendService::executeRequest(HTTPClient& http, const String& method, const String& payload) {
    int httpCode = -1;
    for (int attempt = 0; attempt < retryCount; attempt++) {
        if (attempt > 0) {
            delay(retryDelay);
        }
        if (method == "GET") {
            httpCode = http.GET();
        } else if (method == "PUT") {
            httpCode = http.PUT(payload);
        } else if (method == "DELETE") {
            httpCode = http.sendRequest("DELETE");
        }
        if (httpCode == HTTP_CODE_OK || httpCode == HTTP_CODE_CREATED || httpCode == 204) {
            lastRequest = millis();
            return true;
        }
        if (httpCode == 401 || httpCode == 403) {
            lastError = "HTTP " + String(httpCode);
            return false;
        }
    }
    lastError = failureText(httpCode);
    return false;
}

// "HTTP -1 (connection refused) tls -9984 X509 - ... heap 52000/30000": for a connection that never
// got an HTTP answer, the TLS library's own error and the RAM left (free / largest block)
String HttpBackendService::failureText(int httpCode) {
    String s = "HTTP " + String(httpCode);
    if (httpCode >= 0) return s;
    s += " (" + HTTPClient::errorToString(httpCode) + ")";
    if (pollConn && baseUrl.startsWith("https://")) {
        VerifiedTlsClient* tls = static_cast<VerifiedTlsClient*>(pollConn);
        char buf[96] = {0};
        int e = tls->lastError(buf, sizeof(buf));
        if (e) s += " tls " + String(e) + " " + buf;
        if (tls->verifyError.length()) s += " " + tls->verifyError;
    }
    s += " heap " + String(ESP.getFreeHeap()) + "/" + String(ESP.getMaxAllocHeap());
    return s;
}

void HttpBackendService::ensureKeptAlive() {
    if (pollHttp) return;
    pollHttp = new HTTPClient();
    pollHttp->setReuse(true);
    if (baseUrl.startsWith("https://")) {
        pollConn = new VerifiedTlsClient();  // checks the chain after the handshake (see BackendTls.h)
    } else {
        pollConn = new WiFiClient();
    }
}

void HttpBackendService::dropKeptAlive() {
    if (pollConn) pollConn->stop();
}

HTTPClient& HttpBackendService::clientFor(HTTPClient& fresh, const String& url, bool& ok) {
    if (pollWaitS > 0) {
        ensureKeptAlive();
        ok = beginRequest(*pollHttp, url, pollConn);
        return *pollHttp;
    }
    ok = beginRequest(fresh, url);
    return fresh;
}

// Map backend paths to API paths
// GET: /commands.json -> /api/commands, /audio.json -> /api/audio, /images.json -> /api/images, /reminders.json -> /api/reminders, /config.json -> /api/config
// PUT: /reminders.json -> /api/reminders, /config.json -> /api/config, /audio/X/downloaded.json -> /api/audio/X, /images/X/downloaded.json -> /api/images/X
// DELETE: /commands/X.json -> /api/commands/X
String HttpBackendService::pathToApiPath(const String& fullPath, bool isGet) {
    // A query string ("/audio.json?pending=1") is passed through to the API path
    int q = fullPath.indexOf('?');
    if (q >= 0) {
        String api = pathToApiPath(fullPath.substring(0, q), isGet);
        return api.length() ? api + fullPath.substring(q) : api;
    }
    const String& path = fullPath;
    if (path == "/commands.json") return "/api/commands";
    if (path == "/audio.json") return "/api/audio";
    if (path == "/images.json") return "/api/images";
    if (path == "/reminders.json") return "/api/reminders";
    if (path == "/config.json") return "/api/config";
    if (path.startsWith("/commands/") && path.endsWith(".json")) {
        String id = path.substring(10, path.length() - 5);  // after "/commands/"
        return "/api/commands/" + id;
    }
    if (path.startsWith("/audio/") && path.endsWith("/downloaded.json")) {
        String id = path.substring(7, path.length() - 16);  // "/downloaded.json" = 16 chars
        return "/api/audio/" + id;
    }
    if (path.startsWith("/images/") && path.endsWith("/downloaded.json")) {
        String id = path.substring(8, path.length() - 16);
        return "/api/images/" + id;
    }
    return "";
}

bool HttpBackendService::get(const String& path, String& response) {
    String apiPath = pathToApiPath(path, true);
    if (apiPath.length() == 0) {
        lastError = "Unknown path";
        return false;
    }
    HTTPClient fresh;
    bool begun;
    HTTPClient& http = clientFor(fresh, baseUrl + apiPath, begun);
    if (!begun) return false;
    if (executeRequest(http, "GET")) {
        response = http.getString();
        http.end();
        return true;
    }
    http.end();
    dropKeptAlive();
    return false;
}

bool HttpBackendService::download(const String& path, Stream& out) {
    HTTPClient fresh;
    bool begun;
    HTTPClient& http = clientFor(fresh, baseUrl + path, begun);
    if (!begun) return false;
    http.setTimeout(15000);
    if (!executeRequest(http, "GET")) {
        http.end();
        dropKeptAlive();
        return false;
    }
    // writeToStream reads until Content-Length (or chunked end) is reached, not just until the
    // receive buffer is momentarily empty, so slow transfers aren't cut short
    int expected = http.getSize();
    int written = http.writeToStream(&out);
    http.end();
    if (written <= 0 || (expected > 0 && written != expected)) {
        lastError = "Incomplete (" + String(written) + "/" + String(expected) + ")";
        dropKeptAlive();  // the rest of the body may still be in flight
        return false;
    }
    return true;
}

bool HttpBackendService::put(const String& path, const String& data) {
    String apiPath = pathToApiPath(path, false);
    if (apiPath.length() == 0) {
        lastError = "Unknown path";
        return false;
    }
    String payload = data;
    // Server expects {"downloaded": true} for audio/image mark-downloaded
    if (path.indexOf("/downloaded.json") >= 0 && (data == "true" || data == "\"true\"")) {
        payload = "{\"downloaded\":true}";
    }
    HTTPClient fresh;
    bool begun;
    HTTPClient& http = clientFor(fresh, baseUrl + apiPath, begun);
    if (!begun) return false;
    http.addHeader("Content-Type", "application/json");
    bool ok = executeRequest(http, "PUT", payload);
    http.end();
    if (!ok) dropKeptAlive();
    return ok;
}

bool HttpBackendService::deleteData(const String& path) {
    if (!path.startsWith("/commands/") || !path.endsWith(".json")) {
        lastError = "Unknown path";
        return false;
    }
    String id = path.substring(10, path.length() - 5);  // after "/commands/"
    String url = baseUrl + "/api/commands/" + id;
    HTTPClient http;
    for (int redirects = 0; redirects < 3; redirects++) {
        if (!beginRequest(http, url)) return false;
        int httpCode = -1;
        for (int attempt = 0; attempt < retryCount; attempt++) {
            if (attempt > 0) delay(retryDelay);
            httpCode = http.sendRequest("DELETE");
            if (httpCode == HTTP_CODE_OK || httpCode == HTTP_CODE_CREATED || httpCode == 204) {
                lastRequest = millis();
                http.end();
                return true;
            }
            if (httpCode == 401 || httpCode == 403) {
                lastError = "HTTP " + String(httpCode);
                http.end();
                return false;
            }
            if (httpCode == 301 || httpCode == 302 || httpCode == 307 || httpCode == 308) {
                String location = http.getLocation();
                http.end();
                if (location.length() > 0) {
                    url = location;
                    break;
                }
                lastError = "HTTP " + String(httpCode) + " no Location";
                return false;
            }
        }
        if (httpCode == 301 || httpCode == 302 || httpCode == 307 || httpCode == 308) {
            continue;
        }
        lastError = "HTTP " + String(httpCode);
        http.end();
        return false;
    }
    lastError = "Too many redirects";
    return false;
}

bool HttpBackendService::pollCommandsRaw(String& body, String& commandId) {
    body = "";
    commandId = "";
    if (pollWaitS <= 0) return get("/commands.json", body);
    // One kept-alive connection for every poll: a TLS handshake costs the ESP32 ~5 s, so paying
    // it once (not per poll) is what lets a queued command land within a second.
    ensureKeptAlive();
    String url = baseUrl + "/api/commands?wait=" + String(pollWaitS);
    // Skipped ids go in ack= too, so a server without skip= support still drops them
    String ack = pollAck;
    if (pollSkip.length()) ack += (ack.length() ? "," : "") + pollSkip;
    if (ack.length() > 0) url += "&ack=" + ack;
    if (pollSkip.length() > 0) url += "&skip=" + pollSkip;
    HTTPClient& http = *pollHttp;
    if (!beginRequest(http, url, pollConn)) return false;
    static const char* headerKeys[] = {"X-Command-Id"};
    http.collectHeaders(headerKeys, 1);
    http.setTimeout((pollWaitS + 10) * 1000);  // the server answers by pollWaitS; the rest is slack
    bool ok = executeRequest(http, "GET");
    if (ok) {
        body = http.getString();
        commandId = http.header("X-Command-Id");
    }
    http.end();  // keeps the connection open (setReuse) for the next poll
    if (!ok) {
        pollConn->stop();  // start clean next time
        return false;
    }
    pollAck = "";  // the server has deleted them
    pollSkip = "";
    return true;
}

bool HttpBackendService::pollCommands(DynamicJsonDocument& commands) {
    String response, commandId;
    if (!pollCommandsRaw(response, commandId)) return false;
    if (response.length() == 0 || response == "null") {
        commands.clear();
        return true;
    }
    DeserializationError err = deserializeJson(commands, response);
    if (err) {
        lastError = "Parse error";
        return false;
    }
    return true;
}

bool HttpBackendService::loadConfig(DynamicJsonDocument& doc) {
    String response;
    if (!get("/config.json", response)) return false;
    if (response == "null" || response.length() == 0) return false;
    return deserializeJson(doc, response) == DeserializationError::Ok;
}

bool HttpBackendService::saveConfig(const DynamicJsonDocument& doc) {
    String data;
    serializeJson(doc, data);
    return put("/config.json", data);
}

bool HttpBackendService::isHealthy() {
    HTTPClient http;
    if (!beginRequest(http, baseUrl + "/api/health")) return false;
    bool ok = (http.GET() == HTTP_CODE_OK);
    http.end();
    return ok;
}

bool HttpBackendService::sendHeartbeat(const String& deviceId, const String& name) {
    HTTPClient http;
    if (!beginRequest(http, baseUrl + "/api/devices/heartbeat")) return false;
    http.addHeader("Content-Type", "application/json");
    String body = "{\"device_id\":\"" + deviceId + "\",\"name\":\"" + name + "\"}";
    int code = http.POST(body);
    http.end();
    return (code == HTTP_CODE_OK || code == HTTP_CODE_CREATED);
}
