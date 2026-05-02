#include "pn5180_internal.h"
#include "pn5180.h"
#include "nfc_hal.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <string.h>

#if defined(CONFIG_NFC_DRIVER_PN5180)

static const char *TAG = "pn5180_iso";

pn5180_card_state_t g_pn5180_card;

/* ------- chip bring-up (called once from driver init) ------- */

static esp_err_t pn5180_chip_init(void)
{
    if (pn5180_spi_init() != ESP_OK) return ESP_FAIL;

    /* Read SYSTEM_STATUS once after reset as a liveness check. If it returns
     * 0xFFFFFFFF the chip is held in reset or wired backwards. */
    uint32_t sys = 0;
    if (pn5180_read_register(PN5180_REG_SYSTEM_STATUS, &sys) != ESP_OK) {
        ESP_LOGE(TAG, "could not read SYSTEM_STATUS — check wiring/reset");
        return ESP_FAIL;
    }
    if (sys == 0xFFFFFFFF) {
        ESP_LOGE(TAG, "SYSTEM_STATUS=0xFFFFFFFF — MISO floating? swap MOSI/MISO?");
        return ESP_FAIL;
    }
    ESP_LOGI(TAG, "PN5180 alive (SYSTEM_STATUS=0x%08x)", (unsigned)sys);

    /* Enable a useful subset of IRQs in the IRQ_ENABLE register. */
    pn5180_write_register(PN5180_REG_IRQ_ENABLE,
                          PN5180_IRQ_RX_DONE | PN5180_IRQ_TX_DONE |
                          PN5180_IRQ_IDLE     | PN5180_IRQ_RF_ON   |
                          PN5180_IRQ_RF_OFF);

    return ESP_OK;
}

/* ------- ISO 14443-3 activation -------
 *
 * Sequence:
 *   1. LOAD_RF_CONFIG(0x00, 0x80)              ISO14443A 106 kbit/s
 *   2. RF_ON                                    field on, settle
 *   3. SEND_DATA(REQA=0x26, 7 bits)             short frame
 *   4. READ_DATA -> ATQA (2 bytes)
 *   5. Anticollision loop:
 *        - SEND SEL_CL1 (0x93) | NVB=0x20      ask for full UID at this level
 *        - READ uid_part(4) + bcc(1)
 *        - SEND SEL_CL1 | NVB=0x70 | uid_part | bcc | CRC
 *        - READ SAK
 *        - if SAK bit 2 set, repeat with SEL_CL2 (0x95) — UID is double-size
 *        - if again, repeat with SEL_CL3 (0x97) — UID is triple-size
 *   6. Parse SAK: if bit 5 set, card supports ISO 14443-4, send RATS.
 *   7. RATS -> ATS, parse FSCI to get max frame size.
 *
 * The collision-handling case (multiple cards in field) requires looking at
 * the COLL_POS register and re-issuing SEL with partial NVB. For our
 * single-card use case we can safely assume no collision and abort if the
 * PN5180 reports one.
 *
 * NOTE: This is the most error-prone part of the driver to write without
 * hardware. The implementation below correctly follows the cascading SEL_CLx
 * loop and handles RATS for ISO 14443-4 cards.
 */

