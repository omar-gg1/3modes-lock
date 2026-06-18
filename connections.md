# Smart Door Lock — Wiring & Connections Guide

> **Board:** ESP32‑S3 (Gooouuu Tech ESP32‑S3‑CAM, octal PSRAM, OV3660 camera on board)
> **Firmware target:** `esp32s3` · ESP‑IDF · custom partition table (`partitions.csv`)
> **Last updated:** 2026‑06‑19
>
> Every pin number in this document is taken **directly from the firmware source** — not the
> proposal. Where the proposal and the code disagree (e.g. the proposal says "OV2640", the code
> drives an OV3660; the proposal says "I2C keypad", the code uses a GPIO matrix keypad), the
> **code is the source of truth** and is what's documented here. File references are given so you
> can verify each pin yourself.

---

## 0. TL;DR — the short version

You are wiring **four** things to the ESP32‑S3:

| Thing | Wires to ESP32 | Needs external power? |
|-------|----------------|------------------------|
| **4×4 Keypad** | 8 GPIOs (rows + cols) | No — runs off the ESP32's 3.3 V logic |
| **Relay module** | 1 GPIO (signal) + VCC + GND | Relay coil powered from 5 V |
| **12 V Solenoid lock** | *Does NOT touch the ESP32.* It goes through the relay contacts | Yes — its own 12 V supply |
| **Camera (OV3660)** | Nothing — it's on the same board, already wired | No |

