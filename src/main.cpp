#include <Arduino.h>
#include <WiFi.h>
#include <PubSubClient.h>
#include <EmbeddedMqttBroker.h>
#include <ESPmDNS.h>
#include <ArduinoJson.h>
#include <cstring>

#include "config.h"
#include "Settings.h"
#include "WebDashboard.h"

using namespace mqttBrokerName;

namespace {

MqttBroker* localBroker = nullptr;

WiFiClient localNetworkClient;
WiFiClient mainNetworkClient;

PubSubClient localMqtt(localNetworkClient);
PubSubClient mainMqtt(mainNetworkClient);

CachedState stateCache[MAX_CACHED_STATES];

char favoriteTopics[MAX_CACHED_STATES][MAX_TOPIC_LENGTH + 1] = {};
size_t favoriteTopicCount = 0;

String mainMqttHost;
String lastWidgetsJson;
bool widgetsLoaded = false;

bool apMode = false;
bool servicesStarted = false;

unsigned long lastLocalReconnectAttempt = 0;
unsigned long lastMainReconnectAttempt = 0;
bool localReconnectAttempted = false;
bool mainReconnectAttempted = false;

bool startsWith(const char* text, const char* prefix) {
    return text && prefix &&
           strncmp(text, prefix, strlen(prefix)) == 0;
}

bool endsWith(const char* text, const char* suffix) {
    if (!text || !suffix) {
        return false;
    }

    const size_t textLength = strlen(text);
    const size_t suffixLength = strlen(suffix);

    return textLength >= suffixLength &&
           strcmp(text + textLength - suffixLength, suffix) == 0;
}

bool isCommandTopic(const char* topic) {
    return startsWith(topic, "cmnd/") ||
           endsWith(topic, "/command") ||
           endsWith(topic, "/set");
}

bool validMqttTopic(const char* topic) {
    return topic &&
           topic[0] != '\0' &&
           strchr(topic, '#') == nullptr &&
           strchr(topic, '+') == nullptr;
}

bool shouldRetainOnMain(const char* topic) {
    return startsWith(topic, "homeassistant/") ||
           startsWith(topic, "tasmota/discovery/");
}

bool isFavoriteTopic(const char* topic) {
    for (size_t i = 0; i < favoriteTopicCount; ++i) {
        if (strcmp(topic, favoriteTopics[i]) == 0) {
            return true;
        }
    }

    return false;
}

void refreshFavoriteTopics() {
    const String& json = Settings::widgetsJson();

    if (widgetsLoaded && json == lastWidgetsJson) {
        return;
    }

    lastWidgetsJson = json;
    widgetsLoaded = true;

    JsonDocument doc;
    DeserializationError error = deserializeJson(doc, json);

    if (error || !doc.is<JsonArray>()) {
        favoriteTopicCount = 0;
        memset(stateCache, 0, sizeof(stateCache));
        Serial.println("[WIDGETS] Некорректная сохранённая конфигурация");
        return;
    }

    char newTopics[MAX_CACHED_STATES][MAX_TOPIC_LENGTH + 1] = {};
    size_t newCount = 0;

    for (JsonObject widget : doc.as<JsonArray>()) {
        String configuredTopic = widget["topic"] | "";
        configuredTopic.trim();

        const int hash = configuredTopic.indexOf('#');
        String baseTopic = hash >= 0
            ? configuredTopic.substring(0, hash)
            : configuredTopic;

        baseTopic.trim();

        if (baseTopic.isEmpty() ||
            baseTopic.length() > MAX_TOPIC_LENGTH ||
            baseTopic.indexOf('#') >= 0 ||
            baseTopic.indexOf('+') >= 0) {
            continue;
        }

        bool duplicate = false;
        for (size_t i = 0; i < newCount; ++i) {
            if (baseTopic == newTopics[i]) {
                duplicate = true;
                break;
            }
        }

        if (duplicate || newCount >= MAX_CACHED_STATES) {
            continue;
        }

        strlcpy(
            newTopics[newCount],
            baseTopic.c_str(),
            sizeof(newTopics[newCount])
        );
        ++newCount;
    }

    memcpy(favoriteTopics, newTopics, sizeof(favoriteTopics));
    favoriteTopicCount = newCount;

    memset(stateCache, 0, sizeof(stateCache));

    Serial.printf(
        "[WIDGETS] Топиков для кэширования: %u\n",
        static_cast<unsigned>(favoriteTopicCount)
    );
}

void cacheLatestState(
    const char* topic,
    const byte* payload,
    unsigned int length
) {
    if (!topic ||
        !isFavoriteTopic(topic) ||
        strlen(topic) > MAX_TOPIC_LENGTH ||
        length > MAX_PAYLOAD_LENGTH) {
        return;
    }

    // Веб-панель показывает текстовые MQTT payload.
    for (unsigned int i = 0; i < length; ++i) {
        if (payload[i] == '\0') {
            return;
        }
    }

    CachedState* slot = nullptr;

    for (size_t i = 0; i < MAX_CACHED_STATES; ++i) {
        if (stateCache[i].used &&
            strcmp(stateCache[i].topic, topic) == 0) {
            slot = &stateCache[i];
            break;
        }

        if (!slot && !stateCache[i].used) {
            slot = &stateCache[i];
        }
    }

    if (!slot) {
        return;
    }

    slot->used = true;
    strlcpy(slot->topic, topic, sizeof(slot->topic));

    if (length > 0) {
        memcpy(slot->payload, payload, length);
    }

    slot->payload[length] = '\0';
    slot->updatedAt = millis();
}

void onLocalMqttMessage(
    char* topic,
    byte* payload,
    unsigned int length
) {
    if (!validMqttTopic(topic)) {
        return;
    }

    // Не отправляем команды обратно на главный брокер.
    if (isCommandTopic(topic)) {
        return;
    }

    if (strcmp(topic, GATEWAY_AVAILABILITY_TOPIC) == 0) {
        return;
    }

    if (mainMqtt.connected()) {
        mainMqtt.publish(
            topic,
            payload,
            length,
            shouldRetainOnMain(topic)
        );
    }

    // В памяти остаются только последние значения топиков виджетов.
    cacheLatestState(topic, payload, length);
}

void onMainMqttMessage(
    char* topic,
    byte* payload,
    unsigned int length
) {
    if (!validMqttTopic(topic) || !isCommandTopic(topic)) {
        return;
    }

    if (!localMqtt.connected()) {
        return;
    }

    localMqtt.publish(topic, payload, length, false);
}

bool startSetupAccessPoint() {
    WiFi.disconnect(true);
    delay(100);

    if (!WiFi.mode(WIFI_AP)) {
        Serial.println("[AP] Не удалось включить режим точки доступа");
        return false;
    }

    const size_t passwordLength = strlen(AP_PASSWORD);

    if (passwordLength > 0 && passwordLength < 8) {
        Serial.println("[AP] Пароль AP должен содержать не менее 8 символов");
        return false;
    }

    const bool started = passwordLength == 0
        ? WiFi.softAP(AP_SSID)
        : WiFi.softAP(AP_SSID, AP_PASSWORD);

    if (!started) {
        Serial.println("[AP] Не удалось запустить точку доступа");
        return false;
    }

    apMode = true;

    WebDashboard::begin(
        localMqtt,
        mainMqtt,
        stateCache,
        MAX_CACHED_STATES,
        true
    );

    Serial.printf(
        "[AP] SSID: %s, IP: %s\n",
        AP_SSID,
        WiFi.softAPIP().toString().c_str()
    );

    if (MDNS.begin(DEVICE_HOSTNAME)) {
        Serial.printf("[mDNS] http://%s.local\n", DEVICE_HOSTNAME);
    }

    return true;
}

void startServices() {
    if (servicesStarted || WiFi.status() != WL_CONNECTED) {
        return;
    }

    if (!localBroker) {
        localBroker =
            MqttBrokerFactory::createTcpBroker(LOCAL_MQTT_PORT);

        if (!localBroker) {
            Serial.println("[LOCAL MQTT] Не удалось создать брокер");
            return;
        }

        localBroker->startBroker();
    }

    localMqtt.setServer(WiFi.localIP(), LOCAL_MQTT_PORT);
    localMqtt.setCallback(onLocalMqttMessage);
    localMqtt.setSocketTimeout(2);
    localMqtt.setKeepAlive(30);
    localMqtt.setBufferSize(MQTT_BUFFER_SIZE);

    const SystemSettings& settings = Settings::get();
    mainMqttHost = settings.mqttHost;

    mainMqtt.setServer(mainMqttHost.c_str(), settings.mqttPort);
    mainMqtt.setCallback(onMainMqttMessage);
    mainMqtt.setSocketTimeout(2);
    mainMqtt.setKeepAlive(30);
    mainMqtt.setBufferSize(MQTT_BUFFER_SIZE);

    refreshFavoriteTopics();

    WebDashboard::begin(
        localMqtt,
        mainMqtt,
        stateCache,
        MAX_CACHED_STATES,
        false
    );

    if (MDNS.begin(DEVICE_HOSTNAME)) {
        Serial.printf("[mDNS] http://%s.local\n", DEVICE_HOSTNAME);
    }

    servicesStarted = true;

    Serial.printf(
        "[WIFI] IP: %s\n",
        WiFi.localIP().toString().c_str()
    );
}

void reconnectLocalMqtt() {
    if (localMqtt.connected()) {
        return;
    }

    const unsigned long now = millis();

    if (localReconnectAttempted &&
        now - lastLocalReconnectAttempt < MQTT_RECONNECT_INTERVAL_MS) {
        return;
    }

    localReconnectAttempted = true;
    lastLocalReconnectAttempt = now;

    localMqtt.setServer(WiFi.localIP(), LOCAL_MQTT_PORT);

    if (!localMqtt.connect(LOCAL_CLIENT_ID)) {
        Serial.printf("[LOCAL MQTT] Ошибка, state=%d\n", localMqtt.state());
        return;
    }

    localMqtt.subscribe(LOCAL_SUBSCRIPTION);
    Serial.println("[LOCAL MQTT] Подключён");
}

void publishCachedStatesToMain() {
    for (size_t i = 0; i < MAX_CACHED_STATES; ++i) {
        if (!stateCache[i].used) {
            continue;
        }

        if (!mainMqtt.publish(
                stateCache[i].topic,
                stateCache[i].payload,
                false)) {
            return;
        }
    }
}

void reconnectMainMqtt() {
    if (mainMqtt.connected()) {
        return;
    }

    const unsigned long now = millis();

    if (mainReconnectAttempted &&
        now - lastMainReconnectAttempt < MQTT_RECONNECT_INTERVAL_MS) {
        return;
    }

    mainReconnectAttempted = true;
    lastMainReconnectAttempt = now;

    const SystemSettings& settings = Settings::get();

    if (mainMqttHost.isEmpty()) {
        return;
    }

    bool connected;

    if (!settings.mqttUser.isEmpty()) {
        connected = mainMqtt.connect(
            MAIN_CLIENT_ID,
            settings.mqttUser.c_str(),
            settings.mqttPassword.c_str()
        );
    } else {
        connected = mainMqtt.connect(MAIN_CLIENT_ID);
    }

    if (!connected) {
        Serial.printf("[MAIN MQTT] Ошибка, state=%d\n", mainMqtt.state());
        return;
    }

    mainMqtt.subscribe(MAIN_COMMAND_SUBSCRIPTION);
    publishCachedStatesToMain();

    Serial.println("[MAIN MQTT] Подключён");
}

} // namespace

