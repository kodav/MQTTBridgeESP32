#include <Arduino.h>
#include <WiFi.h>
#include <PubSubClient.h>
#include <EmbeddedMqttBroker.h>
#include <ArduinoJson.h>
#include <ESPmDNS.h>
#include <HTTPClient.h>
#include <WiFiClientSecure.h>

#include <algorithm>
#include <cstring>
#include <vector>
#include <utility>

#include <cctype>
#include <cmath>
#include <cstdlib>
#include <new>
#include <esp_heap_caps.h>
#include <esp_system.h>
#include <esp_task_wdt.h>

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

constexpr uint32_t WATCHDOG_TIMEOUT_S = 120;

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
std::vector<JsonFieldHistory> jsonFieldHistories;

// Освобождает историю всех датчиков, не удаляя текущие значения.
void clearAllHistory() {
    for (CachedState& state : stateCache) {
        state.history.reset();
        state.historyStart = 0;
        state.historyCount = 0;
        state.historyLastAt = 0;
    }

    // Удаляем историю JSON-полей и освобождаем память вектора.
    std::vector<JsonFieldHistory>().swap(jsonFieldHistories);
}

// Считаем все выделенные буферы: обычные и для полей JSON.
size_t countAllocatedHistories() {
    size_t count = 0;

    for (const CachedState& state : stateCache) {
        if (state.history) {
            ++count;
        }
    }

    for (const JsonFieldHistory& field : jsonFieldHistories) {
        if (field.history) {
            ++count;
        }
    }

    return count;
}

// Только имена топиков, необходимых виджетам.
std::vector<String> favoriteTopics;

// Очередь ожидающих отправки сообщений в MAX (защита от переполнения RAM — максимум 10 штук)
std::vector<String> pendingAlerts;
constexpr size_t MAX_PENDING_ALERTS = 10;
unsigned long lastAlertProcessTime = 0;

// Последняя обработанная конфигурация виджетов.
String lastWidgetsJson;
bool widgetsLoaded = false;
// Разобранная конфигурация используется повторно при проверке правил.
JsonDocument alertWidgetsDoc;

constexpr size_t MAX_RULE_ALERT_STATES = 128;

// PubSubClient хранит указатель на строку хоста, поэтому строка
// должна существовать дольше, чем локальная переменная функции.
String mainMqttHost;

bool isApMode = false;
bool servicesStarted = false;

unsigned long lastLocalReconnectAttempt = 0;
unsigned long lastMainReconnectAttempt = 0;
bool lastMainMqttStatus = false;

// Структура для отслеживания предыдущих значений (защита от спама и повторов)
struct LastAlertState {
    String topic;
    float lastValue = 0;
    bool hasValue = false;
    unsigned long lastSentTime = 0;
};
std::vector<LastAlertState> alertStates;

LastAlertState* findAlertState(const String& topic) {
    for (auto& st : alertStates) {
        if (st.topic == topic) return &st;
    }
    return nullptr;
}

struct RuleAlertState {
    String topic;
    int ruleIndex;
    float lastValue = 0;
    String lastStrValue = ""; // Сохраняем точный текст для сравнения change
    bool hasValue = false;
    unsigned long lastSentTime = 0;
};
std::vector<RuleAlertState> ruleAlertStates;

RuleAlertState* findRuleAlertState(const String& topic, int ruleIndex) {
    for (auto& st : ruleAlertStates) {
        if (st.topic == topic && st.ruleIndex == ruleIndex) return &st;
    }
    return nullptr;
}

// ============================================================
// Вспомогательные функции
// ============================================================

bool startsWith(const char* text, const char* prefix) {
    if (text == nullptr || prefix == nullptr) {
        return false;
    }

    return strncmp(text, prefix, strlen(prefix)) == 0;
}

bool endsWith(const char* text, const char* suffix) {
    if (text == nullptr || suffix == nullptr) {
        return false;
    }

    const size_t textLen = strlen(text);
    const size_t suffixLen = strlen(suffix);

    return textLen >= suffixLen &&
           strcmp(text + textLen - suffixLen, suffix) == 0;
}

