#include "ReminderService.h"
#include <ArduinoJson.h>

const char* ReminderService::TAG = "Reminder";

ReminderService::ReminderService(IBackendService* backend) 
    : reminderCount(0), backend(backend) {
}

ReminderService::~ReminderService() {
}

String ReminderService::generateId() {
    return String(millis()) + String(random(1000, 9999));
}

int ReminderService::findReminderIndex(const String& id) {
    for (int i = 0; i < reminderCount; i++) {
        if (reminders[i].id == id && reminders[i].active) {
            return i;
        }
    }
    return -1;
}

void ReminderService::compactReminders() {
    int writeIndex = 0;
    for (int readIndex = 0; readIndex < reminderCount; readIndex++) {
        if (reminders[readIndex].active) {
            if (writeIndex != readIndex) {
                reminders[writeIndex] = reminders[readIndex];
            }
            writeIndex++;
        }
    }
    reminderCount = writeIndex;
}

String ReminderService::addReminder(const String& message, time_t scheduledTime) {
    if (reminderCount >= MAX_REMINDERS) {
        Logger::error(TAG, "Maximum reminders reached");
        compactReminders();  // Try to free up space
        if (reminderCount >= MAX_REMINDERS) {
            return "";
        }
    }
    
    if (scheduledTime <= time(nullptr)) {
        Logger::error(TAG, "Cannot schedule reminder in the past");
        return "";
    }
    
    String id = generateId();
    reminders[reminderCount].id = id;
    reminders[reminderCount].message = message;
    reminders[reminderCount].scheduledTime = scheduledTime;
    reminders[reminderCount].createdTime = time(nullptr);
    reminders[reminderCount].printed = false;
    reminders[reminderCount].active = true;
    reminderCount++;
    
    Logger::info(TAG, "Reminder added: " + id);
    // Note: save() is now called by the caller via queue for non-blocking operation
    
    return id;
}

bool ReminderService::deleteReminder(const String& id) {
    int index = findReminderIndex(id);
    if (index < 0) {
        Logger::warn(TAG, "Reminder not found: " + id);
        return false;
    }
    
    reminders[index].active = false;
    Logger::info(TAG, "Reminder deleted: " + id);
    // Note: save() is now called by the caller via queue for non-blocking operation
    compactReminders();
    
    return true;
}

bool ReminderService::markAsPrinted(const String& id) {
    int index = findReminderIndex(id);
    if (index < 0) {
        return false;
    }
    
    reminders[index].printed = true;
    Logger::info(TAG, "Reminder marked as printed: " + id);
    // Note: save() is now called by the caller via queue for non-blocking operation
    
    return true;
}

const Reminder* ReminderService::getReminder(int index) const {
    if (index < 0 || index >= reminderCount) {
        return nullptr;
    }
    return &reminders[index];
}

const Reminder* ReminderService::getReminderById(const String& id) const {
    for (int i = 0; i < reminderCount; i++) {
        if (reminders[i].id == id && reminders[i].active) {
            return &reminders[i];
        }
    }
    return nullptr;
}

// Due window: fire if now is in [scheduledTime, scheduledTime + DUE_WINDOW_SEC]
// Cleanup: deactivate only when now > scheduledTime + CLEANUP_AFTER_SEC
#define REMINDER_DUE_WINDOW_SEC 300   // 5 min window so we catch even with 60s check interval
#define REMINDER_CLEANUP_AFTER_SEC 300

void ReminderService::checkReminders(std::function<void(const Reminder&)> callback) {
    time_t now = time(nullptr);
    bool needsCleanup = false;

    // Check for due reminders FIRST (never deactivate one we could fire this run)
    for (int i = 0; i < reminderCount; i++) {
        if (reminders[i].active && !reminders[i].printed) {
            if (now >= reminders[i].scheduledTime &&
                now <= reminders[i].scheduledTime + REMINDER_DUE_WINDOW_SEC) {
                Logger::info(TAG, "Reminder due: " + reminders[i].message);
                callback(reminders[i]);
                markAsPrinted(reminders[i].id);
            }
        }
    }

    // Then clean up past reminders (beyond the window)
    for (int i = 0; i < reminderCount; i++) {
        if (reminders[i].active && now > reminders[i].scheduledTime + REMINDER_CLEANUP_AFTER_SEC) {
            reminders[i].active = false;
            needsCleanup = true;
            Logger::debug(TAG, "Removed past reminder: " + reminders[i].id);
        }
    }
    if (needsCleanup) {
        compactReminders();
    }
}

bool ReminderService::load() {
    if (!backend) {
        Logger::error(TAG, "Backend not initialized");
        return false;
    }
    
    String response;
    if (!backend->get("/reminders.json", response)) {
        Logger::warn(TAG, "Failed to load reminders from backend");
        return false;
    }
    
    if (response == "null" || response.length() == 0) {
        Logger::info(TAG, "No reminders in backend");
        reminderCount = 0;
        return true;
    }
    
    return fromJSON(response);
}

bool ReminderService::save() {
    if (!backend) {
        Logger::error(TAG, "Backend not initialized");
        return false;
    }
    
    String json = toJSON();
    if (!backend->put("/reminders.json", json)) {
        Logger::warn(TAG, "Failed to save reminders to backend (will retry)");
        return false;
    }
    
    Logger::debug(TAG, "Reminders saved successfully");
    return true;
}

String ReminderService::toJSON() const {
    DynamicJsonDocument doc(8192);
    
    for (int i = 0; i < reminderCount; i++) {
        if (reminders[i].active) {
            JsonObject obj = doc.createNestedObject(reminders[i].id);
            obj["message"] = reminders[i].message;
            obj["scheduledTime"] = reminders[i].scheduledTime;
            obj["createdTime"] = reminders[i].createdTime;
            obj["printed"] = reminders[i].printed;
            obj["active"] = reminders[i].active;
        }
    }
    
    String json;
    serializeJson(doc, json);
    return json;
}

String ReminderService::toJSONForDisplay() const {
    DynamicJsonDocument doc(8192);
    for (int i = 0; i < reminderCount; i++) {
        if (reminders[i].active && !reminders[i].printed) {
            JsonObject obj = doc.createNestedObject(reminders[i].id);
            obj["message"] = reminders[i].message;
            obj["scheduledTime"] = reminders[i].scheduledTime;
            obj["createdTime"] = reminders[i].createdTime;
        }
    }
    String json;
    serializeJson(doc, json);
    return json;
}

bool ReminderService::fromJSON(const String& json) {
    DynamicJsonDocument doc(8192);
    DeserializationError error = deserializeJson(doc, json);
    
    if (error) {
        Logger::error(TAG, "Failed to parse reminders JSON: " + String(error.c_str()));
        return false;
    }
    
    reminderCount = 0;
    for (JsonPair kv : doc.as<JsonObject>()) {
        if (reminderCount >= MAX_REMINDERS) {
            Logger::warn(TAG, "Max reminders reached while loading");
            break;
        }
        
        JsonObject obj = kv.value();
        reminders[reminderCount].id = kv.key().c_str();
        reminders[reminderCount].message = obj["message"].as<String>();
        reminders[reminderCount].scheduledTime = obj["scheduledTime"].as<time_t>();
        reminders[reminderCount].createdTime = obj["createdTime"] | time(nullptr);
        reminders[reminderCount].printed = obj["printed"] | false;
        reminders[reminderCount].active = obj["active"] | true;
        reminderCount++;
    }
    
    Logger::info(TAG, "Loaded " + String(reminderCount) + " reminders");
    return true;
}
