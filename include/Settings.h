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

    String maxToken;
    String maxUserId; // ID пользователя в MAX
};

namespace Settings {
    void init();
    SystemSettings get();
    void save(const SystemSettings& settings);
    void saveWidgets(const String& widgetsJson);

    // Доступ к JSON виджетов без копирования всей структуры
    const String& widgetsJson();

    // Общий переключатель истории графиков.
    bool historyEnabled();
    bool saveHistoryEnabled(bool enabled);
    void saveMax(const String& token, const String& userId);
}