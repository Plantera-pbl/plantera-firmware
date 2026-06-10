#include <Arduino.h>
#include <ArduinoJson.h>
#include <DHT.h>
#include <PubSubClient.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <time.h>

#if __has_include("plantera_config.h")
#include "plantera_config.h"
#else
#include "plantera_config.example.h"
#endif

constexpr uint8_t DHT_PIN = 6;
constexpr uint8_t LIGHT_PIN = 0;
constexpr uint8_t SOIL_MOISTURE_PIN = 1;
constexpr uint8_t SOIL_MOISTURE_POWER_PIN = 3;
constexpr uint8_t PUMP_PIN = 4;
constexpr uint8_t FAN_PIN = 5;
constexpr uint8_t LED_BOARD_PIN = 7;
constexpr uint8_t DHT_TYPE = DHT22;
constexpr uint8_t FAN_ON_LEVEL = HIGH;
constexpr uint8_t FAN_OFF_LEVEL = LOW;
constexpr uint8_t LED_BOARD_ON_LEVEL = HIGH;
constexpr uint8_t LED_BOARD_OFF_LEVEL = LOW;

constexpr unsigned long DISPLAY_INTERVAL_MS = 500;
constexpr unsigned long DHT_INTERVAL_MS = 2000;
constexpr unsigned long SOIL_MOISTURE_INTERVAL_MS = 5000;
constexpr unsigned long SOIL_MOISTURE_SETTLE_MS = 1000;
constexpr unsigned long MQTT_PUBLISH_INTERVAL_MS = 5000;
constexpr unsigned long WIFI_CONNECT_TIMEOUT_MS = 30000;
constexpr unsigned long MQTT_CONNECT_TIMEOUT_MS = 10000;
constexpr uint16_t MQTT_BUFFER_SIZE = 1536;
constexpr int MAX_NON_WORKING_WINDOWS = 6;
constexpr int PUMP_PWM_CHANNEL = 0;
constexpr int PUMP_PWM_FREQUENCY = 20000;
constexpr int PUMP_PWM_RESOLUTION_BITS = 8;
constexpr int PUMP_PWM_RUN_DUTY = 255; // Full power for pump testing.
constexpr int PUMP_PWM_RAMP_STEP = 16;
constexpr int PUMP_PWM_RAMP_DELAY_MS = 40;
constexpr int ADC_MAX = 4095;
constexpr int LIGHT_SAMPLES = 8;
constexpr int SOIL_MOISTURE_SAMPLES = 10;
constexpr int SOIL_STABLE_DELTA_RAW = 61; // About 1.5% of the 12-bit ADC range.
constexpr int SOIL_STABLE_REQUIRED_READINGS = 3;
constexpr unsigned long SENSOR_WARMUP_MS = 15000;
constexpr int SENSOR_MIN_VALID_SAMPLES = 3;
constexpr float SENSOR_SMOOTHING_ALPHA = 0.35f;
constexpr float SENSOR_MAX_PERCENT_STEP = 35.0f;
constexpr float SOIL_MIN_TRUSTED_PERCENT = 1.0f;
constexpr int SOIL_MIN_VALID_RAW = 20;
constexpr int SOIL_MAX_VALID_RAW = ADC_MAX - 20;
constexpr float DHT_MIN_TEMPERATURE_C = -20.0f;
constexpr float DHT_MAX_TEMPERATURE_C = 80.0f;
constexpr float DHT_MIN_HUMIDITY_PERCENT = 0.0f;
constexpr float DHT_MAX_HUMIDITY_PERCENT = 100.0f;
constexpr float PUMP_SOIL_THRESHOLD_PERCENT = 40.0f;
constexpr unsigned long PUMP_WATERING_BURST_MS = 3000;
constexpr unsigned long PUMP_MIN_OFF_MS = 60UL * 1000UL;
constexpr unsigned long PUMP_COOLDOWN_MS = 10UL * 60UL * 1000UL;
constexpr float FAN_HUMIDITY_THRESHOLD_PERCENT = 60.0f;
constexpr float LED_BOARD_LIGHT_THRESHOLD_PERCENT = 90.0f;

struct TimeWindow {
  int startMinute = 0;
  int endMinute = 0;
};

struct RuntimeConfig {
  bool deviceEnabled = true;
  float wateringMoistureThresholdOn = PUMP_SOIL_THRESHOLD_PERCENT;
  float wateringMoistureThresholdOff = 50.0f;
  float fanHumidityThresholdOn = FAN_HUMIDITY_THRESHOLD_PERCENT;
  float fanHumidityThresholdOff = FAN_HUMIDITY_THRESHOLD_PERCENT;
  float lightIntensityThresholdOn = LED_BOARD_LIGHT_THRESHOLD_PERCENT;
  float lightIntensityThresholdOff = LED_BOARD_LIGHT_THRESHOLD_PERCENT;
  unsigned long wateringDurationMs = PUMP_WATERING_BURST_MS;
  unsigned long wateringCooldownMs = PUMP_COOLDOWN_MS;
  int timezoneOffsetMinutes = 0;
  TimeWindow nonWorkingWindows[MAX_NON_WORKING_WINDOWS];
  int nonWorkingWindowCount = 0;
};

struct SensorValueState {
  float smoothedValue = NAN;
  float lastAcceptedValue = NAN;
  uint16_t acceptedSamples = 0;
  bool valid = false;
  const char *status = "warming_up";
};

