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
    bool playTune(const char* rtttl);  // short RTTTL melody from flash (event chimes, alarm); no SD needed
    void stop();
    void loop();                       // Must be called from main loop() to feed audio buffer

    bool isPlaying() const { return playing; }
    bool isPlayingTune() const { return playing && playingTune; }
    String currentFile() const { return nowPlaying; }

    void setVolume(float gain);   // 0.0 .. 1.0
    float getVolume() const { return volume; }


private:
    bool initialized = false;
    bool playing = false;
    bool playingTune = false;
    String nowPlaying;
    float volume = 0.25f;
};

#endif // AUDIO_SERVICE_H