bool isCommandTopic(const char* topic) {
    return startsWith(topic, "cmnd/") ||
           endsWith(topic, "/command") ||
           endsWith(topic, "/set");
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
    // Конфигурация полей могла измениться.
    jsonFieldHistories.clear();
    // Правила могли быть удалены, изменены или переставлены.
    ruleAlertStates.clear();

    // Сохраняем уже разобранный документ для MQTT callback.
    alertWidgetsDoc = std::move(doc);


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

// Проверка и отправка оповещений в MAX для избранных топиков
// Проверка и отправка оповещений в MAX для избранных топиков
void checkAndSendMaxAlert(const char* topic, const byte* payload, unsigned int length) {
    if (!alertWidgetsDoc.is<JsonArray>()) {
        return;
    }

    String payloadStr = "";
    for (unsigned int i = 0; i < length; ++i) payloadStr += (char)payload[i];

    for (JsonObject w : alertWidgetsDoc.as<JsonArray>()) {
        String wTopic = w["topic"].as<String>();
        String baseTopic = wTopic;
        if (baseTopic.indexOf('#') >= 0) {
            baseTopic = baseTopic.substring(0, baseTopic.indexOf('#'));
            baseTopic.trim();
        }

        if (strcmp(topic, baseTopic.c_str()) != 0) continue;

        JsonArray alerts = w["alerts"].as<JsonArray>();
        if (alerts.isNull()) continue;

        String valToEval = payloadStr;
        if (wTopic.indexOf('#') >= 0) {
            String jsonKey = wTopic.substring(wTopic.indexOf('#') + 1);
            jsonKey.trim();
            JsonDocument jsonDoc;
            if (!deserializeJson(jsonDoc, payloadStr)) {
                if (jsonDoc[jsonKey].is<JsonVariant>()) {
                    valToEval = jsonDoc[jsonKey].as<String>();
                }
            }
        }

        String currentStr = valToEval;
        float currentVal = valToEval.toFloat();

        int ruleIdx = 0;
        for (JsonObject alert : alerts) {
            String alertCond = alert["cond"] | "none";
            if (alertCond == "none" || !alert["msg"].is<String>()) {
                ruleIdx++;
                continue;
            }

            float thresholdVal = alert["val"] | 0.0f;
            float thresholdMax = alert["valMax"] | 0.0f;
            String alertMsg = alert["msg"].as<String>();

            RuleAlertState* ast = findRuleAlertState(topic, ruleIdx);
            if (!ast) {
                if (ruleAlertStates.size() >= MAX_RULE_ALERT_STATES) {
                    Serial.println("[MAX ALERT] Достигнут лимит состояний правил");
                    ++ruleIdx;
                    continue;
                }

                RuleAlertState newAst;
                
                newAst.topic = topic;
                newAst.ruleIndex = ruleIdx;
                newAst.lastValue = currentVal;
                newAst.lastStrValue = currentStr;
                newAst.hasValue = true;
                newAst.lastSentTime = 0;
                ruleAlertStates.push_back(newAst);
                ast = &ruleAlertStates.back();
            }

            bool trigger = false;
            unsigned long now = millis();

            if (alertCond == "change") {
                // Для change сравниваем чистые строки без кулдауна в 60 сек, 
                // но с защитой от быстрого дребезга (не чаще чем раз в 5 сек)
                if (ast->hasValue && currentStr != ast->lastStrValue) {
                    if (now - ast->lastSentTime > 5000UL || ast->lastSentTime == 0) {
                        trigger = true;
                        ast->lastSentTime = now;
                    }
                }
            } else {
                // Для порогов (below, above, between) оставляем кулдаун в 60 сек
                bool cooldownOk = (now - ast->lastSentTime > 60000UL || ast->lastSentTime == 0);
                if (cooldownOk) {
                    if (alertCond == "below" && currentVal < thresholdVal && (ast->lastValue >= thresholdVal || !ast->hasValue)) {
                        trigger = true;
                    } else if (alertCond == "above" && currentVal > thresholdVal && (ast->lastValue <= thresholdVal || !ast->hasValue)) {
                        trigger = true;
                    } else if (alertCond == "between") {
                        bool isInside = (currentVal >= thresholdVal && currentVal <= thresholdMax);
                        bool wasInside = (ast->hasValue && ast->lastValue >= thresholdVal && ast->lastValue <= thresholdMax);
                        if (isInside && (!wasInside || !ast->hasValue)) {
                            trigger = true;
                        }
                    }

                    if (trigger) {
                        ast->lastSentTime = now;
                    }
                }
            }

            if (trigger) {
                String widgetLabel = w["label"] | "Датчик";
                String finalMsg = alertMsg;

                if (alertCond == "change") {
                    if (finalMsg.isEmpty()) {
                        finalMsg = widgetLabel + ": " + currentStr;
                    } else {
                        finalMsg = alertMsg + " — " + widgetLabel + ": " + currentStr;
                    }
                } else {
                    finalMsg = alertMsg + " (" + widgetLabel + ": " + currentStr + ")";
                }

                Serial.printf("[MAX ALERT] Правило #%d для %s сработало: %s\n", ruleIdx, topic, finalMsg.c_str());
                
                if (pendingAlerts.size() < MAX_PENDING_ALERTS) {
                    pendingAlerts.push_back(finalMsg);
                }
            }

            ast->lastValue = currentVal;
            ast->lastStrValue = currentStr;
            ast->hasValue = true;
            ruleIdx++;
        }
        break;
    }
}

// Поиск поля JSON без учёта регистра.
// Ищет также внутри вложенных объектов и массивов.
JsonVariantConst findJsonFieldRecursive(
    JsonVariantConst node,
    const char* targetKey
) {
    if (node.is<JsonObjectConst>()) {
        JsonObjectConst object = node.as<JsonObjectConst>();

        for (JsonPairConst pair : object) {
            if (strcasecmp(pair.key().c_str(), targetKey) == 0) {
                return pair.value();
            }

            JsonVariantConst found =
                findJsonFieldRecursive(pair.value(), targetKey);

            if (!found.isNull()) {
                return found;
            }
        }
    } else if (node.is<JsonArrayConst>()) {
        JsonArrayConst array = node.as<JsonArrayConst>();

        for (JsonVariantConst item : array) {
            JsonVariantConst found =
                findJsonFieldRecursive(item, targetKey);

            if (!found.isNull()) {
                return found;
            }
        }
    }

    return JsonVariantConst();
}

bool parseHistoryNumber(const char* text, float& result) {
    if (text == nullptr) {
        return false;
    }

    char* end = nullptr;
    const float value = strtof(text, &end);

    if (end == text || end == nullptr) {
        return false;
    }

    while (*end != '\0' &&
           std::isspace(static_cast<unsigned char>(*end))) {
        ++end;
    }

    if (*end != '\0' || !std::isfinite(value)) {
        return false;
    }

    result = value;
    return true;
}

JsonFieldHistory* findJsonFieldHistory(const String& topic) {
    for (JsonFieldHistory& field : jsonFieldHistories) {
        if (field.topic == topic) {
            return &field;
        }
    }

    return nullptr;
}

void recordJsonFieldHistories(
    const char* baseTopic,
    const char* payload
) {
    if (!alertWidgetsDoc.is<JsonArray>()) {
        return;
    }

    // JSON разбираем не более одного раза на MQTT-сообщение.
    JsonDocument payloadDoc;
    bool parseAttempted = false;
    bool payloadIsJson = false;

    for (JsonObject widget : alertWidgetsDoc.as<JsonArray>()) {
        const char* widgetType = widget["type"] | "";

        if (strcmp(widgetType, "sensor") != 0) {
            continue;
        }

        String configuredTopic = widget["topic"] | "";
        configuredTopic.trim();

        const int separator = configuredTopic.indexOf('#');

        if (separator < 0) {
            continue;
        }

        String widgetBase = configuredTopic.substring(0, separator);
        String jsonKey = configuredTopic.substring(separator + 1);

        widgetBase.trim();
        jsonKey.trim();

        if (widgetBase != baseTopic || jsonKey.isEmpty()) {
            continue;
        }

        if (!parseAttempted) {
            parseAttempted = true;
            payloadIsJson = !deserializeJson(payloadDoc, payload);
        }

        if (!payloadIsJson) {
            return;
        }

        JsonVariantConst value = findJsonFieldRecursive(
            payloadDoc.as<JsonVariantConst>(),
            jsonKey.c_str()
        );

        if (value.isNull() ||
            value.is<JsonObjectConst>() ||
            value.is<JsonArrayConst>() ||
            value.is<bool>()) {
            continue;
        }

        const String valueText = value.as<String>();
        float numericValue = 0;

        if (!parseHistoryNumber(valueText.c_str(), numericValue)) {
            continue;
        }

        // Используем нормализованное имя, включая поле после #.
        const String historyTopic = widgetBase + "#" + jsonKey;

        JsonFieldHistory* field = findJsonFieldHistory(historyTopic);

        if (field == nullptr) {
            if (countAllocatedHistories() >= MAX_HISTORY_TOPICS) {
                continue;
            }

            const size_t historyBytes =
                sizeof(HistoryPoint) * MAX_HISTORY_POINTS;

            if (ESP.getFreeHeap() <= historyBytes + 40000UL) {
                continue;
            }

            // Сначала выделяем память. Если выделение не удалось,
            // пустая запись в списке не останется.
            std::unique_ptr<HistoryPoint[]> buffer(
                new (std::nothrow) HistoryPoint[MAX_HISTORY_POINTS]
            );

            if (!buffer) {
                Serial.printf(
                    "[HISTORY] Нет памяти для %s\n",
                    historyTopic.c_str()
                );
                continue;
            }

            jsonFieldHistories.emplace_back();
            field = &jsonFieldHistories.back();
            field->topic = historyTopic;
            field->history = std::move(buffer);

            Serial.printf(
                "[HISTORY] Создана история: %s\n",
                field->topic.c_str()
            );
        }

        const unsigned long now = millis();

        if (field->historyCount > 0 &&
            now - field->historyLastAt < HISTORY_INTERVAL_MS) {
            continue;
        }

        size_t index;

        if (field->historyCount < MAX_HISTORY_POINTS) {
            index = (field->historyStart + field->historyCount)
                    % MAX_HISTORY_POINTS;

            ++field->historyCount;
        } else {
            index = field->historyStart;
            field->historyStart =
                (field->historyStart + 1) % MAX_HISTORY_POINTS;
        }

        field->history[index] = {numericValue, now};
        field->historyLastAt = now;
    }
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

        stateCache.emplace_back();
        state = &stateCache.back();

        state->used = true;

        strncpy(state->topic, topic, MAX_TOPIC_LENGTH);
        state->topic[MAX_TOPIC_LENGTH] = '\0';
    }

    if (length > 0) {
        memcpy(state->payload, payload, length);
    }

    state->payload[length] = '\0';
    state->updatedAt = millis();

    if (Settings::historyEnabled()) {
        // Историю записываем только для обычного числового payload.
        // JSON и текстовые значения не превращаем ошибочно в 0.
        char* end = nullptr;
        const char* start = state->payload;

        float currentVal = strtof(start, &end);

        while (end != nullptr &&
            *end != '\0' &&
            std::isspace(static_cast<unsigned char>(*end))) {
            ++end;
        }

        const bool isNumeric =
            end != start &&
            end != nullptr &&
            *end == '\0' &&
            std::isfinite(currentVal);

        const unsigned long now = millis();

        if (isNumeric) {
            // Выделяем память истории один раз.
            if (!state->history) {
                const size_t allocatedHistories = countAllocatedHistories();

                const size_t historyBytes =
                    sizeof(HistoryPoint) * MAX_HISTORY_POINTS;

                // 40 КБ — защитный запас, а не гарантия,
                // что HTTPS или другие операции успешно выделят память.
                if (allocatedHistories < MAX_HISTORY_TOPICS &&
                    ESP.getFreeHeap() > historyBytes + 40000UL) {

                    state->history.reset(
                        new (std::nothrow) HistoryPoint[MAX_HISTORY_POINTS]
                    );

                    if (!state->history) {
                        Serial.printf(
                            "[HISTORY] Не удалось выделить память для %s\n",
                            state->topic
                        );
                    }
                }
            }

            if (state->history &&
                (state->historyCount == 0 ||
                now - state->historyLastAt >= HISTORY_INTERVAL_MS)) {

                size_t index;

                if (state->historyCount < MAX_HISTORY_POINTS) {
                    index = (state->historyStart + state->historyCount)
                            % MAX_HISTORY_POINTS;

                    ++state->historyCount;
                } else {
                    // Перезаписываем самую старую точку.
                    index = state->historyStart;
                    state->historyStart =
                        (state->historyStart + 1) % MAX_HISTORY_POINTS;
                }

                state->history[index] = {currentVal, now};
                state->historyLastAt = now;
            }
        }

        // JSON для истории разбираем не чаще интервала записи истории.
        if (!state->jsonHistoryAttempted ||
            now - state->jsonHistoryLastAttempt >= HISTORY_INTERVAL_MS) {

            state->jsonHistoryAttempted = true;
            state->jsonHistoryLastAttempt = now;

            recordJsonFieldHistories(topic, state->payload);
        }
    }

    // Правила MAX пока оставляем без изменений.
    if (ENABLE_MAX_NOTIFICATIONS) {
        checkAndSendMaxAlert(topic, payload, length);
    }

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
            mainNetworkClient.stop();
            Serial.printf(
                "[MAIN MQTT] Не удалось восстановить состояние: %s\n",
                state.topic
            );
            return;
        }
    }
}

