---
name: PN5180 OSDP Reader — project state
description: Current state, phase plan, and next steps for the PN5180 OSDP reader project
type: project
originSessionId: a3e8548a-cc89-4b3c-a902-a1e183651511
---
GitHub repo: https://github.com/mattshane1977/PN5180_OSDP_READER_PROJECT
Local path: /home/matt/PN5180_OSDP_READER_PROJECT
Current release: v0.1.0 (tag pushed, binaries + zip attached to GitHub release)
Build: clean, 124/124 targets, ~42% app partition free

## Phase plan

**Phase 1 (done)** — DeSFire EV3 card reading + debug web UI
- PN5180 SPI driver (BUSY-aware), ISO 14443-3/4 full stack
- DeSFire: GetVersion, GetApplicationIDs, GetFileIDs, GetFileSettings, SelectApplication
- Reader task (core 1) → JSON → queue → WebSocket dashboard (core 0)
- WS2812B RGB LED + buzzer indicators, WiFi AP/STA with mDNS `pdk-reader.local`
- Remaining Phase 1 items: DeSFire AES-128 auth (AuthenticateEV2First) + ReadData after auth

**Phase 2 (next)** — OSDP Peripheral Device over RS-485
- LibOSDP integration, UART1 RS-485 (HiLetgo auto-flow, no DE/RE pin)
- Badge read → osdp_event_cardread, OSDP LED/buzzer commands, OSDP-SC

**Phase 3 (future)** — LEAF credential support
- LEAF AID F5 1C DB, AES-128 key diversification, credential read/write

## Hardware (user is building the first board now)

- ESP32-S3-WROOM-1
- PN5180 NFC module (SPI2): SCK=12, MISO=13, MOSI=11, NSS=10, BUSY=14, RST=15, IRQ=16
- HiLetgo TTL↔RS-485 (UART1, Phase 2): TX=17, RX=18
- WS2812B RGB LED: GPIO 48 (on-board)
- Active piezo buzzer: GPIO 21
- BOOT button: GPIO 0

**Why:** User is physically assembling the first prototype board. Next session will be first flash + hardware validation.
**How to apply:** When user returns, expect hardware bring-up tasks — serial monitor output, PN5180 comms check, LED/buzzer smoke test.
