#pragma once

#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"

/*
 * Debug web UI.
 *
 * Endpoints:
 *   GET  /     — serves the embedded single-page dashboard
 *   GET  /ws   — WebSocket, bidirectional JSON
 *
 * WebSocket protocol (device -> browser):
 *   {"type":"card","uid":"<hex>","hw":{...},"sw":{...},"uid_real":"<hex>","apps":[...]}
 *   {"type":"no_card"}
 *   {"type":"status","driver":"pn5180","ip":"<ip>"}
 *   {"type":"ack","cmd":"<cmd>","detail":"<msg>"}
 *   {"type":"error","message":"<msg>"}
 *
 * WebSocket protocol (browser -> device):
 *   {"cmd":"osdp_set","address":<0-126>,"baud":<baud>}  — save OSDP config (Phase 2)
 *   {"cmd":"wifi_set","ssid":"<s>","pass":"<p>"}         — save WiFi creds
 *   {"cmd":"reboot"}
 *
 * card_json_queue: carries malloc'd char* JSON strings from the reader task.
 * webui takes ownership and frees each pointer after sending.
 * Pass NULL if not yet connected (webui will wait on the queue safely).
 */
void webui_start(QueueHandle_t card_json_queue);
