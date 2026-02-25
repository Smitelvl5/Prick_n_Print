#ifndef SD_CARD_SERVICE_H
#define SD_CARD_SERVICE_H

#include <Arduino.h>
#include <SPI.h>
#include <SD.h>
#include <FS.h>

// CYD SD card slot is on VSPI
#define SD_PIN_CS    5
#define SD_PIN_SCK  18
#define SD_PIN_MISO 19
#define SD_PIN_MOSI 23

#define DATA_DIR      "/data"
// Messages: saved in /data/messages/messages.jsonl
#define MSG_DIR       "/data/messages"
#define MSG_FILE      "/data/messages/messages.jsonl"
// Audio and images: put files in these folders on the SD card
#define AUDIO_DIR        "/data/audio"
#define IMAGES_DIR       "/data/images"
#define IMAGES_MEDIA_DIR "/data/images/media"   // Firebase/Shortcut uploads (shown in Media > View Images)
#define MAX_CACHED_MESSAGES 20
#define MSG_PREVIEW_LEN     60

struct SavedMessage {
    uint32_t id;        // millis() at save time
    time_t   timestamp; // Unix epoch (0 if NTP not synced)
    char     preview[MSG_PREVIEW_LEN + 1];
    char     source[12];
};

class SDCardService {
public:
    bool begin();
    bool isReady() const { return mounted; }

    // Messages
    bool saveMessage(const String& text, const String& source);
    int  loadRecentMessages(int count = MAX_CACHED_MESSAGES);
    int  getMessageCount() const { return cachedCount; }
    const SavedMessage& getCachedMessage(int index) const;
    String readFullMessage(int index);

    // Audio file helpers
    int  listAudioFiles(String* outNames, int maxFiles);

    // Image file helpers
    int  listImageFiles(String* outNames, int maxFiles);

    // Data persistence (config, groceries, todos, reminders - replaces Firebase for these)
    bool readFile(const char* path, String& out);
    bool writeFile(const char* path, const String& content);

    // General
    uint64_t totalBytes();
    uint64_t usedBytes();

private:
    bool mounted = false;
    SPIClass* vspi = nullptr;
    SavedMessage cache[MAX_CACHED_MESSAGES];
    int cachedCount = 0;

    void ensureDir(const char* path);
    int  countLines(const char* filePath);
    String readLineFromEnd(File& f, int linesFromEnd);
};

#endif // SD_CARD_SERVICE_H
