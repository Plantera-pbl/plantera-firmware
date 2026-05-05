#pragma once

#include <Arduino.h>

// Copy this file to include/plantera_config.h for local credentials or pin changes.
// The real plantera_config.h file is gitignored so WiFi secrets are not committed.

constexpr char DEVICE_ID[] = "plantera-pot-001";

constexpr char WIFI_SSID[] = "";
constexpr char WIFI_PASSWORD[] = "";

// Example: "http://192.168.1.50:3000/api/devices/plantera-pot-001".
// Leave empty to keep the firmware in local autonomous mode.
constexpr char SERVER_BASE_URL[] = "";

// ESP32-C3 SuperMini pin defaults. Change these if your wiring differs.
constexpr uint8_t DHT_PIN = 6;
constexpr uint8_t DHT_TYPE = DHT22;
constexpr uint8_t LIGHT_SENSOR_PIN = 0;
constexpr uint8_t SOIL_SENSOR_PIN = 1;
constexpr uint8_t PUMP_PIN = 7;
constexpr uint8_t GROW_LIGHT_PIN = 8;
constexpr uint8_t FAN_PIN = 10;

constexpr uint8_t ACTUATOR_ON_LEVEL = HIGH;
constexpr uint8_t ACTUATOR_OFF_LEVEL = LOW;

constexpr int ADC_MAX_VALUE = 4095;
constexpr int LIGHT_DARK_RAW = 4095;
constexpr int LIGHT_BRIGHT_RAW = 0;
constexpr int SOIL_DRY_RAW = 3000;
constexpr int SOIL_WET_RAW = 1200;

constexpr float AUTO_SOIL_LOW_PERCENT = 35.0f;
constexpr float AUTO_LIGHT_LOW_PERCENT = 30.0f;
constexpr float AUTO_FAN_TEMP_ON_C = 30.0f;
constexpr float AUTO_FAN_TEMP_OFF_C = 28.0f;
constexpr float AUTO_FAN_HUMIDITY_ON_PERCENT = 78.0f;
constexpr float AUTO_FAN_HUMIDITY_OFF_PERCENT = 70.0f;

constexpr unsigned long SENSOR_READ_INTERVAL_MS = 2000;
constexpr unsigned long CONTROL_INTERVAL_MS = 250;
constexpr unsigned long TELEMETRY_INTERVAL_MS = 30000;
constexpr unsigned long COMMAND_POLL_INTERVAL_MS = 5000;
constexpr unsigned long WIFI_RETRY_INTERVAL_MS = 10000;
constexpr unsigned long SERVER_STALE_MS = 120000;

constexpr unsigned long PUMP_DEFAULT_ON_MS = 3000;
constexpr unsigned long PUMP_MAX_ON_MS = 8000;
constexpr unsigned long AUTO_WATER_COOLDOWN_MS = 10UL * 60UL * 1000UL;