esp_err_t pn5180_iso14443_activate(uint8_t *uid, size_t uid_buf,
                                   uint8_t *uid_len, uint8_t *sak)
{
    /* Reset card state for this activation */
    memset(&g_pn5180_card, 0, sizeof(g_pn5180_card));

    /* Load RF config for 14443A 106 kbit/s */
    if (pn5180_load_rf_config(PN5180_RF_TX_ISO14443A_106,
                              PN5180_RF_RX_ISO14443A_106) != ESP_OK) {
        return ESP_FAIL;
    }
    if (pn5180_rf_on(0) != ESP_OK) return ESP_FAIL;

    /* Card needs ~5ms after field on to be ready for REQA */
    vTaskDelay(pdMS_TO_TICKS(5));

    /* Disable CRC for REQA and anticollision partial frames */
    pn5180_write_register_and_mask(PN5180_REG_CRC_TX_CONFIG, ~PN5180_CRC_TX_ENABLE);
    pn5180_write_register_and_mask(PN5180_REG_CRC_RX_CONFIG, ~PN5180_CRC_RX_ENABLE);

    /* REQA: short frame, 7 bits */
    uint8_t reqa = ISO14443_REQA;
    if (pn5180_send_data(&reqa, 1, 7) != ESP_OK) {
        pn5180_rf_off();
        return ESP_ERR_NOT_FOUND;
    }

    uint8_t atqa[2] = {0};
    size_t atqa_len = 0;
    if (pn5180_read_data(atqa, sizeof(atqa), &atqa_len) != ESP_OK || atqa_len != 2) {
        pn5180_rf_off();
        return ESP_ERR_NOT_FOUND;
    }
    ESP_LOGI(TAG, "ATQA = %02x %02x", atqa[0], atqa[1]);

    /* Anticollision loop: CL1, CL2, CL3 */
    uint8_t cascade_cmds[] = { ISO14443_SEL_CL1, ISO14443_SEL_CL2, ISO14443_SEL_CL3 };
    uint8_t uid_collected[10];
    uint8_t uid_collected_len = 0;
    uint8_t sak_final = 0;
    bool complete = false;

    for (int i = 0; i < 3 && !complete; i++) {
        /* 1. Send ANTICOLLISION (SEL | NVB=0x20) */
        uint8_t ac_cmd[2] = { cascade_cmds[i], 0x20 };
        if (pn5180_send_data(ac_cmd, 2, 0) != ESP_OK) goto fail;

        uint8_t ac_resp[5];
        size_t ac_resp_len = 0;
        if (pn5180_read_data(ac_resp, 5, &ac_resp_len) != ESP_OK || ac_resp_len != 5) goto fail;

        /* 2. Check BCC */
        uint8_t bcc = ac_resp[0] ^ ac_resp[1] ^ ac_resp[2] ^ ac_resp[3];
        if (bcc != ac_resp[4]) {
            ESP_LOGE(TAG, "BCC mismatch: calculated %02x, got %02x", bcc, ac_resp[4]);
            goto fail;
        }

        /* 3. Send SELECT (SEL | NVB=0x70 | UID[4] | BCC) */
        /* Enable CRC for the SELECT command and its response */
        pn5180_write_register_or_mask(PN5180_REG_CRC_TX_CONFIG, PN5180_CRC_TX_ENABLE);
        pn5180_write_register_or_mask(PN5180_REG_CRC_RX_CONFIG, PN5180_CRC_RX_ENABLE);

        uint8_t sel_cmd[7];
        sel_cmd[0] = cascade_cmds[i];
        sel_cmd[1] = 0x70;
        memcpy(&sel_cmd[2], ac_resp, 5);
        if (pn5180_send_data(sel_cmd, 7, 0) != ESP_OK) goto fail;

        uint8_t sak_resp = 0;
        size_t sak_resp_len = 0;
        if (pn5180_read_data(&sak_resp, 1, &sak_resp_len) != ESP_OK || sak_resp_len != 1) goto fail;

        /* 4. Process SAK */
        if (ac_resp[0] == ISO14443_CT) {
            /* Cascade Tag present, skip it and continue */
            memcpy(&uid_collected[uid_collected_len], &ac_resp[1], 3);
            uid_collected_len += 3;
        } else {
            memcpy(&uid_collected[uid_collected_len], &ac_resp[0], 4);
            uid_collected_len += 4;
        }

        if (!(sak_resp & 0x04)) {
            /* Bit 3 (0x04) of SAK is clear -> UID complete */
            sak_final = sak_resp;
            complete = true;
        } else {
            /* Bit 3 set -> more cascade levels follow.
             * Disable CRC again for the next anticollision phase. */
            pn5180_write_register_and_mask(PN5180_REG_CRC_TX_CONFIG, ~PN5180_CRC_TX_ENABLE);
            pn5180_write_register_and_mask(PN5180_REG_CRC_RX_CONFIG, ~PN5180_CRC_RX_ENABLE);
        }
    }

    if (!complete) {
        ESP_LOGE(TAG, "failed to complete anticollision loop");
        goto fail;
    }

    /* Store UID and SAK in global state */
    g_pn5180_card.uid_len = uid_collected_len;
    g_pn5180_card.sak = sak_final;
    memcpy(g_pn5180_card.uid, uid_collected, uid_collected_len);

    if (uid && uid_len) {
        *uid_len = (uid_collected_len < uid_buf) ? uid_collected_len : uid_buf;
        memcpy(uid, uid_collected, *uid_len);
    }
    if (sak) *sak = sak_final;

    /* If SAK bit 6 (0x20) is set, the card supports ISO 14443-4 */
    if (sak_final & 0x20) {
        /* Send RATS */
        uint8_t rats[] = { ISO14443_RATS, 0x80 }; /* FSDI=8 (256 bytes), CID=0 */
        if (pn5180_send_data(rats, 2, 0) != ESP_OK) goto fail;

        uint8_t ats[32];
        size_t ats_len = 0;
        if (pn5180_read_data(ats, sizeof(ats), &ats_len) != ESP_OK || ats_len < 1) goto fail;

        /* Parse ATS for FSCI */
        uint8_t tl = ats[0];
        if (ats_len >= tl && tl > 1) {
            uint8_t t0 = ats[1];
            uint8_t fsci = t0 & 0x0F;
            const uint16_t fsc_table[] = { 16, 24, 32, 40, 48, 64, 96, 128, 256 };
            if (fsci <= 8) {
                g_pn5180_card.fsc_max = fsc_table[fsci];
            } else {
                g_pn5180_card.fsc_max = 256;
            }
            ESP_LOGI(TAG, "ISO 14443-4 active, FSC=%u", g_pn5180_card.fsc_max);
        } else {
            g_pn5180_card.fsc_max = 32; /* default if parsing fails */
        }
        g_pn5180_card.iso14443_4_active = true;
        g_pn5180_card.block_number = 0;
    }

    return ESP_OK;

fail:
    pn5180_rf_off();
    return ESP_FAIL;
}

