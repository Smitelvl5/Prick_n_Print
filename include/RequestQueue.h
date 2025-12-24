#ifndef REQUEST_QUEUE_H
#define REQUEST_QUEUE_H

#include <Arduino.h>
#include "Logger.h"

enum RequestType {
    REQUEST_FIREBASE_GET,
    REQUEST_FIREBASE_PUT,
    REQUEST_FIREBASE_POST,
    REQUEST_FIREBASE_DELETE,
    REQUEST_WEATHER,
    REQUEST_PRINT,
    REQUEST_DISPENSE_START,
    REQUEST_DISPENSE_STOP
};

struct QueuedRequest {
    RequestType type;
    String path;
    String data;
    unsigned long timestamp;
    int retryCount;
    bool processed;
    
    QueuedRequest() : type(REQUEST_FIREBASE_GET), timestamp(0), retryCount(0), processed(false) {}
};

class RequestQueue {
private:
    static const char* TAG;
    static const int MAX_QUEUE_SIZE = 20;
    
    QueuedRequest queue[MAX_QUEUE_SIZE];
    int queueHead;
    int queueTail;
    int queueSize;
    
    unsigned long lastProcessTime;
    unsigned long processInterval;  // Minimum time between processing requests
    
public:
    RequestQueue();
    ~RequestQueue();
    
    // Queue management
    bool enqueue(RequestType type, const String& path = "", const String& data = "");
    bool dequeue(QueuedRequest& request);
    bool isEmpty() const { return queueSize == 0; }
    bool isFull() const { return queueSize >= MAX_QUEUE_SIZE; }
    int getSize() const { return queueSize; }
    
    // Processing
    void setProcessInterval(unsigned long intervalMs);
    bool shouldProcess() const;
    void markProcessed();
    
    // Queue info
    String getQueueStatus() const;
    void clear();
};

#endif // REQUEST_QUEUE_H
