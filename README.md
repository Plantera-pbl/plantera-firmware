# ESP32-C3 SuperMini Plant Sensors

PlatformIO Arduino firmware for an ESP32-C3 SuperMini, DHT22, and 3-pin KY-018 light sensor.

## Wiring

| Part | Pin | ESP32-C3 SuperMini |
| --- | --- | --- |
| DHT22 | VCC | 3V3 |
| DHT22 | GND | GND |
| DHT22 | DATA | GPIO6 |
| KY-018 | VCC | 3V3 |
| KY-018 | GND | GND |
| KY-018 | S / AO | GPIO0 |
| Soil moisture module | VCC | GPIO3 |
| Soil moisture module | GND | GND |
| Soil moisture module | AO | GPIO1 |
| Soil moisture module | DO | Not connected |
| 5V pump | + / VCC | External 5V + |
| 5V pump | - / GND | Pump MOSFET drain |
| Pump MOSFET | Source | GND |
| Pump MOSFET | Drain | Pump - / GND |
| Pump MOSFET | Gate | GPIO4 through 220 ohm resistor |
| Pump MOSFET | Gate to source | 10k pulldown resistor |
| MT3608 boost converter | IN+ | External 5V + |
| MT3608 boost converter | IN- | External 5V GND |
| MT3608 boost converter | OUT+ | Both 12V fan + wires |
| MT3608 boost converter | OUT- | GND / fan MOSFET source |
| 12V fans | + / VCC | MT3608 OUT+ |
| 12V fans | - / GND | Fan MOSFET drain |
| Fan MOSFET | Source | GND / MT3608 OUT- |
| Fan MOSFET | Drain | Both fan - / GND wires |
| Fan MOSFET | Gate | GPIO5 through 220 ohm resistor |
| Fan MOSFET | Gate to source | 10k pulldown resistor |
| 5V LED board | + / VCC | External 5V + |
| 5V LED board | - / GND | IRF3205 drain |
| IRF3205 | Source | GND |
| IRF3205 | Drain | LED board - / GND |
| IRF3205 | Gate | GPIO7 through 100-220 ohm resistor |
| IRF3205 | Gate to source | 10k pulldown resistor |
| External 5V supply | GND | ESP32-C3 GND / IRF3205 source |

If your DHT22 board is the bare 4-pin sensor instead of a 3-pin module, add a 4.7k to 10k pull-up resistor between DATA and 3V3.

The light percentage is corrected so `0%` means dark and `100%` means bright. The LED board turns on when the corrected light reading is below `90%`.

The 5V pump runs a short watering burst when trusted, stable soil moisture is below `40%`. It uses PWM soft-start and then runs at full power for the bounded burst.

The pump does not run on suspicious soil readings. `0%`, ADC rail values, readings during sensor warmup, and sudden jumps from the previous trusted value are treated as invalid.

The 12V fans turn on when air humidity from the DHT22 is above `60%`. They turn off at `60%` or below.

Do not power the 5V LED board, pump, or 12V fans from an ESP32 GPIO. Use external power supplies and connect every supply ground to ESP32 `GND`.

Add a flyback diode across the pump because it is an inductive motor: diode cathode/striped side to pump `+`, diode anode/non-striped side to pump `-`.

Add a flyback diode across each fan too: diode cathode/striped side to fan `+`, diode anode/non-striped side to fan `-`.

Set the MT3608 output to `12V` with a multimeter before connecting the fans. Two 12V fans may draw more current than a small MT3608 can supply from 5V. If the converter gets hot, the voltage drops, or the ESP32 resets, use a stronger 12V supply or a higher-current boost converter.

Note: the IRF3205 is not a logic-level MOSFET, so it may not turn fully on from the ESP32's 3.3V gate signal. If the LED board is dim, flickers, or the MOSFET gets warm, use a logic-level MOSFET such as `IRLZ44N`, `IRLZ34N`, `AO3400`, or a ready-made logic-level MOSFET module.

The soil moisture percentage is corrected so `0%` means dry and `100%` means wet. Use the module's `AO` analog pin; the `DO` digital pin is not used.

To reduce corrosion on resistive soil probes, the soil module's `VCC` is connected to `GPIO3` instead of constant `3V3`. The firmware powers the sensor for 1 second every 5 seconds, averages 10 readings, then turns it off.

Pump control waits until soil moisture is valid and stable. Serial output and MQTT publishing continue while the soil reading is warming up, settling, or invalid. A soil reading is considered stable after 3 consecutive trusted readings stay within about 1.5% of each other.

## Failsafes

Sensor values warm up for 15 seconds and need 3 accepted samples before they can control outputs. Light readings are averaged before smoothing. Soil readings already use a 10-sample average and are smoothed after validation.

Automatic watering is intentionally conservative. Soil readings at ADC rails, at or below `1%`, or jumping by more than `35%` from the previous trusted value are rejected. Rejected soil readings publish as unavailable and block the pump.

The pump is limited to a 3 second burst, at most 5 seconds per request, with a 60 second minimum off time, a 10 minute watering cooldown, and a 20 second runtime budget per hour. If the soil reading does not increase by at least 2 percentage points or recover to 50% within 2 minutes after watering, automatic watering locks with `pump-status` set to `locked_no_soil_response`.

On boot, the LED board should blink 3 times. If the serial monitor says `LED board: ON` but the LED board is still off, check the MOSFET wiring, shared ground, gate resistor, and 5V supply.

## Serial Monitor

Baud rate: `115200`

The sketch prints a status line every 0.5 seconds. DHT22 temperature and humidity are refreshed every 2 seconds because the DHT22 cannot be read reliably faster than that.

```text
Temp: 23.4 C (ok) | Humidity: 45.8 % (ok) | Light: 72.1 % (raw 1142, ok) | LED board: ON | Pump: OFF (ready) | Fans: OFF | Soil: 64.3 % (raw 1462, stable, ok, sensor off)
```

Upload and monitor with:

```sh
pio run --target upload
pio device monitor
```

## Broker Connection

This firmware publishes sensor readings over MQTT to the Plantera broker stack.

Before flashing, start the MQTT broker and API on the Mac:

```sh
brew install mosquitto
brew services start mosquitto
```

In `/Users/sava/Desktop/plantera-broker/.env`, enable MQTT:

```env
MQTT_ENABLED=true
MQTT_HOST=localhost
MQTT_PORT=1883
MQTT_TOPIC_PREFIX=iot/devices
```

Then start the API from `/Users/sava/Desktop/plantera-broker`:

```sh
python main.py
```

Register the ESP once:

```sh
curl -X POST http://localhost:8000/api/v1/devices \
  -H "Content-Type: application/json" \
  -d '{"name":"esp32","url":"","poll_interval":5}'
```

The firmware currently publishes to:

```text
iot/devices/4/data
```

If the registered device ID is not `4`, update `MQTT_TOPIC` in `include/plantera_config.h`.
