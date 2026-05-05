#include <Arduino.h>
#include <ArduinoJson.h>
#include <DHT.h>
#include <HTTPClient.h>
#include <WiFi.h>
#include <cstring>
#include <math.h>

#if __has_include("plantera_config.h")
#include "plantera_config.h"
#else
#include "plantera_config.example.h"
#endif

enum class ControlMode {
  Auto,
  ForcedOn,
  ForcedOff,
};

struct SensorReadings {
  float temperatureC = NAN;
  float humidityPercent = NAN;
  float lightPercent = NAN;
  float soilMoisturePercent = NAN;
  int lightRaw = 0;
  int soilRaw = 0;
  unsigned long updatedAtMs = 0;
};

struct ServerCommand {
  ControlMode pumpMode = ControlMode::Auto;
  ControlMode lightMode = ControlMode::Auto;
  ControlMode fanMode = ControlMode::Auto;
  unsigned long pumpDurationMs = PUMP_DEFAULT_ON_MS;
  unsigned long receivedAtMs = 0;
  bool valid = false;
};

DHT dht(DHT_PIN, DHT_TYPE);

SensorReadings readings;
ServerCommand serverCommand;

unsigned long lastSensorReadMs = 0;
unsigned long lastControlMs = 0;
unsigned long lastTelemetryMs = 0;
unsigned long lastCommandPollMs = 0;
unsigned long lastWifiAttemptMs = 0;
unsigned long pumpOffAtMs = 0;
unsigned long lastAutoWaterMs = 0;

bool pumpOn = false;
bool growLightOn = false;
bool fanOn = false;
bool autoFanOn = false;
bool serverPumpOnLatched = false;
bool wifiConnectedLogged = false;

bool elapsed(unsigned long now, unsigned long last, unsigned long interval) {
  return last == 0 || now - last >= interval;
}

bool deadlineReached(unsigned long now, unsigned long deadline) {
  return static_cast<long>(now - deadline) >= 0;
}

bool wifiConfigured() {
  return !FORCE_LOCAL_MODE && std::strlen(WIFI_SSID) > 0;
}

bool serverConfigured() {
  return !FORCE_LOCAL_MODE && std::strlen(SERVER_BASE_URL) > 0;
}

bool networkReady() {
  return wifiConfigured() && serverConfigured() && WiFi.status() == WL_CONNECTED;
}

const char *controlModeName(ControlMode mode) {
  switch (mode) {
    case ControlMode::Auto:
      return "auto";
    case ControlMode::ForcedOn:
      return "on";
    case ControlMode::ForcedOff:
      return "off";
  }
  return "auto";
}

float percentFromCalibration(int raw, int zeroPercentRaw, int hundredPercentRaw) {
  if (zeroPercentRaw == hundredPercentRaw) {
    return NAN;
  }

  const float percent = ((raw - zeroPercentRaw) * 100.0f) / (hundredPercentRaw - zeroPercentRaw);
  return constrain(percent, 0.0f, 100.0f);
}

unsigned long normalizedPumpDuration(unsigned long requestedMs) {
  if (requestedMs == 0) {
    requestedMs = PUMP_DEFAULT_ON_MS;
  }
  return min(requestedMs, PUMP_MAX_ON_MS);
}

void writeActuator(uint8_t pin, bool on) {
  digitalWrite(pin, on ? ACTUATOR_ON_LEVEL : ACTUATOR_OFF_LEVEL);
}

void setPump(bool on) {
  writeActuator(PUMP_PIN, on);
  if (pumpOn != on) {
    pumpOn = on;
    Serial.print("Pump ");
    Serial.println(on ? "ON" : "OFF");
  }
}

void setGrowLight(bool on) {
  writeActuator(GROW_LIGHT_PIN, on);
  if (growLightOn != on) {
    growLightOn = on;
    Serial.print("Grow light ");
    Serial.println(on ? "ON" : "OFF");
  }
}

void setFan(bool on) {
  writeActuator(FAN_PIN, on);
  if (fanOn != on) {
    fanOn = on;
    Serial.print("Fan ");
    Serial.println(on ? "ON" : "OFF");
  }
}

void requestPumpRun(unsigned long durationMs, const char *reason) {
  const unsigned long now = millis();
  const unsigned long safeDurationMs = normalizedPumpDuration(durationMs);
  pumpOffAtMs = now + safeDurationMs;

  Serial.print("Pump requested for ");
  Serial.print(safeDurationMs);
  Serial.print(" ms by ");
  Serial.println(reason);

  setPump(true);
}

