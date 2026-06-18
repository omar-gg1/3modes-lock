# The Codebase, Explained Simply

> **Purpose of this file:** open it, and you can explain *any* part of the project
> to *anyone* — a supervisor, an examiner, a teammate — without reading the code
> itself. Plain language, no assumed knowledge.
>
> It has three parts:
> 1. **[The big picture](#1-the-big-picture)** — what the device does, in one breath.
> 2. **[Every data type explained](#2-every-data-type-explained)** — what each "kind of value" means.
> 3. **[Every file explained](#3-every-file-explained)** — what each file's job is.

---

## 1. The big picture

This is the firmware for a **smart door lock** built on an **ESP32-S3** (a small
Wi-Fi + camera microcontroller). It does this loop, forever:

1. Watch the **camera** for a face.
2. If the face matches an **enrolled** (registered) person → **unlock**.
3. Also accept a **PIN on the keypad**, or a tap of the **BOOT button**, as backup ways in.
4. The registered faces are stored **encrypted** on the chip, so a stolen device doesn't leak them.

The code is split into small **components** — one folder per job (lock, keypad,
camera, face recognition, crypto, etc.) — and one **`main`** that ties them
together. That separation is the whole design: each folder does one thing and can
be understood on its own.

```
            ┌─────────────── main.c (the conductor) ───────────────┐
            │  runs the forever-loop, decides when to unlock       │
            └───┬───────┬───────┬────────┬─────────┬──────────┬────┘
                │       │       │        │         │          │
             keypad  button  camera   face      crypto      lock
             (PIN)  (BOOT)  (photos) (recognise)(encrypt)  (relay)
```

---

## 2. Every data type explained

A "data type" is just **what kind of value** a piece of data is. Here's every one
used in this project, in plain words.

### 2.1 The basic building blocks

| Type | What it is | Everyday analogy | Example in this code |
|------|-----------|------------------|----------------------|
| `int` | A whole number (can be negative) | A counter | `int pin_len` = how many keypad digits typed so far |
| `int64_t` | A *very large* whole number (64-bit) | A stopwatch in microseconds | `press_start_us` = the exact time a button was pressed |
| `uint8_t` | A number 0–255, one **byte** | A single character of data | The raw bytes of an encrypted file |
| `uint16_t` | A number 0–65,535 | A medium counter | A face's ID number in the database |
| `uint32_t` | A number 0 to ~4 billion | A timeout in milliseconds | `timeout_ms` for Wi-Fi |
| `float` | A number with a decimal point | A percentage/score | `similarity` = how close a face match is (e.g. 0.83) |
| `bool` | true **or** false, nothing else | A light switch | `unlock_in_progress` = is the lock currently open? |
| `char` | A single letter/symbol | One key on a keyboard | `key` = which keypad button was pressed, e.g. `'5'` |
| `size_t` | A count of bytes/length (never negative) | "how many" | `plaintext_len` = how many bytes to encrypt |
| `void` | "nothing" | An empty box | A function that returns `void` gives nothing back |

> **Why so many number types?** Smaller types use less memory. On a tiny chip with
> limited RAM, using `uint8_t` (1 byte) instead of `int` (4 bytes) where you only
> need 0–255 actually matters. Each type is chosen to fit the value exactly.

### 2.2 Pointers and text

| Type | What it is | Plain explanation |
|------|-----------|-------------------|
| `char *` | "A piece of text" (a string) | The `*` means *"the address where the text lives"*. `const char *reason` = a label like `"PIN code"` passed into a function. |
| `uint8_t *` | "A block of raw bytes" | Used for things like the camera image or encrypted data — just a pointer to where those bytes start. |
| `*out_len` | "Write the answer here" | A pointer used as an **output**: the function fills it in for you. E.g. "tell me how many bytes you wrote — put the number at `out_len`." |

> **The `*` (pointer), in one sentence:** instead of copying a big chunk of data
> around (slow), the code passes the *address* of where it lives (fast). A pointer
> is a signpost, not the house.

### 2.3 Project-specific "result" types

| Type | What it is | Why it exists |
|------|-----------|---------------|
| `esp_err_t` | A **result code** | Every function that can fail returns one of these. `ESP_OK` means success; anything else is a named error (`ESP_ERR_NO_MEM` = out of memory, `ESP_ERR_INVALID_CRC` = data was tampered with, etc.). This is how the code says "did it work?" everywhere. |
| `gpio_num_t` | "Which physical pin" | A named pin number on the chip, e.g. `GPIO_NUM_21` (the lock pin). Just a labelled integer. |

### 2.4 Bundles of values (`struct` and `enum`)

A **`struct`** is a **labelled bundle** of several values that belong together —
like a form with named fields.

| Struct | What it bundles | Used for |
|--------|----------------|----------|
| `camera_config_t` | All the camera settings (every pin, resolution, image quality, frame buffers) | Telling the camera driver how to set itself up |
| `camera_fb_t` | One captured photo: the bytes, the length, the format, width, height | A single frame from the camera ("fb" = frame buffer) |
| `gpio_config_t` | A pin's settings (which pin, input or output, pull-up on/off) | Configuring a GPIO before use |
| `dl::image::img_t` | A decoded image ready for the AI: the pixel bytes, width, height, format | Feeding a picture into the face detector |
| `dl::detect::result_t` | One detected face: its box on screen + a confidence score | The output of "is there a face here?" |
| `dl::recognition::result_t` | One recognised face: its ID + similarity score | The output of "*whose* face is this?" |

An **`enum`** is a **fixed menu of named choices** — a value that can only be one
of a small set.

| Enum | The menu of choices | Used for |
|------|---------------------|----------|
| `btn_state_t` | `BTN_IDLE`, `BTN_PRESSED`, `BTN_HELD` | Tracking what the button is doing right now (a tiny state machine) |
| `PIXFORMAT_JPEG`, `FRAMESIZE_QVGA` | (camera format/size options) | Picking the image format (JPEG) and size (320×240) |

> **struct vs enum, in one line:** a `struct` holds *many values at once* (a form);
> an `enum` holds *one value chosen from a list* (a multiple-choice answer).

### 2.5 Two crypto-specific types worth knowing

| Type | What it is | In plain words |
|------|-----------|----------------|
| `mbedtls_gcm_context` | The encryption engine's "workspace" | A scratch area the AES-GCM library uses while encrypting/decrypting. You create one, use it, then free it. |
| The "SLE1 container" | Our own encrypted-file layout | Not a C type — it's the **shape of an encrypted file** we invented: `[magic][nonce][tag][ciphertext]`. Explained fully in [encryption.md](encryption.md). |

---

## 3. Every file explained

Listed in the order it makes sense to understand them. For each: **what it's for**,
**the key pieces**, and **the data types it leans on**.

> **Note on `components/`:** each subfolder is one self-contained module with the
> same shape — an `include/<name>.h` (the "menu" of what it offers) and a
> `<name>.c`/`.cpp` (the actual work). The `.h` is what *other* files read; the
> `.c` is the private implementation.

---

### 🟢 `main/main.c` — the conductor

**What it's for:** the entry point. It starts every subsystem, then runs the
**forever-loop** that watches the keypad, the button, and the camera, and decides
when to unlock.

**Key pieces:**
- `app_main()` — the very first function that runs when the chip powers on.
- It initialises things **in order** (crypto before face, because the face
  database is decrypted using the crypto key): `nvs → crypto → lock → button →
  keypad → camera → face`.
- The `while(1)` loop, every ~50 ms:
  - reads a keypad key and builds up a PIN (`1234#` unlocks, `*9999#` arms
    enrollment),
  - checks the BOOT button,
  - runs the face pipeline (detect → then either enroll or recognise).
- **Two important numbers live here:** `FACE_MATCH_THRESHOLD` (0.6 — how sure the
  match must be) and the PIN codes.

**Data types it uses:** `char` (keypad keys), `int`/`int64_t` (PIN length, timers),
`bool` (enrollment armed?), `esp_err_t` (checking each result), `float` (the
similarity score).

---

### 🟢 `components/lock_ctrl/` — the lock (relay)

**What it's for:** opening and closing the physical lock by flipping one GPIO pin
that drives the relay.

**Key pieces (`lock_ctrl.c`):**
- `lock_ctrl_init()` — set up **GPIO 21** as an output, and start **locked**.
- `lock_ctrl_trigger_unlock(reason)` — unlock, wait 3 seconds, re-lock. The
  `reason` is just a label for the log ("PIN code", "face match", etc.).
- A `bool unlock_in_progress` flag stops two unlocks from overlapping.

**Data types it uses:** `gpio_num_t` (the pin), `bool` (the in-progress flag),
`const char *` (the reason label), `esp_err_t` (init result).

---

### 🟢 `components/keypad_ctrl/` — the PIN keypad

**What it's for:** reading which key is pressed on the 4×4 keypad.

**Key pieces (`keypad_ctrl.c`):**
- It scans a **matrix**: 4 row pins (outputs) and 4 column pins (inputs). It drives
  one row low at a time and checks which column responds — that pair identifies the
  key. (Full pin list in [connections.md](connections.md).)
- `keypad_ctrl_scan()` — returns the pressed key as a `char` (like `'7'`), or `0`
  if nothing new. It only fires **once per press** (debounced) so holding a key
  doesn't spam.
- `KEYMAP` — a small 4×4 table mapping (row, column) → the character.

**Data types it uses:** `char` (the returned key), `gpio_num_t` (the 8 pins),
`int64_t` (debounce timing).

---

### 🟢 `components/button_ctrl/` — the BOOT button

**What it's for:** using the board's built-in BOOT button as a manual unlock and
(potentially) a long-press action.

**Key pieces (`button_ctrl.c`):**
- A little **state machine** (`btn_state_t`: IDLE → PRESSED → HELD) tracks the
  button.
- `button_ctrl_was_pressed()` — true once on a **short** press.
- `button_ctrl_long_press_fired()` — true once when a **2-second hold** is reached.

**Data types it uses:** the `btn_state_t` enum (current state), `bool` (pressed or
not), `int64_t` (how long it's been held).

---

### 🟢 `components/camera_ctrl/` — the camera

**What it's for:** turning the camera on and handing out photos.

**Key pieces (`camera_ctrl.c`):**
- `camera_ctrl_init()` — fills in a big `camera_config_t` (every camera pin + the
  settings: **QVGA 320×240, JPEG**) and starts the camera. Also flips the image
  because this particular board mounts the sensor upside-down.
- `camera_ctrl_get_frame()` — gives you one photo as a `camera_fb_t *`.
- `camera_ctrl_return_frame()` — you **must** hand the photo back when done, or the
  camera runs out of buffers.

**Data types it uses:** `camera_config_t` (the setup form), `camera_fb_t` (a single
photo), `esp_err_t` (did it start?).

---

### 🟢 `components/face_ctrl/` — face detection & recognition

**What it's for:** the AI part. Finding a face in a photo, registering ("enrolling")
new faces, and recognising known ones. Also where the **encrypted database** is
loaded and saved.

**Key pieces (`face_ctrl.cpp` — note: C++ because the AI library is C++):**
- `face_ctrl_init()` — mounts storage, **decrypts** the saved face database, and
  creates the detector + recogniser AI models.
- `face_ctrl_detect_once()` — grab a photo, decode it, ask "is there a face?"
  Returns `true`/`false`.
- `face_ctrl_enroll(id)` — register the most recently seen face, then **re-encrypt**
  the database so it survives a reboot.
- `face_ctrl_recognize(&id, &similarity)` — is the current face a known one? Fills
  in *which* person (`id`) and *how confident* (`similarity`, 0–1).

**Data types it uses:** `dl::image::img_t` (the decoded picture), `dl::detect::result_t`
and `dl::recognition::result_t` (the AI's answers), `float` (similarity), `int`
(face IDs), `esp_err_t` (results everywhere).

---

### 🟢 `components/crypto_ctrl/` — the encryption (replaces the ATECC chip)

**What it's for:** scrambling the face database so a stolen chip leaks nothing, and
**detecting tampering**. This is the software replacement for the ATECC608A
security chip we couldn't buy. (Deep dive: [encryption.md](encryption.md).)

**Key pieces (`crypto_ctrl.c`):**
- `crypto_ctrl_init()` — on first boot, make a random 256-bit key from the chip's
  hardware random-number generator and save it; later, load it back.
- `crypto_ctrl_encrypt()` / `crypto_ctrl_decrypt()` — scramble/unscramble a block of
  bytes using **AES-256-GCM**. "GCM" means it also produces a **tag** that proves
  the data wasn't altered.
- `crypto_ctrl_encrypt_file()` / `crypto_ctrl_decrypt_file()` — same, but for whole
  files (used on `faces.db`).

**Data types it uses:** `uint8_t *` (raw bytes in and out), `size_t` (lengths),
`mbedtls_gcm_context` (the encryption engine), `esp_err_t` (success / tamper-fail).

---

### 🟡 `components/wifi_ctrl/` — Wi-Fi *(built, not used yet)*

**What it's for:** connecting the device to a Wi-Fi network. **Currently not called
from `main.c`** — it's ready for **Mode 2 (Hybrid)**, which is the next thing we
build. Listed here so nobody is confused about why it exists but seems inactive.

**Key pieces (`wifi_ctrl.c`):**
- `wifi_ctrl_connect(ssid, password, timeout_ms)` — connect to a network, retrying
  up to 5 times, and **block until** connected or timed out. Logs the device's IP
  address when it succeeds.

**Data types it uses:** `const char *` (network name & password), `uint32_t`
(timeout), `esp_err_t` (connected or not), plus an internal `EventGroupHandle_t`
(a FreeRTOS signal used to wait for the "connected" event).

---

### 🟡 `components/debug_server/` — camera debug web page *(built, not used yet)*

**What it's for:** a tiny web server so you can **see the live camera feed in a
browser** while developing. Like Wi-Fi, it's **not currently called from `main.c`**
— it's a developer convenience that needs Wi-Fi up first.

**Key pieces (`debug_server.c`):**
- `debug_server_start()` — starts a web server on port 80 with three pages:
  `/` (a page showing the stream), `/stream` (the live video), `/capture` (one
  snapshot).

**Data types it uses:** `httpd_req_t *` (an incoming web request), `camera_fb_t *`
(the photo it sends back), `esp_err_t` (results).

---

### ⚙️ The build files (`CMakeLists.txt`, `partitions.csv`, `sdkconfig`)

These aren't program logic — they're **instructions for the build system**.

| File | What it does |
|------|-------------|
| `CMakeLists.txt` (one per component + root) | Lists a component's source files and **which other components it depends on**. E.g. `face_ctrl` depends on `crypto_ctrl`, so the build knows to link them. |
| `partitions.csv` | A **map of the chip's flash memory**: where the app goes (6 MB), where settings (`nvs`) go, and where the face models + database (`models`, 4 MB) go. |
| `sdkconfig` | The big settings file for the whole framework — chip type (`esp32s3`), memory options (PSRAM on), etc. Mostly auto-generated. |

---

### 📄 The documentation files (the `.md` files in the root)

| File | What it explains |
|------|------------------|
| `CODE_EXPLAINED.md` | **This file** — plain-language tour of types and files. |
| `connections.md` | Every physical wire: keypad, relay, solenoid, power, grounds. |
| `encryption.md` | The on-device AES design, the ATECC replacement story, and the viva answer. |
| `architecture-decision.txt` | The chosen backend stack (FastAPI, MQTT, etc.) for the cloud modes. |

---

## 4. One-paragraph summary (for the absolute quickest explanation)

> *"The firmware is organised as small independent **components**, each handling one
> job — keypad, button, camera, face AI, encryption, and the lock relay — and a
> **main** file that runs a loop tying them together. A face, a PIN, or the BOOT
> button can unlock the door. Registered faces are stored **encrypted** with
> AES-256 (our software stand-in for the security chip we couldn't source), so a
> stolen device gives up nothing. The Wi-Fi and web-server components are built and
> waiting for the next phase, the cloud-connected **Hybrid** mode."*