DHT dht(DHT_PIN, DHT_TYPE);
WiFiClientSecure wifiClient;
PubSubClient mqtt(wifiClient);
RuntimeConfig runtimeConfig;
size_t wifiNetworkIndex = 0;
unsigned long lastDisplayMs = 0;
unsigned long lastDhtMs = 0;
unsigned long lastSoilMoistureMs = 0;
unsigned long lastMqttPublishMs = 0;
unsigned long pumpStartedMs = 0;
unsigned long pumpRunDurationMs = 0;
unsigned long lastPumpStoppedMs = 0;
unsigned long lastWateringMs = 0;
int soilMoistureRaw = -1;
int lightRaw = -1;
int previousSoilMoistureRaw = -1;
int soilStableReadings = 0;
bool soilStable = false;
float humidity = NAN;
float temperatureC = NAN;
float lightPercent = NAN;
float soilMoisturePercent = NAN;
SensorValueState temperatureState;
SensorValueState humidityState;
SensorValueState lightState;
SensorValueState soilState;
bool pumpRunning = false;
bool fanRunning = false;
bool ledBoardRunning = false;
const char *pumpSafetyStatus = "ready";

float soilMoisturePercentFromRaw(int reading) {
  return 100.0f - ((reading * 100.0f) / ADC_MAX);
}

bool sensorWarmupComplete(unsigned long now) {
  return now >= SENSOR_WARMUP_MS;
}

bool timeElapsed(unsigned long now, unsigned long since, unsigned long interval) {
  return since != 0 && now - since >= interval;
}

bool boolFromJson(JsonVariantConst value, bool fallback) {
  if (value.isNull()) {
    return fallback;
  }
  if (value.is<bool>()) {
    return value.as<bool>();
  }
  if (value.is<int>()) {
    return value.as<int>() != 0;
  }

  const char *text = value.as<const char *>();
  if (text == nullptr) {
    return fallback;
  }

  String normalized = text;
  normalized.trim();
  normalized.toLowerCase();
  if (normalized == "1" || normalized == "true" || normalized == "on" || normalized == "enabled") {
    return true;
  }
  if (normalized == "0" || normalized == "false" || normalized == "off" || normalized == "disabled") {
    return false;
  }
  return fallback;
}

float floatFromJson(JsonVariantConst root, const char *key, float fallback) {
  JsonVariantConst value = root[key];
  return value.isNull() ? fallback : value.as<float>();
}

unsigned long secondsFromJson(JsonVariantConst root, const char *key, unsigned long fallbackMs) {
  JsonVariantConst value = root[key];
  if (value.isNull()) {
    return fallbackMs;
  }
  return static_cast<unsigned long>(max(value.as<float>(), 0.0f) * 1000.0f);
}

unsigned long minutesFromJson(JsonVariantConst root, const char *key, unsigned long fallbackMs) {
  JsonVariantConst value = root[key];
  if (value.isNull()) {
    return fallbackMs;
  }
  return static_cast<unsigned long>(max(value.as<float>(), 0.0f) * 60.0f * 1000.0f);
}

int parseMinuteOfDay(const char *text) {
  if (text == nullptr) {
    return -1;
  }

  int hour = -1;
  int minute = -1;
  if (sscanf(text, "%d:%d", &hour, &minute) != 2) {
    return -1;
  }
  if (hour < 0 || hour > 23 || minute < 0 || minute > 59) {
    return -1;
  }
  return hour * 60 + minute;
}

bool minuteInWindow(int minuteOfDay, const TimeWindow &window) {
  if (window.startMinute == window.endMinute) {
    return false;
  }
  if (window.startMinute < window.endMinute) {
    return minuteOfDay >= window.startMinute && minuteOfDay < window.endMinute;
  }
  return minuteOfDay >= window.startMinute || minuteOfDay < window.endMinute;
}

int localMinuteOfDay() {
  time_t nowUtc = time(nullptr);
  if (nowUtc < 24 * 60 * 60) {
    return -1;
  }

  long localSeconds = static_cast<long>(nowUtc) + static_cast<long>(runtimeConfig.timezoneOffsetMinutes) * 60L;
  long minute = (localSeconds / 60L) % (24L * 60L);
  if (minute < 0) {
    minute += 24L * 60L;
  }
  return static_cast<int>(minute);
}

bool inNonWorkingWindow() {
  const int minute = localMinuteOfDay();
  if (minute < 0) {
    return false;
  }

  for (int i = 0; i < runtimeConfig.nonWorkingWindowCount; i++) {
    if (minuteInWindow(minute, runtimeConfig.nonWorkingWindows[i])) {
      return true;
    }
  }
  return false;
}

void loadNonWorkingWindows(JsonVariantConst value) {
  runtimeConfig.nonWorkingWindowCount = 0;
  if (!value.is<JsonArrayConst>()) {
    return;
  }

  for (JsonVariantConst item : value.as<JsonArrayConst>()) {
    if (runtimeConfig.nonWorkingWindowCount >= MAX_NON_WORKING_WINDOWS) {
      break;
    }

    const char *start = item["start"] | item["from"] | item["start_time"] | nullptr;
    const char *end = item["end"] | item["to"] | item["end_time"] | nullptr;

    String windowText;
    if (start == nullptr && end == nullptr && item.is<const char *>()) {
      windowText = item.as<const char *>();
      const int separator = windowText.indexOf('-');
      if (separator > 0) {
        windowText.setCharAt(separator, '\0');
        start = windowText.c_str();
        end = windowText.c_str() + separator + 1;
      }
    }

    const int startMinute = parseMinuteOfDay(start);
    const int endMinute = parseMinuteOfDay(end);
    if (startMinute < 0 || endMinute < 0) {
      continue;
    }

    runtimeConfig.nonWorkingWindows[runtimeConfig.nonWorkingWindowCount].startMinute = startMinute;
    runtimeConfig.nonWorkingWindows[runtimeConfig.nonWorkingWindowCount].endMinute = endMinute;
    runtimeConfig.nonWorkingWindowCount++;
  }
}

