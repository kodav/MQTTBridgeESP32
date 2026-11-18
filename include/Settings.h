#pragma once

#include <Arduino.h>
#include <stdint.h>

struct SystemSettings {
    String wifiSsid;
    String wifiPassword;

    String mqttHost;
    uint16_t mqttPort = 1883;

    String mqttUser;
    String mqttPassword;

    bool configured = false;
};

namespace Settings {
    void init();

    const SystemSettings& get();
    const String& widgetsJson();

    bool save(const SystemSettings& settings);
    bool saveWidgets(const String& json);
}