void onLocalMqttMessage(
    char* topic,
    byte* payload,
    unsigned int length
) {
    // Команды, поступившие с главного брокера, нельзя возвращать обратно.
    // Иначе /set и /command могут образовать бесконечную MQTT-петлю.
    if (isCommandTopic(topic)) {
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
            // Сообщение помещалось в буфер, значит отказ — это проблема канала.
            // Закрываем сокет, чтобы следующие сообщения не ждали таймаут.
            if (strlen(topic) + length + 7 <= MQTT_BUFFER_SIZE) {
                mainNetworkClient.stop();
            }
            Serial.printf(
                "[MAIN MQTT] Не удалось переслать %s (%u байт)\n",
                topic,
                length
            );
        }
    }

    // 2. Только избранное сохраняем в оперативную память.
    saveStateToCache(topic, payload, length);
}

void onMainMqttMessage(char* topic, byte* payload, unsigned int length) {
    // Пропускаем все телеметрические статусы и state, оставляем только команды
    if (!isCommandTopic(topic)) {
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

    // Запуск mDNS (чтобы устройство было доступно по имени cottage-gateway.local)
    if (MDNS.begin(DEVICE_HOSTNAME)) {
        Serial.printf("[mDNS] Сервер запущен. Адрес в сети: http://%s.local\n", DEVICE_HOSTNAME);
    } else {
        Serial.println("[mDNS] Ошибка запуска службы mDNS");
    }

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

    // Запуск mDNS (чтобы устройство было доступно по имени cottage-gateway.local)
    if (MDNS.begin(DEVICE_HOSTNAME)) {
        Serial.printf("[mDNS] Сервер запущен. Адрес в сети: http://%s.local\n", DEVICE_HOSTNAME);
    } else {
        Serial.println("[mDNS] Ошибка запуска службы mDNS");
    }

    return true;
}

void setupWatchdog() {
#if ESP_ARDUINO_VERSION_MAJOR >= 3
    esp_task_wdt_config_t config = {};
    config.timeout_ms = WATCHDOG_TIMEOUT_S * 1000;
    config.idle_core_mask = 0;
    config.trigger_panic = true;

    if (esp_task_wdt_reconfigure(&config) != ESP_OK) {
        esp_task_wdt_init(&config);
    }
#else
    esp_task_wdt_init(WATCHDOG_TIMEOUT_S, true);
#endif
    esp_task_wdt_add(NULL);
}

// ============================================================
// Arduino setup / loop
// ============================================================
void setup() {
    Serial.begin(115200);
    setupWatchdog();

    pinMode(LED_PIN, OUTPUT);
    writeLed(false);

    Settings::init();
    // Выделяем память под кэш один раз при старте.
    stateCache.reserve(MAX_FAVORITE_TOPICS);
    pendingAlerts.reserve(MAX_PENDING_ALERTS);

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
            Serial.println(
                "[WIFI] Таймаут первоначального подключения. "
                "Продолжаем автоматическое переподключение."
            );
            break;
        }

        delay(50);
    }

    if (WiFi.status() != WL_CONNECTED) {
        return;
    }
        
    Serial.printf(
        "[WIFI] Подключён. IP: %s\n",
        WiFi.localIP().toString().c_str()
    );
    // Отправка уведомления о старте шлюза (только один раз за загрузку)
    if (pendingAlerts.size() < MAX_PENDING_ALERTS) {
        pendingAlerts.push_back("🟢 MQTT Bridge online");
        Serial.println("[MAX ALERT] Стартовое уведомление добавлено в очередь.");
    }
}

