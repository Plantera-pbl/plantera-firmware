#include <Arduino.h>
#include <DHT.h>
#include <PubSubClient.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>

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
constexpr float PUMP_SOIL_RECOVERY_PERCENT = 50.0f;
constexpr float PUMP_SOIL_RESPONSE_MIN_INCREASE_PERCENT = 2.0f;
constexpr unsigned long PUMP_WATERING_BURST_MS = 3000;
constexpr unsigned long PUMP_MAX_RUN_MS = 5000;
constexpr unsigned long PUMP_MIN_OFF_MS = 60UL * 1000UL;
constexpr unsigned long PUMP_COOLDOWN_MS = 10UL * 60UL * 1000UL;
constexpr unsigned long PUMP_SAFETY_WINDOW_MS = 60UL * 60UL * 1000UL;
constexpr unsigned long PUMP_MAX_RUNTIME_PER_WINDOW_MS = 20000;
constexpr unsigned long PUMP_SOIL_RESPONSE_TIMEOUT_MS = 2UL * 60UL * 1000UL;
constexpr float FAN_HUMIDITY_THRESHOLD_PERCENT = 60.0f;
constexpr float LED_BOARD_LIGHT_THRESHOLD_PERCENT = 90.0f;

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
unsigned long lastDisplayMs = 0;
unsigned long lastDhtMs = 0;
unsigned long lastSoilMoistureMs = 0;
unsigned long lastMqttPublishMs = 0;
unsigned long pumpStartedMs = 0;
unsigned long pumpRunDurationMs = 0;
unsigned long lastPumpStoppedMs = 0;
unsigned long lastWateringMs = 0;
unsigned long pumpBudgetWindowStartedMs = 0;
unsigned long pumpRuntimeReservedMs = 0;
unsigned long waterResponseStartedMs = 0;
int soilMoistureRaw = -1;
int lightRaw = -1;
int previousSoilMoistureRaw = -1;
int soilStableReadings = 0;
bool soilStable = false;
float humidity = NAN;
float temperatureC = NAN;
float lightPercent = NAN;
float soilMoisturePercent = NAN;
float waterResponseStartSoilPercent = NAN;
SensorValueState temperatureState;
SensorValueState humidityState;
SensorValueState lightState;
SensorValueState soilState;
bool pumpRunning = false;
bool waitingForSoilResponse = false;
bool autoWaterLocked = false;
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
  if (durationMs == 0 || durationMs > PUMP_MAX_RUN_MS) {
    return PUMP_MAX_RUN_MS;
  }

  return durationMs;
}

void refreshPumpBudgetWindow(unsigned long now) {
  if (pumpBudgetWindowStartedMs == 0 || now - pumpBudgetWindowStartedMs >= PUMP_SAFETY_WINDOW_MS) {
    pumpBudgetWindowStartedMs = now;
    pumpRuntimeReservedMs = 0;
  }
}

bool soilSensorShowsWateringResponse() {
  if (!soilState.valid || isnan(soilMoisturePercent) || isnan(waterResponseStartSoilPercent)) {
    return false;
  }

  return soilMoisturePercent >= waterResponseStartSoilPercent + PUMP_SOIL_RESPONSE_MIN_INCREASE_PERCENT ||
         soilMoisturePercent >= PUMP_SOIL_RECOVERY_PERCENT;
}

void clearWaterResponseWatch(const char *status) {
  waitingForSoilResponse = false;
  autoWaterLocked = false;
  waterResponseStartedMs = 0;
  waterResponseStartSoilPercent = NAN;
  pumpSafetyStatus = status;
}

void startWaterResponseWatch(unsigned long now) {
  waitingForSoilResponse = true;
  autoWaterLocked = false;
  waterResponseStartedMs = now;
  waterResponseStartSoilPercent = soilMoisturePercent;
  pumpSafetyStatus = "watering";
}

void updateWaterResponseSafety(unsigned long now) {
  if (autoWaterLocked) {
    if (soilSensorShowsWateringResponse()) {
      clearWaterResponseWatch("ready");
      Serial.println("Automatic watering re-enabled after soil response");
    } else {
      pumpSafetyStatus = "locked_no_soil_response";
    }
    return;
  }

  if (!waitingForSoilResponse) {
    return;
  }

  if (soilSensorShowsWateringResponse()) {
    clearWaterResponseWatch("ready");
    Serial.println("Automatic watering re-enabled after soil response");
    return;
  }

  if (!soilState.valid || isnan(soilMoisturePercent)) {
    pumpSafetyStatus = "waiting_for_valid_soil";
    return;
  }

  if (timeElapsed(now, waterResponseStartedMs, PUMP_SOIL_RESPONSE_TIMEOUT_MS)) {
    waitingForSoilResponse = false;
    autoWaterLocked = true;
    pumpSafetyStatus = "locked_no_soil_response";
    Serial.println("Automatic watering locked: soil did not respond after pump run");
    return;
  }

  pumpSafetyStatus = "waiting_for_soil_response";
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
    stopPump(waitingForSoilResponse ? "waiting_for_soil_response" : "ready");
  }
}

