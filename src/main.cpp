#include <Arduino.h>
#include <WiFi.h>
#include <PubSubClient.h>
#include <EmbeddedMqttBroker.h>
#include <ArduinoJson.h>

#include <algorithm>
#include <cstring>
#include <vector>

#include "config.h"
#include "Settings.h"
#include "WebDashboard.h"

using namespace mqttBrokerName;

// ============================================================
// Ограничения и настройки
// ============================================================

// Максимальное количество разных топиков избранных виджетов.
// Память под состояния выделяется по мере получения данных.
constexpr size_t MAX_FAVORITE_TOPICS = 48;

// На разных ревизиях плат полярность LED может отличаться.
// Если светодиод работает наоборот, замените на false.
constexpr bool LED_ACTIVE_HIGH = true;

// ============================================================
// Объекты
// ============================================================

MqttBroker* localBroker = nullptr;

WiFiClient localNetworkClient;
WiFiClient mainNetworkClient;

PubSubClient localMqtt(localNetworkClient);
PubSubClient mainMqtt(mainNetworkClient);

// Вектор передаётся в WebDashboard и существует всё время работы.
std::vector<CachedState> stateCache;

// Только имена топиков, необходимых виджетам.
std::vector<String> favoriteTopics;

// Последняя обработанная конфигурация виджетов.
String lastWidgetsJson;
bool widgetsLoaded = false;

// PubSubClient хранит указатель на строку хоста, поэтому строка
// должна существовать дольше, чем локальная переменная функции.
String mainMqttHost;

bool isApMode = false;
bool servicesStarted = false;

unsigned long lastLocalReconnectAttempt = 0;
unsigned long lastMainReconnectAttempt = 0;

// ============================================================
// Вспомогательные функции
// ============================================================

bool startsWith(const char* text, const char* prefix) {
    if (text == nullptr || prefix == nullptr) {
        return false;
    }

    return strncmp(text, prefix, strlen(prefix)) == 0;
}

// Проверка топика для публикации.
//
// Здесь намеренно нет ограничения MAX_TOPIC_LENGTH:
// этот лимит относится к локальному кэшу, а не к транзиту.
// Фактический предел транзита также зависит от MQTT_BUFFER_SIZE.
bool isPublishTopicValid(const char* topic) {
    if (topic == nullptr || topic[0] == '\0') {
        return false;
    }

    return strchr(topic, '#') == nullptr
        && strchr(topic, '+') == nullptr;
}

CachedState* findCachedState(const char* topic) {
    for (auto& state : stateCache) {
        if (strcmp(state.topic, topic) == 0) {
            return &state;
        }
    }

    return nullptr;
}

bool isFavoriteTopic(const char* topic) {
    for (const String& favorite : favoriteTopics) {
        if (favorite == topic) {
            return true;
        }
    }

    return false;
}

// ============================================================
// Список избранных топиков
// ============================================================

// Конфигурация JSON разбирается только после её изменения,
// а не на каждое сообщение MQTT.
void refreshFavoriteTopics() {
    const String& widgetsJson = Settings::widgetsJson();

    if (widgetsLoaded && widgetsJson == lastWidgetsJson) {
        return;
    }

    lastWidgetsJson = widgetsJson;
    widgetsLoaded = true;

    JsonDocument doc;
    DeserializationError error = deserializeJson(doc, widgetsJson);

    if (error || !doc.is<JsonArray>()) {
        Serial.printf(
            "[CACHE] Ошибка конфигурации виджетов: %s\n",
            error ? error.c_str() : "ожидался JSON-массив"
        );

        // При ошибке оставляем ранее действующий список.
        return;
    }

    std::vector<String> newTopics;

    for (JsonObject widget : doc.as<JsonArray>()) {
        const char* configuredTopic = widget["topic"] | "";

        String topic = configuredTopic;
        topic.trim();

        // В виджете после # указывается поле JSON.
        const int separator = topic.indexOf('#');

        if (separator >= 0) {
            topic = topic.substring(0, separator);
            topic.trim();
        }

        if (
            topic.isEmpty() ||
            topic.length() > MAX_TOPIC_LENGTH ||
            topic.indexOf('#') >= 0 ||
            topic.indexOf('+') >= 0
        ) {
            continue;
        }

        bool alreadyExists = false;

        for (const String& existing : newTopics) {
            if (existing == topic) {
                alreadyExists = true;
                break;
            }
        }

        if (alreadyExists) {
            continue;
        }

        if (newTopics.size() >= MAX_FAVORITE_TOPICS) {
            Serial.printf(
                "[CACHE] Достигнут лимит избранных топиков: %u\n",
                static_cast<unsigned>(MAX_FAVORITE_TOPICS)
            );
            break;
        }

        newTopics.push_back(topic);
    }

    favoriteTopics.swap(newTopics);

    // Удаляем из кэша состояния виджетов, которые больше не нужны.
    stateCache.erase(
        std::remove_if(
            stateCache.begin(),
            stateCache.end(),
            [](const CachedState& state) {
                return !isFavoriteTopic(state.topic);
            }
        ),
        stateCache.end()
    );

    Serial.printf(
        "[CACHE] Избранных топиков: %u; состояний в памяти: %u\n",
        static_cast<unsigned>(favoriteTopics.size()),
        static_cast<unsigned>(stateCache.size())
    );
}