// После неудачной отправки делаем паузу, чтобы не блокировать loop()
// повторными HTTPS-попытками при отсутствии интернета.
constexpr unsigned long MAX_FAIL_PAUSE_MS = 300000UL; // 5 минут
bool maxSendPaused = false;
unsigned long maxSendPausedAt = 0;

void processPendingAlerts() {
    if (!ENABLE_MAX_NOTIFICATIONS) {
        pendingAlerts.clear();
        return;
    }

    if (pendingAlerts.empty() ||
        isApMode ||
        WiFi.status() != WL_CONNECTED) {
        return;
    }

    const unsigned long now = millis();

    if (maxSendPaused) {
        if (now - maxSendPausedAt < MAX_FAIL_PAUSE_MS) {
            pendingAlerts.clear();
            return;
        }
        maxSendPaused = false;
    }

    if (now - lastAlertProcessTime <= 3000UL) {
        return;
    }

    String msg = pendingAlerts.front();
    pendingAlerts.erase(pendingAlerts.begin());

    if (!sendMaxNotification(msg)) {
        maxSendPaused = true;
        maxSendPausedAt = millis();
        pendingAlerts.clear();
    }

    // Интервал отсчитываем от конца запроса, а не от начала.
    lastAlertProcessTime = millis();
}

bool sendMaxNotification(const String& message) {
    SystemSettings s = Settings::get();
    if (s.maxToken.length() == 0 || s.maxUserId.length() == 0) {
        Serial.println("[MAX] Токен или User ID не настроены");
        return false;
    }

    const size_t freeInternal =
        heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);

    const size_t largestInternal =
        heap_caps_get_largest_free_block(
            MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT
        );

    if (freeInternal < 50000 || largestInternal < 16000) {
        Serial.printf(
            "[MAX] Отправка пропущена: internal=%u, largest=%u\n",
            static_cast<unsigned>(freeInternal),
            static_cast<unsigned>(largestInternal)
        );
        return false;
    }

    bool success = false;

    // Локальный блок видимости: вся SSL-память выделяется при входе 
    // и гарантированно полностью освобождается при выходе из скобок { }
    {
        WiFiClientSecure secureClient;
        secureClient.setInsecure(); // Пропуск проверки сертификата
        secureClient.setHandshakeTimeout(5);

        HTTPClient https;
        https.setConnectTimeout(5000);
        https.setTimeout(5000);
        String url = "https://platform-api.max.ru/messages?user_id=" + s.maxUserId;

        Serial.printf("[MAX] Отправка запроса (свободно RAM: %u)...\n", ESP.getFreeHeap());

        if (https.begin(secureClient, url)) {
            https.addHeader("Authorization", s.maxToken);
            https.addHeader("Content-Type", "application/json");

            JsonDocument doc;
            doc["text"] = message;
            String payload;
            serializeJson(doc, payload);

            int httpCode = https.POST(payload);
            success = (httpCode > 0 && httpCode == 200);

            if (success) {
                Serial.println("[MAX] Сообщение успешно отправлено!");
            } else {
                Serial.printf("[MAX] Ошибка отправки! HTTP код: %d\n", httpCode);
            }

            https.end();
        } else {
            Serial.println("[MAX] Не удалось инициализировать HTTPS-клиент");
        }
    } // Деструктор WiFiClientSecure очищает heap-память здесь

    return success;
}