JsonVariantConst firstPresent(JsonVariantConst root, const char *keyA, const char *keyB) {
  JsonVariantConst value = root[keyA];
  return value.isNull() ? root[keyB] : value;
}

void applyConfigPayload(const char *payload, size_t length) {
  JsonDocument doc;
  const DeserializationError error = deserializeJson(doc, payload, length);
  if (error) {
    Serial.print("Config JSON parse failed: ");
    Serial.println(error.c_str());
    return;
  }

  JsonVariantConst root = doc.as<JsonVariantConst>();
  JsonVariantConst watering = root["watering"];
  JsonVariantConst fans = root["fans"];
  JsonVariantConst lights = root["lights"];

  runtimeConfig.deviceEnabled = boolFromJson(firstPresent(root, "device_state", "deviceState"), runtimeConfig.deviceEnabled);
  runtimeConfig.deviceEnabled = boolFromJson(root["enabled"], runtimeConfig.deviceEnabled);

  runtimeConfig.wateringCooldownMs = minutesFromJson(root, "watering_cooldown", runtimeConfig.wateringCooldownMs);
  runtimeConfig.wateringCooldownMs = minutesFromJson(root, "wateringCooldown", runtimeConfig.wateringCooldownMs);
  runtimeConfig.wateringCooldownMs = minutesFromJson(watering, "cooldown_minutes", runtimeConfig.wateringCooldownMs);
  runtimeConfig.wateringCooldownMs = minutesFromJson(watering, "cooldownMinutes", runtimeConfig.wateringCooldownMs);

  runtimeConfig.wateringMoistureThresholdOn = floatFromJson(root, "watering_moisture_threshold_on", runtimeConfig.wateringMoistureThresholdOn);
  runtimeConfig.wateringMoistureThresholdOn = floatFromJson(root, "wateringMoistureThresholdOn", runtimeConfig.wateringMoistureThresholdOn);
  runtimeConfig.wateringMoistureThresholdOn = floatFromJson(root, "watering_moisture_on", runtimeConfig.wateringMoistureThresholdOn);
  runtimeConfig.wateringMoistureThresholdOn = floatFromJson(root, "wateringMoistureOn", runtimeConfig.wateringMoistureThresholdOn);
  runtimeConfig.wateringMoistureThresholdOn = floatFromJson(watering, "moisture_threshold_on", runtimeConfig.wateringMoistureThresholdOn);
  runtimeConfig.wateringMoistureThresholdOn = floatFromJson(watering, "moistureThresholdOn", runtimeConfig.wateringMoistureThresholdOn);
  runtimeConfig.wateringMoistureThresholdOn = floatFromJson(watering, "moisture_on", runtimeConfig.wateringMoistureThresholdOn);
  runtimeConfig.wateringMoistureThresholdOn = floatFromJson(watering, "moistureOn", runtimeConfig.wateringMoistureThresholdOn);

  runtimeConfig.wateringMoistureThresholdOff = floatFromJson(root, "watering_moisture_threshold_off", runtimeConfig.wateringMoistureThresholdOff);
  runtimeConfig.wateringMoistureThresholdOff = floatFromJson(root, "wateringMoistureThresholdOff", runtimeConfig.wateringMoistureThresholdOff);
  runtimeConfig.wateringMoistureThresholdOff = floatFromJson(root, "watering_moisture_off", runtimeConfig.wateringMoistureThresholdOff);
  runtimeConfig.wateringMoistureThresholdOff = floatFromJson(root, "wateringMoistureOff", runtimeConfig.wateringMoistureThresholdOff);
  runtimeConfig.wateringMoistureThresholdOff = floatFromJson(watering, "moisture_threshold_off", runtimeConfig.wateringMoistureThresholdOff);
  runtimeConfig.wateringMoistureThresholdOff = floatFromJson(watering, "moistureThresholdOff", runtimeConfig.wateringMoistureThresholdOff);
  runtimeConfig.wateringMoistureThresholdOff = floatFromJson(watering, "moisture_off", runtimeConfig.wateringMoistureThresholdOff);
  runtimeConfig.wateringMoistureThresholdOff = floatFromJson(watering, "moistureOff", runtimeConfig.wateringMoistureThresholdOff);

  runtimeConfig.fanHumidityThresholdOn = floatFromJson(root, "fan_humidity_threshold_on", runtimeConfig.fanHumidityThresholdOn);
  runtimeConfig.fanHumidityThresholdOn = floatFromJson(root, "fanHumidityThresholdOn", runtimeConfig.fanHumidityThresholdOn);
  runtimeConfig.fanHumidityThresholdOn = floatFromJson(root, "fan_humidity_on", runtimeConfig.fanHumidityThresholdOn);
  runtimeConfig.fanHumidityThresholdOn = floatFromJson(root, "fanHumidityOn", runtimeConfig.fanHumidityThresholdOn);
  runtimeConfig.fanHumidityThresholdOn = floatFromJson(fans, "humidity_threshold_on", runtimeConfig.fanHumidityThresholdOn);
  runtimeConfig.fanHumidityThresholdOn = floatFromJson(fans, "humidityThresholdOn", runtimeConfig.fanHumidityThresholdOn);
  runtimeConfig.fanHumidityThresholdOn = floatFromJson(fans, "humidity_on", runtimeConfig.fanHumidityThresholdOn);
  runtimeConfig.fanHumidityThresholdOn = floatFromJson(fans, "humidityOn", runtimeConfig.fanHumidityThresholdOn);

  runtimeConfig.fanHumidityThresholdOff = floatFromJson(root, "fan_humidity_threshold_off", runtimeConfig.fanHumidityThresholdOff);
  runtimeConfig.fanHumidityThresholdOff = floatFromJson(root, "fanHumidityThresholdOff", runtimeConfig.fanHumidityThresholdOff);
  runtimeConfig.fanHumidityThresholdOff = floatFromJson(root, "fan_humidity_off", runtimeConfig.fanHumidityThresholdOff);
  runtimeConfig.fanHumidityThresholdOff = floatFromJson(root, "fanHumidityOff", runtimeConfig.fanHumidityThresholdOff);
  runtimeConfig.fanHumidityThresholdOff = floatFromJson(fans, "humidity_threshold_off", runtimeConfig.fanHumidityThresholdOff);
  runtimeConfig.fanHumidityThresholdOff = floatFromJson(fans, "humidityThresholdOff", runtimeConfig.fanHumidityThresholdOff);
  runtimeConfig.fanHumidityThresholdOff = floatFromJson(fans, "humidity_off", runtimeConfig.fanHumidityThresholdOff);
  runtimeConfig.fanHumidityThresholdOff = floatFromJson(fans, "humidityOff", runtimeConfig.fanHumidityThresholdOff);

  runtimeConfig.lightIntensityThresholdOn = floatFromJson(root, "light_intensity_threshold_on", runtimeConfig.lightIntensityThresholdOn);
  runtimeConfig.lightIntensityThresholdOn = floatFromJson(root, "lightIntensityThresholdOn", runtimeConfig.lightIntensityThresholdOn);
  runtimeConfig.lightIntensityThresholdOn = floatFromJson(root, "light_intensity_on", runtimeConfig.lightIntensityThresholdOn);
  runtimeConfig.lightIntensityThresholdOn = floatFromJson(root, "lightIntensityOn", runtimeConfig.lightIntensityThresholdOn);
  runtimeConfig.lightIntensityThresholdOn = floatFromJson(lights, "intensity_threshold_on", runtimeConfig.lightIntensityThresholdOn);
  runtimeConfig.lightIntensityThresholdOn = floatFromJson(lights, "intensityThresholdOn", runtimeConfig.lightIntensityThresholdOn);
  runtimeConfig.lightIntensityThresholdOn = floatFromJson(lights, "intensity_on", runtimeConfig.lightIntensityThresholdOn);
  runtimeConfig.lightIntensityThresholdOn = floatFromJson(lights, "intensityOn", runtimeConfig.lightIntensityThresholdOn);

  runtimeConfig.lightIntensityThresholdOff = floatFromJson(root, "light_intensity_threshold_off", runtimeConfig.lightIntensityThresholdOff);
  runtimeConfig.lightIntensityThresholdOff = floatFromJson(root, "lightIntensityThresholdOff", runtimeConfig.lightIntensityThresholdOff);
  runtimeConfig.lightIntensityThresholdOff = floatFromJson(root, "light_intensity_off", runtimeConfig.lightIntensityThresholdOff);
  runtimeConfig.lightIntensityThresholdOff = floatFromJson(root, "lightIntensityOff", runtimeConfig.lightIntensityThresholdOff);
  runtimeConfig.lightIntensityThresholdOff = floatFromJson(lights, "intensity_threshold_off", runtimeConfig.lightIntensityThresholdOff);
  runtimeConfig.lightIntensityThresholdOff = floatFromJson(lights, "intensityThresholdOff", runtimeConfig.lightIntensityThresholdOff);
  runtimeConfig.lightIntensityThresholdOff = floatFromJson(lights, "intensity_off", runtimeConfig.lightIntensityThresholdOff);
  runtimeConfig.lightIntensityThresholdOff = floatFromJson(lights, "intensityOff", runtimeConfig.lightIntensityThresholdOff);

  runtimeConfig.wateringDurationMs = secondsFromJson(root, "watering_duration", runtimeConfig.wateringDurationMs);
  runtimeConfig.wateringDurationMs = secondsFromJson(root, "wateringDuration", runtimeConfig.wateringDurationMs);
  runtimeConfig.wateringDurationMs = secondsFromJson(watering, "duration_seconds", runtimeConfig.wateringDurationMs);
  runtimeConfig.wateringDurationMs = secondsFromJson(watering, "durationSeconds", runtimeConfig.wateringDurationMs);

  runtimeConfig.timezoneOffsetMinutes = firstPresent(root, "timezone_offset", "timezoneOffset").isNull()
    ? runtimeConfig.timezoneOffsetMinutes
    : firstPresent(root, "timezone_offset", "timezoneOffset").as<int>();
  runtimeConfig.timezoneOffsetMinutes = firstPresent(root, "timezone_offset_minutes", "timezoneOffsetMinutes").isNull()
    ? runtimeConfig.timezoneOffsetMinutes
    : firstPresent(root, "timezone_offset_minutes", "timezoneOffsetMinutes").as<int>();

  JsonVariantConst windows = firstPresent(root, "non_working_time_windows", "nonWorkingTimeWindows");
  if (windows.isNull()) {
    windows = firstPresent(root, "non_working_windows", "nonWorkingWindows");
  }
  if (!windows.isNull()) {
    loadNonWorkingWindows(windows);
  }

  Serial.print("Config applied: device=");
  Serial.print(runtimeConfig.deviceEnabled ? "on" : "off");
  Serial.print(" water<");
  Serial.print(runtimeConfig.wateringMoistureThresholdOn, 1);
  Serial.print(" fans>");
  Serial.print(runtimeConfig.fanHumidityThresholdOn, 1);
  Serial.print(" light<");
  Serial.print(runtimeConfig.lightIntensityThresholdOn, 1);
  Serial.print(" cooldown_ms=");
  Serial.println(runtimeConfig.wateringCooldownMs);
}

