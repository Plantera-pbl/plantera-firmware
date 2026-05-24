# Plant Controller Architecture

## Purpose

This firmware runs an ESP32-C3 SuperMini as a plant controller.

It reads sensors, controls outputs, prints status over USB serial, and publishes sensor data to MQTT.

## Project Config

Secrets are not stored in `src/main.cpp`.

Firmware loads credentials from:

```cpp
#if __has_include("plantera_config.h")
#include "plantera_config.h"
#else
#include "plantera_config.example.h"
#endif
```

Use this workflow:

```text
copy include/plantera_config.example.h to include/plantera_config.h
fill in WiFi and MQTT values locally
do not commit include/plantera_config.h
```

`platformio.ini` enables ESP32-C3 native USB serial:

```text
ARDUINO_USB_MODE=1
ARDUINO_USB_CDC_ON_BOOT=1
```

## Hardware

| Device | ESP32 Pin | Role |
| --- | --- | --- |
| DHT22 | GPIO6 | Air temperature and air humidity |
| KY-018 light sensor AO | GPIO0 | Ambient light level |
| Soil moisture AO | GPIO1 | Soil moisture reading |
| Soil moisture VCC | GPIO3 | Switched sensor power to reduce corrosion |
| 5V pump MOSFET gate | GPIO4 | Pump control |
| 12V fan MOSFET gate | GPIO5 | Fan control |
| 5V LED board MOSFET gate | GPIO7 | Grow light / LED board control |

## Power

The ESP32 controls high-current devices through MOSFETs. The ESP32 GPIO pins do not power the pump, fans, or LED board directly.

High-current devices use external power:

| Device | Power |
| --- | --- |
| ESP32 | 5V into `5V`/`VBUS` pin or USB |
| Pump | External 5V |
| LED board | External 5V |
| Fans | 12V from MT3608 boost converter |

All grounds must be common:

```text
ESP32 GND
5V supply GND
MOSFET sources
MT3608 IN-/OUT-
```

Use star wiring for power. Do not route pump/LED/fan current through breadboard rails or through the ESP32.

## MOSFET Switching

The pump, fans, and LED board are low-side switched.

Pattern:

```text
Power + -> load +
load - -> MOSFET drain
MOSFET source -> GND
ESP32 GPIO -> 220 ohm -> MOSFET gate
MOSFET gate -> 10k -> GND
```

The `220 ohm` gate resistor limits gate charge current from the ESP32.

The `10k` pulldown keeps the MOSFET off during boot/reset.

Motors need flyback diodes:

```text
diode stripe -> motor +
diode other side -> motor -
```

## Sensors

### DHT22

Reads air temperature and humidity every 2 seconds.

Reason: DHT22 sensors are slow and should not be polled faster than about every 2 seconds.

### Light Sensor

The KY-018 gives low raw values in bright light and high raw values in darkness.

The firmware corrects this:

```text
0% = dark
100% = bright
```

### Soil Moisture

The soil sensor is powered from `GPIO3`, not constant `3V3`.

Reason: resistive soil probes corrode when continuously powered in wet soil.

Read cycle:

```text
turn soil sensor on
wait 1 second
take 10 analog readings
average them
turn soil sensor off
```

Soil is read every 5 seconds.

Soil percentage is corrected:

```text
0% = dry
100% = wet
```

Soil stability rule:

```text
3 consecutive readings within about 1.5% = stable
```

Pump control waits for stable soil. MQTT does not wait for stable soil.

## Outputs

### LED Board

Turns on when corrected light is below 90%.

Reason: if ambient light is not high enough, the LED board supplements it.

Logic:

```text
light < 90% -> LED board ON
light >= 90% -> LED board OFF
```

### Pump

Turns on when stable soil moisture is below 40%.

Current test behavior allows `0%` to trigger the pump.

Logic:

```text
soil stable AND soil moisture >= 0% AND soil moisture < 40% -> pump ON
otherwise -> pump OFF
```

The pump uses PWM on GPIO4.

PWM details:

```text
channel: 0
frequency: 20 kHz
resolution: 8-bit
soft-start ramp step: 16
soft-start ramp delay: 40 ms
```

Current setting:

```text
PWM duty = 255 / 255 = full power
```

Reason: the pump was noisy and not moving water at low PWM, so full power is used for testing.

Important: many small 5V pumps are not self-priming. If the pump only has an input tube in a bottle, it may spin but not suck water. Put the pump below the water level, fill the inlet tube, or submerge the pump if it is submersible.

### Fans

Two 12V fans are controlled together from GPIO5 through a MOSFET.

Logic:

```text
air humidity > 60% -> fans ON
air humidity <= 60% -> fans OFF
```

The fans use 12V from the MT3608 boost converter.

Set the MT3608 to 12V with a multimeter before connecting fans.

Fan control does not wait for soil stability. It only depends on DHT22 humidity.

## Boot Behavior

On boot, the firmware:

