#ifndef REMINDER_SERVICE_H
#define REMINDER_SERVICE_H

#include <Arduino.h>
#include <time.h>
#include "Logger.h"
#include "FirebaseService.h"

#define MAX_REMINDERS 50

struct Reminder {
    String id;
    String message;
    time_t scheduledTime;
    time_t createdTime;
    bool printed;
    bool active;
    
    Reminder() : scheduledTime(0), createdTime(0), printed(false), active(false) {}
};

class ReminderService {
private:
    static const char* TAG;
    Reminder reminders[MAX_REMINDERS];
    int reminderCount;
    FirebaseService* firebase;
    
    String generateId();
    int findReminderIndex(const String& id);
    void compactReminders();
    
public:
    ReminderService(FirebaseService* fb);
    ~ReminderService();
    
    // Reminder management
    String addReminder(const String& message, time_t scheduledTime);
    bool deleteReminder(const String& id);
    bool markAsPrinted(const String& id);
    
    // Query
    int getReminderCount() const { return reminderCount; }
    const Reminder* getReminder(int index) const;
    const Reminder* getReminderById(const String& id) const;
    
    // Check for due reminders
    void checkReminders(std::function<void(const Reminder&)> callback);
    
    // Persistence
    bool load();
    bool save();
    
    // Export to JSON
    String toJSON() const;
    bool fromJSON(const String& json);
};

#endif // REMINDER_SERVICE_H
