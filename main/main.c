#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "nvs_flash.h"
#include "esp_log.h"

#include "board_io.h"
#include "pn5180.h"
#include "netconfig.h"
#include "webui.h"
#include "reader_task.h"

static const char *TAG = "main";

void app_main(void)
{
    /* NVS — stores WiFi creds and OSDP config */
    esp_err_t e = nvs_flash_init();
    if (e == ESP_ERR_NVS_NO_FREE_PAGES || e == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        e = nvs_flash_init();
    }
    ESP_ERROR_CHECK(e);

    /* Hardware init: LEDs, buzzer, button */
    board_init();

    /* Register the PN5180 driver with the NFC HAL.
     * nfc_init() is called inside reader_task after the task starts. */
    pn5180_driver_register();

    /* Shared queue: reader_task posts JSON strings, webui consumes them.
     * Depth 4 so a burst of rapid taps doesn't stall the reader. */
    QueueHandle_t card_q = xQueueCreate(4, sizeof(char *));
    if (!card_q) {
        ESP_LOGE(TAG, "failed to create card queue");
        return;
    }

    /* Network (WiFi AP or STA) — blocks up to 10s trying STA */
    if (netconfig_init() != ESP_OK) {
        ESP_LOGE(TAG, "netconfig failed; continuing without WiFi");
    }

    /* Web debug UI on core 0 */
    webui_start(card_q);

    /* NFC reader on core 1 */
    reader_task_start(card_q);

    ESP_LOGI(TAG, "system up — driver=pn5180 wifi=%s",
             netconfig_sta_connected() ? "STA" :
             netconfig_in_ap_mode()    ? "AP"  : "down");
}