void stopPumpIfExpired(unsigned long now) {
  if (pumpOn && deadlineReached(now, pumpOffAtMs)) {
    setPump(false);
  }
}

String buildUrl(const String &suffix) {
  String base = SERVER_BASE_URL;
  while (base.endsWith("/")) {
    base.remove(base.length() - 1);
  }
  return base + suffix;
}

void setJsonFloat(JsonObject object, const char *key, float value) {
  if (isnan(value)) {
    object[key] = nullptr;
    return;
  }
  object[key] = value;
}

void logReadings() {
  Serial.print("Temp: ");
  if (isnan(readings.temperatureC)) {
    Serial.print("--.- C");
  } else {
    Serial.print(readings.temperatureC, 1);
    Serial.print(" C");
  }

  Serial.print(" | Humidity: ");
  if (isnan(readings.humidityPercent)) {
    Serial.print("--.- %");
  } else {
    Serial.print(readings.humidityPercent, 1);
    Serial.print(" %");
  }

  Serial.print(" | Light: ");
  Serial.print(readings.lightPercent, 1);
  Serial.print(" % raw=");
  Serial.print(readings.lightRaw);

  Serial.print(" | Soil: ");
  Serial.print(readings.soilMoisturePercent, 1);
  Serial.print(" % raw=");
  Serial.println(readings.soilRaw);
}

void readSensors(unsigned long now) {
  if (!elapsed(now, lastSensorReadMs, SENSOR_READ_INTERVAL_MS)) {
    return;
  }
  lastSensorReadMs = now;

  const float newHumidity = dht.readHumidity();
  const float newTemperatureC = dht.readTemperature();

  if (!isnan(newHumidity)) {
    readings.humidityPercent = newHumidity;
  }
  if (!isnan(newTemperatureC)) {
    readings.temperatureC = newTemperatureC;
  }

  readings.lightRaw = analogRead(LIGHT_SENSOR_PIN);
  readings.soilRaw = analogRead(SOIL_SENSOR_PIN);
  readings.lightPercent = percentFromCalibration(readings.lightRaw, LIGHT_DARK_RAW, LIGHT_BRIGHT_RAW);
  readings.soilMoisturePercent = percentFromCalibration(readings.soilRaw, SOIL_DRY_RAW, SOIL_WET_RAW);
  readings.updatedAtMs = now;

  logReadings();
}

void startWifi() {
  if (!wifiConfigured()) {
    if (FORCE_LOCAL_MODE) {
      Serial.println("Local mode forced by configuration");
    } else {
      Serial.println("WiFi SSID is empty; using local autonomous mode");
    }
    return;
  }

  WiFi.mode(WIFI_STA);
  WiFi.setAutoReconnect(true);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  lastWifiAttemptMs = millis();

  Serial.print("WiFi connecting to ");
  Serial.println(WIFI_SSID);
}

void maintainWifi(unsigned long now) {
  if (!wifiConfigured()) {
    return;
  }

  if (WiFi.status() == WL_CONNECTED) {
    if (!wifiConnectedLogged) {
      wifiConnectedLogged = true;
      Serial.print("WiFi connected, IP: ");
      Serial.println(WiFi.localIP());
    }
    return;
  }

  wifiConnectedLogged = false;
  if (!elapsed(now, lastWifiAttemptMs, WIFI_RETRY_INTERVAL_MS)) {
    return;
  }

  lastWifiAttemptMs = now;
  WiFi.disconnect(false);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  Serial.println("WiFi reconnect requested");
}

bool serverControlActive(unsigned long now) {
  return networkReady() && serverCommand.valid && now - serverCommand.receivedAtMs <= SERVER_STALE_MS;
}

bool readControlMode(JsonVariant value, ControlMode &mode) {
  if (value.isNull()) {
    return false;
  }

  if (value.is<bool>()) {
    mode = value.as<bool>() ? ControlMode::ForcedOn : ControlMode::ForcedOff;
    return true;
  }

  const char *rawText = value.as<const char *>();
  if (rawText == nullptr) {
    return false;
  }

  String text = rawText;
  text.trim();
  text.toLowerCase();

  if (text == "auto") {
    mode = ControlMode::Auto;
    return true;
  }
  if (text == "on" || text == "true" || text == "1") {
    mode = ControlMode::ForcedOn;
    return true;
  }
  if (text == "off" || text == "false" || text == "0") {
    mode = ControlMode::ForcedOff;
    return true;
  }

  return false;
}

