#include "reader_task.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"

#include "board_io.h"
#include "nfc_hal.h"
#include "desfire.h"

#include <string.h>
#include <stdio.h>
#include <stdlib.h>

static const char *TAG = "reader";

/* JSON build buffer — large enough for 28 apps × a few files each */
#define JSON_BUF 4096

/* ------- JSON helpers ------- */

static int append(char *buf, int pos, int cap, const char *fmt, ...)
    __attribute__((format(printf, 4, 5)));

static int append(char *buf, int pos, int cap, const char *fmt, ...)
{
    if (pos >= cap - 1) return pos;
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(buf + pos, cap - pos, fmt, ap);
    va_end(ap);
    if (n < 0) return pos;
    return pos + (n < cap - pos ? n : cap - pos - 1);
}

static void hex_str(const uint8_t *data, size_t len, char *out, size_t out_len)
{
    out[0] = '\0';
    for (size_t i = 0; i < len && i * 2 + 2 < out_len; i++) {
        snprintf(out + i * 2, 3, "%02x", data[i]);
    }
}

static uint32_t storage_bytes(uint8_t encoded)
{
    return (uint32_t)1 << (encoded >> 1);
}

/* ------- card enumeration and JSON build ------- */

static char *build_card_json(const uint8_t *uid, uint8_t uid_len)
{
    char *buf = malloc(JSON_BUF);
    if (!buf) return NULL;

    int pos = 0;
    char uid_hex[16];
    hex_str(uid, uid_len, uid_hex, sizeof(uid_hex));

    pos = append(buf, pos, JSON_BUF, "{\"type\":\"card\",\"uid\":\"%s\"", uid_hex);

    /* GetVersion */
    desfire_card_version_t ver;
    if (desfire_get_version(&ver) == ESP_OK) {
        char uid_real[16];
        hex_str(ver.uid, 7, uid_real, sizeof(uid_real));

        pos = append(buf, pos, JSON_BUF,
            ",\"hw\":{\"vendor\":%u,\"type\":%u,\"subtype\":%u"
            ",\"ver\":\"%u.%u\",\"storage_enc\":%u}"
            ",\"sw\":{\"vendor\":%u,\"type\":%u,\"subtype\":%u"
            ",\"ver\":\"%u.%u\"}"
            ",\"uid_real\":\"%s\"",
            ver.hw_vendor_id, ver.hw_type, ver.hw_subtype,
            ver.hw_major_version, ver.hw_minor_version, ver.hw_storage_size,
            ver.sw_vendor_id, ver.sw_type, ver.sw_subtype,
            ver.sw_major_version, ver.sw_minor_version,
            uid_real);

        ESP_LOGI(TAG, "HW v%u.%u / SW v%u.%u  storage~%luB  real-UID:%s",
                 ver.hw_major_version, ver.hw_minor_version,
                 ver.sw_major_version, ver.sw_minor_version,
                 (unsigned long)storage_bytes(ver.hw_storage_size),
                 uid_real);
    } else {
        ESP_LOGW(TAG, "GetVersion failed (status 0x%02x)", desfire_last_status());
    }

    /* GetApplicationIDs */
    uint8_t aids[DESFIRE_MAX_APPS][DESFIRE_AID_LEN];
    uint8_t aid_count = 0;
    pos = append(buf, pos, JSON_BUF, ",\"apps\":[");

    if (desfire_get_application_ids(aids, &aid_count, DESFIRE_MAX_APPS) == ESP_OK) {
        ESP_LOGI(TAG, "%u application(s) on card", aid_count);

        for (int a = 0; a < aid_count; a++) {
            if (a > 0) pos = append(buf, pos, JSON_BUF, ",");
            pos = append(buf, pos, JSON_BUF,
                         "{\"aid\":\"%02x%02x%02x\"",
                         aids[a][0], aids[a][1], aids[a][2]);

            /* Select this application and enumerate its files */
            if (desfire_select_application(aids[a]) != ESP_OK) {
                pos = append(buf, pos, JSON_BUF, ",\"files\":null}");
                continue;
            }

            uint8_t file_ids[DESFIRE_MAX_FILES];
            uint8_t file_count = 0;
            esp_err_t fid_err = desfire_get_file_ids(file_ids, &file_count,
                                                     DESFIRE_MAX_FILES);

            if (fid_err != ESP_OK) {
                /* Permission denied or other error — report AID without files */
                bool denied = (desfire_last_status() == 0x9D);
                pos = append(buf, pos, JSON_BUF,
                             ",\"files\":null,\"denied\":%s}",
                             denied ? "true" : "false");
                ESP_LOGI(TAG, "  AID %02x%02x%02x: GetFileIDs status 0x%02x",
                         aids[a][0], aids[a][1], aids[a][2],
                         desfire_last_status());
                continue;
            }

            pos = append(buf, pos, JSON_BUF, ",\"files\":[");
            for (int f = 0; f < file_count; f++) {
                desfire_file_settings_t fs;
                if (f > 0) pos = append(buf, pos, JSON_BUF, ",");

                if (desfire_get_file_settings(file_ids[f], &fs) != ESP_OK) {
                    pos = append(buf, pos, JSON_BUF,
                                 "{\"id\":%u,\"type\":\"?\",\"size\":0"
                                 ",\"comm\":\"?\",\"ar\":\"????\"}",
                                 file_ids[f]);
                    continue;
                }

                static const char *type_names[] = {"std","bkp","val","lin","cyc"};
                const char *tname = (fs.type <= 4) ? type_names[fs.type] : "?";
                const char *cname = (fs.comm_mode == DESFIRE_COMM_ENCRYPTED) ? "enc"
                                  : (fs.comm_mode == DESFIRE_COMM_CMAC)      ? "mac"
                                  : "plain";

                pos = append(buf, pos, JSON_BUF,
                             "{\"id\":%u,\"type\":\"%s\",\"size\":%lu"
                             ",\"comm\":\"%s\",\"ar\":\"%04x\"}",
                             file_ids[f], tname, (unsigned long)fs.size,
                             cname, fs.access_rights);

                ESP_LOGI(TAG, "    file %u: %s %s %luB ar=0x%04x",
                         file_ids[f], tname, cname,
                         (unsigned long)fs.size, fs.access_rights);
            }
            pos = append(buf, pos, JSON_BUF, "]}");
        }
    } else {
        ESP_LOGW(TAG, "GetApplicationIDs failed");
    }

    pos = append(buf, pos, JSON_BUF, "]}");
    return buf;
}