float smoothingAlpha() {
  return constrain(SENSOR_SMOOTHING_ALPHA, 0.0f, 1.0f);
}

void rejectSensorValue(SensorValueState &state, const char *status) {
  state.valid = false;
  state.status = status;
}

bool sensorValueJumped(const SensorValueState &state, float value) {
  return state.acceptedSamples >= SENSOR_MIN_VALID_SAMPLES && !isnan(state.lastAcceptedValue) &&
         fabsf(value - state.lastAcceptedValue) > SENSOR_MAX_PERCENT_STEP;
}

bool acceptSensorValue(SensorValueState &state, float value) {
  state.lastAcceptedValue = value;

  if (isnan(state.smoothedValue)) {
    state.smoothedValue = value;
  } else {
    state.smoothedValue += (value - state.smoothedValue) * smoothingAlpha();
  }

  if (state.acceptedSamples < UINT16_MAX) {
    state.acceptedSamples++;
  }

  if (state.acceptedSamples < SENSOR_MIN_VALID_SAMPLES) {
    rejectSensorValue(state, "settling");
    return false;
  }

  state.valid = true;
  state.status = "ok";
  return true;
}

bool updateDhtSensor(SensorValueState &state, float value, unsigned long now, float minimum, float maximum) {
  if (!sensorWarmupComplete(now)) {
    rejectSensorValue(state, "warming_up");
    return false;
  }
  if (isnan(value)) {
    rejectSensorValue(state, "read_failed");
    return false;
  }
  if (value < minimum || value > maximum) {
    rejectSensorValue(state, "out_of_range");
    return false;
  }

  return acceptSensorValue(state, value);
}

