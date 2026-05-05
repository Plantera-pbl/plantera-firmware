# Plantera Firmware

Firmware for an ESP32-C3 SuperMini smart pot with a dome. It reads DHT22 air temperature/humidity, KY-018 light level, and an analog 3-pin soil moisture module, then controls a pump, LED/grow-light strip, and dome fan.

The firmware can run with a server or fully locally. If WiFi credentials, server URL, or recent server commands are unavailable, it automatically uses local autonomous control.

## Hardware

Default pins are in `include/plantera_config.example.h` and can be changed in a local `include/plantera_config.h`.

| Function | Default GPIO | Notes |
| --- | ---: | --- |
| DHT22 data | 6 | Add the usual DHT data pull-up if the module does not include one. |
| KY-018 analog signal | 0 | ADC input. Default calibration assumes raw high is dark and raw low is bright. |
| Soil analog signal | 1 | ADC input. Calibrate dry/wet raw values for your exact module. |
| Water pump driver | 7 | Drive through a MOSFET/relay module, not directly from the ESP32. |
| LED/grow-light strip driver | 3 | Drive through a MOSFET/LED driver with external power. |
| Dome fan driver | 10 | Use a MOSFET/transistor driver and flyback protection if needed. |

Use a common ground between the ESP32, sensor modules, and actuator power supply. Pumps, fans, and LED strips should not be powered from the ESP32 3.3 V pin.

## Configuration

Local deployment settings live in `include/plantera_config.h`, which is ignored by git so credentials do not get committed. Use `include/plantera_config.example.h` as the template.

Important settings:

| Setting | Purpose |
| --- | --- |
| `DEVICE_ID` | Device identifier sent with telemetry and command requests. |
| `WIFI_SSID`, `WIFI_PASSWORD` | Leave empty for local-only operation. |
| `SERVER_BASE_URL` | Base URL used for `/telemetry` and `/command`. Leave empty for local-only operation. |
| `FORCE_LOCAL_MODE` | Set `true` to ignore WiFi/server even when configured. |
| `LIGHT_DARK_RAW`, `LIGHT_BRIGHT_RAW` | KY-018 calibration for 0% dark and 100% bright. |
| `SOIL_DRY_RAW`, `SOIL_WET_RAW` | Soil sensor calibration for 0% dry and 100% wet. |
| `ACTUATOR_ON_LEVEL`, `ACTUATOR_OFF_LEVEL` | Change for active-low relay modules. |

## Local Mode

Local mode is used when the server is not configured, WiFi is disconnected, `FORCE_LOCAL_MODE` is `true`, or the latest server command is older than `SERVER_STALE_MS`.

Default local behavior:

| Actuator | Behavior |
| --- | --- |
| Pump | Runs for `PUMP_DEFAULT_ON_MS` when soil moisture drops below `AUTO_SOIL_LOW_PERCENT`, then waits `AUTO_WATER_COOLDOWN_MS`. |
| LED/grow light | Turns on when light is below `AUTO_LIGHT_LOW_PERCENT`. |
| Fan | Turns on above the configured temperature or humidity thresholds and turns off using lower hysteresis thresholds. |

The pump always has a maximum run limit of `PUMP_MAX_ON_MS`, including server-triggered watering.

## Server API

Set `SERVER_BASE_URL` to the device base route, for example:

```text
http://192.168.1.50:3000/api/devices/plantera-pot-001
```

Telemetry is sent as JSON:

```http
POST {SERVER_BASE_URL}/telemetry
Content-Type: application/json
```

Example telemetry body:

```json
{
  "deviceId": "plantera-pot-001",
  "uptimeMs": 30000,
  "controlSource": "server",
  "wifiRssi": -55,
  "sensors": {
    "temperatureC": 24.7,
    "humidityPercent": 61.2,
    "lightPercent": 42.5,
    "soilMoisturePercent": 54.1,
    "lightRaw": 2350,
    "soilRaw": 2026,
    "updatedAtMs": 29800
  },
  "actuators": {
    "pump": false,
    "light": false,
    "fan": true
  },
  "modes": {
    "pump": "auto",
    "light": "auto",
    "fan": "on"
  }
}
```

Commands are polled periodically:

```http
GET {SERVER_BASE_URL}/command?deviceId=plantera-pot-001
```

The server can return `204 No Content`, or a JSON command:

```json
{
  "pump": "auto",
  "light": "auto",
  "fan": "auto",
  "pumpDurationMs": 3000
}
```

Command values can be `"auto"`, `"on"`, or `"off"`. Boolean values are also accepted, where `true` means forced on and `false` means forced off. Pump `"on"` is treated as a safe one-shot watering request and will not repeat until the server sends pump `"off"`/`"auto"` and then `"on"` again, or changes `pumpDurationMs`.

## Build And Upload

This project uses PlatformIO.

```powershell
pio run
pio run --target upload
pio device monitor --baud 115200
```

The configured PlatformIO board is `esp32-c3-devkitm-1`, which works as a practical target for common ESP32-C3 SuperMini boards. If your specific board package uses another target, change `board` in `platformio.ini`.

## Notes From `demo.txt`

The demo sketch was useful for the basic DHT22 polling interval, ADC light conversion idea, and simple non-blocking loop structure. This firmware keeps those useful parts but adds calibrated sensor mapping, actuator safety, WiFi/HTTP integration, server command parsing, and autonomous fallback behavior.
