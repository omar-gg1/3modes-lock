# Smart Lock — Mode 2 (Hybrid) Backend

The cloud half of **Mode 2**. The lock still makes every unlock decision
**locally** (nothing here can open the door); this backend only **records access
events** and serves them to the mobile app.

```
   ESP32 lock  --MQTT publish-->  Mosquitto broker  --subscribe-->  FastAPI
                                                                      |
                                                          stores in  SQLite
                                                                      |
   Mobile app  <----------------- REST (GET /events) -----------------+
```

## What's here

| Path | Role |
|------|------|
| `docker-compose.yml` | Brings up the broker + API together |
| `mosquitto/mosquitto.conf` | Broker config (dev: anonymous allowed) |
| `app/main.py` | FastAPI app + REST endpoints |
| `app/mqtt_client.py` | Subscribes to events, stores them |
| `app/database.py` | SQLAlchemy model (`AccessEvent`) + engine |
| `app/schemas.py` | Pydantic validation of the firmware's JSON |

## Run it (dev, on your PC)

```bash
cd backend
docker compose up --build
```

Then:
- **API + docs:** http://localhost:8000/docs
- **Broker:** `localhost:1883`

## Test it without the ESP32

Publish a fake access event with any MQTT client (the broker container ships
`mosquitto_pub`):

```bash
docker exec smartlock-mosquitto mosquitto_pub \
  -t "smartlock/lock01/events" \
  -m '{"event":"access","method":"face","id":1,"score":0.71,"result":"granted","ts":1718800000}'
```

Then read it back:

```bash
curl http://localhost:8000/events
curl http://localhost:8000/stats
```

## Event topic & payload

- **Topic:** `smartlock/<device_id>/events` (the API subscribes to
  `smartlock/+/events`, so any device id works).
- **Payload (JSON):**

  ```json
  {
    "event": "access",
    "method": "face",        // face | pin | button
    "id": 1,                  // matched user id (null if none)
    "score": 0.71,            // similarity (null for pin/button)
    "result": "granted",      // granted | denied
    "ts": 1718800000          // device epoch seconds
  }
  ```

This is exactly what the firmware publishes (next step).

## REST endpoints

| Method | Path | Purpose |
|--------|------|---------|
| GET | `/` | Health check |
| GET | `/events` | Recent events, newest first. Filters: `device_id`, `method`, `result`, `limit` |
| GET | `/events/{id}` | One event |
| GET | `/stats` | Counts by result and method |

## Going to the AWS demo later

For the demo host (AWS free tier — see the project decision notes):
- Switch `DATABASE_URL` to Postgres (RDS) — no code change needed.
- Lock the broker down: set `allow_anonymous false`, add a password file, and
  enable TLS on a `listener 8883`. Point the firmware at `mqtts://`.
- Run the same `docker compose` on an EC2 instance.