// ============================================================
// Кэш состояний
// ============================================================

bool saveStateToCache(
    const char* topic,
    const byte* payload,
    unsigned int length
) {
    if (!isFavoriteTopic(topic)) {
        return false;
    }

    // Ограничения кэша не должны ограничивать ретрансляцию.
    if (strlen(topic) > MAX_TOPIC_LENGTH) {
        Serial.printf("[CACHE] Слишком длинный топик: %s\n", topic);
        return false;
    }

    if (length > MAX_PAYLOAD_LENGTH) {
        Serial.printf(
            "[CACHE] Сообщение не помещается: %s, %u байт; предел %u\n",
            topic,
            length,
            static_cast<unsigned>(MAX_PAYLOAD_LENGTH)
        );
        return false;
    }

    // Веб-интерфейс ожидает текст, а не двоичный payload.
    for (unsigned int i = 0; i < length; ++i) {
        if (payload[i] == '\0') {
            Serial.printf("[CACHE] Нетекстовое сообщение: %s\n", topic);
            return false;
        }
    }

    CachedState* state = findCachedState(topic);

    if (state == nullptr) {
        if (stateCache.size() >= MAX_FAVORITE_TOPICS) {
            Serial.println("[CACHE] Достигнут лимит состояний");
            return false;
        }

        CachedState newState{};
        newState.used = true;

        strncpy(newState.topic, topic, MAX_TOPIC_LENGTH);
        newState.topic[MAX_TOPIC_LENGTH] = '\0';

        stateCache.push_back(newState);
        state = &stateCache.back();
    }

    if (length > 0) {
        memcpy(state->payload, payload, length);
    }

    state->payload[length] = '\0';
    state->updatedAt = millis();

    return true;
}

// ============================================================
// Ретрансляция MQTT
// ============================================================

// PubSubClient не сообщает исходный флаг retain в callback.
// Поэтому это явная политика шлюза, а не сохранение оригинала:
//
// - discovery-конфигурации публикуем с retain;
// - прочие сообщения публикуем без retain.
//
// При необходимости эту политику можно изменить отдельно.
bool shouldRetainOnMain(const char* topic) {
    return startsWith(topic, "homeassistant/")
        || startsWith(topic, "tasmota/discovery/");
}

void publishCachedStatesToMain() {
    if (!mainMqtt.connected()) {
        return;
    }

    for (const CachedState& state : stateCache) {
        // Отправляем последние избранные значения без retain.
        bool ok = mainMqtt.publish(
            state.topic,
            state.payload,
            false
        );

        if (!ok) {
            Serial.printf(
                "[MAIN MQTT] Не удалось восстановить состояние: %s\n",
                state.topic
            );
        }
    }
}

void onLocalMqttMessage(
    char* topic,
    byte* payload,
    unsigned int length
) {
    // Не пересылаем команды обратно на главный брокер:
    // это защищает от петли main -> local -> main.
    if (startsWith(topic, "cmnd/")) {
        return;
    }

    if (!isPublishTopicValid(topic)) {
        return;
    }

    if (strcmp(topic, GATEWAY_AVAILABILITY_TOPIC) == 0) {
        return;
    }

    // 1. Независимо от кэша пересылаем входящее сообщение.
    if (mainMqtt.connected()) {
        bool ok = mainMqtt.publish(
            topic,
            payload,
            length,
            shouldRetainOnMain(topic)
        );

        if (!ok) {
            Serial.printf(
                "[MAIN MQTT] Не удалось переслать %s (%u байт)\n",
                topic,
                length
            );
        }
    }

    // 2. Только избранное сохраняем в оперативную память.
    refreshFavoriteTopics();
    saveStateToCache(topic, payload, length);
}

void onMainMqttMessage(
    char* topic,
    byte* payload,
    unsigned int length
) {
    // Текущая настройка принимает команды Tasmota.
    if (!startsWith(topic, "cmnd/")) {
        return;
    }

    if (!isPublishTopicValid(topic)) {
        return;
    }

    if (!localMqtt.connected()) {
        Serial.printf(
            "[LOCAL MQTT] Команда отброшена: нет связи с брокером, %s\n",
            topic
        );
        return;
    }

    bool ok = localMqtt.publish(
        topic,
        payload,
        length,
        false
    );

    if (!ok) {
        Serial.printf(
            "[LOCAL MQTT] Не удалось передать команду: %s\n",
            topic
        );
    }
}

