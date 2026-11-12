#ifndef FIRMWARE_SINK_H
#define FIRMWARE_SINK_H

// A Stream that writes an HTTP download straight into the spare app partition (Update), feeding the
// task watchdog as it goes. Shared by both boards' "update from the Pi" code. Usage:
//   Update.begin(size); Update.setMD5(md5); FirmwareSink s(size, onProgress);
//   http.writeToStream(&s);  then  s.written == size && Update.end()  (end() checks the MD5)

#include <Arduino.h>
#include <Update.h>
#include <esp_task_wdt.h>

class FirmwareSink : public Stream {
public:
    typedef void (*ProgressFn)(int pct);
    size_t total, written = 0;

    FirmwareSink(size_t total, ProgressFn onProgress = nullptr) : total(total), progress(onProgress) {}

    size_t write(uint8_t c) override { return write(&c, 1); }
    size_t write(const uint8_t* buf, size_t len) override {
        esp_task_wdt_reset();  // a ~1.5 MB download over TLS takes a minute or more
        size_t w = Update.write(const_cast<uint8_t*>(buf), len);
        written += w;
        int pct = total ? (int)(written * 100 / total) : 0;
        if (progress && pct != lastPct) progress(pct);
        lastPct = pct;
        return w;
    }
    int available() override { return 0; }
    int read() override { return -1; }
    int peek() override { return -1; }
    void flush() override {}

private:
    ProgressFn progress;
    int lastPct = -1;
};

#endif // FIRMWARE_SINK_H