```text
starts serial at 115200
waits 1 second
sets ADC resolution to 12-bit
sets pump/fans/LED board off
sets soil sensor power off
starts DHT22
configures insecure TLS MQTT
blinks the LED board 3 times
connects WiFi
connects MQTT
```

The LED blink is a quick output/MOSFET wiring test.

## Connectivity

WiFi setup:

```text
station mode
auto reconnect enabled
WiFi sleep disabled
TX power set to 19.5 dBm
30 second connection timeout
```

MQTT setup:

```text
TLS port 8883
keepalive 30 seconds
socket timeout 10 seconds
connect timeout 10 seconds
```

The firmware calls reconnect logic in the main loop. If WiFi is lost, MQTT is disconnected and the loop waits before retrying.

TLS is currently configured with:

```cpp
wifiClient.setInsecure();
```

That encrypts traffic but does not verify the broker certificate.

## MQTT

Broker:

```text
c9f76c724f1e4252968735c5722b875b.s1.eu.hivemq.cloud:8883
```

Topic:

```text
iot/devices/4/data
```

Publish interval:

```text
5 seconds
```

Payload:

```json
{
  "light": 0,
  "soil-moisture": 0,
  "temp": 21.6,
  "ambient-humidity": 51.2,
  "soil-stable": true
}
```

If DHT22 has not produced a valid value yet, `temp` or `ambient-humidity` is sent as `null`.

If soil has not been read yet, `soil-moisture` is sent as `-1`.

MQTT publishing does not wait for soil stability. The payload includes `soil-stable` so the broker/UI can decide whether to trust the soil value for decisions.

`light` and `soil-moisture` are corrected raw ADC values:

```text
0 = low
4095 = high
```

The local `mqtt_subscribe.py` script converts these to percentages.

The helper scripts read MQTT connection values from environment variables:

```text
MQTT_HOST
MQTT_PORT
MQTT_USERNAME
MQTT_PASSWORD
MQTT_TOPIC
```

`mqtt_subscribe.py` displays missing values as `--`.

## Serial Output

Serial baud:

```text
115200
```

Example:

```text
Temp: 21.6 C | Humidity: 51.2 % | Light: 89.3 % (raw 439) | LED board: ON | Pump: OFF | Fans: OFF | Soil: 55.3 % (raw 1831, stable, sensor off)
```

## Thresholds

| Thing | Threshold | Action |
| --- | --- | --- |
| Light | `< 90%` | LED board ON |
| Soil moisture | `< 40%` and stable | Pump ON |
| Air humidity | `> 60%` | Fans ON |

There is no hysteresis implemented yet. Outputs switch directly at their thresholds.

## Firmware Filters And Modifiers

Implemented filters/modifiers:

| Feature | Why it exists |
| --- | --- |
| 1 second boot delay | Gives serial/sensors time to start |
| DHT22 2 second interval | DHT22 cannot be read reliably faster |
| Soil sensor power cycling | Reduces resistive probe corrosion |
| Soil 1 second settle delay | Lets sensor output stabilize after power-on |
| Soil 10-sample average | Reduces ADC noise |
| Soil stability check | Prevents pump decisions from noisy soil readings |
| Light inversion | KY-018 raw value is low when bright |
| Soil inversion | Soil raw value is high when dry |
| Pump PWM soft-start | Reduces motor startup shock/current spike |
| MQTT 5 second interval | Avoids publishing every loop |
| MQTT null/-1 placeholders | Keeps publishing even when some sensors are not ready |
| WiFi/MQTT reconnect | Recovers from network drops |

## Soil Calibration

Current observed scale:

| Reading | Meaning |
| --- | --- |
| 30-35% | Dry |
| 50-60% | Good / moist |
| 70%+ | Freshly watered |

Practical rule:

```text
below 40% = water
50-60% = good
70%+ = recently watered / wet
```

## Known Hardware Risks

### Brownouts

If the ESP32 resets when pump + LED are on, the 5V rail is dipping too low.

Fixes:

```text
use stronger 5V supply
use star wiring
add 1000uF near ESP32
add 1000uF-4700uF near loads
add flyback diodes to motors
use logic-level MOSFETs
avoid breadboards for motor current
```

### IRF3205

IRF3205 is not ideal for ESP32 3.3V gate drive.

It may work, but it may not fully turn on.

Better MOSFETs:

```text
IRLZ44N
IRLZ34N
FQP30N06L
AO3400 module
logic-level MOSFET module
```

### Pump Priming

If the pump makes noise but does not move water, the issue is usually not code.

Likely causes:

```text
pump is not primed
inlet tube contains air
pump is above water level
wrong inlet/outlet direction
air leak on inlet tube
too much lift height
low voltage at pump
```

## Current Test Deviations

These are temporary test settings:

```text
pump runs at full PWM
pump is allowed to run at 0% soil moisture
pump runs continuously while dry
```

For safer automatic watering later, revert to:

```text
do not pump at 0%
short pump burst
10 minute cooldown after watering
```