// ============================================================
// Светодиод
// ============================================================

void writeLed(bool enabled) {
    const uint8_t level = LED_ACTIVE_HIGH
        ? (enabled ? HIGH : LOW)
        : (enabled ? LOW : HIGH);

    digitalWrite(LED_PIN, level);
}

void updateLedIndicator() {
    static unsigned long lastToggle = 0;
    static bool ledOn = false;

    const unsigned long now = millis();

    // MQTT подключён: переключение каждые 2 секунды.
    // MQTT недоступен, Wi-Fi недоступен или AP: каждые 300 мс.
    const unsigned long interval =
        (WiFi.status() == WL_CONNECTED && mainMqtt.connected())
            ? 2000UL
            : 300UL;

    if (now - lastToggle >= interval) {
        lastToggle = now;
        ledOn = !ledOn;
        writeLed(ledOn);
    }
}

// ============================================================
// Запуск сервисов
// ============================================================

void startServices() {
    if (servicesStarted) {
        return;
    }

    if (localBroker == nullptr) {
        localBroker = MqttBrokerFactory::createTcpBroker(
            LOCAL_MQTT_PORT
        );

        if (localBroker == nullptr) {
            Serial.println("[LOCAL MQTT] Не удалось создать брокер");
            return;
        }

        localBroker->startBroker();

        Serial.printf(
            "[LOCAL MQTT] Запуск брокера на порту %u\n",
            static_cast<unsigned>(LOCAL_MQTT_PORT)
        );
    }

    localMqtt.setServer(
        WiFi.localIP(),
        LOCAL_MQTT_PORT
    );

    localMqtt.setCallback(onLocalMqttMessage);
    localMqtt.setSocketTimeout(2);

    if (!localMqtt.setBufferSize(MQTT_BUFFER_SIZE)) {
        Serial.println("[LOCAL MQTT] Не удалось выделить MQTT-буфер");
    }

    SystemSettings settings = Settings::get();

    mainMqttHost = settings.mqttHost;

    mainMqtt.setServer(
        mainMqttHost.c_str(),
        settings.mqttPort
    );

    mainMqtt.setCallback(onMainMqttMessage);
    mainMqtt.setSocketTimeout(2);

    if (!mainMqtt.setBufferSize(MQTT_BUFFER_SIZE)) {
        Serial.println("[MAIN MQTT] Не удалось выделить MQTT-буфер");
    }

    refreshFavoriteTopics();

    WebDashboard::begin(
        localMqtt,
        mainMqtt,
        stateCache,
        false
    );

    servicesStarted = true;

    Serial.printf(
        "[WEB] http://%s/\n",
        WiFi.localIP().toString().c_str()
    );
}

// ============================================================
// Подключение к локальному MQTT
// ============================================================

void reconnectLocalMqtt() {
    if (localMqtt.connected()) {
        return;
    }

    const unsigned long now = millis();

    if (
        lastLocalReconnectAttempt != 0 &&
        now - lastLocalReconnectAttempt <
            MQTT_RECONNECT_INTERVAL_MS
    ) {
        return;
    }

    lastLocalReconnectAttempt = now;

    localMqtt.setServer(
        WiFi.localIP(),
        LOCAL_MQTT_PORT
    );

    Serial.println("[LOCAL MQTT] Подключение...");

    if (!localMqtt.connect(LOCAL_CLIENT_ID)) {
        Serial.printf(
            "[LOCAL MQTT] Ошибка: state=%d\n",
            localMqtt.state()
        );
        return;
    }

    Serial.println("[LOCAL MQTT] Подключён");

    if (!localMqtt.subscribe(LOCAL_SUBSCRIPTION)) {
        Serial.println("[LOCAL MQTT] Ошибка подписки");
    }
}

// ============================================================
// Подключение к главному MQTT
// ============================================================

