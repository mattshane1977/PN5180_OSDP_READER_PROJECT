# PN5180 OSDP Reader

ESP32-S3 based NFC access control reader supporting MiFare DESFire EV3 cards with OSDP output over RS-485.

---

## Table of Contents

- [Overview](#overview)
- [Hardware](#hardware)
  - [Bill of Materials](#bill-of-materials)
  - [Wiring](#wiring)
  - [Important Notes](#important-notes)
- [Software Architecture](#software-architecture)
  - [Component Map](#component-map)
  - [Data Flow](#data-flow)
- [Building and Flashing](#building-and-flashing)
- [Debug Web UI](#debug-web-ui)
- [LED Indicators](#led-indicators)
- [Roadmap](#roadmap)
- [Changelog](#changelog)

---

## Overview

This project implements an NFC card reader on the ESP32-S3 that:

1. **Phase 1 (current)** — Reads MiFare DESFire EV3 cards via a PN5180 NFC frontend. On each card tap, enumerates all applications and files on the card and displays the card structure in a live browser-based debug dashboard over WiFi.

2. **Phase 2 (planned)** — Adds OSDP Peripheral Device (PD) communication over RS-485, allowing the reader to integrate with standard access control panels (ACPs).

3. **Phase 3 (planned)** — LEAF credential support: authenticate against a specific DESFire application using AES-128 session keys and report a structured badge credential over OSDP.

---

## Hardware

### Bill of Materials

| # | Part | Description | Link |
|---|------|-------------|------|
| 1 | ESP32-S3-WROOM-1 | Main microcontroller board | [Amazon B0DB1WK3CW](https://www.amazon.com/dp/B0DB1WK3CW) |
| 2 | PN5180 NFC Module | ISO 14443-A NFC frontend (SPI) | [Amazon B09ZTYW4ZW](https://www.amazon.com/dp/B09ZTYW4ZW) |
| 3 | HiLetgo TTL↔RS-485 | Auto-flow RS-485 module for OSDP (Phase 2) | [Amazon B082Y19KV9](https://www.amazon.com/dp/B082Y19KV9) |

Additional: USB-C cable, breadboard or PCB, decoupling capacitors (see notes below).

---

### Wiring

#### PN5180 → ESP32-S3 (SPI2)

| PN5180 Pin | ESP32-S3 GPIO | Notes |
|------------|---------------|-------|
| VCC (TVDD) | 5V | RF TX driver requires 5V |
| GND | GND | |
| SCK | GPIO 12 | SPI2 clock, max 7 MHz |
| MISO | GPIO 13 | SPI2 MISO |
| MOSI | GPIO 11 | SPI2 MOSI |
| NSS | GPIO 10 | Chip select, active LOW — manual (not SPI hardware CS) |
| **BUSY** | **GPIO 14** | **Must be LOW before every SPI command** |
| RST | GPIO 15 | Active LOW reset |
| IRQ | GPIO 16 | RF event interrupt |

#### RS-485 Module → ESP32-S3 (UART1) — Phase 2

| RS-485 Pin | ESP32-S3 GPIO | Notes |
|------------|---------------|-------|
| VCC | 5V | |
| GND | GND | |
| RXD (module input) | GPIO 17 | UART1 TX from ESP32 |
| TXD (module output) | GPIO 18 | UART1 RX into ESP32 |
| A / B | OSDP bus | Half-duplex RS-485 to ACP |

> **Note:** The HiLetgo RS-485 module uses automatic flow control — no DE/RE pin is needed from the ESP32. The module handles TX/RX direction switching internally.

#### On-board Indicators (no external hardware required)

| Component | GPIO | Notes |
|-----------|-------|-------|
| WS2812B RGB LED | GPIO 48 | Built into the ESP32-S3 dev board |
| Active piezo buzzer | GPIO 21 | HIGH = on |
| BOOT button | GPIO 0 | Active LOW, internal pull-up |

---

### Important Notes

**PN5180 decoupling:** Place a **10 µF + 100 nF** capacitor pair directly at the PN5180 TVDD pin. The RF transmit driver draws bursts of current during card writes; without decoupling, DESFire write operations will drop the bus voltage and fail intermittently.

**PN5180 BUSY line:** The PN5180 requires the host to wait for BUSY to go LOW after every SPI transaction before issuing the next command. This is enforced in `pn5180_spi.c`. Do not share the SPI bus with other devices unless you replicate the BUSY-wait logic around every CS assertion.

**SPI mode selection:** Some PN5180 breakout boards have a mode-select jumper or DIP switch. Ensure it is set to **SPI mode** (not UART or I2C).

**GPIO conflicts to avoid on ESP32-S3-WROOM-1:**
- GPIO 19, 20 — USB D−/D+ (native USB)
- GPIO 26–32 — internal octal flash
- GPIO 33–37 — PSRAM (if present)
- GPIO 43, 44 — UART0 (USB-UART bridge, used for `idf.py monitor`)
- GPIO 0 — BOOT strapping pin (safe as input after boot)
- GPIO 3, 45, 46 — strapping pins (avoid for output)

---

## Software Architecture

### Component Map

```
PN5180_OSDP_READER_PROJECT/
│
├── main/
│   ├── main.c              — App entry: NVS init, board init, driver register,
│   │                         create card queue, start netconfig/webui/reader
│   ├── reader_task.c/h     — Core scan loop running on CPU core 1:
│   │                         nfc_find_target → GetVersion → GetApplicationIDs
│   │                         → per-AID: SelectApp → GetFileIDs → GetFileSettings
│   │                         → build JSON → post to queue
│   └── Kconfig.projbuild   — NFC driver selection (PN5180)
│
└── components/
    ├── board/              — Pin definitions, WS2812B RGB LED driver (RMT),
    │                         buzzer, BOOT button with debounce + long-press
    │
    ├── pn5180/             — PN5180 NFC frontend driver (three layers):
    │   ├── pn5180_spi.c    —   SPI register R/W, BUSY-line polling
    │   ├── pn5180_transceive.c — RF on/off, SEND_DATA, READ_DATA, wait_irq
    │   └── pn5180_iso14443.c — ISO 14443-3 anticollision + SELECT, RATS,
    │                           ISO 14443-4 I-block framing + chaining + WTX
    │
    ├── nfc_hal/            — Driver-agnostic HAL; higher layers only call
    │                         nfc_init / nfc_find_target / nfc_apdu_exchange
    │
    ├── desfire/            — DESFire EV3 protocol layer:
    │   ├── desfire.c       —   GetVersion, GetApplicationIDs, GetFileIDs,
    │   │                       GetFileSettings, SelectApplication,
    │   │                       ReadData, WriteData, CreateApp, FormatPICC
    │   ├── desfire_auth.c  —   AuthenticateEV2First (AES-128 mutual auth,
    │   │                       session key derivation per NXP AN12343)
    │   ├── desfire_crypto.c —  AES-128 CBC (mbedTLS HW-backed), CMAC, CRC32
    │   └── desfire_enc.c   —   EV2 encrypted/CMAC command wrappers,
    │                           ChangeKey
    │
    ├── netconfig/          — WiFi: STA (credentials from NVS) with AP fallback.
    │                         SoftAP SSID: pdk-reader-XXXXXX (open, no password)
    │                         mDNS: http://pdk-reader.local/
    │
    └── webui/              — Embedded HTTP + WebSocket debug dashboard.
        ├── webui.c         —   Single WS client model; event push task receives
        │                       JSON from card queue and forwards to browser
        └── assets/index.html — Single-page app: live card structure tree,
                                event log, OSDP config stub, WiFi config
```

### Data Flow

```
PN5180 hardware
     │  SPI (BUSY-aware, 7 MHz)
     ▼
pn5180_spi / pn5180_transceive
     │  raw RF bytes
     ▼
pn5180_iso14443
     │  ISO 14443-4 APDUs (I-block framing, chaining, WTX handled)
     ▼
nfc_hal  (nfc_apdu_exchange)
     │
     ▼
desfire.c  (GetVersion / GetApplicationIDs / GetFileIDs / GetFileSettings)
     │  card info struct
     ▼
reader_task.c  →  builds JSON string  →  xQueueSend(card_q)
                                                │
                                                ▼
                                         webui event_push_task
                                                │  WebSocket
                                                ▼
                                         Browser dashboard
```

---

## Building and Flashing

**Prerequisites:**
- [ESP-IDF v5.x](https://docs.espressif.com/projects/esp-idf/en/stable/esp32s3/get-started/) installed and sourced
- `idf.py` in PATH
- USB cable connected to the ESP32-S3 board's USB-UART port (not the native USB port)

```bash
# Enter project directory
cd ~/PN5180_OSDP_READER_PROJECT

# Source ESP-IDF (adjust path if installed elsewhere)
. $HOME/esp/esp-idf/export.sh

# Set target (only needed once per clone)
idf.py set-target esp32s3

# Install managed components (mdns, led_strip) — first build only
idf.py fullclean   # optional: ensures clean state

# Build
idf.py build

# Flash and open serial monitor (adjust port as needed)
idf.py -p /dev/ttyUSB0 flash monitor
```

On a successful boot you should see:

```
I (xxx) board: board init complete
I (xxx) pn5180_iso: PN5180 alive (SYSTEM_STATUS=0x...)
I (xxx) netconfig: SoftAP 'pdk-reader-aabbcc' (open) — connect and browse to 192.168.4.1
I (xxx) webui: web UI started — http://pdk-reader.local/ or http://192.168.4.1/
I (xxx) reader: PN5180 ready, scanning...
I (xxx) main: system up — driver=pn5180 wifi=AP
```

**Serial port naming:**
- Linux: `/dev/ttyUSB0` or `/dev/ttyACM0`
- macOS: `/dev/cu.usbserial-XXXX`
- Windows: `COM3` (or similar — check Device Manager)

---

## Debug Web UI

1. On boot the reader starts a WiFi access point: **`pdk-reader-XXXXXX`** (last 3 bytes of MAC, no password).
2. Connect your laptop or phone to that network.
3. Open **http://192.168.4.1/** (or **http://pdk-reader.local/** if your OS supports mDNS).

**To use your own WiFi instead of AP mode:**
- Enter SSID and password in the WiFi panel and click **Save & reboot**.
- Credentials are stored in NVS and persist across reboots.
- On the next boot the reader connects as a STA; the IP is shown in the serial log and in the dashboard status bar.

### Dashboard panels

| Panel | Description |
|-------|-------------|
| **Last Card** | Card UID, hardware/software version, storage size, and a tree of all applications and files found on the card |
| **Event log** | Timestamped stream of card tap, card removal, and command ack events |
| **OSDP Config** | Save PD address and baud rate to NVS — takes effect on reboot (Phase 2) |
| **WiFi** | Update STA credentials without reflashing |
| **Reboot** | Remote reboot button |

### WebSocket message reference

**Device → Browser:**

```jsonc
// Card detected and enumerated
{"type":"card","uid":"04a1b2c3d4e5","hw":{"vendor":4,"type":1,"subtype":1,"ver":"1.0","storage_enc":24},"sw":{"vendor":4,"type":1,"subtype":1,"ver":"3.17"},"uid_real":"04a1b2c3d4e5","apps":[{"aid":"f51cdb","files":[{"id":1,"type":"std","size":48,"comm":"enc","ar":"eeee"}]}]}

// Card removed from field
{"type":"no_card"}

// System status (sent on WebSocket connect)
{"type":"status","driver":"pn5180","ip":"192.168.4.1"}
```

**Browser → Device:**

```jsonc
{"cmd":"osdp_set","address":101,"baud":9600}
{"cmd":"wifi_set","ssid":"MyNetwork","pass":"password"}
{"cmd":"reboot"}
```

---

## LED Indicators

The on-board WS2812B RGB LED (GPIO 48) provides visual feedback without any external hardware:

| Pattern | Color | Meaning |
|---------|-------|---------|
| Slow dim blue pulse | Blue | Idle — system running, no card present |
| Solid green | Green | Armed — scanning for cards |
| Green flash + short beep | Green | Card successfully read and enumerated |
| Red double-flash + double beep | Red | Card read error or NFC comms failure |
| Fast red flash + long beep | Red | NFC hardware init failure — check PN5180 wiring |

---

## Roadmap

### Phase 1 — DeSFire EV3 card reading ✅
- [x] PN5180 SPI driver with full BUSY-line protocol
- [x] ISO 14443-3 anticollision and card SELECT
- [x] ISO 14443-4 I-block framing, chaining, WTX handling
- [x] DESFire GetVersion, GetApplicationIDs, GetFileIDs, GetFileSettings
- [x] Reader task: card detect → enumerate → JSON
- [x] WebSocket debug dashboard with live card tree
- [x] WS2812B RGB LED indicators
- [x] WiFi AP/STA with mDNS
- [ ] DESFire AES-128 authentication (AuthenticateEV2First) against a known key
- [ ] DESFire ReadData (plain and encrypted) after authentication

### Phase 2 — OSDP PD over RS-485
- [ ] LibOSDP integration (OSDP Peripheral Device role)
- [ ] UART1 RS-485 driver (HiLetgo auto-flow board)
- [ ] Badge read event → OSDP osdp_event_cardread
- [ ] OSDP LED and buzzer commands mapped to board indicators
- [ ] OSDP address and baud configurable via web UI (NVS stored)
- [ ] OSDP Secure Channel (OSDP-SC) Phase 2

### Phase 3 — LEAF credential support
- [ ] LEAF AID selection (F5 1C DB)
- [ ] AES-128 key diversification from site key + card UID
- [ ] LEAF credential structure read and decode
- [ ] Credential output via OSDP card read event
- [ ] Card personalization (write LEAF credential to factory DeSFire card)

---

## Changelog

### v0.2.0 — 2026-05-02
**Fix: PN5180 RF field never activating — cards not read on first board bring-up.**
- Fixed `PN5180_IRQ_RF_ON` bit assignment (was `1<<7`, correct value is `1<<6` per datasheet TX_RFON_IRQ_STAT). The v0.1.0 release waited for a bit that never fires, causing `pn5180_rf_on()` to always time out and bail before REQA was sent.
- Replaced the IRQ-based RF field ready wait with a 5 ms stabilization delay; simpler and avoids re-triggering the issue if IRQ enable masks change.
- Fixed `PN5180_IRQ_RF_OFF` bit assignment (was `1<<6`, correct value is `1<<5` per datasheet TX_RFOFF_IRQ_STAT).

### v0.1.0 — 2026-05-01
**Initial release.**
- Project scaffolded from scratch on ESP-IDF v5.x targeting ESP32-S3.
- PN5180 SPI driver (BUSY-line aware): register R/W, RF on/off, ISO 14443-3 full anticollision loop, RATS, ISO 14443-4 I-block exchange with chaining and WTX.
- DeSFire EV3 protocol layer: GetVersion, GetApplicationIDs, GetFileIDs, GetFileSettings, SelectApplication, AuthenticateEV2First, ReadData, WriteData, CreateApplication, ChangeKey, FormatPICC.
- Reader task: card detect → full card enumeration → JSON → WebSocket queue.
- WebSocket debug dashboard: live card structure tree, event log, OSDP config stub, WiFi config.
- WS2812B RGB LED indicators (on-board, no external hardware).
- WiFi netconfig: STA with AP fallback, mDNS `pdk-reader.local`.
- Hardware: ESP32-S3-WROOM-1 + PN5180 (SPI2) + HiLetgo RS-485 (UART1, Phase 2 stub).