void setup() {
    Serial.begin(115200);
    delay(100);

    Settings::init();

    const SystemSettings& settings = Settings::get();

    if (settings.wifiSsid.isEmpty()) {
        startSetupAccessPoint();
        return;
    }

    WiFi.mode(WIFI_STA);
    WiFi.setHostname(DEVICE_HOSTNAME);
    WiFi.setAutoReconnect(true);

    WiFi.begin(
        settings.wifiSsid.c_str(),
        settings.wifiPassword.c_str()
    );

    Serial.printf("[WIFI] Подключение к %s\n", settings.wifiSsid.c_str());

    const unsigned long startedAt = millis();

    while (WiFi.status() != WL_CONNECTED &&
           millis() - startedAt < WIFI_CONNECT_TIMEOUT_MS) {
        delay(100);
    }

    if (WiFi.status() != WL_CONNECTED) {
        startSetupAccessPoint();
        return;
    }

    startServices();
}

void loop() {
    if (apMode) {
        WebDashboard::handleClient();
        delay(5);
        return;
    }

    WebDashboard::handleClient();

    if (WiFi.status() != WL_CONNECTED) {
        delay(100);
        return;
    }

    if (!servicesStarted) {
        startServices();

        if (!servicesStarted) {
            delay(1000);
            return;
        }
    }

    refreshFavoriteTopics();

    if (localMqtt.connected()) {
        localMqtt.loop();
    } else {
        reconnectLocalMqtt();
    }

    if (mainMqtt.connected()) {
        mainMqtt.loop();
    } else {
        reconnectMainMqtt();
    }

    delay(5);
}