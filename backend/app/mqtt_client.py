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

from .database import SessionLocal, AccessEvent, WifiStatus
from .schemas import AccessEventIn
from . import reachability, commands

log = logging.getLogger("mqtt")

MQTT_BROKER_HOST = os.environ.get("MQTT_BROKER_HOST", "localhost")
MQTT_BROKER_PORT = int(os.environ.get("MQTT_BROKER_PORT", "1883"))
# '+' is a single-level wildcard: matches any device id in that slot.
MQTT_EVENT_TOPIC = os.environ.get("MQTT_EVENT_TOPIC", "smartlock/+/events")
# Acks from the lock (command results), same wildcard shape.
MQTT_ACK_TOPIC = os.environ.get("MQTT_ACK_TOPIC", "smartlock/+/acks")
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
        log.info("connected to broker, subscribing to %s and %s",
                 MQTT_EVENT_TOPIC, MQTT_ACK_TOPIC)
        client.subscribe(MQTT_EVENT_TOPIC)
        client.subscribe(MQTT_ACK_TOPIC, qos=1)
    else:
        log.error("broker connect failed: %s", reason_code)


def publish(topic: str, payload: dict) -> None:
    """Publish a JSON command to the lock (QoS 1, non-retained)."""
    if _client is None:
        log.warning("publish before client init: %s", topic)
        return
    _client.publish(topic, json.dumps(payload), qos=1, retain=False)


def _on_message(client, userdata, msg):
    """Route an inbound message: acks -> command registry, events -> DB."""
    device_id = _device_id_from_topic(msg.topic)
    # Any message from the device proves it is alive.
    reachability.mark_heard(device_id)
    kind = msg.topic.split("/")[-1] if "/" in msg.topic else ""

    try:
        payload = json.loads(msg.payload.decode("utf-8"))
    except Exception as e:
        # A bad message must never crash the subscriber — log and drop it.
        log.warning("dropping bad message on %s: %s", msg.topic, e)
        return

    if kind == "acks":
        commands.handle_ack(payload)
        from . import ws
        ws.broadcast_threadsafe({"kind": "ack", "device_id": device_id, **payload})
        return

    # wifi status branch — a different shape on the same events topic. Upsert the
    # latest SSID/connected so the app can show which network the lock is on.
    if payload.get("event") == "wifi":
        session = SessionLocal()
        try:
            row = session.get(WifiStatus, device_id) or WifiStatus(device_id=device_id)
            row.ssid = str(payload.get("ssid", ""))
            row.connected = bool(payload.get("connected", False))
            session.merge(row)
            session.commit()
            log.info("wifi status: %s ssid=%s connected=%s",
                     device_id, row.ssid, row.connected)
            from . import ws
            ws.broadcast_threadsafe({"kind": "wifi", "device_id": device_id, **payload})
        except Exception as e:
            session.rollback()
            log.error("failed to store wifi status: %s", e)
        finally:
            session.close()
        return

    # events branch — original behavior (validate + store)
    try:
        evt = AccessEventIn(**payload)         # raises if the shape is wrong
    except Exception as e:
        log.warning("dropping bad event on %s: %s", msg.topic, e)
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
        from . import ws
        ws.broadcast_threadsafe({"kind": "event", "device_id": device_id, **payload})
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