bool pumpRuntimeAvailable(unsigned long now, unsigned long durationMs) {
  refreshPumpBudgetWindow(now);
  return pumpRuntimeReservedMs + durationMs <= PUMP_MAX_RUNTIME_PER_WINDOW_MS;
}

bool canStartAutomaticWatering(unsigned long now, unsigned long durationMs) {
  updateWaterResponseSafety(now);

  if (pumpRunning || waitingForSoilResponse || autoWaterLocked) {
    return false;
  }
  if (!soilState.valid || !soilStable || isnan(soilMoisturePercent)) {
    pumpSafetyStatus = soilState.valid ? "soil_stabilizing" : soilState.status;
    return false;
  }
  if (soilMoisturePercent >= PUMP_SOIL_THRESHOLD_PERCENT) {
    pumpSafetyStatus = "ready";
    return false;
  }
  if (lastWateringMs != 0 && now - lastWateringMs < PUMP_COOLDOWN_MS) {
    pumpSafetyStatus = "cooldown";
    return false;
  }
  if (lastPumpStoppedMs != 0 && now - lastPumpStoppedMs < PUMP_MIN_OFF_MS) {
    pumpSafetyStatus = "minimum_off_time";
    return false;
  }
  if (!pumpRuntimeAvailable(now, durationMs)) {
    pumpSafetyStatus = "runtime_budget_exceeded";
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
  pumpRuntimeReservedMs += safeDurationMs;
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

  Serial.print("Connecting to WiFi SSID: ");
  Serial.println(WIFI_SSID);

  WiFi.mode(WIFI_STA);
  WiFi.persistent(false);
  WiFi.setAutoReconnect(true);
  WiFi.setSleep(false);
  WiFi.setTxPower(WIFI_POWER_19_5dBm);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);

  const unsigned long startedAt = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - startedAt < WIFI_CONNECT_TIMEOUT_MS) {
    delay(500);
    Serial.print('.');
  }

  Serial.println();

  if (WiFi.status() != WL_CONNECTED) {
    Serial.print("WiFi failed, status: ");
    Serial.println(WiFi.status());
    return;
  }

  Serial.print("WiFi connected, IP: ");
  Serial.print(WiFi.localIP());
  Serial.print(" RSSI: ");
  Serial.println(WiFi.RSSI());
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
  refreshPumpBudgetWindow(millis());

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
    "\"pump-running\":%s,\"pump-status\":\"%s\",\"pump-runtime-ms\":%lu,\"pump-runtime-budget-ms\":%lu}",
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
    pumpRuntimeReservedMs,
    PUMP_MAX_RUNTIME_PER_WINDOW_MS
  );

  if (mqtt.publish(MQTT_TOPIC, payload)) {
    Serial.print("MQTT published: ");
    Serial.println(payload);
  } else {
    Serial.println("MQTT publish failed");
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
  mqtt.setKeepAlive(30);
  mqtt.setSocketTimeout(10);

  Serial.println("ESP32-C3 plant sensor MQTT firmware");
  Serial.println("DHT22: GPIO6, KY-018 analog: GPIO0, soil moisture AO: GPIO1, soil moisture VCC: GPIO3, pump MOSFET gate: GPIO4, fan MOSFET gate: GPIO5, LED board MOSFET gate: GPIO7");
  Serial.print("MQTT topic: ");
  Serial.println(MQTT_TOPIC);
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
  updateWaterResponseSafety(now);

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
    updateWaterResponseSafety(now);
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

  const bool ledBoardOn = lightState.valid && !isnan(lightPercent) && lightPercent < LED_BOARD_LIGHT_THRESHOLD_PERCENT;
  const bool fansOn = humidityState.valid && !isnan(humidity) && humidity > FAN_HUMIDITY_THRESHOLD_PERCENT;

  setLedBoard(ledBoardOn);
  setFans(fansOn);

  if (requestPumpRun(PUMP_WATERING_BURST_MS, "auto-soil", now)) {
    startWaterResponseWatch(now);
  }

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