bool applyServerCommandPayload(const String &payload, unsigned long now) {
  JsonDocument doc;
  const DeserializationError error = deserializeJson(doc, payload);
  if (error) {
    Serial.print("Command JSON parse failed: ");
    Serial.println(error.c_str());
    return false;
  }

  const ControlMode previousPumpMode = serverCommand.pumpMode;
  const unsigned long previousPumpDurationMs = serverCommand.pumpDurationMs;

  readControlMode(doc["pump"], serverCommand.pumpMode);
  readControlMode(doc["light"], serverCommand.lightMode);
  readControlMode(doc["growLight"], serverCommand.lightMode);
  readControlMode(doc["fan"], serverCommand.fanMode);

  if (!doc["pumpDurationMs"].isNull()) {
    serverCommand.pumpDurationMs = normalizedPumpDuration(doc["pumpDurationMs"].as<unsigned long>());
  }

  serverCommand.receivedAtMs = now;
  serverCommand.valid = true;

  if (serverCommand.pumpMode != ControlMode::ForcedOn || previousPumpMode != ControlMode::ForcedOn ||
      previousPumpDurationMs != serverCommand.pumpDurationMs) {
    serverPumpOnLatched = false;
  }

  Serial.print("Server command: pump=");
  Serial.print(controlModeName(serverCommand.pumpMode));
  Serial.print(" light=");
  Serial.print(controlModeName(serverCommand.lightMode));
  Serial.print(" fan=");
  Serial.print(controlModeName(serverCommand.fanMode));
  Serial.print(" pumpDurationMs=");
  Serial.println(serverCommand.pumpDurationMs);

  return true;
}

void pollServerCommand(unsigned long now) {
  if (!networkReady() || !elapsed(now, lastCommandPollMs, COMMAND_POLL_INTERVAL_MS)) {
    return;
  }
  lastCommandPollMs = now;

  HTTPClient http;
  const String url = buildUrl("/command?deviceId=" + String(DEVICE_ID));
  http.setTimeout(5000);

  if (!http.begin(url)) {
    Serial.println("Command request setup failed");
    return;
  }

  const int status = http.GET();
  if (status == HTTP_CODE_OK) {
    const String payload = http.getString();
    applyServerCommandPayload(payload, now);
  } else if (status == HTTP_CODE_NO_CONTENT) {
    serverCommand.receivedAtMs = now;
    Serial.println("Server command: no content");
  } else {
    Serial.print("Command request failed, HTTP status ");
    Serial.println(status);
  }

  http.end();
}

bool shouldRunAutoLight() {
  return !isnan(readings.lightPercent) && readings.lightPercent < AUTO_LIGHT_LOW_PERCENT;
}

bool shouldStartAutoWater(unsigned long now) {
  if (isnan(readings.soilMoisturePercent) || readings.soilMoisturePercent >= AUTO_SOIL_LOW_PERCENT) {
    return false;
  }
  return lastAutoWaterMs == 0 || now - lastAutoWaterMs >= AUTO_WATER_COOLDOWN_MS;
}

bool shouldRunAutoFan() {
  const bool tempHot = !isnan(readings.temperatureC) && readings.temperatureC >= AUTO_FAN_TEMP_ON_C;
  const bool humid = !isnan(readings.humidityPercent) && readings.humidityPercent >= AUTO_FAN_HUMIDITY_ON_PERCENT;
  if (tempHot || humid) {
    autoFanOn = true;
  }

  const bool tempCool = isnan(readings.temperatureC) || readings.temperatureC <= AUTO_FAN_TEMP_OFF_C;
  const bool humidityOk = isnan(readings.humidityPercent) || readings.humidityPercent <= AUTO_FAN_HUMIDITY_OFF_PERCENT;
  if (tempCool && humidityOk) {
    autoFanOn = false;
  }

  return autoFanOn;
}

