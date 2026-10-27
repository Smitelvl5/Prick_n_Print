#include "HttpBackendService.h"

const char* HttpBackendService::TAG = "HttpBackend";

HttpBackendService::HttpBackendService(const String& url, int timeoutMs, const String& key)
    : baseUrl(url), apiKey(key), timeout(timeoutMs), retryCount(1), retryDelay(500),
      lastRequest(0) {
    // Ensure baseUrl has no trailing slash
    while (baseUrl.length() > 0 && baseUrl.endsWith("/"))
        baseUrl.remove(baseUrl.length() - 1);
}

HttpBackendService::~HttpBackendService() {
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
bool HttpBackendService::beginRequest(HTTPClient& http, const String& url) {
    if (!http.begin(url)) {
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
    lastError = "HTTP " + String(httpCode);
    return false;
}

// Map backend paths to API paths
// GET: /commands.json -> /api/commands, /audio.json -> /api/audio, /images.json -> /api/images, /reminders.json -> /api/reminders, /config.json -> /api/config
// PUT: /reminders.json -> /api/reminders, /config.json -> /api/config, /audio/X/downloaded.json -> /api/audio/X, /images/X/downloaded.json -> /api/images/X
// DELETE: /commands/X.json -> /api/commands/X
String HttpBackendService::pathToApiPath(const String& path, bool isGet) {
    if (path == "/commands.json") return "/api/commands";
    if (path == "/audio.json") return "/api/audio";
    if (path == "/images.json") return "/api/images";
    if (path == "/reminders.json") return "/api/reminders";
    if (path == "/config.json") return "/api/config";
    if (path.startsWith("/commands/") && path.endsWith(".json")) {
        String id = path.substring(9, path.length() - 5);
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
    HTTPClient http;
    if (!beginRequest(http, baseUrl + apiPath)) return false;
    if (executeRequest(http, "GET")) {
        response = http.getString();
        http.end();
        return true;
    }
    http.end();
    return false;
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
    HTTPClient http;
    if (!beginRequest(http, baseUrl + apiPath)) return false;
    http.addHeader("Content-Type", "application/json");
    bool ok = executeRequest(http, "PUT", payload);
    http.end();
    return ok;
}

bool HttpBackendService::deleteData(const String& path) {
    if (!path.startsWith("/commands/") || !path.endsWith(".json")) {
        lastError = "Unknown path";
        return false;
    }
    String id = path.substring(9, path.length() - 5);
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

bool HttpBackendService::pollCommands(DynamicJsonDocument& commands) {
    String response;
    if (!get("/commands.json", response)) return false;
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
