#include "Settings.h"
#include "config.h"

#include <Preferences.h>

namespace {
Preferences preferences;
SystemSettings currentSettings;
String currentWidgetsJson = "[]";
}

namespace Settings {

void init() {
    currentSettings = SystemSettings{};
    currentSettings.mqttPort = DEFAULT_MQTT_PORT;
    currentWidgetsJson = "[]";

    if (!preferences.begin("gateway", true)) {
        return;
    }

    currentSettings.configured =
        preferences.getBool("configured", false);

    currentSettings.wifiSsid =
        preferences.getString("wifi_ssid", DEFAULT_WIFI_SSID);

    currentSettings.wifiPassword =
        preferences.getString("wifi_pass", DEFAULT_WIFI_PASSWORD);

    currentSettings.mqttHost =
        preferences.getString("mqtt_host", DEFAULT_MQTT_HOST);

    currentSettings.mqttPort =
        preferences.getUShort("mqtt_port", DEFAULT_MQTT_PORT);

    if (currentSettings.mqttPort == 0) {
        currentSettings.mqttPort = DEFAULT_MQTT_PORT;
    }

    currentSettings.mqttUser =
        preferences.getString("mqtt_user", DEFAULT_MQTT_USER);

    currentSettings.mqttPassword =
        preferences.getString("mqtt_pass", DEFAULT_MQTT_PASSWORD);

    currentWidgetsJson =
        preferences.getString("widgets", "[]");

    preferences.end();
}

const SystemSettings& get() {
    return currentSettings;
}

const String& widgetsJson() {
    return currentWidgetsJson;
}

bool save(const SystemSettings& settings) {
    if (!preferences.begin("gateway", false)) {
        return false;
    }

    bool ok = true;

    ok &= preferences.putString("wifi_ssid", settings.wifiSsid) > 0;
    ok &= preferences.putString("wifi_pass", settings.wifiPassword) > 0;
    ok &= preferences.putString("mqtt_host", settings.mqttHost) > 0;
    ok &= preferences.putUShort("mqtt_port", settings.mqttPort) > 0;
    ok &= preferences.putString("mqtt_user", settings.mqttUser) > 0;
    ok &= preferences.putString("mqtt_pass", settings.mqttPassword) > 0;
    ok &= preferences.putBool("configured", true) > 0;

    preferences.end();

    if (ok) {
        currentSettings = settings;
        currentSettings.configured = true;
    }

    return ok;
}

bool saveWidgets(const String& json) {
    if (!preferences.begin("gateway", false)) {
        return false;
    }

    const size_t written = preferences.putString("widgets", json);
    preferences.end();

    if (written == 0) {
        return false;
    }

    currentWidgetsJson = json;
    return true;
}

} // namespace Settings