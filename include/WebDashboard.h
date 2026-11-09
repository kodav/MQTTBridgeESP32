#pragma once

#include <Arduino.h>
#include <PubSubClient.h>
#include <vector>
#include "config.h"

struct HistoryPoint {
    float value;
    unsigned long timestamp;
};

struct CachedState {
    bool used = false;
    char topic[MAX_TOPIC_LENGTH + 1] = {};
    char payload[MAX_PAYLOAD_LENGTH + 1] = {};
    unsigned long updatedAt = 0;
    std::vector<HistoryPoint> history; // Буфер истории для графика
};

// Глобальная функция отправки в MAX
bool sendMaxNotification(const String& message);

namespace WebDashboard {
    void begin(
        PubSubClient& localMqtt,
        PubSubClient& mainMqtt,
        std::vector<CachedState>& states,
        bool apMode
    );

    void handleClient();
}