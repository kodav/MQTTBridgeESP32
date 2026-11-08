# Cottage MQTT Gateway

An autonomous MQTT gateway for ESP32-based cottage installations.

The gateway provides a local MQTT broker for devices on the cottage network, a lightweight web dashboard, and an MQTT client connection to a remote broker such as the one used by Home Assistant. Local devices can continue communicating with the gateway when the remote broker is unavailable.

## Features

- Local MQTT broker running on the ESP32.
- Web dashboard for monitoring selected sensor values and controlling devices.
- Configurable dashboard widgets: sensors and switches.
- Optional JSON-field extraction for sensor states using a `#` suffix in the widget state topic.
- MQTT telemetry forwarding from the local broker to the remote broker when connected.
- Forwarding of remote `cmnd/#` commands to the local broker.
- RAM cache limited to selected dashboard topics.
- Wi-Fi and remote MQTT settings stored in ESP32 NVS using `Preferences`.
- Intended Wi-Fi setup access point for initial configuration.

## Architecture

```text
Local devices
(Tasmota, ESPHome, other MQTT clients)
              |
              | MQTT
              v
      ESP32 local broker
          /        \
         /          \
 Web dashboard    MQTT gateway client
                        |
                        | MQTT, when reachable
                        v
              Remote MQTT broker
                 (Home Assistant)