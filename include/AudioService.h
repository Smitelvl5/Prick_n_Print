#ifndef AUDIO_SERVICE_H
#define AUDIO_SERVICE_H

#include <Arduino.h>

// CYD speaker on GPIO26 (DAC channel 2) via onboard amplifier
#define AUDIO_OUT_PIN 26

class AudioService {
public:
    bool begin();
    bool isReady() const { return initialized; }

    bool playFile(const String& sdPath);
    void stop();
    void loop();                       // Must be called from main loop() to feed audio buffer

    bool isPlaying() const { return playing; }
    String currentFile() const { return nowPlaying; }

    void setVolume(float gain);   // 0.0 .. 1.0
    float getVolume() const { return volume; }

    bool downloadToSD(const String& url, const String& filename);

private:
    bool initialized = false;
    bool playing = false;
    String nowPlaying;
    float volume = 0.5f;
};

#endif // AUDIO_SERVICE_H