bool updateLightSensor(int raw, unsigned long now) {
  if (!sensorWarmupComplete(now)) {
    rejectSensorValue(lightState, "warming_up");
    return false;
  }

  const float percent = 100.0f - ((raw * 100.0f) / ADC_MAX);
  return acceptSensorValue(lightState, constrain(percent, 0.0f, 100.0f));
}

bool updateSoilSensor(int raw, unsigned long now) {
  if (!sensorWarmupComplete(now)) {
    rejectSensorValue(soilState, "warming_up");
    return false;
  }
  if (raw <= SOIL_MIN_VALID_RAW || raw >= SOIL_MAX_VALID_RAW) {
    rejectSensorValue(soilState, "raw_out_of_range");
    return false;
  }

  const float percent = soilMoisturePercentFromRaw(raw);
  if (percent <= SOIL_MIN_TRUSTED_PERCENT) {
    rejectSensorValue(soilState, "too_dry_to_trust");
    return false;
  }
  if (sensorValueJumped(soilState, percent)) {
    rejectSensorValue(soilState, "sudden_change");
    return false;
  }

  return acceptSensorValue(soilState, constrain(percent, 0.0f, 100.0f));
}

int readAveragedAnalog(uint8_t pin, int samples) {
  const int sampleCount = max(samples, 1);
  uint32_t total = 0;

  for (int sample = 0; sample < sampleCount; sample++) {
    total += analogRead(pin);
    if (sample + 1 < sampleCount) {
      delayMicroseconds(150);
    }
  }

  return total / sampleCount;
}

void setLedBoard(bool on) {
  digitalWrite(LED_BOARD_PIN, on ? LED_BOARD_ON_LEVEL : LED_BOARD_OFF_LEVEL);
  ledBoardRunning = on;
}

bool automationAllowed() {
  return runtimeConfig.deviceEnabled && !inNonWorkingWindow();
}

bool shouldRunLedBoard() {
  if (!automationAllowed() || !lightState.valid || isnan(lightPercent)) {
    return false;
  }
  if (ledBoardRunning) {
    return lightPercent < runtimeConfig.lightIntensityThresholdOff;
  }
  return lightPercent < runtimeConfig.lightIntensityThresholdOn;
}

bool shouldRunFans() {
  if (!automationAllowed() || !humidityState.valid || isnan(humidity)) {
    return false;
  }
  if (fanRunning) {
    return humidity > runtimeConfig.fanHumidityThresholdOff;
  }
  return humidity > runtimeConfig.fanHumidityThresholdOn;
}

