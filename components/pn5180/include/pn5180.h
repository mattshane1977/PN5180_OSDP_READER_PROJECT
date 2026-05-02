#pragma once

/*
 * PN5180 NFC front end driver.
 *
 * External interface is just the HAL registration entry point — everything
 * else flows through nfc_hal.h.
 *
 * Internal layering (see source files):
 *   pn5180_spi.c       — register R/W, command frames, BUSY-line polling
 *   pn5180_transceive.c— RF on/off, send/receive raw bytes via the transceiver
 *   pn5180_iso14443.c  — REQA, anticollision, RATS, I-block PCB, chaining
 */

void pn5180_driver_register(void);
