#pragma once

// Copy to include/plantera_config.h and fill in local credentials.
// plantera_config.h is gitignored so secrets are not committed.

constexpr char WIFI_SSID[] = "";
constexpr char WIFI_PASSWORD[] = "";

struct WifiNetwork {
  const char *ssid;
  const char *password;
};

constexpr WifiNetwork WIFI_NETWORKS[] = {
  {WIFI_SSID, WIFI_PASSWORD},
};

constexpr size_t WIFI_NETWORK_COUNT = sizeof(WIFI_NETWORKS) / sizeof(WIFI_NETWORKS[0]);

constexpr char MQTT_HOST[] = "";
constexpr uint16_t MQTT_PORT = 8883;
constexpr char MQTT_USERNAME[] = "";
constexpr char MQTT_PASSWORD[] = "";
constexpr char MQTT_CLIENT_ID[] = "esp32-c3-plant-1";
constexpr char MQTT_TOPIC[] = "iot/devices/4/data";
constexpr char MQTT_CONFIG_TOPIC[] = "iot/devices/4/config";
