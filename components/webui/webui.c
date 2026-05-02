#include "webui.h"

#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_netif.h"
#include "nvs.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"

#include <string.h>
#include <stdlib.h>
#include <ctype.h>

static const char *TAG = "webui";

extern const uint8_t index_html_start[] asm("_binary_index_html_start");
extern const uint8_t index_html_end[]   asm("_binary_index_html_end");

static httpd_handle_t s_server  = NULL;
static int            s_ws_fd   = -1;
static char          *s_last_json = NULL;   /* most recent card JSON for new connects */

/* ------- tiny JSON key parser (no cJSON dependency) ------- */

static const char *find_key(const char *json, const char *key)
{
    char needle[40];
    int n = snprintf(needle, sizeof(needle), "\"%s\"", key);
    if (n <= 0 || n >= (int)sizeof(needle)) return NULL;
    const char *p = strstr(json, needle);
    if (!p) return NULL;
    p += n;
    while (*p && (*p == ' ' || *p == ':' || *p == '\t')) p++;
    return *p ? p : NULL;
}

static bool get_str(const char *json, const char *key, char *out, size_t out_len)
{
    const char *p = find_key(json, key);
    if (!p || *p != '"') return false;
    p++;
    size_t i = 0;
    while (*p && *p != '"' && i < out_len - 1) out[i++] = *p++;
    out[i] = '\0';
    return *p == '"';
}

static bool get_int(const char *json, const char *key, long *out)
{
    const char *p = find_key(json, key);
    if (!p) return false;
    char *end = NULL;
    long v = strtol(p, &end, 0);
    if (end == p) return false;
    *out = v;
    return true;
}

/* ------- WS send helpers ------- */

static esp_err_t ws_send_text(int fd, const char *payload)
{
    if (!s_server || fd < 0) return ESP_ERR_INVALID_STATE;
    httpd_ws_frame_t f = {
        .final = true,
        .type = HTTPD_WS_TYPE_TEXT,
        .payload = (uint8_t *)payload,
        .len = strlen(payload),
    };
    return httpd_ws_send_frame_async(s_server, fd, &f);
}

static void ws_ack(int fd, const char *cmd, const char *detail)
{
    char buf[160];
    snprintf(buf, sizeof(buf),
             "{\"type\":\"ack\",\"cmd\":\"%s\",\"detail\":\"%s\"}",
             cmd, detail ? detail : "");
    ws_send_text(fd, buf);
}

static void ws_error(int fd, const char *msg)
{
    char buf[160];
    snprintf(buf, sizeof(buf), "{\"type\":\"error\",\"message\":\"%s\"}", msg);
    ws_send_text(fd, buf);
}

static void ws_send_status(int fd)
{
    esp_netif_t *nif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    esp_netif_ip_info_t ip = {0};
    if (nif) esp_netif_get_ip_info(nif, &ip);

    char buf[180];
    snprintf(buf, sizeof(buf),
             "{\"type\":\"status\",\"driver\":\"pn5180\",\"ip\":\"" IPSTR "\"}",
             IP2STR(&ip.ip));
    ws_send_text(fd, buf);
}

/* ------- NVS helpers ------- */

static esp_err_t save_osdp_config(int addr, int baud)
{
    nvs_handle_t h;
    esp_err_t e = nvs_open("osdp", NVS_READWRITE, &h);
    if (e != ESP_OK) return e;
    nvs_set_i32(h, "addr", addr);
    nvs_set_i32(h, "baud", baud);
    nvs_commit(h);
    nvs_close(h);
    return ESP_OK;
}

/* ------- WS command handler ------- */