void setPump(bool on) {
  if (!on) {
    if (pumpRunning) {
      lastPumpStoppedMs = millis();
      Serial.println("Pump OFF");
    }
    ledcWrite(PUMP_PWM_CHANNEL, 0);
    pumpRunning = false;
    return;
  }

  if (pumpRunning) {
    ledcWrite(PUMP_PWM_CHANNEL, PUMP_PWM_RUN_DUTY);
    return;
  }

  for (int duty = PUMP_PWM_RAMP_STEP; duty < PUMP_PWM_RUN_DUTY; duty += PUMP_PWM_RAMP_STEP) {
    ledcWrite(PUMP_PWM_CHANNEL, duty);
    delay(PUMP_PWM_RAMP_DELAY_MS);
  }
  ledcWrite(PUMP_PWM_CHANNEL, PUMP_PWM_RUN_DUTY);
  pumpRunning = true;
  Serial.println("Pump ON");
}

unsigned long normalizedPumpDuration(unsigned long durationMs) {
  if (durationMs == 0) {
    return PUMP_WATERING_BURST_MS;
  }

  return durationMs;
}

void stopPump(const char *status) {
  pumpRunDurationMs = 0;
  setPump(false);
  pumpSafetyStatus = status;
}

void servicePump(unsigned long now) {
  if (!pumpRunning) {
    return;
  }

  if (!soilState.valid || isnan(soilMoisturePercent)) {
    stopPump("stopped_soil_invalid");
    return;
  }

  if (timeElapsed(now, pumpStartedMs, pumpRunDurationMs)) {
    stopPump("ready");
  }
}

bool canStartAutomaticWatering(unsigned long now, unsigned long durationMs) {
  (void)durationMs;

  if (pumpRunning) {
    return false;
  }
  if (!soilState.valid || !soilStable || isnan(soilMoisturePercent)) {
    pumpSafetyStatus = soilState.valid ? "soil_stabilizing" : soilState.status;
    return false;
  }
  if (!automationAllowed()) {
    pumpSafetyStatus = runtimeConfig.deviceEnabled ? "non_working_window" : "device_off";
    return false;
  }
  if (soilMoisturePercent >= runtimeConfig.wateringMoistureThresholdOn) {
    pumpSafetyStatus = "ready";
    return false;
  }
  if (lastWateringMs != 0 && now - lastWateringMs < runtimeConfig.wateringCooldownMs) {
    pumpSafetyStatus = "cooldown";
    return false;
  }
  if (lastPumpStoppedMs != 0 && now - lastPumpStoppedMs < PUMP_MIN_OFF_MS) {
    pumpSafetyStatus = "minimum_off_time";
    return false;
  }

  return true;
}

bool requestPumpRun(unsigned long durationMs, const char *reason, unsigned long now) {
  const unsigned long safeDurationMs = normalizedPumpDuration(durationMs);
  if (!canStartAutomaticWatering(now, safeDurationMs)) {
    return false;
  }

  pumpStartedMs = now;
  pumpRunDurationMs = safeDurationMs;
  lastWateringMs = now;
  pumpSafetyStatus = "watering";

  Serial.print("Pump requested for ");
  Serial.print(safeDurationMs);
  Serial.print(" ms by ");
  Serial.println(reason);

  setPump(true);
  return true;
}

void setFans(bool on) {
  digitalWrite(FAN_PIN, on ? FAN_ON_LEVEL : FAN_OFF_LEVEL);
  fanRunning = on;
}

int readSoilMoisture() {
  digitalWrite(SOIL_MOISTURE_POWER_PIN, HIGH);
  delay(SOIL_MOISTURE_SETTLE_MS);

  long total = 0;
  for (int i = 0; i < SOIL_MOISTURE_SAMPLES; i++) {
    total += analogRead(SOIL_MOISTURE_PIN);
    delay(10);
  }

  digitalWrite(SOIL_MOISTURE_POWER_PIN, LOW);
  return total / SOIL_MOISTURE_SAMPLES;
}

void updateSoilStability(int reading) {
  if (!soilState.valid) {
    soilStableReadings = 0;
    soilStable = false;
    previousSoilMoistureRaw = reading;
    return;
  }

  if (previousSoilMoistureRaw < 0) {
    previousSoilMoistureRaw = reading;
    soilStableReadings = 1;
    soilStable = false;
    return;
  }

  if (abs(reading - previousSoilMoistureRaw) <= SOIL_STABLE_DELTA_RAW) {
    soilStableReadings++;
  } else {
    soilStableReadings = 1;
    soilStable = false;
  }

  previousSoilMoistureRaw = reading;
  soilStable = soilStableReadings >= SOIL_STABLE_REQUIRED_READINGS;
}

void printFloatOrPlaceholder(float value, int decimals) {
  if (isnan(value)) {
    Serial.print("--.-");
    return;
  }

  Serial.print(value, decimals);
}

