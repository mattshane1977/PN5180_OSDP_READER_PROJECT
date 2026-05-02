#include "pn5180_internal.h"
#include "board.h"

#include "driver/spi_master.h"
#include "driver/gpio.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_timer.h"
#include "esp_log.h"

#include <string.h>

#if defined(CONFIG_NFC_DRIVER_PN5180)

static const char *TAG = "pn5180_spi";
static spi_device_handle_t s_spi;

/*
 * SPI protocol (datasheet §11):
 *   - SPI mode 0, MSB first, max 7 MHz.
 *   - Each command is sent in a single CS-asserted transaction.
 *   - After CS goes high, the chip pulls BUSY high and processes. We MUST wait
 *     for BUSY to go low before issuing the next command.
 *   - Response retrieval (for commands that return data) is a separate
 *     transaction issued AFTER BUSY drops.
 *
 * BUSY semantics caveat: BUSY can go high within a few µs of CS rising, so
 * polling immediately after CS-high without a tiny settling delay can race.
 * We add a short delay before the busy-wait loop.
 */

static inline void cs_low(void)  { gpio_set_level(BOARD_PN5180_PIN_NSS, 0); }
static inline void cs_high(void) { gpio_set_level(BOARD_PN5180_PIN_NSS, 1); }

static esp_err_t wait_busy_low(uint32_t timeout_ms)
{
    /* Settling delay — datasheet recommends ≥1µs after CS edge before sampling */
    esp_rom_delay_us(2);

    int64_t deadline = esp_timer_get_time() + (int64_t)timeout_ms * 1000;
    while (gpio_get_level(BOARD_PN5180_PIN_BUSY) == 1) {
        if (esp_timer_get_time() > deadline) {
            ESP_LOGW(TAG, "BUSY did not drop within %u ms", (unsigned)timeout_ms);
            return ESP_ERR_TIMEOUT;
        }
        vTaskDelay(1);   /* ~1 tick = 10ms at 100Hz; consider esp_rom_delay_us if hot */
    }
    return ESP_OK;
}

static esp_err_t spi_xfer(const uint8_t *tx, uint8_t *rx, size_t len)
{
    spi_transaction_t t = {
        .length    = len * 8,
        .tx_buffer = tx,
        .rx_buffer = rx,
    };
    cs_low();
    esp_err_t e = spi_device_polling_transmit(s_spi, &t);
    cs_high();
    return e;
}

esp_err_t pn5180_spi_init(void)
{
    /* GPIOs: NSS, BUSY (input), RST */
    gpio_config_t out = {
        .pin_bit_mask = (1ULL << BOARD_PN5180_PIN_NSS) |
                        (1ULL << BOARD_PN5180_PIN_RST),
        .mode = GPIO_MODE_OUTPUT,
    };
    gpio_config(&out);

    gpio_config_t in = {
        .pin_bit_mask = (1ULL << BOARD_PN5180_PIN_BUSY),
        .mode = GPIO_MODE_INPUT,
    };
    gpio_config(&in);

    cs_high();
    pn5180_spi_reset();

    spi_bus_config_t bus = {
        .miso_io_num = BOARD_PN5180_PIN_MISO,
        .mosi_io_num = BOARD_PN5180_PIN_MOSI,
        .sclk_io_num = BOARD_PN5180_PIN_SCK,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = 512,
    };
    /* spi_bus_initialize returns INVALID_STATE if the bus was already brought
     * up by another driver in this build — fine, just continue. */
    esp_err_t e = spi_bus_initialize(BOARD_PN5180_SPI_HOST, &bus, SPI_DMA_CH_AUTO);
    if (e != ESP_OK && e != ESP_ERR_INVALID_STATE) return e;

    spi_device_interface_config_t dev = {
        .clock_speed_hz = BOARD_PN5180_SPI_HZ,
        .mode = 0,
        .spics_io_num = -1,             /* manual CS for BUSY semantics */
        .queue_size = 4,
        .flags = 0,                     /* MSB first — datasheet default */
    };
    return spi_bus_add_device(BOARD_PN5180_SPI_HOST, &dev, &s_spi);
}

esp_err_t pn5180_spi_reset(void)
{
    gpio_set_level(BOARD_PN5180_PIN_RST, 0);
    vTaskDelay(pdMS_TO_TICKS(2));
    gpio_set_level(BOARD_PN5180_PIN_RST, 1);
    vTaskDelay(pdMS_TO_TICKS(10));
    return ESP_OK;
}

esp_err_t pn5180_send_command(const uint8_t *frame, size_t len,
                              uint8_t *resp, size_t resp_buf, size_t *resp_len)
{
    if (wait_busy_low(50) != ESP_OK) return ESP_ERR_TIMEOUT;

    esp_err_t e = spi_xfer(frame, NULL, len);
    if (e != ESP_OK) return e;

    /* Wait for the chip to finish processing before any read */
    if (wait_busy_low(50) != ESP_OK) return ESP_ERR_TIMEOUT;

    if (resp && resp_buf) {
        e = spi_xfer(NULL, resp, resp_buf);
        if (resp_len) *resp_len = resp_buf;
    } else if (resp_len) {
        *resp_len = 0;
    }
    return e;
}

esp_err_t pn5180_write_register(uint8_t reg, uint32_t value)
{
    uint8_t f[6];
    f[0] = PN5180_CMD_WRITE_REGISTER;
    f[1] = reg;
    f[2] = (uint8_t)(value & 0xFF);
    f[3] = (uint8_t)((value >> 8) & 0xFF);
    f[4] = (uint8_t)((value >> 16) & 0xFF);
    f[5] = (uint8_t)((value >> 24) & 0xFF);
    return pn5180_send_command(f, sizeof(f), NULL, 0, NULL);
}

esp_err_t pn5180_write_register_or_mask(uint8_t reg, uint32_t mask)
{
    uint8_t f[6];
    f[0] = PN5180_CMD_WRITE_REGISTER_OR_MASK;
    f[1] = reg;
    f[2] = (uint8_t)(mask & 0xFF);
    f[3] = (uint8_t)((mask >> 8) & 0xFF);
    f[4] = (uint8_t)((mask >> 16) & 0xFF);
    f[5] = (uint8_t)((mask >> 24) & 0xFF);
    return pn5180_send_command(f, sizeof(f), NULL, 0, NULL);
}

esp_err_t pn5180_write_register_and_mask(uint8_t reg, uint32_t mask)
{
    uint8_t f[6];
    f[0] = PN5180_CMD_WRITE_REGISTER_AND_MASK;
    f[1] = reg;
    f[2] = (uint8_t)(mask & 0xFF);
    f[3] = (uint8_t)((mask >> 8) & 0xFF);
    f[4] = (uint8_t)((mask >> 16) & 0xFF);
    f[5] = (uint8_t)((mask >> 24) & 0xFF);
    return pn5180_send_command(f, sizeof(f), NULL, 0, NULL);
}

esp_err_t pn5180_read_register(uint8_t reg, uint32_t *out)
{
    uint8_t f[2] = { PN5180_CMD_READ_REGISTER, reg };
    uint8_t r[4] = {0};
    size_t  rl = 0;
    esp_err_t e = pn5180_send_command(f, sizeof(f), r, sizeof(r), &rl);
    if (e != ESP_OK) return e;
    *out = (uint32_t)r[0]
         | ((uint32_t)r[1] << 8)
         | ((uint32_t)r[2] << 16)
         | ((uint32_t)r[3] << 24);
    return ESP_OK;
}

#endif /* CONFIG_NFC_DRIVER_PN5180 */
