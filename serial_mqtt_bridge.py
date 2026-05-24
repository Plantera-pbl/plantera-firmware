import argparse
import glob
import json
import os
import ssl
import sys
import time
import urllib.error
import urllib.request

import paho.mqtt.client as mqtt
import serial


MQTT_HOST = os.getenv("MQTT_HOST", "")
MQTT_PORT = int(os.getenv("MQTT_PORT", "8883"))
MQTT_USERNAME = os.getenv("MQTT_USERNAME", "")
MQTT_PASSWORD = os.getenv("MQTT_PASSWORD", "")
MQTT_TOPIC = os.getenv("MQTT_TOPIC", "iot/devices/4/data")
DEVICE_ID = 4


def parse_args():
    parser = argparse.ArgumentParser(description="Read ESP serial JSON and forward it to the broker")
    parser.add_argument("--port", default="")
    parser.add_argument("--baud", default=115200, type=int)
    parser.add_argument("--broker-url", default="", help="Railway broker base URL, e.g. https://name.up.railway.app")
    parser.add_argument("--device-id", default=DEVICE_ID, type=int)
    parser.add_argument("--mqtt", action="store_true", help="Also publish to HiveMQ MQTT")
    return parser.parse_args()


def detect_port():
    ports = sorted(glob.glob("/dev/cu.usbmodem*"))
    if not ports:
        raise RuntimeError("No /dev/cu.usbmodem* serial device found. Is the ESP plugged in?")
    return ports[0]


def make_mqtt_client():
    if not MQTT_HOST:
        raise RuntimeError("Set MQTT_HOST before using --mqtt")

    client = mqtt.Client(mqtt.CallbackAPIVersion.VERSION2)
    client.username_pw_set(MQTT_USERNAME, MQTT_PASSWORD)
    client.tls_set(tls_version=ssl.PROTOCOL_TLS_CLIENT)
    client.connect(MQTT_HOST, MQTT_PORT, keepalive=60)
    client.loop_start()
    return client


def post_to_broker(broker_url: str, device_id: int, payload: str):
    url = broker_url.rstrip("/") + f"/api/v1/devices/{device_id}/push"
    request = urllib.request.Request(
        url,
        data=payload.encode("utf-8"),
        headers={"Content-Type": "application/json"},
        method="POST",
    )

    with urllib.request.urlopen(request, timeout=15) as response:
        body = response.read().decode("utf-8", errors="replace")
        return response.status, body


def main():
    args = parse_args()
    port = args.port or detect_port()
    client = make_mqtt_client() if args.mqtt else None

    if not args.broker_url and not args.mqtt:
        raise SystemExit("Pass --broker-url https://YOUR-RAILWAY-DOMAIN or use --mqtt")

    print(f"Reading serial from {port} at {args.baud}")
    if args.broker_url:
        print(f"Posting to broker {args.broker_url.rstrip('/')}/api/v1/devices/{args.device_id}/push")
    if client:
        print(f"Publishing to MQTT topic {MQTT_TOPIC}")

    with serial.Serial(port, args.baud, timeout=1) as ser:
        time.sleep(2)
        while True:
            line = ser.readline().decode("utf-8", errors="replace").strip()
            if not line:
                continue

            print(line)
            if not line.startswith("SERIAL_JSON "):
                continue

            payload = line.removeprefix("SERIAL_JSON ")
            try:
                json.loads(payload)
            except json.JSONDecodeError as exc:
                print(f"Skipping invalid JSON: {exc}", file=sys.stderr)
                continue

            if args.broker_url:
                try:
                    status, body = post_to_broker(args.broker_url, args.device_id, payload)
                    print(f"Broker POST status={status}: {body}")
                except urllib.error.URLError as exc:
                    print(f"Broker POST failed: {exc}", file=sys.stderr)

            if client:
                result = client.publish(MQTT_TOPIC, payload, qos=1)
                result.wait_for_publish()
                print(f"MQTT published rc={result.rc}: {payload}")


if __name__ == "__main__":
    try:
        main()
    except KeyboardInterrupt:
        print("Stopped.")
