#pragma once

#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"

/*
 * Reader task — scans for DESFire EV3 cards on the PN5180.
 *
 * On each card presentation:
 *   1. GetVersion    — HW/SW info, real UID
 *   2. GetApplicationIDs — all AIDs on the PICC
 *   3. Per AID: SelectApplication → GetFileIDs → GetFileSettings per file
 *      (silently skips file enumeration if access denied)
 *   4. Allocates a JSON string and posts it to card_json_queue.
 *
 * When the card leaves: posts a {"type":"no_card"} JSON string.
 *
 * The caller (main.c) creates the queue and passes it here AND to webui_start().
 * The queue carries malloc'd char* — the consumer (webui) must free each pointer.
 */
void reader_task_start(QueueHandle_t card_json_queue);