void publishGatewayHealth() {
    if (!mainMqtt.connected()) {
        return;
    }

    static unsigned long lastPublish = 0;
    const unsigned long now = millis();

    if (now - lastPublish < 60000UL) {
        return;
    }

    lastPublish = now;

    const size_t freeInternal = heap_caps_get_free_size(
        MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT
    );

    const size_t largestInternal = heap_caps_get_largest_free_block(
        MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT
    );

    char payload[240];

    const int written = snprintf(
        payload,
        sizeof(payload),
        "{\"uptime_s\":%lu,"
        "\"free_heap\":%u,"
        "\"min_free_heap\":%u,"
        "\"free_internal\":%u,"
        "\"largest_internal\":%u,"
        "\"reset_reason\":%d,"
        "\"local_mqtt\":%s}",
        now / 1000UL,
        static_cast<unsigned>(ESP.getFreeHeap()),
        static_cast<unsigned>(ESP.getMinFreeHeap()),
        static_cast<unsigned>(freeInternal),
        static_cast<unsigned>(largestInternal),
        static_cast<int>(esp_reset_reason()),
        localMqtt.connected() ? "true" : "false"
    );

    if (written <= 0 ||
        static_cast<size_t>(written) >= sizeof(payload)) {
        return;
    }

    // Без retain: старое состояние не будет выдаваться как актуальное.
    mainMqtt.publish(
        "cottage/gateway/health",
        payload,
        false
    );
}

