#pragma once

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include "esp_err.h"

/*
 * NFC HAL — driver-agnostic interface to whatever RFID front end is wired up.
 *
 * The DESFire driver and the rest of the system only ever call the nfc_*
 * functions below. The actual implementation is selected at build time via
 * Kconfig (see Kconfig.projbuild in the application root):
 *
 *   CONFIG_NFC_DRIVER_PN532   — uses components/pn532
 *   CONFIG_NFC_DRIVER_PN5180  — uses components/pn5180
 *
 * Both drivers register themselves with the HAL via nfc_register_driver().
 *
 * Why a HAL: PN532 and PN5180 have very different command sets (the PN532
 * does ISO 14443-4 framing in firmware; the PN5180 exposes the transceiver
 * directly and the host has to do framing). Higher layers shouldn't care.
 */

/* Driver vtable — populated by each NFC driver's register call. */
typedef struct {
    const char *name;
    esp_err_t (*init)(void);
    esp_err_t (*find_target)(uint8_t *uid, size_t uid_buf_len,
                             uint8_t *uid_len, uint8_t *sak);
    esp_err_t (*apdu_exchange)(const uint8_t *tx, size_t tx_len,
                               uint8_t *rx, size_t rx_buf_len, size_t *rx_len);
    esp_err_t (*release)(void);
} nfc_driver_t;

/* Drivers call this once during their init to plug themselves in. */
void nfc_register_driver(const nfc_driver_t *drv);

/* Public API used by everyone above the HAL */
esp_err_t nfc_init(void);
esp_err_t nfc_find_target(uint8_t *uid, size_t uid_buf_len,
                          uint8_t *uid_len, uint8_t *sak);
esp_err_t nfc_apdu_exchange(const uint8_t *tx, size_t tx_len,
                            uint8_t *rx, size_t rx_buf_len, size_t *rx_len);
esp_err_t nfc_release(void);

const char *nfc_driver_name(void);
bool nfc_is_ready(void);
