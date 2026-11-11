#ifndef ALARM_SERVICE_H
#define ALARM_SERVICE_H

#include <Arduino.h>
#include <time.h>

#define MAX_ALARMS 10

// Wall-clock alarms on the TZT (local time, so they follow DST): ring at hour:minute, once or every day
struct Alarm {
    String id;
    uint8_t hour = 0;
    uint8_t minute = 0;
    bool daily = false;
    String label;
    time_t lastFired = 0;   // so a daily alarm rings once per day, not every check in its minute
};

class AlarmService {
private:
    Alarm alarms[MAX_ALARMS];
    int alarmCount = 0;
    int findIndex(const String& id) const;
    void removeAt(int index);

public:
    // hour 0-23, minute 0-59; returns the new id, or "" if full / out of range
    String add(uint8_t hour, uint8_t minute, bool daily, const String& label);
    bool remove(const String& id);
    void clear() { alarmCount = 0; }
    int count() const { return alarmCount; }

    // Call every few seconds once the clock is synced. Returns true (and the alarm's label) when one is
    // due; a one-time alarm is removed when it fires. `changed` is set when the list needs saving.
    bool checkDue(time_t now, String& labelOut, bool& changed);

    // {"<id>": {"time": "07:30", "daily": true, "label": "..."}}
    String toJSON() const;
    bool fromJSON(const String& json);
};

#endif // ALARM_SERVICE_H