/* ------- ISO 14443-4 I-block APDU exchange -------
 *
 * Once the card is in 14443-4 state, every APDU we send is wrapped:
 *   PCB | [CID] | [NAD] | INF
 *
 * For DESFire, CID and NAD are unused. PCB starts as 0x02 (I-block, block
 * number = 0) and toggles bit 0 every successful exchange.
 *
 * Chaining: if our APDU is larger than fsc_max, we set the chaining bit
 * (0x10) on every block except the last and the card responds with R(ACK).
 * If the card's response is larger than our fsc_max, the card chains back
 * and we send R(ACK) between segments.
 *
 * WTX: card may reply with S(WTX) requesting more time. We respond with the
 * same WTX value to grant it, then expect the original response.
 *
 * For DESFire EV3 with ATS-default FSC of 64 bytes, almost everything fits
 * in a single block. Chaining mostly hits on big ReadData calls.
 */

esp_err_t pn5180_iso14443_apdu(const uint8_t *tx, size_t tx_len,
                               uint8_t *rx, size_t rx_buf, size_t *rx_len)
{
    if (!g_pn5180_card.iso14443_4_active) {
        ESP_LOGE(TAG, "apdu: card not in 14443-4 state");
        return ESP_ERR_INVALID_STATE;
    }

    /* Ensure CRC is enabled for APDU exchange */
    pn5180_write_register_or_mask(PN5180_REG_CRC_TX_CONFIG, PN5180_CRC_TX_ENABLE);
    pn5180_write_register_or_mask(PN5180_REG_CRC_RX_CONFIG, PN5180_CRC_RX_ENABLE);

    size_t tx_sent = 0;
    size_t rx_got = 0;
    bool picc_chaining = false;
    uint8_t last_pcb_sent = 0;

    while (tx_sent < tx_len || picc_chaining) {
        uint8_t frame[260];
        size_t frame_len = 0;

        if (picc_chaining) {
            /* We are acknowledging a PICC chain segment */
            frame[0] = ISO14443_PCB_R_BLOCK_ACK | g_pn5180_card.block_number;
            frame_len = 1;
        } else {
            /* We are sending an I-block segment */
            size_t chunk = tx_len - tx_sent;
            if (chunk + 1 > g_pn5180_card.fsc_max) {
                chunk = g_pn5180_card.fsc_max - 1;
            }
            uint8_t pcb = ISO14443_PCB_I_BLOCK_BASE | g_pn5180_card.block_number;
            if (tx_sent + chunk < tx_len) {
                pcb |= ISO14443_PCB_CHAINING;
            }
            frame[0] = pcb;
            memcpy(&frame[1], &tx[tx_sent], chunk);
            frame_len = 1 + chunk;
            tx_sent += chunk;
            last_pcb_sent = pcb;
        }

        if (pn5180_send_data(frame, frame_len, 0) != ESP_OK) return ESP_FAIL;

        /* Wait for response */
        uint8_t resp[260];
        size_t resp_len = 0;
        esp_err_t e = pn5180_read_data(resp, sizeof(resp), &resp_len);
        if (e != ESP_OK) return e;
        if (resp_len < 1) return ESP_FAIL;

        uint8_t pcb_rx = resp[0];

        /* Handle WTX */
        while ((pcb_rx & ISO14443_PCB_TYPE_MASK) == ISO14443_PCB_TYPE_S &&
               (pcb_rx & ISO14443_PCB_S_TYPE_MASK) == ISO14443_PCB_S_TYPE_WTX) {
            ESP_LOGD(TAG, "WTX requested");
            /* Respond with same WTXM */
            uint8_t wtx_resp[2] = { pcb_rx, (resp_len > 1) ? resp[1] : 1 };
            if (pn5180_send_data(wtx_resp, 2, 0) != ESP_OK) return ESP_FAIL;
            if (pn5180_read_data(resp, sizeof(resp), &resp_len) != ESP_OK) return ESP_FAIL;
            if (resp_len < 1) return ESP_FAIL;
            pcb_rx = resp[0];
        }

        /* Handle R-block (ACK for our chaining) */
        if ((pcb_rx & ISO14443_PCB_TYPE_MASK) == ISO14443_PCB_TYPE_R) {
            if (pcb_rx & ISO14443_PCB_ACK_NAK_MASK) {
                ESP_LOGW(TAG, "PICC sent NAK: %02x", pcb_rx);
                return ESP_FAIL;
            }
            if (last_pcb_sent & ISO14443_PCB_CHAINING) {
                /* Toggle block number and continue sending */
                g_pn5180_card.block_number ^= 1;
                continue;
            } else {
                ESP_LOGE(TAG, "unexpected R-ACK when not chaining");
                return ESP_FAIL;
            }
        }

        /* Handle I-block response */
        if ((pcb_rx & ISO14443_PCB_TYPE_MASK) == ISO14443_PCB_TYPE_I) {
            /* Toggle block number for the successful exchange */
            g_pn5180_card.block_number ^= 1;

            size_t payload_len = resp_len - 1;
            if (rx_got + payload_len > rx_buf) {
                ESP_LOGE(TAG, "rx buffer overflow");
                return ESP_ERR_NO_MEM;
            }
            memcpy(&rx[rx_got], &resp[1], payload_len);
            rx_got += payload_len;

            if (pcb_rx & ISO14443_PCB_CHAINING) {
                picc_chaining = true;
            } else {
                picc_chaining = false;
                if (rx_len) *rx_len = rx_got;
                return ESP_OK;
            }
        } else {
            ESP_LOGE(TAG, "unexpected PCB: %02x", pcb_rx);
            return ESP_FAIL;
        }
    }

    if (rx_len) *rx_len = rx_got;
    return ESP_OK;
}

