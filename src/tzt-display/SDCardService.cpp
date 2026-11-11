#include "SDCardService.h"
#include <ArduinoJson.h>
#include <HTTPClient.h>
#include <time.h>
#include "BackendTls.h"
#include "config_tzt.h"
#include "secrets.h"  // BACKEND_API_KEY

static const SavedMessage EMPTY_MSG = {0, 0, "(empty)", ""};

bool SDCardService::begin() {
    if (mounted) return true;
    vspi = new SPIClass(VSPI);
    vspi->begin(SD_PIN_SCK, SD_PIN_MISO, SD_PIN_MOSI, SD_PIN_CS);
    const uint32_t speeds[] = {4000000, 1000000};
    for (int i = 0; i < 2; i++) {
        if (SD.begin(SD_PIN_CS, *vspi, speeds[i])) break;
        if (i == 0) {
            Serial.println("[SD] Mount failed at 4MHz, retrying at 1MHz...");
            SD.end();
        } else {
            Serial.println("[SD] Mount failed. Check: card inserted, pins CS=" + String(SD_PIN_CS) + " SCK=" + String(SD_PIN_SCK) + " MISO=" + String(SD_PIN_MISO) + " MOSI=" + String(SD_PIN_MOSI));
            return false;
        }
    }
    uint8_t ct = SD.cardType();
    if (ct == CARD_NONE) {
        Serial.println("[SD] No card detected");
        return false;
    }
    mounted = true;
    ensureDir(DATA_DIR);        // Create /data first, then subdirs
    ensureDir(MSG_DIR);         // /data/messages
    ensureDir(AUDIO_DIR);       // /data/audio
    ensureDir(IMAGES_DIR);      // /data/images
    ensureDir(IMAGES_MEDIA_DIR); // /data/images/media (backend/shortcut uploads)
    Serial.println("[SD] OK " + String((unsigned long)((SD.totalBytes() - SD.usedBytes()) / (1024ULL * 1024))) + " MB free");
    return true;
}

void SDCardService::ensureDir(const char* path) {
    if (!SD.exists(path)) SD.mkdir(path);
}

bool SDCardService::downloadToFile(const String& url, const String& path, PiFetcher fetchFromPi) {
    if (!mounted) return false;
    if (SD.exists(path)) return true;
    int slash = path.lastIndexOf('/');
    if (slash > 0) ensureDir(path.substring(0, slash).c_str());

    // The Pi bakes its own address (whatever the uploader used, often a LAN IP) into upload URLs;
    // fetch Pi uploads through BACKEND_URL so they work off the home network too
    int up = url.indexOf("/uploads/");
    if (up > 0 && fetchFromPi) {
        File f = SD.open(path, FILE_WRITE);
        if (!f) {
            Serial.println("[SD] Cannot create file: " + path);
            return false;
        }
        bool ok = fetchFromPi(url.substring(up), f);
        f.close();
        if (!ok) {
            Serial.println("[SD] Download failed: " + url);
            SD.remove(path);
        }
        return ok;
    }
    String fetchUrl = url;
    if (up > 0) fetchUrl = String(BACKEND_URL) + url.substring(up);

    HTTPClient http;
    if (!beginBackendHttp(http, fetchUrl)) return false;
    // The Pi's public (Funnel) listener asks for a password unless a request carries the API key
    if (fetchUrl.startsWith(BACKEND_URL)) http.addHeader("X-API-Key", BACKEND_API_KEY);
    http.setTimeout(15000);
    int code = http.GET();
    if (code != HTTP_CODE_OK) {
        Serial.println("[SD] Download HTTP " + String(code) + ": " + url);
        http.end();
        return false;
    }
    File f = SD.open(path, FILE_WRITE);
    if (!f) {
        Serial.println("[SD] Cannot create file: " + path);
        http.end();
        return false;
    }
    // writeToStream reads until Content-Length (or chunked end) is reached, not just until the
    // receive buffer is momentarily empty, so slow transfers aren't cut short
    int expected = http.getSize();
    int written = http.writeToStream(&f);
    f.close();
    http.end();
    if (written <= 0 || (expected > 0 && written != expected)) {
        Serial.println("[SD] Download incomplete (" + String(written) + "/" + String(expected) + "): " + path);
        SD.remove(path);
        return false;
    }
    return true;
}

