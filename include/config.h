#pragma once

#include <stddef.h>
#include <stdint.h>

constexpr const char* DEFAULT_WIFI_SSID = "";
constexpr const char* DEFAULT_WIFI_PASSWORD = "";
constexpr const char* DEVICE_HOSTNAME = "cottage-gateway";

constexpr const char* AP_SSID = "CottageGateway-Setup";
constexpr const char* AP_PASSWORD = "";

constexpr unsigned long WIFI_CONNECT_TIMEOUT_MS = 20000UL;

constexpr const char* DEFAULT_MQTT_HOST = "";
constexpr uint16_t DEFAULT_MQTT_PORT = 1883;
constexpr const char* DEFAULT_MQTT_USER = "";
constexpr const char* DEFAULT_MQTT_PASSWORD = "";

constexpr uint16_t LOCAL_MQTT_PORT = 1883;
constexpr const char* LOCAL_CLIENT_ID = "cottage_gateway_local";
constexpr const char* MAIN_CLIENT_ID = "cottage_gateway_remote";

constexpr const char* LOCAL_SUBSCRIPTION = "#";
constexpr const char* MAIN_COMMAND_SUBSCRIPTION = "#";

constexpr const char* GATEWAY_AVAILABILITY_TOPIC =
    "cottage/gateway/availability";

constexpr size_t MAX_TOPIC_LENGTH = 128;
constexpr size_t MAX_PAYLOAD_LENGTH = 192;
constexpr size_t MAX_CACHED_STATES = 48;
constexpr size_t MAX_WIDGETS_JSON_BYTES = 8192;

constexpr size_t MAX_COMMAND_PAYLOAD_LENGTH = 512;
constexpr uint16_t MQTT_BUFFER_SIZE = 1024;

constexpr unsigned long MQTT_RECONNECT_INTERVAL_MS = 10000UL;

constexpr const char* FIRMWARE_VERSION = "v1.1.4";