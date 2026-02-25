#include "AudioService.h"
#include "SDCardService.h"
#include <SD.h>
#include <HTTPClient.h>
#include <AudioFileSourceSD.h>
#include <AudioGeneratorMP3.h>
#include <AudioGeneratorWAV.h>
#include <AudioOutputI2S.h>

static AudioFileSourceSD*   audioSrc = nullptr;
static AudioOutputI2S*      audioOut = nullptr;
static AudioGeneratorMP3*   mp3Gen   = nullptr;
static AudioGeneratorWAV*   wavGen   = nullptr;
static bool                 usingMp3 = false;

bool AudioService::begin() {
    if (initialized) return true;
    audioOut = new AudioOutputI2S(0, AudioOutputI2S::INTERNAL_DAC);
    volume = 0.5f;
    audioOut->SetGain(volume);
    initialized = true;
    return true;
}

void AudioService::setVolume(float gain) {
    if (gain < 0.0f) gain = 0.0f;
    if (gain > 1.0f) gain = 1.0f;
    volume = gain;
    if (audioOut) audioOut->SetGain(volume);
}

bool AudioService::playFile(const String& sdPath) {
    if (!initialized) return false;
    stop();
    // Let I2S buffer drain so only one audio plays at a time (no roboty overlap)
    delay(120);

    String fullPath = sdPath;
    if (!fullPath.startsWith("/")) fullPath = "/" + fullPath;
    if (!SD.exists(fullPath)) {
        Serial.println("[Audio] File not found: " + fullPath);
        return false;
    }

    audioSrc = new AudioFileSourceSD(fullPath.c_str());
    usingMp3 = fullPath.endsWith(".mp3") || fullPath.endsWith(".MP3");

    bool ok = false;
    if (usingMp3) {
        mp3Gen = new AudioGeneratorMP3();
        ok = mp3Gen->begin(audioSrc, audioOut);
    } else {
        wavGen = new AudioGeneratorWAV();
        ok = wavGen->begin(audioSrc, audioOut);
    }

    if (ok) {
        playing = true;
        nowPlaying = fullPath;
    } else {
        Serial.println("[Audio] Failed to open: " + fullPath);
        delete audioSrc; audioSrc = nullptr;
        if (usingMp3) { delete mp3Gen; mp3Gen = nullptr; }
        else          { delete wavGen; wavGen = nullptr; }
    }
    return ok;
}

void AudioService::stop() {
    playing = false;
    nowPlaying = "";
    if (mp3Gen) {
        if (mp3Gen->isRunning()) mp3Gen->stop();
        delete mp3Gen;
        mp3Gen = nullptr;
    }
    if (wavGen) {
        if (wavGen->isRunning()) wavGen->stop();
        delete wavGen;
        wavGen = nullptr;
    }
    if (audioSrc) {
        delete audioSrc;
        audioSrc = nullptr;
    }
}

void AudioService::loop() {
    if (!playing) return;
    bool running = false;
    if (usingMp3 && mp3Gen) running = mp3Gen->loop();
    else if (wavGen)        running = wavGen->loop();
    if (!running) stop();
}

bool AudioService::downloadToSD(const String& url, const String& filename) {
    if (!SD.exists(AUDIO_DIR)) SD.mkdir(AUDIO_DIR);

    String path = String(AUDIO_DIR) + "/" + filename;
    if (SD.exists(path)) return true;

    HTTPClient http;
    http.begin(url);
    http.setTimeout(15000);
    int code = http.GET();
    if (code != HTTP_CODE_OK) {
        Serial.println("[Audio] HTTP " + String(code));
        http.end();
        return false;
    }

    File f = SD.open(path, FILE_WRITE);
    if (!f) {
        Serial.println("[Audio] Cannot create file: " + path);
        http.end();
        return false;
    }

    WiFiClient* stream = http.getStreamPtr();
    uint8_t buf[1024];
    int total = 0;
    while (http.connected() && stream->available()) {
        int n = stream->readBytes(buf, sizeof(buf));
        if (n > 0) { f.write(buf, n); total += n; }
    }
    f.close();
    http.end();
    // Saved to SD
    return total > 0;
}
