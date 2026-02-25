#include "SDCardService.h"
#include <ArduinoJson.h>
#include <time.h>

static const SavedMessage EMPTY_MSG = {0, 0, "(empty)", ""};

bool SDCardService::begin() {
    if (mounted) return true;
    vspi = new SPIClass(VSPI);
    vspi->begin(SD_PIN_SCK, SD_PIN_MISO, SD_PIN_MOSI, SD_PIN_CS);
    if (!SD.begin(SD_PIN_CS, *vspi)) {
        Serial.println("[SD] Mount failed");
        return false;
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
    ensureDir(IMAGES_MEDIA_DIR); // /data/images/media (Firebase/Shortcut uploads)
    Serial.println("[SD] OK " + String((unsigned long)((SD.totalBytes() - SD.usedBytes()) / (1024ULL * 1024))) + " MB free");
    return true;
}

void SDCardService::ensureDir(const char* path) {
    if (!SD.exists(path)) SD.mkdir(path);
}

// ── Messages ────────────────────────────────────────────────────

bool SDCardService::saveMessage(const String& text, const String& source) {
    if (!mounted) return false;

    File f = SD.open(MSG_FILE, FILE_APPEND);
    if (!f) {
        Serial.println("[SD] Cannot open messages file for append");
        return false;
    }

    time_t now = time(nullptr);
    uint32_t id = (uint32_t)millis();

    StaticJsonDocument<512> doc;
    doc["id"]  = id;
    doc["ts"]  = (unsigned long)now;
    doc["src"] = source;
    doc["txt"] = text;

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

        StaticJsonDocument<512> doc;
        if (deserializeJson(doc, line) != DeserializationError::Ok) continue;

        SavedMessage& m = cache[cachedCount];
        m.id = doc["id"] | 0;
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

    while (f.available()) {
        String line = f.readStringUntil('\n');
        line.trim();
        if (line.length() == 0) continue;
        StaticJsonDocument<512> doc;
        if (deserializeJson(doc, line) != DeserializationError::Ok) continue;
        if ((doc["id"] | (uint32_t)0) == targetId) {
            f.close();
            return doc["txt"].as<String>();
        }
    }
    f.close();
    return "";
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

int SDCardService::listAudioFiles(String* outNames, int maxFiles) {
    if (!mounted) return 0;
    File dir = SD.open(AUDIO_DIR);
    if (!dir || !dir.isDirectory()) return 0;
    int count = 0;
    File entry = dir.openNextFile();
    while (entry && count < maxFiles) {
        if (!entry.isDirectory()) {
            String name = String(entry.name());
            if (name.endsWith(".mp3") || name.endsWith(".wav") || name.endsWith(".MP3") || name.endsWith(".WAV")) {
                outNames[count++] = name;
            }
        }
        entry = dir.openNextFile();
    }
    return count;
}

int SDCardService::listImageFiles(String* outNames, int maxFiles) {
    if (!mounted) return 0;
    int count = 0;
    for (int pass = 0; pass < 2 && count < maxFiles; pass++) {
        const char* dirPath = (pass == 0) ? IMAGES_MEDIA_DIR : IMAGES_DIR;
        const char* prefix = (pass == 0) ? "media/" : "";
        File dir = SD.open(dirPath);
        if (!dir || !dir.isDirectory()) continue;
        File entry = dir.openNextFile();
        while (entry && count < maxFiles) {
            if (!entry.isDirectory()) {
                String name = String(entry.name());
                if (name.length() > 0 && name[0] != '.') {
                    String lower = name;
                    lower.toLowerCase();
                    if (lower.endsWith(".jpg") || lower.endsWith(".jpeg") || lower.endsWith(".png") || lower.endsWith(".bmp")) {
                        outNames[count++] = (prefix[0]) ? (String(prefix) + name) : name;
                    }
                }
            }
            entry = dir.openNextFile();
        }
    }
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