void applyControls(unsigned long now) {
  if (!elapsed(now, lastControlMs, CONTROL_INTERVAL_MS)) {
    stopPumpIfExpired(now);
    return;
  }
  lastControlMs = now;

  stopPumpIfExpired(now);

  const bool useServer = serverControlActive(now);
  const ControlMode pumpMode = useServer ? serverCommand.pumpMode : ControlMode::Auto;
  const ControlMode lightMode = useServer ? serverCommand.lightMode : ControlMode::Auto;
  const ControlMode fanMode = useServer ? serverCommand.fanMode : ControlMode::Auto;

  if (!useServer) {
    serverPumpOnLatched = false;
  }

  if (pumpMode == ControlMode::ForcedOff) {
    setPump(false);
  } else if (pumpMode == ControlMode::ForcedOn) {
    if (!serverPumpOnLatched) {
      requestPumpRun(serverCommand.pumpDurationMs, "server");
      serverPumpOnLatched = true;
    }
  } else if (!pumpOn && shouldStartAutoWater(now)) {
    lastAutoWaterMs = now;
    requestPumpRun(PUMP_DEFAULT_ON_MS, useServer ? "server-auto" : "local-auto");
  }

  if (lightMode == ControlMode::ForcedOn) {
    setGrowLight(true);
  } else if (lightMode == ControlMode::ForcedOff) {
    setGrowLight(false);
  } else {
    setGrowLight(shouldRunAutoLight());
  }

  if (fanMode == ControlMode::ForcedOn) {
    setFan(true);
  } else if (fanMode == ControlMode::ForcedOff) {
    setFan(false);
  } else {
    setFan(shouldRunAutoFan());
  }
}

void sendTelemetry(unsigned long now) {
  if (!networkReady() || !elapsed(now, lastTelemetryMs, TELEMETRY_INTERVAL_MS)) {
    return;
  }
  lastTelemetryMs = now;

  JsonDocument doc;
  doc["deviceId"] = DEVICE_ID;
  doc["uptimeMs"] = now;
  doc["controlSource"] = serverControlActive(now) ? "server" : "local";
  doc["wifiRssi"] = WiFi.RSSI();

  JsonObject sensorObject = doc["sensors"].to<JsonObject>();
  setJsonFloat(sensorObject, "temperatureC", readings.temperatureC);
  setJsonFloat(sensorObject, "humidityPercent", readings.humidityPercent);
  setJsonFloat(sensorObject, "lightPercent", readings.lightPercent);
  setJsonFloat(sensorObject, "soilMoisturePercent", readings.soilMoisturePercent);
  sensorObject["lightRaw"] = readings.lightRaw;
  sensorObject["soilRaw"] = readings.soilRaw;
  sensorObject["updatedAtMs"] = readings.updatedAtMs;

  JsonObject actuatorObject = doc["actuators"].to<JsonObject>();
  actuatorObject["pump"] = pumpOn;
  actuatorObject["light"] = growLightOn;
  actuatorObject["fan"] = fanOn;

  JsonObject modeObject = doc["modes"].to<JsonObject>();
  modeObject["pump"] = controlModeName(serverControlActive(now) ? serverCommand.pumpMode : ControlMode::Auto);
  modeObject["light"] = controlModeName(serverControlActive(now) ? serverCommand.lightMode : ControlMode::Auto);
  modeObject["fan"] = controlModeName(serverControlActive(now) ? serverCommand.fanMode : ControlMode::Auto);

  String payload;
  serializeJson(doc, payload);

  HTTPClient http;
  const String url = buildUrl("/telemetry");
  http.setTimeout(5000);

  if (!http.begin(url)) {
    Serial.println("Telemetry request setup failed");
    return;
  }

  http.addHeader("Content-Type", "application/json");
  const int status = http.POST(payload);
  if (status >= 200 && status < 300) {
    Serial.println("Telemetry sent");
  } else {
    Serial.print("Telemetry failed, HTTP status ");
    Serial.println(status);
  }

  http.end();
}

void setup() {
  Serial.begin(115200);
  delay(1000);

  analogReadResolution(12);
  analogSetPinAttenuation(LIGHT_SENSOR_PIN, ADC_11db);
  analogSetPinAttenuation(SOIL_SENSOR_PIN, ADC_11db);

  pinMode(LIGHT_SENSOR_PIN, INPUT);
  pinMode(SOIL_SENSOR_PIN, INPUT);
  pinMode(PUMP_PIN, OUTPUT);
  pinMode(GROW_LIGHT_PIN, OUTPUT);
  pinMode(FAN_PIN, OUTPUT);

  setPump(false);
  setGrowLight(false);
  setFan(false);
  dht.begin();

  Serial.println();
  Serial.println("Plantera smart pot firmware");
  Serial.print("Device ID: ");
  Serial.println(DEVICE_ID);
  Serial.println("Sensors: DHT22, KY-018 light, analog soil moisture");
  Serial.println("Actuators: pump, grow light/LED strip, dome fan");

  if (!serverConfigured()) {
    Serial.println("Server URL is empty or local mode is forced; server features disabled");
  }

  startWifi();
}

void loop() {
  const unsigned long now = millis();

  maintainWifi(now);
  readSensors(now);
  pollServerCommand(now);
  applyControls(now);
  sendTelemetry(now);
}
