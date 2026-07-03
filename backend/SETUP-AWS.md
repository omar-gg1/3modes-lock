# Deploying the Smart Lock backend to AWS EC2

The lock talks straight to the cloud: **device → MQTT → EC2 → MySQL**. Your PC is
only used to push code; GitHub Actions builds the image and deploys to EC2.

```
 push to GitHub ─► Actions builds API image ─► GHCR ─► SSH to EC2 ─► docker compose up
                                                                          │
 ESP32 ──MQTT──────────────────────────────────────────────────────────► stack
 you  ──HTTP──► REST :8000 / phpMyAdmin :8080 ────────────────────────────┘
```

---

## Part A — Launch the EC2 instance (AWS console, ~10 min)

1. **EC2 → Launch instance.**
   - **Name:** `smartlock`
   - **AMI:** Ubuntu Server 22.04 LTS (64-bit x86) — free-tier eligible
   - **Instance type:** `t3.micro` (or `t2.micro`) — free-tier eligible
   - **Key pair:** Create a new one, name it `smartlock-key`, type **ED25519**,
     format **.pem**. **Download it** — you cannot re-download it later. This is
     your SSH key; keep it safe.
   - **Storage:** 20 GB gp3 (default 8 GB is too small for the images).

2. **Network / Security group** — create one named `smartlock-sg` with these
   inbound rules (Source = **My IP** for everything except MQTT — see the note):

   | Type | Port | Source | Why |
   |------|------|--------|-----|
   | SSH | 22 | My IP | you connect |
   | Custom TCP | 8000 | My IP | REST API |
   | Custom TCP | 8080 | My IP | phpMyAdmin — **never 0.0.0.0** |
   | Custom TCP | 1883 | *see note* | MQTT (the lock) |

   **MQTT note:** the lock connects from your home network, whose IP may change.
   Options: (a) set 1883 Source to your home IP and update it if it changes, or
   (b) `0.0.0.0/0` — allowed here ONLY because the broker requires a password.
   Do **NOT** open 3306 (MySQL) to anyone — it stays internal to the containers.

3. **Launch.** Note the instance's **Public IPv4 address** — call it `EC2_IP`.

---

## Part B — First-time server setup (SSH in once, ~10 min)

From your PC (PowerShell), SSH in with the key you downloaded:

```powershell
ssh -i path\to\smartlock-key.pem ubuntu@EC2_IP
```

Then, on the server, run these:

```bash
# 1. Install Docker + compose plugin
sudo apt-get update && sudo apt-get install -y ca-certificates curl git
curl -fsSL https://get.docker.com | sudo sh
sudo usermod -aG docker $USER
newgrp docker      # apply the group without logging out

# 2. Clone the repo (public clone is fine; the deploy step logs into GHCR later)
git clone https://github.com/omar-gg1/3modes-lock.git ~/smartlock
cd ~/smartlock/backend

# 3. Create the production .env with STRONG unique secrets (NOT the local ones!)
cp .env.example .env
nano .env      # fill in real strong passwords + IMAGE_PREFIX=ghcr.io/omar-gg1/3modes-lock

# 4. Generate the broker password file (must match MQTT_USERNAME/PASSWORD in .env)
docker run --rm -v "$PWD/mosquitto:/m" eclipse-mosquitto:2 \
  mosquitto_passwd -b -c /m/passwd <MQTT_USERNAME> <MQTT_PASSWORD>

# 5. Log into GHCR so the host can pull the private API image
#    Use a GitHub Personal Access Token (classic) with read:packages scope.
echo <YOUR_GHCR_TOKEN> | docker login ghcr.io -u omar-gg1 --password-stdin

# 6. First manual bring-up (later deploys are automatic via Actions)
docker compose -f docker-compose.prod.yml pull
docker compose -f docker-compose.prod.yml up -d
docker compose -f docker-compose.prod.yml ps
```

Check it: `http://EC2_IP:8000/docs` and `http://EC2_IP:8080` (phpMyAdmin).

---

## Part C — Wire up auto-deploy (GitHub, ~5 min)

In the GitHub repo → **Settings → Secrets and variables → Actions → New secret**,
add these four:

| Secret | Value |
|--------|-------|
| `EC2_HOST` | the `EC2_IP` |
| `EC2_USER` | `ubuntu` |
| `EC2_SSH_KEY` | the **full contents** of `smartlock-key.pem` |
| `GHCR_TOKEN` | a GitHub PAT (classic) with `read:packages` scope |

Now every push to `master` that touches `backend/` builds the image and deploys
it automatically. You can also trigger it manually: **Actions → Build & Deploy →
Run workflow**.

---

## Part D — Point the lock at the cloud

In the firmware's gitignored `wifi_config.h`:

```c
#define MQTT_BROKER_URI "mqtt://EC2_IP:1883"
#define MQTT_USERNAME   "<same as server .env>"
#define MQTT_PASSWORD   "<same as server .env>"
```

Rebuild + flash. The lock now reports to AWS. Watch the events land in
phpMyAdmin at `http://EC2_IP:8080`.

---

## Hardening (do before calling it "production")

- **TLS:** run MQTT over `mqtts://` on 8883 with a cert (Let's Encrypt or
  self-signed) so credentials aren't sent in clear text over the internet.
- **HTTPS** for the API/phpMyAdmin (a reverse proxy like Caddy gives you certs
  automatically).
- Restrict 8080 to your IP only; consider putting phpMyAdmin behind the proxy.
