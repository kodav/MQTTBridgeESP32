#pragma once

#include <Arduino.h>
#include <PubSubClient.h>
#include <vector>
#include <memory>
#include "config.h"

struct HistoryPoint {
    float value;
    unsigned long timestamp;
};

// История отдельного поля JSON.
// Например: tele/device/SENSOR#Temperature
struct JsonFieldHistory {
    String topic;

    std::unique_ptr<HistoryPoint[]> history;

    size_t historyStart = 0;
    size_t historyCount = 0;
    unsigned long historyLastAt = 0;
};

// Хранится в main.cpp, читается в WebDashboard.cpp.
extern std::vector<JsonFieldHistory> jsonFieldHistories;

struct CachedState {
    bool used = false;

    char topic[MAX_TOPIC_LENGTH + 1] = {};
    char payload[MAX_PAYLOAD_LENGTH + 1] = {};

    unsigned long updatedAt = 0;

    // Память под историю выделяется только для числовых значений
    // и только при наличии свободного лимита.
    std::unique_ptr<HistoryPoint[]> history;

    size_t historyStart = 0;
    size_t historyCount = 0;
    unsigned long historyLastAt = 0;
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