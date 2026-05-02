#include "pn5180_internal.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_timer.h"
#include <string.h>

#if defined(CONFIG_NFC_DRIVER_PN5180)

static const char *TAG = "pn5180_xcvr";

esp_err_t pn5180_load_rf_config(uint8_t tx_idx, uint8_t rx_idx)
{
    uint8_t f[3] = { PN5180_CMD_LOAD_RF_CONFIG, tx_idx, rx_idx };
    return pn5180_send_command(f, sizeof(f), NULL, 0, NULL);
}

esp_err_t pn5180_rf_on(uint8_t config_idx)
{
    /* RF_ON command: 0x16 | flags(1).
     * Bit 0 of flags = collision avoidance enable. We want it on for a real
     * reader. */
    uint8_t f[2] = { PN5180_CMD_RF_ON, 0x01 };
    esp_err_t e = pn5180_send_command(f, sizeof(f), NULL, 0, NULL);
    if (e != ESP_OK) return e;
    /* The chip raises IRQ_RF_ON when the field has stabilized. */
    return pn5180_wait_irq(PN5180_IRQ_RF_ON, 100);
}

esp_err_t pn5180_rf_off(void)
{
    uint8_t f[2] = { PN5180_CMD_RF_OFF, 0x00 };
    esp_err_t e = pn5180_send_command(f, sizeof(f), NULL, 0, NULL);
    if (e != ESP_OK) return e;
    return pn5180_wait_irq(PN5180_IRQ_RF_OFF, 100);
}

esp_err_t pn5180_wait_irq(uint32_t mask, uint32_t timeout_ms)
{
    int64_t deadline = esp_timer_get_time() + (int64_t)timeout_ms * 1000;
    uint32_t status = 0;
    do {
        if (pn5180_read_register(PN5180_REG_IRQ_STATUS, &status) != ESP_OK) {
            return ESP_FAIL;
        }
        if (status & mask) {
            /* Clear matched bits — IRQ_CLEAR is write-1-to-clear */
            pn5180_write_register(PN5180_REG_IRQ_CLEAR, status & mask);
            return ESP_OK;
        }
        vTaskDelay(1);
    } while (esp_timer_get_time() < deadline);
    ESP_LOGW(TAG, "wait_irq mask=0x%08x timeout (status=0x%08x)",
             (unsigned)mask, (unsigned)status);
    return ESP_ERR_TIMEOUT;
}

esp_err_t pn5180_send_data(const uint8_t *data, size_t len,
                           uint8_t valid_bits_in_last_byte)
{
    /*
     * SEND_DATA frame: 0x09 | num_valid_bits_last_byte | data...
     *
     * num_valid_bits_last_byte: 0..7 means partial byte at end, 8 means full.
     * The chip transmits len-1 full bytes followed by num_valid_bits_last_byte
     * bits of the last byte. Used during anticollision when sending NVB-
     * controlled partial frames.
     *
     * For ordinary full-byte transmissions we pass valid_bits_in_last_byte=0
     * (interpreted as "all 8 bits of the last byte are valid" — yes, the
     * encoding is annoying; the datasheet table shows 0 means full byte for
     * SEND_DATA specifically).
     *
     * Frame size limit: per command, up to 260 bytes.
     */
    if (len > 260) return ESP_ERR_INVALID_SIZE;

    uint8_t buf[2 + 260];
    buf[0] = PN5180_CMD_SEND_DATA;
    buf[1] = valid_bits_in_last_byte & 0x07;
    if (len) memcpy(&buf[2], data, len);

    /* Clear any stale TX/RX done bits before sending */
    pn5180_write_register(PN5180_REG_IRQ_CLEAR,
                          PN5180_IRQ_TX_DONE | PN5180_IRQ_RX_DONE);

    esp_err_t e = pn5180_send_command(buf, 2 + len, NULL, 0, NULL);
    if (e != ESP_OK) return e;

    /* Wait for TX_DONE — the chip raises this when the last bit hits the air */
    return pn5180_wait_irq(PN5180_IRQ_TX_DONE, 50);
}

esp_err_t pn5180_read_data(uint8_t *out, size_t buf_len, size_t *out_len)
{
    /*
     * Two-step: wait for RX_DONE, then read RX_STATUS to learn how many
     * bytes are available, then issue READ_DATA to retrieve them.
     */
    esp_err_t e = pn5180_wait_irq(PN5180_IRQ_RX_DONE, 50);
    if (e != ESP_OK) return e;

    uint32_t rx_status = 0;
    e = pn5180_read_register(PN5180_REG_RX_STATUS, &rx_status);
    if (e != ESP_OK) return e;

    /* RX_STATUS layout: bit 18=CRC error, bit 17=collision.
     * For our single-card use case, both are fatal. */
    if (rx_status & (PN5180_RX_STATUS_ERROR_MASK | PN5180_RX_STATUS_COLLISION_MASK)) {
        ESP_LOGW(TAG, "rx error: rx_status=0x%08x", (unsigned)rx_status);
        return (rx_status & PN5180_RX_STATUS_COLLISION_MASK) ? ESP_ERR_INVALID_STATE : ESP_ERR_INVALID_CRC;
    }

    size_t avail = rx_status & PN5180_RX_STATUS_LEN_MASK;
    if (avail > buf_len) {
        ESP_LOGW(TAG, "rx %u bytes; buffer only %u", (unsigned)avail, (unsigned)buf_len);
        return ESP_ERR_INVALID_SIZE;
    }

    /* READ_DATA: 0x0A | 0x00 (always zero per datasheet) */
    uint8_t f[2] = { PN5180_CMD_READ_DATA, 0x00 };
    size_t got = 0;
    e = pn5180_send_command(f, sizeof(f), out, avail, &got);
    if (e != ESP_OK) return e;
    if (out_len) *out_len = got;
    return ESP_OK;
}

#endif /* CONFIG_NFC_DRIVER_PN5180 */
