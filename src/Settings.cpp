#include "Settings.h"
#include "config.h"
#include <Preferences.h>

Preferences preferences;
SystemSettings currentSettings;
// По умолчанию сохраняем прежнее поведение.
static bool historyEnabledValue = true;

namespace Settings {
const String& widgetsJson() {
    return currentSettings.widgetsJson;
}

void init() {
    preferences.begin("gateway", false);

    currentSettings.configured = preferences.getBool("configured", false);

    currentSettings.wifiSsid = preferences.getString("wifi_ssid", DEFAULT_WIFI_SSID);
    if (currentSettings.wifiSsid.length() == 0) currentSettings.wifiSsid = DEFAULT_WIFI_SSID;

    currentSettings.wifiPassword = preferences.getString("wifi_pass", DEFAULT_WIFI_PASSWORD);
    
    currentSettings.mqttHost = preferences.getString("mqtt_host", DEFAULT_MQTT_HOST);
    if (currentSettings.mqttHost.length() == 0) currentSettings.mqttHost = DEFAULT_MQTT_HOST;

    currentSettings.mqttPort = preferences.getUShort("mqtt_port", DEFAULT_MQTT_PORT);
    if (currentSettings.mqttPort == 0) currentSettings.mqttPort = DEFAULT_MQTT_PORT;

    currentSettings.mqttUser = preferences.getString("mqtt_user", DEFAULT_MQTT_USER);
    currentSettings.mqttPassword = preferences.getString("mqtt_pass", DEFAULT_MQTT_PASSWORD);

    // Загружаем сохраненные виджеты дашборда
    currentSettings.widgetsJson = preferences.getString("widgets", "[]");

    currentSettings.maxToken = preferences.getString("max_token", DEFAULT_MAX_TOKEN);
    currentSettings.maxUserId = preferences.getString("max_userid", DEFAULT_MAX_USER_ID);

    historyEnabledValue = preferences.getBool("history_on", true);

    preferences.end();
}

SystemSettings get() {
    return currentSettings;
}

void save(const SystemSettings& settings) {
    preferences.begin("gateway", false);

    preferences.putString("wifi_ssid", settings.wifiSsid);
    preferences.putString("wifi_pass", settings.wifiPassword);
    
    preferences.putString("mqtt_host", settings.mqttHost);
    preferences.putUShort("mqtt_port", settings.mqttPort);
    preferences.putString("mqtt_user", settings.mqttUser);
    preferences.putString("mqtt_pass", settings.mqttPassword);
    preferences.putBool("configured", true);

    preferences.end();
    
    currentSettings.wifiSsid = settings.wifiSsid;
    currentSettings.wifiPassword = settings.wifiPassword;
    currentSettings.mqttHost = settings.mqttHost;
    currentSettings.mqttPort = settings.mqttPort;
    currentSettings.mqttUser = settings.mqttUser;
    currentSettings.mqttPassword = settings.mqttPassword;
    currentSettings.configured = true;
}

void saveWidgets(const String& widgetsJson) {
    preferences.begin("gateway", false);
    preferences.putString("widgets", widgetsJson);
    preferences.end();
    currentSettings.widgetsJson = widgetsJson;
}

bool historyEnabled() {
    return historyEnabledValue;
}

bool saveHistoryEnabled(bool enabled) {
    if (!preferences.begin("gateway", false)) {
        return false;
    }

    const size_t written = preferences.putBool("history_on", enabled);
    preferences.end();

    if (written != 1) {
        return false;
    }

    historyEnabledValue = enabled;
    return true;
}

// Новая функция сохранения настроек MAX:
void saveMax(const String& token, const String& userId) {
    preferences.begin("gateway", false);
    preferences.putString("max_token", token);
    preferences.putString("max_userid", userId);
    preferences.end();
    currentSettings.maxToken = token;
    currentSettings.maxUserId = userId;
}

} // namespace Settings