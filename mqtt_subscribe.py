import json
import os
import ssl

import paho.mqtt.client as mqtt


def load_dotenv(path=".env"):
    if not os.path.exists(path):
        return

    with open(path, "r", encoding="utf-8") as file:
        for line in file:
            line = line.strip()
            if not line or line.startswith("#") or "=" not in line:
                continue

            key, value = line.split("=", 1)
            os.environ.setdefault(key.strip(), value.strip().split()[0])


load_dotenv()

MQTT_HOST = os.getenv("MQTT_HOST", "")
MQTT_PORT = int(os.getenv("MQTT_PORT", "8883"))
MQTT_USERNAME = os.getenv("MQTT_USERNAME", "")
MQTT_PASSWORD = os.getenv("MQTT_PASSWORD", "")
MQTT_TOPIC = os.getenv("MQTT_TOPIC", "iot/devices/4/data")
MQTT_CONFIG_TOPIC = os.getenv(
    "MQTT_CONFIG_TOPIC",
    MQTT_TOPIC.removesuffix("/data") + "/config" if MQTT_TOPIC.endswith("/data") else "iot/devices/4/config",
)
ADC_MAX = 4095


def raw_to_percent(value):
    if value is None or float(value) < 0:
        return None
    return (float(value) * 100.0) / ADC_MAX


def format_percent(value):
    if value is None:
        return "--"
    return f"{float(value):.1f}%"


def format_temp(value):
    if value is None:
        return "--"
    return f"{float(value):.1f} C"


def on_connect(client, userdata, flags, reason_code, properties=None):
    print(f"connected: {reason_code}")
    client.subscribe([(MQTT_TOPIC, 0), (MQTT_CONFIG_TOPIC, 0)])
    print(f"subscribed: {MQTT_TOPIC}")
    print(f"subscribed: {MQTT_CONFIG_TOPIC}")


def on_message(client, userdata, message):
    payload = message.payload.decode("utf-8", errors="replace")

    if message.topic == MQTT_CONFIG_TOPIC:
        try:
            config = json.loads(payload)
            pretty = json.dumps(config, sort_keys=True)
        except json.JSONDecodeError:
            pretty = payload
        print(f"CONFIG retain={message.retain}: {pretty}")
        return

    try:
        data = json.loads(payload)
        light_percent = raw_to_percent(data["light"])
        soil_percent = raw_to_percent(data["soil-moisture"])
        temp = data["temp"]
        humidity_percent = data["ambient-humidity"]
        soil_stability = "stable" if data.get("soil-stable") else "stabilizing"
        soil_status = data.get("soil-status", soil_stability)
        pump_status = data.get("pump-status", "unknown")
        pump_running = "ON" if data.get("pump-running") else "OFF"
    except (KeyError, TypeError, ValueError, json.JSONDecodeError) as exc:
        print(f"{message.topic} invalid payload: {payload} ({exc})")
        return

    print(
        f"Temp: {format_temp(temp)} | "
        f"Air humidity: {format_percent(humidity_percent)} | "
        f"Light: {format_percent(light_percent)} | "
        f"Soil moisture: {format_percent(soil_percent)} ({soil_stability}, {soil_status}) | "
        f"Pump: {pump_running} ({pump_status})"
    )


def main():
    if not MQTT_HOST:
        raise SystemExit("Set MQTT_HOST before running this script")

    client = mqtt.Client(mqtt.CallbackAPIVersion.VERSION2)
    client.username_pw_set(MQTT_USERNAME, MQTT_PASSWORD)
    client.tls_set(tls_version=ssl.PROTOCOL_TLS_CLIENT)
    client.on_connect = on_connect
    client.on_message = on_message
    client.connect(MQTT_HOST, MQTT_PORT, keepalive=60)
    client.loop_forever()


if __name__ == "__main__":
    try:
        main()
    except KeyboardInterrupt:
        print("stopped")