void reconnectMainMqtt() {
    if (mainMqtt.connected()) {
        return;
    }

    const unsigned long now = millis();

    if (
        lastMainReconnectAttempt != 0 &&
        now - lastMainReconnectAttempt <
            MQTT_RECONNECT_INTERVAL_MS
    ) {
        return;
    }

    lastMainReconnectAttempt = now;

    SystemSettings settings = Settings::get();

    if (mainMqttHost.isEmpty()) {
        Serial.println("[MAIN MQTT] Адрес брокера не задан");
        return;
    }

    Serial.printf(
        "[MAIN MQTT] Подключение к %s:%u, client=%s\n",
        mainMqttHost.c_str(),
        static_cast<unsigned>(settings.mqttPort),
        MAIN_CLIENT_ID
    );

    bool connected;

    if (!settings.mqttUser.isEmpty()) {
        connected = mainMqtt.connect(
            MAIN_CLIENT_ID,
            settings.mqttUser.c_str(),
            settings.mqttPassword.c_str(),
            GATEWAY_AVAILABILITY_TOPIC,
            0,
            true,
            "offline"
        );
    } else {
        connected = mainMqtt.connect(
            MAIN_CLIENT_ID,
            GATEWAY_AVAILABILITY_TOPIC,
            0,
            true,
            "offline"
        );
    }

    if (!connected) {
        Serial.printf(
            "[MAIN MQTT] Ошибка подключения: state=%d\n",
            mainMqtt.state()
        );
        return;
    }

    Serial.println("[MAIN MQTT] Подключён");

    if (!mainMqtt.subscribe(MAIN_COMMAND_SUBSCRIPTION)) {
        Serial.println("[MAIN MQTT] Ошибка подписки на команды");
    }

    if (!mainMqtt.publish(
        GATEWAY_AVAILABILITY_TOPIC,
        "online",
        true
    )) {
        Serial.println("[MAIN MQTT] Ошибка публикации availability");
    }

    publishCachedStatesToMain();
}

// ============================================================
// Точка доступа настройки
// ============================================================

bool startSetupAccessPoint() {
    WiFi.disconnect(true);
    delay(100);

    if (!WiFi.mode(WIFI_AP)) {
        Serial.println("[AP] Не удалось установить режим AP");
        return false;
    }

    delay(100);

    const size_t passwordLength = strlen(AP_PASSWORD);

    if (passwordLength > 0 && passwordLength < 8) {
        Serial.println("[AP] Пароль должен содержать не менее 8 байт");
        return false;
    }

    const bool started = passwordLength == 0
        ? WiFi.softAP(AP_SSID)
        : WiFi.softAP(AP_SSID, AP_PASSWORD);

    if (!started) {
        Serial.println("[AP] Не удалось запустить точку доступа");
        return false;
    }

    isApMode = true;

    Serial.printf(
        "[AP] SSID: %s, IP: %s\n",
        AP_SSID,
        WiFi.softAPIP().toString().c_str()
    );

    if (passwordLength == 0) {
        Serial.println("[AP] ВНИМАНИЕ: сеть открыта, без пароля");
    }

    // В режиме настройки брокер не нужен.
    WebDashboard::begin(
        localMqtt,
        mainMqtt,
        stateCache,
        true
    );

    return true;
}

// ============================================================
// Arduino setup / loop
// ============================================================

void setup() {
    Serial.begin(115200);

    pinMode(LED_PIN, OUTPUT);
    writeLed(false);

    Settings::init();

    SystemSettings settings = Settings::get();

    if (settings.wifiSsid.isEmpty()) {
        Serial.println("[WIFI] SSID не задан. Режим настройки.");

        if (!startSetupAccessPoint()) {
            Serial.println("[AP] Ошибка запуска. Перезагрузите устройство.");
        }

        return;
    }

    WiFi.mode(WIFI_STA);
    WiFi.setAutoReconnect(true);
    WiFi.setHostname(DEVICE_HOSTNAME);

    Serial.printf(
        "[WIFI] Подключение к сети: %s\n",
        settings.wifiSsid.c_str()
    );

    WiFi.begin(
        settings.wifiSsid.c_str(),
        settings.wifiPassword.c_str()
    );

    const unsigned long startedAt = millis();

    while (WiFi.status() != WL_CONNECTED) {
        updateLedIndicator();

        if (millis() - startedAt >= WIFI_CONNECT_TIMEOUT_MS) {
            Serial.println("[WIFI] Таймаут. Режим настройки.");

            if (!startSetupAccessPoint()) {
                Serial.println(
                    "[AP] Ошибка запуска. Перезагрузите устройство."
                );
            }

            return;
        }

        delay(50);
    }

    Serial.printf(
        "[WIFI] Подключён. IP: %s\n",
        WiFi.localIP().toString().c_str()
    );
}

void loop() {
    // Индикатор работает также при отсутствии Wi-Fi и в режиме AP.
    updateLedIndicator();

    if (isApMode) {
        // Это необходимо, даже когда WiFi.status() не WL_CONNECTED.
        WebDashboard::handleClient();
        delay(5);
        return;
    }

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

    WebDashboard::handleClient();

    if (!localMqtt.connected()) {
        reconnectLocalMqtt();
    } else {
        localMqtt.loop();
    }

    if (!mainMqtt.connected()) {
        reconnectMainMqtt();
    } else {
        mainMqtt.loop();
    }

    delay(5);
}