#include "netconfig.h"

#include "esp_wifi.h"
#include "esp_netif.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "nvs.h"
#include "mdns.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"

#include <stdio.h>
#include <string.h>

static const char *TAG = "netconfig";

#define BIT_STA_CONNECTED  (1 << 0)
#define BIT_STA_FAILED     (1 << 1)
#define MAX_RETRY          5

static EventGroupHandle_t s_evt;
static bool s_sta_connected = false;
static bool s_ap_mode       = false;
static int  s_retry_count   = 0;

static void on_wifi_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    if (base != WIFI_EVENT) return;
    switch (id) {
    case WIFI_EVENT_STA_START:
        esp_wifi_connect();
        break;
    case WIFI_EVENT_STA_DISCONNECTED:
        s_sta_connected = false;
        if (s_retry_count < MAX_RETRY) {
            s_retry_count++;
            ESP_LOGI(TAG, "STA disconnect, retry %d/%d", s_retry_count, MAX_RETRY);
            esp_wifi_connect();
        } else {
            ESP_LOGW(TAG, "STA failed after %d retries", MAX_RETRY);
            xEventGroupSetBits(s_evt, BIT_STA_FAILED);
        }
        break;
    default:
        break;
    }
}

static void on_ip_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    if (base != IP_EVENT || id != IP_EVENT_STA_GOT_IP) return;
    ip_event_got_ip_t *e = (ip_event_got_ip_t *)data;
    ESP_LOGI(TAG, "STA got IP: " IPSTR, IP2STR(&e->ip_info.ip));
    mdns_init();
    mdns_hostname_set("pdk-reader");
    mdns_instance_name_set("PDK PN5180 OSDP Reader");
    mdns_service_add(NULL, "_http", "_tcp", 80, NULL, 0);
    s_retry_count = 0;
    s_sta_connected = true;
    xEventGroupSetBits(s_evt, BIT_STA_CONNECTED);
}

static bool load_wifi_creds(char *ssid, size_t ssid_buf,
                            char *pass, size_t pass_buf)
{
    nvs_handle_t h;
    if (nvs_open("wifi", NVS_READONLY, &h) != ESP_OK) return false;
    size_t sl = ssid_buf, pl = pass_buf;
    bool ok = (nvs_get_str(h, "ssid", ssid, &sl) == ESP_OK) &&
              (nvs_get_str(h, "pass", pass, &pl) == ESP_OK);
    nvs_close(h);
    return ok && ssid[0];
}

static esp_err_t start_sta(const char *ssid, const char *pass)
{
    wifi_config_t cfg = {0};
    strncpy((char *)cfg.sta.ssid,     ssid, sizeof(cfg.sta.ssid) - 1);
    strncpy((char *)cfg.sta.password, pass, sizeof(cfg.sta.password) - 1);
    cfg.sta.threshold.authmode = WIFI_AUTH_WPA2_PSK;

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &cfg));
    ESP_ERROR_CHECK(esp_wifi_start());

    EventBits_t bits = xEventGroupWaitBits(s_evt,
        BIT_STA_CONNECTED | BIT_STA_FAILED,
        pdFALSE, pdFALSE, pdMS_TO_TICKS(10000));

    return (bits & BIT_STA_CONNECTED) ? ESP_OK : ESP_FAIL;
}

static esp_err_t start_ap(void)
{
    uint8_t mac[6];
    esp_read_mac(mac, ESP_MAC_WIFI_SOFTAP);

    wifi_config_t cfg = {0};
    snprintf((char *)cfg.ap.ssid, sizeof(cfg.ap.ssid),
             "pdk-reader-%02x%02x%02x", mac[3], mac[4], mac[5]);
    cfg.ap.ssid_len       = strlen((char *)cfg.ap.ssid);
    cfg.ap.channel        = 6;
    cfg.ap.max_connection = 4;
    cfg.ap.authmode       = WIFI_AUTH_OPEN;
    cfg.ap.beacon_interval = 100;

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_AP));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &cfg));
    ESP_ERROR_CHECK(esp_wifi_start());

    mdns_init();
    mdns_hostname_set("pdk-reader");
    mdns_service_add(NULL, "_http", "_tcp", 80, NULL, 0);

    s_ap_mode = true;
    ESP_LOGW(TAG, "SoftAP '%s' (open) — connect and browse to 192.168.4.1",
             (char *)cfg.ap.ssid);
    return ESP_OK;
}

esp_err_t netconfig_init(void)
{
    s_evt = xEventGroupCreate();

    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_sta();
    esp_netif_create_default_wifi_ap();

    wifi_init_config_t init = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&init));
    ESP_ERROR_CHECK(esp_event_handler_register(
        WIFI_EVENT, ESP_EVENT_ANY_ID, on_wifi_event, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(
        IP_EVENT, IP_EVENT_STA_GOT_IP, on_ip_event, NULL));

    char ssid[33] = {0}, pass[65] = {0};
    if (load_wifi_creds(ssid, sizeof(ssid), pass, sizeof(pass))) {
        ESP_LOGI(TAG, "trying STA: ssid='%s'", ssid);
        if (start_sta(ssid, pass) == ESP_OK) return ESP_OK;
        ESP_LOGW(TAG, "STA failed; falling back to SoftAP");
        esp_wifi_stop();
    } else {
        ESP_LOGW(TAG, "no WiFi creds in NVS — starting SoftAP");
    }
    return start_ap();
}

bool netconfig_sta_connected(void) { return s_sta_connected; }
bool netconfig_in_ap_mode(void)    { return s_ap_mode; }

esp_err_t wifi_save_creds(const char *ssid, const char *pass)
{
    nvs_handle_t h;
    esp_err_t e = nvs_open("wifi", NVS_READWRITE, &h);
    if (e != ESP_OK) return e;
    e = nvs_set_str(h, "ssid", ssid);
    if (e == ESP_OK) e = nvs_set_str(h, "pass", pass);
    if (e == ESP_OK) e = nvs_commit(h);
    nvs_close(h);
    return e;
}

void wifi_get_status(char *ssid_out, size_t ssid_buf,
                     int *rssi_out, esp_ip4_addr_t *ip_out)
{
    ssid_out[0] = '\0';
    if (rssi_out) *rssi_out = 0;
    if (ip_out) ip_out->addr = 0;

    wifi_ap_record_t ap;
    if (esp_wifi_sta_get_ap_info(&ap) == ESP_OK) {
        size_t n = strlen((char *)ap.ssid);
        if (n >= ssid_buf) n = ssid_buf - 1;
        memcpy(ssid_out, ap.ssid, n);
        ssid_out[n] = '\0';
        if (rssi_out) *rssi_out = ap.rssi;
    }
    esp_netif_t *nif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    esp_netif_ip_info_t info;
    if (nif && esp_netif_get_ip_info(nif, &info) == ESP_OK && ip_out) {
        *ip_out = info.ip;
    }
}