void connectWifi() {
  if (WiFi.status() == WL_CONNECTED) {
    return;
  }

  if (WIFI_NETWORK_COUNT == 0) {
    Serial.println("WiFi failed: no networks configured");
    return;
  }

  if (wifiNetworkIndex >= WIFI_NETWORK_COUNT) {
    wifiNetworkIndex = 0;
  }

  const WifiNetwork &network = WIFI_NETWORKS[wifiNetworkIndex];

  if (network.ssid == nullptr || network.ssid[0] == '\0') {
    Serial.println("WiFi skipped: empty SSID");
    wifiNetworkIndex = (wifiNetworkIndex + 1) % WIFI_NETWORK_COUNT;
    return;
  }

  Serial.print("Connecting to WiFi SSID: ");
  Serial.println(network.ssid);

  WiFi.mode(WIFI_STA);
  WiFi.persistent(false);
  WiFi.setAutoReconnect(true);
  WiFi.setSleep(false);
  WiFi.setTxPower(WIFI_POWER_19_5dBm);
  WiFi.begin(network.ssid, network.password);

  const unsigned long startedAt = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - startedAt < WIFI_CONNECT_TIMEOUT_MS) {
    delay(500);
    Serial.print('.');
  }

  Serial.println();

  if (WiFi.status() != WL_CONNECTED) {
    Serial.print("WiFi failed, status: ");
    Serial.println(WiFi.status());
    wifiNetworkIndex = (wifiNetworkIndex + 1) % WIFI_NETWORK_COUNT;
    Serial.print("Next WiFi SSID: ");
    Serial.println(WIFI_NETWORKS[wifiNetworkIndex].ssid);
    return;
  }

  Serial.print("WiFi connected, IP: ");
  Serial.print(WiFi.localIP());
  Serial.print(" RSSI: ");
  Serial.println(WiFi.RSSI());
  configTime(0, 0, "pool.ntp.org", "time.nist.gov");
}

void mqttCallback(char *topic, byte *payload, unsigned int length) {
  if (strcmp(topic, MQTT_CONFIG_TOPIC) != 0) {
    return;
  }

  Serial.print("Config message received on ");
  Serial.print(topic);
  Serial.print(" bytes=");
  Serial.println(length);
  applyConfigPayload(reinterpret_cast<const char *>(payload), length);
}

void connectMqtt() {
  if (mqtt.connected() || WiFi.status() != WL_CONNECTED) {
    return;
  }

  Serial.print("Connecting to MQTT");
  const unsigned long startedAt = millis();
  while (!mqtt.connected() && WiFi.status() == WL_CONNECTED && millis() - startedAt < MQTT_CONNECT_TIMEOUT_MS) {
    if (mqtt.connect(MQTT_CLIENT_ID, MQTT_USERNAME, MQTT_PASSWORD)) {
      Serial.println(" connected");
      if (mqtt.subscribe(MQTT_CONFIG_TOPIC)) {
        Serial.print("MQTT subscribed: ");
        Serial.println(MQTT_CONFIG_TOPIC);
      } else {
        Serial.print("MQTT subscribe failed: ");
        Serial.println(MQTT_CONFIG_TOPIC);
      }
    } else {
      Serial.print(" failed, rc=");
      Serial.print(mqtt.state());
      Serial.println(" retrying in 2s");
      delay(2000);
    }
  }

  if (!mqtt.connected()) {
    Serial.println("MQTT connect timed out");
  }
}

void publishReading(int lightRaw, int soilMoistureRaw) {
  const int correctedLightRaw = lightState.valid && lightRaw >= 0 ? ADC_MAX - lightRaw : -1;
  const int correctedSoilMoistureRaw = soilState.valid && soilMoistureRaw >= 0 ? ADC_MAX - soilMoistureRaw : -1;

  const String temperatureJson = isnan(temperatureC) ? "null" : String(temperatureC, 1);
  const String humidityJson = isnan(humidity) ? "null" : String(humidity, 1);

  char payload[560];
  snprintf(
    payload,
    sizeof(payload),
    "{\"light\":%d,\"soil-moisture\":%d,\"temp\":%s,\"ambient-humidity\":%s,\"soil-stable\":%s,"
    "\"temperature-status\":\"%s\",\"humidity-status\":\"%s\",\"light-status\":\"%s\",\"soil-status\":\"%s\","
    "\"pump-running\":%s,\"pump-status\":\"%s\",\"pump-duration-ms\":%lu,\"pump-cooldown-ms\":%lu}",
    correctedLightRaw,
    correctedSoilMoistureRaw,
    temperatureJson.c_str(),
    humidityJson.c_str(),
    soilState.valid && soilStable ? "true" : "false",
    temperatureState.status,
    humidityState.status,
    lightState.status,
    soilState.status,
    pumpRunning ? "true" : "false",
    pumpSafetyStatus,
    runtimeConfig.wateringDurationMs,
    runtimeConfig.wateringCooldownMs
  );

  if (mqtt.publish(MQTT_TOPIC, payload)) {
    Serial.print("MQTT published: ");
    Serial.println(payload);
  } else {
    Serial.print("MQTT publish failed, connected=");
    Serial.print(mqtt.connected() ? "yes" : "no");
    Serial.print(" state=");
    Serial.print(mqtt.state());
    Serial.print(" payload_bytes=");
    Serial.println(strlen(payload));
  }
}

