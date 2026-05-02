#pragma once

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include "esp_err.h"

/* ------- PN5180 host commands (datasheet §11) ------- */

#define PN5180_CMD_WRITE_REGISTER          0x00
#define PN5180_CMD_WRITE_REGISTER_OR_MASK  0x01
#define PN5180_CMD_WRITE_REGISTER_AND_MASK 0x02
#define PN5180_CMD_READ_REGISTER           0x04
#define PN5180_CMD_WRITE_EEPROM            0x06
#define PN5180_CMD_READ_EEPROM             0x07
#define PN5180_CMD_SEND_DATA               0x09
#define PN5180_CMD_READ_DATA               0x0A
#define PN5180_CMD_LOAD_RF_CONFIG          0x11
#define PN5180_CMD_RF_ON                   0x16
#define PN5180_CMD_RF_OFF                  0x17

/* ------- key registers (datasheet §10) ------- */

#define PN5180_REG_SYSTEM_CONFIG           0x00
#define PN5180_REG_IRQ_ENABLE              0x01
#define PN5180_REG_IRQ_STATUS              0x02
#define PN5180_REG_IRQ_CLEAR               0x03
#define PN5180_REG_TRANSCEIVE_CONTROL      0x04
#define PN5180_REG_PIN_CONFIG              0x05
#define PN5180_REG_RX_WAIT_CONFIG          0x11
#define PN5180_REG_RX_STATUS               0x13
#define PN5180_REG_TX_DATA_MOD             0x16
#define PN5180_REG_TX_CONFIG               0x18
#define PN5180_REG_CRC_TX_CONFIG           0x19
#define PN5180_REG_RX_CONFIG               0x1A
#define PN5180_REG_CRC_RX_CONFIG           0x1B
#define PN5180_REG_RF_STATUS               0x1D
#define PN5180_REG_SYSTEM_STATUS           0x24
#define PN5180_REG_TIMER1_CONFIG           0x0C
#define PN5180_REG_TIMER1_RELOAD           0x0D
#define PN5180_REG_TIMER1_COUNTER          0x0E

/* ------- Register bitmasks ------- */

#define PN5180_CRC_TX_ENABLE               (1 << 0)
#define PN5180_CRC_RX_ENABLE               (1 << 0)

#define PN5180_RX_STATUS_ERROR_MASK        (1 << 18)
#define PN5180_RX_STATUS_COLLISION_MASK    (1 << 17)
#define PN5180_RX_STATUS_LEN_MASK          0x1FF

/* ------- IRQ status bits ------- */

#define PN5180_IRQ_RX_DONE                 (1 << 0)
#define PN5180_IRQ_TX_DONE                 (1 << 1)
#define PN5180_IRQ_IDLE                    (1 << 2)
#define PN5180_IRQ_RF_OFF                  (1 << 6)
#define PN5180_IRQ_RF_ON                   (1 << 7)

/* ------- RF config indices for LOAD_RF_CONFIG ------- */

#define PN5180_RF_TX_ISO14443A_106         0x00
#define PN5180_RF_RX_ISO14443A_106         0x80
#define PN5180_RF_TX_ISO14443A_212         0x01
#define PN5180_RF_RX_ISO14443A_212         0x81
#define PN5180_RF_TX_ISO14443A_424         0x02
#define PN5180_RF_RX_ISO14443A_424         0x82
#define PN5180_RF_TX_ISO14443A_848         0x03
#define PN5180_RF_RX_ISO14443A_848         0x83

/* ------- ISO 14443-3 / 14443-4 constants ------- */

#define ISO14443_REQA                      0x26
#define ISO14443_WUPA                      0x52
#define ISO14443_HLTA_1                    0x50
#define ISO14443_HLTA_2                    0x00
#define ISO14443_RATS                      0xE0

#define ISO14443_SEL_CL1                   0x93
#define ISO14443_SEL_CL2                   0x95
#define ISO14443_SEL_CL3                   0x97
#define ISO14443_NVB_FULL                  0x70
#define ISO14443_CT                        0x88   /* cascade tag */