void loop() {
    esp_task_wdt_reset();
    // Индикатор работает также при отсутствии Wi-Fi и в режиме AP.
    updateLedIndicator();
    processPendingAlerts(); // Фоновая отправка оповещений MAX без блокировки MQTT

    if (isApMode) {
        // Это необходимо, даже когда WiFi.status() не WL_CONNECTED.
        WebDashboard::handleClient();
        delay(5);
        return;
    }

    // Если автоподключение Wi-Fi не восстановилось за 15 минут,
    // перезапускаем ESP32 и начинаем подключение заново.
    static bool wifiDisconnected = false;
    static unsigned long wifiDisconnectedSince = 0;

    if (WiFi.status() != WL_CONNECTED) {
        const unsigned long now = millis();

        if (!wifiDisconnected) {
            wifiDisconnected = true;
            wifiDisconnectedSince = now;
        }

        if (now - wifiDisconnectedSince >= 15UL * 60UL * 1000UL) {
            ESP.restart();
        }

        delay(100);
        return;
    }

    // Wi-Fi восстановился: сбрасываем таймер.
    wifiDisconnected = false;

    if (!servicesStarted) {
        startServices();

        if (!servicesStarted) {
            delay(1000);
            return;
        }
    }

    WebDashboard::handleClient();

    // Обновляем конфигурацию виджетов не чаще раза в секунду.
    static unsigned long lastWidgetsCheck = 0;
    const unsigned long widgetsCheckNow = millis();

    if (widgetsCheckNow - lastWidgetsCheck >= 1000UL) {
        lastWidgetsCheck = widgetsCheckNow;
        refreshFavoriteTopics();
    }

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

    publishGatewayHealth();

    // Отслеживание изменения статуса главного MQTT-брокера с защитой от ложных срабатываний
    static unsigned long disconnectStartTime = 0;
    static bool alertSentOffline = false;
    bool currentMainStatus = mainMqtt.connected();

    if (currentMainStatus) {
        // Если связь есть: сбрасываем таймер обрыва
        disconnectStartTime = 0;
        
        // Если до этого уходило уведомление об офлайне, сообщаем о восстановлении
        if (!lastMainMqttStatus) {
            lastMainMqttStatus = true;
            if (alertSentOffline) {
                Serial.println("[MAX ALERT] Главный MQTT-сервер: связь восстановлена!");
                if (pendingAlerts.size() < MAX_PENDING_ALERTS) {
                    pendingAlerts.push_back("🟢 Связь с главным MQTT-сервером восстановлена!");
                }
                alertSentOffline = false;
            }
        }
    } else {
        // Связь пропала
        if (lastMainMqttStatus) {
            if (disconnectStartTime == 0) {
                disconnectStartTime = millis(); // Запоминаем момент начала обрыва
            } else if (millis() - disconnectStartTime > 30000UL) { 
                // Считаем связь потерянной ТОЛЬКО если она отсутствует дольше 30 секунд подряд
                lastMainMqttStatus = false;
                if (WiFi.status() == WL_CONNECTED && servicesStarted && !alertSentOffline) {
                    Serial.println("[MAX ALERT] Главный MQTT-сервер: связь потеряна!");
                    if (pendingAlerts.size() < MAX_PENDING_ALERTS) {
                        pendingAlerts.push_back("🔴 Внимание: потеряна связь с главным MQTT-сервером!");
                    }
                    alertSentOffline = true;
                }
            }
        }
    }
    
    delay(5);
}
