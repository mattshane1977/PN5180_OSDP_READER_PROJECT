#pragma once

#include "esp_err.h"
#include "esp_netif.h"
#include <stddef.h>
#include <stdbool.h>

/*
 * WiFi bring-up.
 *
 * On boot: loads SSID/password from NVS namespace "wifi".
 *   - If present, attempts STA connection (up to 5 retries, 10s timeout).
 *   - On failure or if no credentials exist, falls back to SoftAP:
 *       SSID: "pdk-reader-XXXXXX"  (last 3 MAC bytes)
 *       Password: none
 *     mDNS hostname: "pdk-reader"  -> http://pdk-reader.local/
 *
 * Returns ESP_OK once any interface is up.
 */

esp_err_t netconfig_init(void);
bool      netconfig_sta_connected(void);
bool      netconfig_in_ap_mode(void);

/* Save STA credentials to NVS (call from webui config handler) */
esp_err_t wifi_save_creds(const char *ssid, const char *pass);

/* Get current STA connection info */
void wifi_get_status(char *ssid_out, size_t ssid_buf,
                     int *rssi_out, esp_ip4_addr_t *ip_out);