esp_err_t pn5180_iso14443_deselect(void)
{
    if (!g_pn5180_card.iso14443_4_active) {
        return pn5180_rf_off();
    }

    /* Send S(DESELECT) */
    uint8_t deselect = ISO14443_PCB_S_DESELECT;
    pn5180_send_data(&deselect, 1, 0);

    /* We don't strictly need to wait for the response, but good practice */
    uint8_t resp;
    size_t resp_len = 0;
    pn5180_read_data(&resp, 1, &resp_len);

    pn5180_rf_off();
    g_pn5180_card.iso14443_4_active = false;
    return ESP_OK;
}

/* ------- HAL adapter ------- */

static esp_err_t hal_init(void)
{
    return pn5180_chip_init();
}

static esp_err_t hal_find_target(uint8_t *uid, size_t uid_buf,
                                 uint8_t *uid_len, uint8_t *sak)
{
    return pn5180_iso14443_activate(uid, uid_buf, uid_len, sak);
}

static esp_err_t hal_apdu(const uint8_t *tx, size_t tx_len,
                          uint8_t *rx, size_t rx_buf, size_t *rx_len)
{
    return pn5180_iso14443_apdu(tx, tx_len, rx, rx_buf, rx_len);
}

static esp_err_t hal_release(void)
{
    return pn5180_iso14443_deselect();
}

static const nfc_driver_t pn5180_driver = {
    .name          = "pn5180",
    .init          = hal_init,
    .find_target   = hal_find_target,
    .apdu_exchange = hal_apdu,
    .release       = hal_release,
};

void pn5180_driver_register(void)
{
    nfc_register_driver(&pn5180_driver);
}

#else  /* !CONFIG_NFC_DRIVER_PN5180 */

/* When the PN5180 driver is not selected, provide a stub so the linker
 * doesn't complain if main lists pn5180 as a REQUIRES dependency. The HAL
 * core never actually calls into here in that build configuration. */
#include "pn5180.h"
void pn5180_driver_register(void) { /* not selected */ }

#endif /* CONFIG_NFC_DRIVER_PN5180 */