static void handle_ws_command(int fd, const char *json)
{
    char cmd[20] = {0};
    if (!get_str(json, "cmd", cmd, sizeof(cmd))) {
        ws_error(fd, "missing cmd");
        return;
    }

    if (!strcmp(cmd, "osdp_set")) {
        long addr = 0, baud = 0;
        if (!get_int(json, "address", &addr) || !get_int(json, "baud", &baud)) {
            ws_error(fd, "osdp_set: need address and baud"); return;
        }
        if (addr < 0 || addr > 126) {
            ws_error(fd, "osdp_set: address must be 0-126"); return;
        }
        if (save_osdp_config((int)addr, (int)baud) != ESP_OK) {
            ws_error(fd, "osdp_set: nvs write failed"); return;
        }
        ws_ack(fd, "osdp_set", "saved — reboot to apply");
        return;
    }

    if (!strcmp(cmd, "wifi_set")) {
        char ssid[33] = {0}, pass[65] = {0};
        if (!get_str(json, "ssid", ssid, sizeof(ssid))) {
            ws_error(fd, "wifi_set: missing ssid"); return;
        }
        get_str(json, "pass", pass, sizeof(pass));   /* password optional for open APs */

        nvs_handle_t h;
        if (nvs_open("wifi", NVS_READWRITE, &h) == ESP_OK) {
            nvs_set_str(h, "ssid", ssid);
            nvs_set_str(h, "pass", pass);
            nvs_commit(h);
            nvs_close(h);
        }
        ws_ack(fd, "wifi_set", "saved — reboot to connect");
        return;
    }

    if (!strcmp(cmd, "reboot")) {
        ws_ack(fd, "reboot", "in 200ms");
        vTaskDelay(pdMS_TO_TICKS(200));
        esp_restart();
        return;
    }

    ws_error(fd, "unknown cmd");
}

/* ------- HTTP handlers ------- */

static esp_err_t handle_index(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    return httpd_resp_send(req, (const char *)index_html_start,
                           index_html_end - index_html_start);
}

static esp_err_t handle_ws(httpd_req_t *req)
{
    if (req->method == HTTP_GET) {
        s_ws_fd = httpd_req_to_sockfd(req);
        ESP_LOGI(TAG, "WS client connected fd=%d", s_ws_fd);
        return ESP_OK;
    }

    httpd_ws_frame_t f = {0};
    esp_err_t e = httpd_ws_recv_frame(req, &f, 0);
    if (e != ESP_OK || f.len == 0 || f.len > 1024) return ESP_OK;

    uint8_t *buf = malloc(f.len + 1);
    if (!buf) return ESP_ERR_NO_MEM;
    f.payload = buf;
    e = httpd_ws_recv_frame(req, &f, f.len);
    if (e == ESP_OK && f.type == HTTPD_WS_TYPE_TEXT) {
        buf[f.len] = '\0';
        s_ws_fd = httpd_req_to_sockfd(req);
        handle_ws_command(s_ws_fd, (char *)buf);
    }
    free(buf);
    return ESP_OK;
}

/* ------- event push task ------- */

static void event_push_task(void *arg)
{
    QueueHandle_t q = (QueueHandle_t)arg;

    /* Send initial status a moment after boot */
    vTaskDelay(pdMS_TO_TICKS(1200));
    if (s_ws_fd >= 0) {
        ws_send_status(s_ws_fd);
        if (s_last_json) ws_send_text(s_ws_fd, s_last_json);
    }

    for (;;) {
        char *json = NULL;
        if (xQueueReceive(q, &json, portMAX_DELAY) != pdTRUE || !json) continue;

        /* Cache for new WS connections */
        free(s_last_json);
        s_last_json = strdup(json);

        /* Push to connected client */
        if (s_ws_fd >= 0) {
            if (ws_send_text(s_ws_fd, json) != ESP_OK) {
                ESP_LOGI(TAG, "WS send failed; dropping fd %d", s_ws_fd);
                s_ws_fd = -1;
            }
        }
        free(json);
    }
}

/* ------- start ------- */

void webui_start(QueueHandle_t card_json_queue)
{
    httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
    cfg.lru_purge_enable = true;
    cfg.max_open_sockets = 5;

    if (httpd_start(&s_server, &cfg) != ESP_OK) {
        ESP_LOGE(TAG, "httpd_start failed");
        return;
    }

    httpd_uri_t index_uri = {
        .uri = "/", .method = HTTP_GET,
        .handler = handle_index,
    };
    httpd_register_uri_handler(s_server, &index_uri);

    httpd_uri_t ws_uri = {
        .uri = "/ws", .method = HTTP_GET,
        .handler = handle_ws,
        .is_websocket = true,
    };
    httpd_register_uri_handler(s_server, &ws_uri);

    xTaskCreatePinnedToCore(event_push_task, "wspush", 4096,
                            card_json_queue, 4, NULL, 0);

    ESP_LOGI(TAG, "web UI started — http://pdk-reader.local/ or http://192.168.4.1/");
}