// ── Messages ────────────────────────────────────────────────────

// Parse one messages.jsonl line in place (zero-copy: the document points into `line`, so it stays
// small however long the text is). Use the document only while `line` is alive.
static bool parseLine(JsonDocument& doc, String& line) {
    return deserializeJson(doc, line.begin(), line.length()) == DeserializationError::Ok;
}

bool SDCardService::saveMessage(const String& text, const String& source) {
    if (!mounted) return false;

    File f = SD.open(MSG_FILE, FILE_APPEND);
    if (!f) {
        Serial.println("[SD] Cannot open messages file for append");
        return false;
    }

    time_t now = time(nullptr);
    // Random, not millis(): millis() restarts at every boot, so ids would repeat and opening a
    // message could show an older one that happened to arrive at the same point after a boot
    uint32_t id = esp_random();

    // const char* values are stored by pointer, not copied: the text (up to 12.7 KB) needs no room
    // here. A copied String would overflow the document and the line would be saved without its text.
    StaticJsonDocument<256> doc;
    doc["id"]  = id;
    doc["ts"]  = (unsigned long)now;
    doc["src"] = source.c_str();
    doc["txt"] = text.c_str();

    serializeJson(doc, f);
    f.println();
    f.close();

    // Update preview cache (shift oldest out, add at end)
    if (cachedCount < MAX_CACHED_MESSAGES) {
        cachedCount++;
    } else {
        memmove(&cache[0], &cache[1], sizeof(SavedMessage) * (MAX_CACHED_MESSAGES - 1));
    }
    SavedMessage& m = cache[cachedCount - 1];
    m.id = id;
    m.timestamp = now;
    strncpy(m.source, source.c_str(), sizeof(m.source) - 1);
    m.source[sizeof(m.source) - 1] = '\0';
    text.toCharArray(m.preview, sizeof(m.preview));

    return true;
}

int SDCardService::loadRecentMessages(int count) {
    cachedCount = 0;
    if (!mounted) return 0;
    if (!SD.exists(MSG_FILE)) return 0;
    if (count > MAX_CACHED_MESSAGES) count = MAX_CACHED_MESSAGES;

    int totalLines = countLines(MSG_FILE);
    int skip = (totalLines > count) ? totalLines - count : 0;

    File f = SD.open(MSG_FILE, FILE_READ);
    if (!f) return 0;

    int lineNum = 0;
    while (f.available() && cachedCount < count) {
        String line = f.readStringUntil('\n');
        line.trim();
        if (line.length() == 0) continue;
        lineNum++;
        if (lineNum <= skip) continue;

        StaticJsonDocument<256> doc;
        if (!parseLine(doc, line)) continue;

        SavedMessage& m = cache[cachedCount];
        m.id = doc["id"] | (uint32_t)0;  // not "| 0": an int default turns ids over 2^31 into 0
        m.timestamp = (time_t)(doc["ts"] | (unsigned long)0);
        const char* src = doc["src"] | "";
        strncpy(m.source, src, sizeof(m.source) - 1);
        m.source[sizeof(m.source) - 1] = '\0';
        const char* txt = doc["txt"] | "";
        strncpy(m.preview, txt, MSG_PREVIEW_LEN);
        m.preview[MSG_PREVIEW_LEN] = '\0';
        cachedCount++;
    }
    f.close();
    return cachedCount;
}

const SavedMessage& SDCardService::getCachedMessage(int index) const {
    if (index < 0 || index >= cachedCount) return EMPTY_MSG;
    return cache[index];
}

String SDCardService::readFullMessage(int index) {
    if (!mounted || index < 0 || index >= cachedCount) return "";
    uint32_t targetId = cache[index].id;

    File f = SD.open(MSG_FILE, FILE_READ);
    if (!f) return "";

    String found;  // the last match: ids from older firmware (millis()) can repeat
    while (f.available()) {
        String line = f.readStringUntil('\n');
        line.trim();
        if (line.length() == 0) continue;
        StaticJsonDocument<256> doc;
        if (!parseLine(doc, line)) continue;
        if ((doc["id"] | (uint32_t)0) == targetId) found = doc["txt"].as<String>();
    }
    f.close();
    return found;
}