/* I-block PCB (ISO 14443-4 §7.1)
 *   bit 7..6 = 00 (I-block)
 *   bit 5    = chaining
 *   bit 4    = CID present
 *   bit 3    = NAD present (always 0 for us)
 *   bit 2    = always 1 (datasheet/standard)
 *   bit 1    = always 0 (standard says b1 is 1? wait...)
 *
 * Correct mapping (ISO 14443-4 §7.1.1.1):
 * I-block: 00 | Ch | CID | NAD | 0 | 1 | B#
 * R-block: 10 | 0  | A/N | CID | 0 | 1 | B#
 * S-block: 11 | Type | CID | 0 | 0 | 1 | 0 | 0 (Deselect/WTX)
 *
 * Actually, standard says:
 * I-block: [00] [Ch] [CID] [NAD] [0] [1] [B#]  -> 0x02 | (Ch<<4) | (CID<<3) | (NAD<<2) | B#
 * R-block: [10] [0]  [A/N] [CID] [0] [1] [B#]  -> 0x82 | (A/N<<5) | (CID<<3) | B#
 * S-block: [11] [Type] [CID] [0] [0] [1] [0] [0] -> 0xC2 | (Type<<4) | (CID<<3)
 * (Using bit indices 7..0)
 */
#define ISO14443_PCB_I_BLOCK_BASE          0x02
#define ISO14443_PCB_R_BLOCK_ACK           0x82
#define ISO14443_PCB_R_BLOCK_NAK           0xA2
#define ISO14443_PCB_S_WTX_REQ             0xF2
#define ISO14443_PCB_S_DESELECT            0xC2

#define ISO14443_PCB_TYPE_MASK             0xC0
#define ISO14443_PCB_TYPE_I                0x00
#define ISO14443_PCB_TYPE_R                0x80
#define ISO14443_PCB_TYPE_S                0xC0
#define ISO14443_PCB_BLOCK_NUM_MASK        0x01
#define ISO14443_PCB_CHAINING              0x10
#define ISO14443_PCB_ACK_NAK_MASK          0x20
#define ISO14443_PCB_S_TYPE_MASK           0x30
#define ISO14443_PCB_S_TYPE_DESELECT       0x00
#define ISO14443_PCB_S_TYPE_WTX            0x30

/* ------- internal state shared across the three sub-modules ------- */

typedef struct {
    bool      iso14443_4_active;
    uint8_t   block_number;            /* toggles 0/1 per exchange */
    uint16_t  fsc_max;                 /* max frame size from ATS */
    uint8_t   uid[10];
    uint8_t   uid_len;
    uint8_t   sak;
} pn5180_card_state_t;

extern pn5180_card_state_t g_pn5180_card;

/* ------- pn5180_spi.c ------- */

esp_err_t pn5180_spi_init(void);
esp_err_t pn5180_spi_reset(void);
esp_err_t pn5180_write_register(uint8_t reg, uint32_t value);
esp_err_t pn5180_read_register(uint8_t reg, uint32_t *out);
esp_err_t pn5180_write_register_or_mask(uint8_t reg, uint32_t mask);
esp_err_t pn5180_write_register_and_mask(uint8_t reg, uint32_t mask);
esp_err_t pn5180_send_command(const uint8_t *frame, size_t len,
                              uint8_t *resp, size_t resp_buf, size_t *resp_len);

/* ------- pn5180_transceive.c ------- */

esp_err_t pn5180_rf_on(uint8_t config_idx);
esp_err_t pn5180_rf_off(void);
esp_err_t pn5180_load_rf_config(uint8_t tx_idx, uint8_t rx_idx);

/* Send raw bytes to the card with `valid_bits_in_last_byte` controlling
 * partial-byte transmissions (used for anticollision NVB framing).
 * If valid_bits_in_last_byte == 8 (or 0 with no partial), full bytes go. */
esp_err_t pn5180_send_data(const uint8_t *data, size_t len,
                           uint8_t valid_bits_in_last_byte);

/* Read up to len bytes from the receive buffer. */
esp_err_t pn5180_read_data(uint8_t *out, size_t buf_len, size_t *out_len);

/* Block until the IRQ status register shows one of the bits in `mask`,
 * or timeout_ms elapses. Clears matched bits before returning. */
esp_err_t pn5180_wait_irq(uint32_t mask, uint32_t timeout_ms);

/* ------- pn5180_iso14443.c ------- */

esp_err_t pn5180_iso14443_activate(uint8_t *uid, size_t uid_buf,
                                   uint8_t *uid_len, uint8_t *sak);
esp_err_t pn5180_iso14443_deselect(void);
esp_err_t pn5180_iso14443_apdu(const uint8_t *tx, size_t tx_len,
                               uint8_t *rx, size_t rx_buf, size_t *rx_len);
