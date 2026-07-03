"""MQTT subscriber — ingests access events from the lock into the database.

Subscribes to `smartlock/<device_id>/events`. For every message it:
  1. extracts the device_id from the topic,
  2. validates the JSON payload against AccessEventIn,
  3. stores an AccessEvent row.

Runs on a background thread (paho's loop_start), so it lives alongside the
FastAPI request loop without blocking it.
"""
import json
import logging
import os

import paho.mqtt.client as mqtt

from .database import SessionLocal, AccessEvent
from .schemas import AccessEventIn

log = logging.getLogger("mqtt")

MQTT_BROKER_HOST = os.environ.get("MQTT_BROKER_HOST", "localhost")
MQTT_BROKER_PORT = int(os.environ.get("MQTT_BROKER_PORT", "1883"))
# '+' is a single-level wildcard: matches any device id in that slot.
MQTT_EVENT_TOPIC = os.environ.get("MQTT_EVENT_TOPIC", "smartlock/+/events")
# Broker auth (empty = anonymous, for a local no-auth broker). On AWS the broker
# requires these, matching the firmware's MQTT_USERNAME/MQTT_PASSWORD.
MQTT_USERNAME = os.environ.get("MQTT_USERNAME", "")
MQTT_PASSWORD = os.environ.get("MQTT_PASSWORD", "")

_client: mqtt.Client | None = None


def _device_id_from_topic(topic: str) -> str:
    """smartlock/<device_id>/events  ->  <device_id>."""
    parts = topic.split("/")
    return parts[1] if len(parts) >= 3 else "unknown"


def _on_connect(client, userdata, flags, reason_code, properties=None):
    if reason_code == 0:
        log.info("connected to broker, subscribing to %s", MQTT_EVENT_TOPIC)
        client.subscribe(MQTT_EVENT_TOPIC)
    else:
        log.error("broker connect failed: %s", reason_code)


def _on_message(client, userdata, msg):
    """Validate and persist one incoming access event."""
    device_id = _device_id_from_topic(msg.topic)
    try:
        payload = json.loads(msg.payload.decode("utf-8"))
        evt = AccessEventIn(**payload)         # raises if the shape is wrong
    except Exception as e:
        # A bad message must never crash the subscriber — log and drop it.
        log.warning("dropping bad message on %s: %s", msg.topic, e)
        return

    session = SessionLocal()
    try:
        row = AccessEvent(
            device_id=device_id,
            event=evt.event,
            method=evt.method,
            result=evt.result,
            user_id=evt.id,
            score=evt.score,
            device_ts=evt.ts,
        )
        session.add(row)
        session.commit()
        log.info("stored event: %s/%s %s by %s (id=%s score=%s)",
                 device_id, evt.event, evt.result, evt.method, evt.id, evt.score)
    except Exception as e:
        session.rollback()
        log.error("failed to store event: %s", e)
    finally:
        session.close()


def start():
    """Connect and start the background network loop. Called on API startup."""
    global _client
    _client = mqtt.Client(callback_api_version=mqtt.CallbackAPIVersion.VERSION2)
    if MQTT_USERNAME:
        _client.username_pw_set(MQTT_USERNAME, MQTT_PASSWORD)
    _client.on_connect = _on_connect
    _client.on_message = _on_message

    log.info("connecting to broker %s:%s", MQTT_BROKER_HOST, MQTT_BROKER_PORT)
    # connect_async + loop_start so we don't block if the broker isn't up yet;
    # paho keeps retrying in the background.
    _client.connect_async(MQTT_BROKER_HOST, MQTT_BROKER_PORT, keepalive=60)
    _client.loop_start()


def stop():
    """Clean shutdown of the network loop. Called on API shutdown."""
    if _client is not None:
        _client.loop_stop()
        _client.disconnect()