int SDCardService::countLines(const char* filePath) {
    File f = SD.open(filePath, FILE_READ);
    if (!f) return 0;
    int n = 0;
    while (f.available()) {
        if (f.read() == '\n') n++;
    }
    f.close();
    return n;
}

// ── Audio helpers ───────────────────────────────────────────────

// Keep the maxFiles newest (by last-write time) in out/outTimes, newest first
static void keepNewest(String* out, time_t* outTimes, int& count, int maxFiles, const String& name, time_t t) {
    int pos = count;
    while (pos > 0 && outTimes[pos - 1] < t) pos--;
    if (pos >= maxFiles) return;
    int last = (count < maxFiles) ? count : maxFiles - 1;
    for (int i = last; i > pos; i--) { out[i] = out[i - 1]; outTimes[i] = outTimes[i - 1]; }
    out[pos] = name;
    outTimes[pos] = t;
    if (count < maxFiles) count++;
}

int SDCardService::listAudioFiles(String* outNames, int maxFiles) {
    if (!mounted || maxFiles <= 0) return 0;
    File dir = SD.open(AUDIO_DIR);
    if (!dir || !dir.isDirectory()) return 0;
    time_t* times = new time_t[maxFiles];
    int count = 0;
    File entry = dir.openNextFile();
    while (entry) {
        if (!entry.isDirectory()) {
            String name = String(entry.name());
            if (name.endsWith(".mp3") || name.endsWith(".wav") || name.endsWith(".MP3") || name.endsWith(".WAV"))
                keepNewest(outNames, times, count, maxFiles, name, entry.getLastWrite());
        }
        entry = dir.openNextFile();
    }
    delete[] times;
    return count;
}

int SDCardService::listImageFiles(String* outNames, int maxFiles) {
    if (!mounted || maxFiles <= 0) return 0;
    time_t* times = new time_t[maxFiles];
    int count = 0;
    for (int pass = 0; pass < 2; pass++) {
        const char* dirPath = (pass == 0) ? IMAGES_MEDIA_DIR : IMAGES_DIR;
        const char* prefix = (pass == 0) ? "media/" : "";
        File dir = SD.open(dirPath);
        if (!dir || !dir.isDirectory()) continue;
        File entry = dir.openNextFile();
        while (entry) {
            if (!entry.isDirectory()) {
                String name = String(entry.name());
                if (name.length() > 0 && name[0] != '.') {
                    String lower = name;
                    lower.toLowerCase();
                    if (lower.endsWith(".jpg") || lower.endsWith(".jpeg") || lower.endsWith(".png") || lower.endsWith(".bmp"))
                        keepNewest(outNames, times, count, maxFiles, prefix[0] ? String(prefix) + name : name, entry.getLastWrite());
                }
            }
            entry = dir.openNextFile();
        }
    }
    delete[] times;
    return count;
}

// ── Data persistence (config, groceries, todos, reminders) ────────────────

bool SDCardService::readFile(const char* path, String& out) {
    out = "";
    if (!mounted || !path || path[0] != '/') return false;
    if (!SD.exists(path)) return false;
    File f = SD.open(path, FILE_READ);
    if (!f) return false;
    out = f.readString();
    f.close();
    return true;
}

bool SDCardService::writeFile(const char* path, const String& content) {
    if (!mounted || !path || path[0] != '/') return false;
    File f = SD.open(path, FILE_WRITE);
    if (!f) {
        Serial.println("[SD] Cannot open for write: " + String(path));
        return false;
    }
    size_t n = f.print(content);
    f.close();
    return (n == content.length());
}

uint64_t SDCardService::totalBytes() { return mounted ? SD.totalBytes() : 0; }
uint64_t SDCardService::usedBytes()  { return mounted ? SD.usedBytes()  : 0; }
