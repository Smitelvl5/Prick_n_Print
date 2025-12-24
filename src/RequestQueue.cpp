#include "RequestQueue.h"

const char* RequestQueue::TAG = "Queue";

RequestQueue::RequestQueue() 
    : queueHead(0), queueTail(0), queueSize(0),
      lastProcessTime(0), processInterval(2000) {
}

RequestQueue::~RequestQueue() {
}

bool RequestQueue::enqueue(RequestType type, const String& path, const String& data) {
    if (isFull()) {
        Logger::warn(TAG, "Queue is full, dropping request");
        return false;
    }
    
    queue[queueTail].type = type;
    queue[queueTail].path = path;
    queue[queueTail].data = data;
    queue[queueTail].timestamp = millis();
    queue[queueTail].retryCount = 0;
    queue[queueTail].processed = false;
    
    queueTail = (queueTail + 1) % MAX_QUEUE_SIZE;
    queueSize++;
    
    Logger::debug(TAG, "Request queued (type: " + String(type) + ", queue size: " + String(queueSize) + ")");
    return true;
}

bool RequestQueue::dequeue(QueuedRequest& request) {
    if (isEmpty()) {
        return false;
    }
    
    request = queue[queueHead];
    queueHead = (queueHead + 1) % MAX_QUEUE_SIZE;
    queueSize--;
    
    return true;
}

void RequestQueue::setProcessInterval(unsigned long intervalMs) {
    processInterval = intervalMs;
    Logger::debug(TAG, "Process interval set to " + String(intervalMs) + "ms");
}

bool RequestQueue::shouldProcess() const {
    if (isEmpty()) {
        return false;
    }
    
    unsigned long now = millis();
    if (now - lastProcessTime >= processInterval) {
        return true;
    }
    
    return false;
}

void RequestQueue::markProcessed() {
    lastProcessTime = millis();
}

String RequestQueue::getQueueStatus() const {
    String status = "Queue Status:\n";
    status += "  Size: " + String(queueSize) + "/" + String(MAX_QUEUE_SIZE) + "\n";
    status += "  Head: " + String(queueHead) + "\n";
    status += "  Tail: " + String(queueTail) + "\n";
    status += "  Last Process: " + String(millis() - lastProcessTime) + "ms ago\n";
    return status;
}

void RequestQueue::clear() {
    queueHead = 0;
    queueTail = 0;
    queueSize = 0;
    Logger::info(TAG, "Queue cleared");
}
