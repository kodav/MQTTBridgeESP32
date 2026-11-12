#pragma once

#include <stdint.h>
#include <stddef.h>

// Значения по умолчанию (используются при первом запуске или сбросе)
constexpr const char* DEFAULT_WIFI_SSID = "";
constexpr const char* DEFAULT_WIFI_PASSWORD = "";
constexpr const char* DEVICE_HOSTNAME = "cottage-gateway";

// Параметры режима настройки (AP)
constexpr const char* AP_SSID = "CottageGateway-Setup";
constexpr const char* AP_PASSWORD = ""; // без пароля или укажите свой
constexpr unsigned long WIFI_CONNECT_TIMEOUT_MS = 20000; // 20 секунд на попытку подключения

constexpr const char* DEFAULT_MQTT_HOST = "";
constexpr uint16_t DEFAULT_MQTT_PORT = 1883;
constexpr const char* DEFAULT_MQTT_USER = "";
constexpr const char* DEFAULT_MQTT_PASSWORD = "";

// Системные параметры
constexpr uint16_t LOCAL_MQTT_PORT = 1883;
constexpr const char* LOCAL_CLIENT_ID = "cottage_gateway_local";
constexpr const char* MAIN_CLIENT_ID = "cottage_gateway_remote";

constexpr const char* LOCAL_SUBSCRIPTION = "#";
constexpr const char* MAIN_COMMAND_SUBSCRIPTION = "#";
constexpr const char* GATEWAY_AVAILABILITY_TOPIC = "cottage/gateway/availability";

constexpr uint16_t MAX_TOPIC_LENGTH = 128;
constexpr uint16_t MAX_PAYLOAD_LENGTH = 192;
constexpr uint16_t MQTT_BUFFER_SIZE = 1024;

constexpr unsigned long MQTT_RECONNECT_INTERVAL_MS = 10000;

// Встроенный светодиод на большинстве плат ESP32 (NodeMCU / DevKit) — это GPIO 2
constexpr int LED_PIN = 2;

constexpr const char* DEFAULT_MAX_TOKEN = "";
constexpr const char* DEFAULT_MAX_USER_ID = "";

// История: одна точка в минуту, до 6 часов.
constexpr unsigned long HISTORY_INTERVAL_MS = 60000UL;
constexpr size_t MAX_HISTORY_POINTS = 360;

// Максимум топиков, которым выделяется буфер истории.
// Остальные виджеты работают без графика.
constexpr size_t MAX_HISTORY_TOPICS = 12;

constexpr const char* FIRMWARE_VERSION = "v1.1.0";