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
constexpr int SOIL_MOISTURE_SAMPLES = 10;
constexpr int SOIL_STABLE_DELTA_RAW = 61; // About 1.5% of the 12-bit ADC range.
constexpr int SOIL_STABLE_REQUIRED_READINGS = 3;
constexpr float PUMP_SOIL_THRESHOLD_PERCENT = 40.0f;
constexpr float FAN_HUMIDITY_THRESHOLD_PERCENT = 60.0f;
constexpr float LED_BOARD_LIGHT_THRESHOLD_PERCENT = 90.0f;

DHT dht(DHT_PIN, DHT_TYPE);
WiFiClientSecure wifiClient;
PubSubClient mqtt(wifiClient);
unsigned long lastDisplayMs = 0;
unsigned long lastDhtMs = 0;
unsigned long lastSoilMoistureMs = 0;
unsigned long lastMqttPublishMs = 0;
int soilMoistureRaw = -1;
int previousSoilMoistureRaw = -1;
int soilStableReadings = 0;
bool soilStable = false;
float humidity = NAN;
float temperatureC = NAN;
bool pumpRunning = false;

float soilMoisturePercentFromRaw(int reading) {
  return 100.0f - ((reading * 100.0f) / ADC_MAX);
}

void setLedBoard(bool on) {
  digitalWrite(LED_BOARD_PIN, on ? LED_BOARD_ON_LEVEL : LED_BOARD_OFF_LEVEL);
}

void setPump(bool on) {
  if (!on) {
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
}

void updatePump(bool shouldRun, unsigned long now) {
  (void)now;
  setPump(shouldRun);
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
  const int correctedLightRaw = ADC_MAX - lightRaw;
  const int correctedSoilMoistureRaw = soilMoistureRaw < 0 ? -1 : ADC_MAX - soilMoistureRaw;

  const String temperatureJson = isnan(temperatureC) ? "null" : String(temperatureC, 1);
  const String humidityJson = isnan(humidity) ? "null" : String(humidity, 1);

  char payload[220];
  snprintf(
    payload,
    sizeof(payload),
    "{\"light\":%d,\"soil-moisture\":%d,\"temp\":%s,\"ambient-humidity\":%s,\"soil-stable\":%s}",
    correctedLightRaw,
    correctedSoilMoistureRaw,
    temperatureJson.c_str(),
    humidityJson.c_str(),
    soilStable ? "true" : "false"
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
  Serial.println("5V pump runs continuously when stable soil moisture is under 40%, using PWM soft-start and full run power.");
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
  const unsigned long now = millis();

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

  if (now - lastDhtMs >= DHT_INTERVAL_MS || lastDhtMs == 0) {
    lastDhtMs = now;

    const float newHumidity = dht.readHumidity();
    const float newTemperatureC = dht.readTemperature();

    if (!isnan(newHumidity)) {
      humidity = newHumidity;
    }
    if (!isnan(newTemperatureC)) {
      temperatureC = newTemperatureC;
    }
  }

  if (now - lastSoilMoistureMs >= SOIL_MOISTURE_INTERVAL_MS || lastSoilMoistureMs == 0) {
    lastSoilMoistureMs = now;
    soilMoistureRaw = readSoilMoisture();
    updateSoilStability(soilMoistureRaw);
  }

  if (now - lastDisplayMs < DISPLAY_INTERVAL_MS) {
    return;
  }
  lastDisplayMs = now;

  const int lightRaw = analogRead(LIGHT_PIN);
  const float lightPercent = 100.0f - ((lightRaw * 100.0f) / ADC_MAX);
  const bool ledBoardOn = lightPercent < LED_BOARD_LIGHT_THRESHOLD_PERCENT;
  const bool fansOn = !isnan(humidity) && humidity > FAN_HUMIDITY_THRESHOLD_PERCENT;

  setLedBoard(ledBoardOn);
  setFans(fansOn);

  const float soilMoisturePercent = soilMoistureRaw < 0 ? NAN : soilMoisturePercentFromRaw(soilMoistureRaw);
  const bool pumpShouldRun = soilStable && soilMoisturePercent >= 0.0f && soilMoisturePercent < PUMP_SOIL_THRESHOLD_PERCENT;
  updatePump(pumpShouldRun, now);

  Serial.print("Temp: ");
  if (isnan(temperatureC)) {
    Serial.print("--.- C");
  } else {
    Serial.print(temperatureC, 1);
    Serial.print(" C");
  }

  Serial.print(" | Humidity: ");
  if (isnan(humidity)) {
    Serial.print("--.- %");
  } else {
    Serial.print(humidity, 1);
    Serial.print(" %");
  }

  Serial.print(" | Light: ");
  Serial.print(lightPercent, 1);
  Serial.print(" % (raw ");
  Serial.print(lightRaw);
  Serial.print(") | LED board: ");
  Serial.print(ledBoardOn ? "ON" : "OFF");
  Serial.print(" | Pump: ");
  Serial.print(pumpRunning ? "ON" : "OFF");
  Serial.print(" | Fans: ");
  Serial.print(fansOn ? "ON" : "OFF");
  Serial.print(" | Soil: ");
  if (soilMoistureRaw < 0) {
    Serial.println("--.- % (sensor off)");
  } else {
    Serial.print(soilMoisturePercent, 1);
    Serial.print(" % (raw ");
    Serial.print(soilMoistureRaw);
    Serial.print(soilStable ? ", stable, sensor off)" : ", stabilizing, sensor off)");
    Serial.println();
  }

  if (now - lastMqttPublishMs >= MQTT_PUBLISH_INTERVAL_MS || lastMqttPublishMs == 0) {
    lastMqttPublishMs = now;
    publishReading(lightRaw, soilMoistureRaw);
  }
}
