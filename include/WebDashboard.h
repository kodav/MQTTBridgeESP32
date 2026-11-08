#pragma once

#include <Arduino.h>
#include <PubSubClient.h>
#include <vector>
#include "config.h"

struct CachedState {
    bool used = false;
    char topic[MAX_TOPIC_LENGTH + 1] = {};
    char payload[MAX_PAYLOAD_LENGTH + 1] = {};
    unsigned long updatedAt = 0;
};

namespace WebDashboard {
    void begin(
        PubSubClient& localMqtt,
        PubSubClient& mainMqtt,
        std::vector<CachedState>& states,
        bool apMode
    );

    void handleClient();
}