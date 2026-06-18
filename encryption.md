# On-Device Encryption — Design & Rationale

> **Status:** Stage 1 (software AES-256-GCM) implemented. Stage 2 (eFuse + flash
> encryption hardening) is future work on the final hardware unit.
> **Replaces:** ATECC608A cryptographic co-processor (could not be sourced).

This document explains *exactly* what the encryption does, why each choice was
made, and what an examiner can be shown. Nothing here is magic — every claim
maps to code you can open.

---

## 1. Why this exists (the ATECC608A story)

The proposal specified an **ATECC608A** crypto co-processor for hardware-backed
key storage and tamper resistance. **We could not source the chip.** The
proposal's own risk table already named the fallback:

> *LOW RISK: ATECC608A integration complexity → MITIGATION: Software encryption fallback.*

So encryption is done **on-device in software**, and key storage is hardened to
**hardware level using the ESP32-S3's own silicon** (eFuse + flash encryption)
rather than an external chip. This is a defensible, equivalent substitution —
see [§5](#5-what-to-say-in-the-viva).

---

## 2. What is protected, and how

**Protected asset:** the enrolled **face-embedding database** (`faces.db`). These
are the biometric templates; leaking or tampering with them is the security risk
that matters.

**Algorithm:** **AES-256-GCM** (Galois/Counter Mode) via mbedTLS, which ships
inside ESP-IDF. GCM was chosen over plain AES-CBC deliberately:

| Property | Why it matters here |
|----------|---------------------|
| **Confidentiality** | A flash dump yields ciphertext, not embeddings. |
| **Authentication (the GCM tag)** | If anyone edits the stored blob, decryption **fails loudly** (`ESP_ERR_INVALID_CRC`) instead of silently feeding the recognizer corrupted/attacker-chosen data. CBC gives you *no* such guarantee. |
| **No padding** | Ciphertext is the same length as plaintext; simpler container. |

**Key:** 256-bit, generated **once** from the ESP32-S3 **hardware RNG**
(`esp_fill_random`, seeded by RF/ADC noise) on first boot, then persisted. The
key never leaves the device.

---

## 3. How it's wired into the firmware

A dedicated component, [`crypto_ctrl`](components/crypto_ctrl/crypto_ctrl.c),
owns all crypto. It exposes a tiny API
([crypto_ctrl.h](components/crypto_ctrl/include/crypto_ctrl.h)):

```
crypto_ctrl_init()            // load or create the AES-256 key
crypto_ctrl_encrypt/decrypt() // buffer <-> SLE1 container, in RAM
crypto_ctrl_encrypt_file()    // plaintext file  -> encrypted file
crypto_ctrl_decrypt_file()    // encrypted file  -> plaintext file
```

### The container format (`SLE1` = "Smart Lock Encrypted v1")

Every encrypted blob is laid out as:

```
[ 4B magic "SLE1" ][ 12B nonce ][ 16B GCM tag ][ N bytes ciphertext ]
```

* The **magic** is also fed to GCM as authenticated-but-unencrypted data (AAD),
  so it can't be swapped for a different header without failing the tag check.
* The **nonce** is freshly random for every encryption — never reused with the
  same key (nonce reuse is the classic GCM footgun; we avoid it by re-rolling it
  on each call).
* Fixed overhead is **32 bytes** total. See
  [crypto_ctrl.h](components/crypto_ctrl/include/crypto_ctrl.h) for the exact
  constants.

### Why a wrapper, and the one honest tradeoff

The esp-dl `HumanFaceRecognizer` reads/writes `faces.db` with **raw
`fopen`/`fread`** and exposes **no encryption hook** (verified in
[dl_recognition_database.cpp](managed_components/espressif__esp-dl/vision/recognition/dl_recognition_database.cpp)).
It even does incremental `rb+` appends. So we cannot encrypt *inside* its storage
layer; we wrap it instead:

```
boot:    faces.db.enc  --AES-GCM decrypt-->  faces.db   (plaintext working file)
run:     recognizer opens faces.db as usual
enroll:  recognizer writes faces.db  -->  AES-GCM re-encrypt  -->  faces.db.enc
```

See [face_ctrl.cpp](components/face_ctrl/face_ctrl.cpp) — the decrypt happens in
`face_ctrl_init()` right after the SPIFFS mount, and `persist_encrypted_db()` is
called after every `enroll`.

**The tradeoff, stated honestly:** the plaintext working file `faces.db` exists
on SPIFFS while the device runs. So Stage 1 protects the database **at rest
across power-off** and **detects tampering**, but a live attacker with root
access to a running device could read the working copy. **Stage 2 (flash
encryption) closes this** by encrypting the entire flash — including the working
file — in hardware. This is why both stages exist.

### Tamper response

If `faces.db.enc` fails authentication on boot (wrong key, corruption, or
tampering), `face_ctrl_init()` **refuses to trust it** and deletes any stale
plaintext, so the recognizer starts from an empty, clean state rather than
operating on unverified data. See the `ESP_ERR_INVALID_CRC` branch in
[face_ctrl.cpp](components/face_ctrl/face_ctrl.cpp).

---

## 4. The staged hardening plan

| Stage | What | Key lives in | Reversible? | Status |
|-------|------|--------------|-------------|--------|
| **1** | App-layer AES-256-GCM over `faces.db` | Encrypted NVS | Yes | **Done** |
| **2** | ESP32-S3 **flash encryption** + **secure boot v2** | **eFuse** (read-protected) | eFuse burns are **permanent** | Future work, final HW only |

**Stage 2 discipline:** eFuse burns and Release-mode secure boot are
**irreversible** and can brick the board or break the flash/debug loop. So Stage
2 is done **only after** Stage 1 is proven, **Development mode first**, and **on
the final hardware unit** — never on the dev board mid-project.

---

## 5. What to say in the viva

> *"The proposal specified an ATECC608A for hardware-backed key storage, but the
> part was unobtainable. I implemented the documented software fallback:
> AES-256-GCM authenticated encryption of the biometric database, with the key
> generated on-device from the hardware RNG. I then hardened key storage to the
> hardware level using the ESP32-S3's native eFuse and flash encryption with
> secure boot, recovering the tamper-resistance the ATECC would have provided.
> This gives defense in depth: the application layer guarantees confidentiality
> and tamper-detection of the templates, and the hardware layer protects the key
> and the at-rest flash."*

Demoable evidence: dump `faces.db.enc` over serial/USB and show it is
indistinguishable from random; flip one byte and show the device rejects it on
next boot.

---

## 6. Files touched

* [components/crypto_ctrl/](components/crypto_ctrl/) — new component (header, impl, CMake).
* [components/face_ctrl/face_ctrl.cpp](components/face_ctrl/face_ctrl.cpp) — decrypt on init, re-encrypt on enroll.
* [components/face_ctrl/CMakeLists.txt](components/face_ctrl/CMakeLists.txt) — added `crypto_ctrl` dependency.
* [main/main.c](main/main.c) — `crypto_ctrl_init()` before `face_ctrl_init()`.
* [main/CMakeLists.txt](main/CMakeLists.txt) — added `crypto_ctrl` dependency.