void setup() {
  Serial.begin(115200);
  delay(1000);

  analogReadResolution(12);
  ledcSetup(PUMP_PWM_CHANNEL, PUMP_PWM_FREQUENCY, PUMP_PWM_RESOLUTION_BITS);
  ledcAttachPin(PUMP_PIN, PUMP_PWM_CHANNEL);
  pinMode(FAN_PIN, OUTPUT);
  pinMode(LED_BOARD_PIN, OUTPUT);
  pinMode(SOIL_MOISTURE_POWER_PIN, OUTPUT);
  setPump(false);
  setFans(false);
  setLedBoard(false);
  digitalWrite(SOIL_MOISTURE_POWER_PIN, LOW);
  dht.begin();
  wifiClient.setInsecure();
  mqtt.setServer(MQTT_HOST, MQTT_PORT);
  mqtt.setBufferSize(MQTT_BUFFER_SIZE);
  mqtt.setCallback(mqttCallback);
  mqtt.setKeepAlive(30);
  mqtt.setSocketTimeout(10);

  Serial.println("ESP32-C3 plant sensor MQTT firmware");
  Serial.println("DHT22: GPIO6, KY-018 analog: GPIO0, soil moisture AO: GPIO1, soil moisture VCC: GPIO3, pump MOSFET gate: GPIO4, fan MOSFET gate: GPIO5, LED board MOSFET gate: GPIO7");
  Serial.print("MQTT topic: ");
  Serial.println(MQTT_TOPIC);
  Serial.print("MQTT config topic: ");
  Serial.println(MQTT_CONFIG_TOPIC);
  Serial.println("Light is corrected so 0% = dark and 100% = bright.");
  Serial.println("5V LED board turns on when corrected light is under 90%.");
  Serial.println("5V pump uses short safe bursts when trusted stable soil moisture is under 40%.");
  Serial.println("12V fans turn on when air humidity is over 60%.");
  Serial.println();

  for (int i = 0; i < 3; i++) {
    setLedBoard(true);
    delay(200);
    setLedBoard(false);
    delay(200);
  }

  connectWifi();
  connectMqtt();
}

void loop() {
  unsigned long now = millis();

  servicePump(now);
  if ((WiFi.status() != WL_CONNECTED || !mqtt.connected()) && pumpRunning) {
    stopPump("stopped_network_reconnect");
  }

  connectWifi();
  if (WiFi.status() != WL_CONNECTED) {
    if (mqtt.connected()) {
      mqtt.disconnect();
    }
    delay(5000);
    return;
  }

  connectMqtt();
  mqtt.loop();
  now = millis();
  servicePump(now);

  if (now - lastDhtMs >= DHT_INTERVAL_MS || lastDhtMs == 0) {
    lastDhtMs = now;

    const float newHumidity = dht.readHumidity();
    const float newTemperatureC = dht.readTemperature();

    if (updateDhtSensor(humidityState, newHumidity, now, DHT_MIN_HUMIDITY_PERCENT, DHT_MAX_HUMIDITY_PERCENT)) {
      humidity = humidityState.smoothedValue;
    } else {
      humidity = NAN;
    }

    if (updateDhtSensor(temperatureState, newTemperatureC, now, DHT_MIN_TEMPERATURE_C, DHT_MAX_TEMPERATURE_C)) {
      temperatureC = temperatureState.smoothedValue;
    } else {
      temperatureC = NAN;
    }
  }

  if (now - lastSoilMoistureMs >= SOIL_MOISTURE_INTERVAL_MS || lastSoilMoistureMs == 0) {
    lastSoilMoistureMs = now;
    soilMoistureRaw = readSoilMoisture();
    if (updateSoilSensor(soilMoistureRaw, now)) {
      soilMoisturePercent = soilState.smoothedValue;
    } else {
      soilMoisturePercent = NAN;
    }
    updateSoilStability(soilMoistureRaw);
  }

  if (now - lastDisplayMs < DISPLAY_INTERVAL_MS) {
    return;
  }
  lastDisplayMs = now;

  lightRaw = readAveragedAnalog(LIGHT_PIN, LIGHT_SAMPLES);
  if (updateLightSensor(lightRaw, now)) {
    lightPercent = lightState.smoothedValue;
  } else {
    lightPercent = NAN;
  }

  if (!automationAllowed() && pumpRunning) {
    stopPump(runtimeConfig.deviceEnabled ? "stopped_non_working_window" : "stopped_device_off");
  }

  const bool ledBoardOn = shouldRunLedBoard();
  const bool fansOn = shouldRunFans();

  setLedBoard(ledBoardOn);
  setFans(fansOn);

  requestPumpRun(runtimeConfig.wateringDurationMs, "auto-soil", now);

  Serial.print("Temp: ");
  printFloatOrPlaceholder(temperatureC, 1);
  Serial.print(" C (");
  Serial.print(temperatureState.status);
  Serial.print(')');

  Serial.print(" | Humidity: ");
  printFloatOrPlaceholder(humidity, 1);
  Serial.print(" % (");
  Serial.print(humidityState.status);
  Serial.print(')');

  Serial.print(" | Light: ");
  printFloatOrPlaceholder(lightPercent, 1);
  Serial.print(" % (raw ");
  Serial.print(lightRaw);
  Serial.print(", ");
  Serial.print(lightState.status);
  Serial.print(") | LED board: ");
  Serial.print(ledBoardOn ? "ON" : "OFF");
  Serial.print(" | Pump: ");
  Serial.print(pumpRunning ? "ON" : "OFF");
  Serial.print(" (");
  Serial.print(pumpSafetyStatus);
  Serial.print(')');
  Serial.print(" | Fans: ");
  Serial.print(fansOn ? "ON" : "OFF");
  Serial.print(" | Soil: ");
  if (soilMoistureRaw < 0) {
    Serial.println("--.- % (sensor off)");
  } else {
    printFloatOrPlaceholder(soilMoisturePercent, 1);
    Serial.print(" % (raw ");
    Serial.print(soilMoistureRaw);
    Serial.print(soilStable ? ", stable, " : ", not trusted, ");
    Serial.print(soilState.status);
    Serial.print(", sensor off)");
    Serial.println();
  }

  if (now - lastMqttPublishMs >= MQTT_PUBLISH_INTERVAL_MS || lastMqttPublishMs == 0) {
    lastMqttPublishMs = now;
    publishReading(lightRaw, soilMoistureRaw);
  }
}