/* ------- scan loop ------- */

static void reader_task(void *arg)
{
    QueueHandle_t q = (QueueHandle_t)arg;

    esp_err_t err = nfc_init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "NFC init failed (0x%x) — check PN5180 wiring", err);
        board_indicate(IND_ERROR);
        vTaskDelete(NULL);
        return;
    }
    desfire_init();
    board_indicate(IND_ARMED_READ);
    ESP_LOGI(TAG, "PN5180 ready, scanning...");

    bool card_in_field = false;
    int  empty_count   = 0;

    for (;;) {
        uint8_t uid[10] = {0};
        uint8_t uid_len = 0, sak = 0;
        esp_err_t e = nfc_find_target(uid, sizeof(uid), &uid_len, &sak);

        if (e == ESP_OK) {
            empty_count = 0;
            if (!card_in_field) {
                card_in_field = true;
                char uid_hex[22] = {0};
                hex_str(uid, uid_len, uid_hex, sizeof(uid_hex));
                ESP_LOGI(TAG, "card detected UID=%s SAK=0x%02x", uid_hex, sak);

                char *json = build_card_json(uid, uid_len);
                if (json) {
                    if (xQueueSend(q, &json, pdMS_TO_TICKS(200)) != pdTRUE) {
                        free(json);     /* queue full — drop */
                    }
                }
                board_indicate(IND_GRANT);
                nfc_release();
            }
        } else {
            if (card_in_field) {
                if (++empty_count >= 3) {
                    card_in_field = false;
                    ESP_LOGI(TAG, "card removed");

                    /* Notify webui */
                    char *gone = strdup("{\"type\":\"no_card\"}");
                    if (gone) {
                        if (xQueueSend(q, &gone, pdMS_TO_TICKS(200)) != pdTRUE) {
                            free(gone);
                        }
                    }
                    board_indicate(IND_ARMED_READ);
                }
            }
        }

        vTaskDelay(pdMS_TO_TICKS(200));
    }
}

void reader_task_start(QueueHandle_t card_json_queue)
{
    xTaskCreatePinnedToCore(reader_task, "reader", 8192,
                            card_json_queue, 5, NULL, 1);
}