The single most important safety rule: **the 12 V solenoid never connects to an ESP32 pin.**
The ESP32 only ever switches the relay's *signal* pin (3.3 V logic). The relay's metal contacts
do the actual 12 V switching, fully isolated from the microcontroller. Wire it any other way and
you destroy the board (see [§6 Why](#6-why-it-is-wired-this-way-read-this)).

---

## 1. Power — where + and − go, and why it matters

There are **two separate power domains**. Keeping them straight is the whole game.

### Domain A — Logic / 5 V (powers the ESP32 + keypad + relay coil)

```
 5 V USB or 5 V supply  ── (+) ──►  ESP32-S3  5V pin   ──►  on-board 3.3 V regulator ──► chip + camera
                        ── (−) ──►  ESP32-S3  GND pin
```

* During development just use the **USB‑C cable** — it provides the 5 V and the serial console.
* The ESP32's onboard regulator drops 5 V → 3.3 V for the chip and the camera. You do **not** feed
  3.3 V in from outside.
* The keypad draws negligible current and is powered straight from the ESP32's GPIO pins (no VCC
  wire needed — see [§3](#3-keypad-4×4-matrix)).

### Domain B — Lock / 12 V (powers the solenoid only)

```
 12 V supply  ── (+ , RED of supply) ──►  relay COM  (common contact)
 12 V supply  ── (− , BLACK of supply) ─►  solenoid BLACK wire   ◄── this closes the circuit
```

The solenoid's **RED** wire is fed 12 V *through the relay* (see [§4](#4-solenoid-lock-12-v--the-relay)).
The solenoid's **BLACK** wire goes straight back to the **−** of the 12 V supply.

### The one wire that ties both domains together: **common ground**

```
   ESP32 GND  ───────┬───────  5 V supply (−)
                     │
                     └───────  12 V supply (−)   ◄── MUST be joined
```

**Why "must":** the relay is told to switch by an ESP32 GPIO. That GPIO outputs a voltage
*relative to the ESP32's ground*. If the relay board's ground and the ESP32's ground aren't the
same reference point, the relay sees a floating/garbage signal and either chatters, never
triggers, or back‑feeds voltage into the ESP32. So **all grounds (−) are tied together** — 5 V
(−), 12 V (−), ESP32 GND, relay GND. The **positives stay separate** (5 V and 12 V never touch).

> If you only remember one sentence about power: **all minuses join; the pluses (5 V and 12 V)
> stay apart.**

---

## 2. Full pin map (master table)

Verified against source. Click a file link to confirm any row.

| Peripheral | Signal | ESP32‑S3 GPIO | Source |
|------------|--------|---------------|--------|
| **Solenoid lock** | Relay signal (IN) | **GPIO 21** | [lock_ctrl.c:7](components/lock_ctrl/lock_ctrl.c#L7) |
| **BOOT button** | Manual‑unlock input | **GPIO 0** | [button_ctrl.c:6](components/button_ctrl/button_ctrl.c#L6) |
| **Keypad** | Row 0 (1 2 3 A) | **GPIO 38** | [keypad_ctrl.c:18](components/keypad_ctrl/keypad_ctrl.c#L18) |
| **Keypad** | Row 1 (4 5 6 B) | **GPIO 39** | [keypad_ctrl.c:19](components/keypad_ctrl/keypad_ctrl.c#L19) |
| **Keypad** | Row 2 (7 8 9 C) | **GPIO 40** | [keypad_ctrl.c:20](components/keypad_ctrl/keypad_ctrl.c#L20) |
| **Keypad** | Row 3 (\* 0 # D) | **GPIO 41** | [keypad_ctrl.c:21](components/keypad_ctrl/keypad_ctrl.c#L21) |
| **Keypad** | Col 0 (1 4 7 \*) | **GPIO 42** | [keypad_ctrl.c:33](components/keypad_ctrl/keypad_ctrl.c#L33) |
| **Keypad** | Col 1 (2 5 8 0) | **GPIO 47** | [keypad_ctrl.c:34](components/keypad_ctrl/keypad_ctrl.c#L34) |
| **Keypad** | Col 2 (3 6 9 #) | **GPIO 1** | [keypad_ctrl.c:35](components/keypad_ctrl/keypad_ctrl.c#L35) |
| **Keypad** | Col 3 (A B C D) | **GPIO 2** | [keypad_ctrl.c:36](components/keypad_ctrl/keypad_ctrl.c#L36) |
| **Camera** | XCLK | GPIO 15 | [camera_ctrl.c:6](components/camera_ctrl/camera_ctrl.c#L6) |
| **Camera** | SIOD (SDA) | GPIO 4 | [camera_ctrl.c:8](components/camera_ctrl/camera_ctrl.c#L8) |
| **Camera** | SIOC (SCL) | GPIO 5 | [camera_ctrl.c:9](components/camera_ctrl/camera_ctrl.c#L9) |
| **Camera** | D7..D0 | 16,17,18,12,10,8,9,11 | [camera_ctrl.c:10‑17](components/camera_ctrl/camera_ctrl.c#L10-L17) |
| **Camera** | VSYNC / HREF / PCLK | 6 / 7 / 13 | [camera_ctrl.c:18‑20](components/camera_ctrl/camera_ctrl.c#L18-L20) |
| **Camera** | PWDN / RESET | −1 (not used) | [camera_ctrl.c:4‑5](components/camera_ctrl/camera_ctrl.c#L4-L5) |

> **Camera note:** all the camera pins are already routed on the ESP32‑S3‑CAM PCB. You do **not**
> wire the camera by hand — it's listed only so you know those GPIOs are *taken* and must not be
> reused for anything else.

**No pin collisions.** The lock + button + keypad pins (21, 0, 38–42, 47, 1, 2) are all clear of
the camera's pins and clear of the GPIOs the board reserves for USB (19/20) and octal PSRAM/flash
(26–32, 33–37 region). You have safe headroom if you add the ATECC608A later (see [§7](#7-future-work-reserved--not-yet-wired)).

---

## 3. Keypad (4×4 matrix)

A membrane keypad is just 8 wires: 4 **rows** and 4 **columns**. No power pin, no ground pin — the
ESP32 powers it by driving the rows and reading the columns. The firmware scans it
([keypad_ctrl.c:95](components/keypad_ctrl/keypad_ctrl.c#L95)).

### Wiring

The keypad's flat cable has 8 pins. Looking at the keypad **face‑on**, pin 1 is the rightmost:

| Keypad ribbon pin | Role | ESP32‑S3 GPIO |
|-------------------|------|---------------|
| Pin 8 (leftmost) | Row 0 | GPIO 38 |
| Pin 7 | Row 1 | GPIO 39 |
| Pin 6 | Row 2 | GPIO 40 |
| Pin 5 | Row 3 | GPIO 41 |
| Pin 4 | Col 0 | GPIO 42 |
| Pin 3 | Col 1 | GPIO 47 |
| Pin 2 | Col 2 | GPIO 1 |
| Pin 1 (rightmost) | Col 3 | GPIO 2 |

> This row/col ↔ ribbon‑pin mapping is the **multimeter‑verified** order recorded in the source
> ([keypad_ctrl.c:11‑37](components/keypad_ctrl/keypad_ctrl.c#L11-L37)). If your keypad's ribbon
> order differs, that's fine — what matters is that *rows go to 38–41 and cols go to 42/47/1/2*.
> Re‑probe with a multimeter (continuity between a pressed key's row and column) if buttons map
> wrong.

### How the scan works (so it's not magic)

* Rows are **outputs**, idle HIGH. Columns are **inputs with internal pull‑ups** (idle HIGH).
* To scan, the firmware drives **one row LOW** at a time and reads all 4 columns. If a key in that
  row is pressed, it bridges the row to a column, pulling that column LOW. The (row, column) pair
  identifies the key via `KEYMAP` ([keypad_ctrl.c:40](components/keypad_ctrl/keypad_ctrl.c#L40)).
* A 30 ms debounce + edge detection means holding a key fires it **once**, not repeatedly
  ([keypad_ctrl.c:121](components/keypad_ctrl/keypad_ctrl.c#L121)).

### What the keys do (from [main.c](main/main.c))

* `1234#` → unlock ([main.c:21](main/main.c#L21))
* `*9999#` → arm enrollment, then show your face within 10 s ([main.c:22](main/main.c#L22), [main.c:50](main/main.c#L50))
* `*` mid‑entry → clear and restart
* `#` → commit the entry
* `A`–`D` → currently ignored (reserved)

> **Heads‑up — the GPIO 48 story:** the source notes Col 2 *used* to be GPIO 48 but that pin
> drives the board's onboard RGB LED and sat stuck LOW, breaking the scan. It was moved to
> **GPIO 1** ([keypad_ctrl.c:30‑31](components/keypad_ctrl/keypad_ctrl.c#L30-L31)). Don't move it
> back.

---

## 4. Solenoid lock + the relay

This is the part where wiring matters for safety. The **12 V solenoid is switched by a relay**,
and the **ESP32 only controls the relay's logic input**.

### The three connections from ESP32 → relay module

| Relay module pin | Connect to | Why |
|------------------|------------|-----|
| **IN / SIG** | **GPIO 21** | The control signal the firmware toggles ([lock_ctrl.c:7](components/lock_ctrl/lock_ctrl.c#L7)) |
| **VCC** | **5 V** | Powers the relay's coil + opto‑isolator |
| **GND** | **ESP32 GND** | Shared reference (the common ground from [§1](#the-one-wire-that-ties-both-domains-together-common-ground)) |

### The 12 V side (relay contacts → solenoid)

The relay has three screw terminals on its high‑voltage side: **COM**, **NO** (normally open),
**NC** (normally closed). The solenoid has two wires: **RED (+)** and **BLACK (−)**.

```
   12 V supply (+, red)  ─────►  COM  ───[relay]───  NO  ─────►  Solenoid RED (+)
   12 V supply (−, black) ───────────────────────────────────►  Solenoid BLACK (−)
```

* Wire the **solenoid RED through COM → NO**. With the relay idle (de‑energized), NO is open, so
  the solenoid gets **no power → bolt stays out → door LOCKED**. When the firmware energizes the
  relay, NO closes, 12 V reaches the solenoid, and it retracts → **UNLOCKED**.
* The solenoid **BLACK** wire goes **straight to the 12 V supply −**.

> **Which terminal — NO or NC?** Use **NO (normally open)**. This is a **fail‑secure**
> arrangement: if power dies or the ESP32 hangs, the relay falls open and the door stays
> **locked**. (If you ever want fail‑*safe* — door pops open on power loss — you'd use NC instead.
> The proposal calls for fail‑secure, so use NO.)

### The firmware's view of "locked" vs "unlocked"

The firmware drives **GPIO 21 HIGH = locked, LOW = unlocked**
([lock_ctrl.c:8‑9](components/lock_ctrl/lock_ctrl.c#L8-L9)), and on boot it forces the locked
state ([lock_ctrl.c:38](components/lock_ctrl/lock_ctrl.c#L38)). On an unlock it holds open for
3 s then auto‑relocks ([lock_ctrl.c:10](components/lock_ctrl/lock_ctrl.c#L10)).

> **Relay polarity — check yours.** Some relay modules are **active‑LOW** (the coil energizes when
> IN is LOW), others **active‑HIGH**. The firmware treats `LEVEL_UNLOCKED = 0` (GPIO LOW = unlock).
> That assumes an **active‑LOW** relay *or* a wiring where GPIO‑LOW should close the contact. If
> your relay clicks the *wrong way* (locks when it should unlock), you have two clean fixes:
> 1. Swap the solenoid wire from **NO** to **NC** on the relay, **or**
> 2. Flip `LEVEL_LOCKED`/`LEVEL_UNLOCKED` in [lock_ctrl.c:8‑9](components/lock_ctrl/lock_ctrl.c#L8-L9).
>
> Decide this once with the door **disassembled** so a wrong guess can't lock you out.

### Flyback / the solenoid's nasty inductive kick

A solenoid is a coil. When the relay cuts its 12 V, the collapsing magnetic field generates a
**large reverse voltage spike**. A bare relay contact will arc and degrade, and the spike can
glitch nearby electronics. **Fit a flyback diode** (e.g. **1N4007**) across the solenoid
terminals — **band/stripe toward +12 V (the red wire)**, plain end toward black. Most relay
modules already have coil‑side protection, but the **solenoid** itself needs its own diode. This
isn't optional for reliability.

---

## 5. BOOT button (already on the board)

* **GPIO 0**, active‑LOW with internal pull‑up ([button_ctrl.c:6](components/button_ctrl/button_ctrl.c#L6), [button_ctrl.c:46](components/button_ctrl/button_ctrl.c#L46)).
* A short press triggers a manual unlock ([main.c:112‑114](main/main.c#L112-L114)). No wiring —
  it's the board's existing BOOT button.
* GPIO 0 is also the **bootstrapping pin**: hold it at power‑up to enter download mode. That's
  normal and doesn't conflict with using it as a button afterward.

---

## 6. Why it is wired this way (read this)

A quick "nothing here is magic" rundown of the non‑obvious choices:

1. **Why a relay instead of driving the solenoid from a GPIO?**
   A GPIO sources ~20 mA at 3.3 V. A 12 V solenoid pulls **hundreds of mA**. Connecting it
   directly would instantly fry the pin. The relay is a switch the tiny GPIO can flick, while the
   beefy 12 V flows through isolated metal contacts. The opto‑isolator on most relay boards adds a
   second layer of isolation between the 12 V world and the ESP32.

2. **Why tie all grounds but keep 5 V and 12 V apart?**
   Logic signals are voltages *measured against ground*. Two devices that don't share a ground
   don't agree on what "0 V" is, so the signal is meaningless. Joining grounds gives one shared
   reference. The **positives** stay apart because 12 V on a 5 V/3.3 V pin is instantly fatal to
   the chip.

3. **Why fail‑secure (NO terminal)?**
   A door lock that springs open the moment the power blinks is a security hole. With NO, loss of
   power = relay open = no 12 V to the solenoid = **locked**. The backup key cylinder and the PIN
   pad (and the BOOT button during dev) are the deliberate ways back in.

4. **Why does the camera need no wiring?**
   On the ESP32‑S3‑CAM the OV3660 is soldered to the same PCB; its 16 DVP/SCCB lines are already
   routed to the GPIOs in [§2](#2-full-pin-map-master-table). Those pins are simply off‑limits for
   anything else.

5. **Why these particular GPIOs for the keypad?**
   38–42 and 47 are plain GPIOs with no special boot/flash/PSRAM role on the S3, so they're safe
   to repurpose. GPIO 48 was avoided because it drives the onboard RGB LED (see the GPIO 48 note in
   [§3](#3-keypad-4×4-matrix)).

---

## 7. Future work (reserved — not yet wired)

Per the current plan, these are **not** connected yet and don't appear in the firmware:

* **ATECC608A crypto co‑processor** — *unobtainable for now.* Hardware‑backed key storage is
  deferred to future work. **Encryption will be done on‑device in software** (AES‑256 for face
  embeddings) for the time being. When the chip is sourced it speaks **I²C** and would take two
  free GPIOs (SDA/SCL) plus 3.3 V and GND. **Do not** reuse the camera's SCCB pins (4/5) for it —
  pick two unused GPIOs.
* **Mode 2 (Hybrid)** and **Mode 3 (Cloud‑Assisted)** — Wi‑Fi is already on‑chip (internal radio,
  no pins). Mode 1 (fully local) is the working baseline; Modes 2 & 3 come next, then the Flutter
  mobile app.
* **18650 battery backup** — would feed the 5 V domain through a boost/charge board; not part of
  the current bench wiring.

---

## 8. Bench wiring checklist (assembly order)

1. [ ] Power the ESP32‑S3 from **USB‑C** (gives 5 V + serial).
2. [ ] Relay **VCC → 5 V**, **GND → ESP32 GND**, **IN → GPIO 21**.
3. [ ] **Join grounds:** ESP32 GND ↔ 12 V supply (−). (5 V and 12 V positives stay separate.)
4. [ ] 12 V (+) → relay **COM**; relay **NO** → solenoid **RED**; solenoid **BLACK** → 12 V (−).
5. [ ] Flyback **diode across the solenoid**, stripe toward the red/+12 V wire.
6. [ ] Keypad rows → **38, 39, 40, 41**; cols → **42, 47, 1, 2**.
7. [ ] Power up. Console should print `Lock initialized in LOCKED state (GPIO 21)`.
8. [ ] Test in this order: **BOOT tap** → unlock; **`1234#`** → unlock; **`*9999#` + face** →
       enroll; show face → unlock. If the relay clicks the wrong way, see the polarity note in
       [§4](#4-solenoid-lock--the-relay).

---

## 9. Quick reference card

```
   ESP32-S3-CAM
   ┌───────────────────────────────┐
   │ GPIO21 ───────────────► Relay IN
   │ 5V     ───────────────► Relay VCC
   │ GND ─┬─────────────────► Relay GND
   │      ├─────────────────► 12V supply (−)
   │      └──(common ground)
   │
   │ GPIO38 ─► Keypad Row0      GPIO42 ─► Keypad Col0
   │ GPIO39 ─► Keypad Row1      GPIO47 ─► Keypad Col1
   │ GPIO40 ─► Keypad Row2      GPIO1  ─► Keypad Col2
   │ GPIO41 ─► Keypad Row3      GPIO2  ─► Keypad Col3
   │
   │ GPIO0  ─► BOOT button (on board)
   │ (camera GPIOs: on-board, do not touch)
   └───────────────────────────────┘

   Relay (12V side, fail-secure):
     12V(+) ─► COM ──[NO]──► Solenoid RED(+)
     12V(−) ──────────────► Solenoid BLACK(−)
     Flyback diode across solenoid, stripe → RED
```
