#include "AlarmService.h"
#include <ArduinoJson.h>

int AlarmService::findIndex(const String& id) const {
    for (int i = 0; i < alarmCount; i++)
        if (alarms[i].id == id) return i;
    return -1;
}

void AlarmService::removeAt(int index) {
    for (int i = index; i < alarmCount - 1; i++) alarms[i] = alarms[i + 1];
    alarmCount--;
}

String AlarmService::add(uint8_t hour, uint8_t minute, bool daily, const String& label) {
    if (alarmCount >= MAX_ALARMS || hour > 23 || minute > 59) return "";
    Alarm& a = alarms[alarmCount++];
    a.id = String((uint32_t)esp_random(), HEX);
    a.hour = hour;
    a.minute = minute;
    a.daily = daily;
    a.label = label.substring(0, 60);
    a.lastFired = 0;
    return a.id;
}

bool AlarmService::remove(const String& id) {
    int i = findIndex(id);
    if (i < 0) return false;
    removeAt(i);
    return true;
}

bool AlarmService::checkDue(time_t now, String& labelOut, bool& changed) {
    struct tm t;
    localtime_r(&now, &t);
    for (int i = 0; i < alarmCount; i++) {
        Alarm& a = alarms[i];
        if (a.hour != t.tm_hour || a.minute != t.tm_min) continue;
        if (a.lastFired && now - a.lastFired < 120) continue;  // already rang this minute
        labelOut = a.label;
        if (a.daily) a.lastFired = now;
        else removeAt(i);
        changed = true;
        return true;
    }
    return false;
}

String AlarmService::toJSON() const {
    DynamicJsonDocument doc(256 * MAX_ALARMS);
    JsonObject root = doc.to<JsonObject>();  // serializes as {} when empty, not null
    for (int i = 0; i < alarmCount; i++) {
        JsonObject o = root.createNestedObject(alarms[i].id);
        char hm[6];
        snprintf(hm, sizeof(hm), "%02u:%02u", alarms[i].hour, alarms[i].minute);
        o["time"] = hm;
        o["daily"] = alarms[i].daily;
        o["label"] = alarms[i].label;
    }
    String out;
    serializeJson(doc, out);
    return out;
}

bool AlarmService::fromJSON(const String& json) {
    DynamicJsonDocument doc(256 * MAX_ALARMS);
    if (deserializeJson(doc, json) != DeserializationError::Ok) return false;
    alarmCount = 0;
    for (JsonPair kv : doc.as<JsonObject>()) {
        if (alarmCount >= MAX_ALARMS) break;
        String hm = kv.value()["time"] | "";
        if (hm.length() != 5 || hm[2] != ':') continue;
        Alarm& a = alarms[alarmCount++];
        a.id = kv.key().c_str();
        a.hour = (uint8_t)hm.substring(0, 2).toInt();
        a.minute = (uint8_t)hm.substring(3, 5).toInt();
        a.daily = kv.value()["daily"] | false;
        a.label = kv.value()["label"] | "";
        a.lastFired = 0;
    }
    return true;
}
