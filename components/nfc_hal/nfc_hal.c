#include "nfc_hal.h"
#include "esp_log.h"
#include "sdkconfig.h"
#include <string.h>

static const char *TAG = "nfc_hal";
static const nfc_driver_t *s_drv = NULL;

void nfc_register_driver(const nfc_driver_t *drv)
{
    if (s_drv) {
        ESP_LOGW(TAG, "driver already registered (%s); replacing with %s",
                 s_drv->name, drv ? drv->name : "(null)");
    }
    s_drv = drv;
}

esp_err_t nfc_init(void)
{
    if (!s_drv) {
        ESP_LOGE(TAG, "no NFC driver registered! Call <chip>_driver_register() before nfc_init()");
        return ESP_ERR_INVALID_STATE;
    }
    ESP_LOGI(TAG, "init: %s", s_drv->name);
    return s_drv->init ? s_drv->init() : ESP_OK;
}

esp_err_t nfc_find_target(uint8_t *uid, size_t uid_buf_len,
                          uint8_t *uid_len, uint8_t *sak)
{
    if (!s_drv || !s_drv->find_target) return ESP_ERR_INVALID_STATE;
    return s_drv->find_target(uid, uid_buf_len, uid_len, sak);
}

esp_err_t nfc_apdu_exchange(const uint8_t *tx, size_t tx_len,
                            uint8_t *rx, size_t rx_buf_len, size_t *rx_len)
{
    if (!s_drv || !s_drv->apdu_exchange) return ESP_ERR_INVALID_STATE;
    return s_drv->apdu_exchange(tx, tx_len, rx, rx_buf_len, rx_len);
}

esp_err_t nfc_release(void)
{
    if (!s_drv || !s_drv->release) return ESP_OK;
    return s_drv->release();
}

const char *nfc_driver_name(void)
{
    return s_drv ? s_drv->name : "none";
}

bool nfc_is_ready(void)
{
    return s_drv != NULL;
}
