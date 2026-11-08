#pragma once

#include <Arduino.h>

struct SystemSettings {
    String wifiSsid;
    String wifiPassword;

    String mqttHost;
    uint16_t mqttPort;

    String mqttUser;
    String mqttPassword;

    bool configured;

    // Данные виджетов, загруженные из Preferences
    String widgetsJson;
};

namespace Settings {
    void init();
    SystemSettings get();
    void save(const SystemSettings& settings);
    void saveWidgets(const String& widgetsJson);

    // Доступ к JSON виджетов без копирования всей структуры
    const String& widgetsJson();
}