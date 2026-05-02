#include "desfire.h"
#include "desfire_internal.h"
#include "nfc_hal.h"
#include "esp_log.h"

#include <string.h>

static const char *TAG = "desfire";
static uint8_t s_last_status = DF_OPERATION_OK;

void desfire_init(void) { memset(&g_session, 0, sizeof(g_session)); }
uint8_t desfire_last_status(void) { return s_last_status; }

/* Native DESFire APDU wrapper:
 *   CLA=0x90, INS=cmd, P1=0x00, P2=0x00, Lc=len(params), params, Le=0x00
 * Response: data || 0x91 || DESFire-status
 */
int desfire_xfer(uint8_t cmd, const uint8_t *params, size_t params_len,
                 uint8_t *resp_data, size_t resp_buf, size_t *resp_len)
{
    uint8_t apdu[5 + 256 + 1];
    if (params_len > 255) return -1;

    size_t i = 0;
    apdu[i++] = 0x90;
    apdu[i++] = cmd;
    apdu[i++] = 0x00;
    apdu[i++] = 0x00;
    apdu[i++] = (uint8_t)params_len;
    if (params_len) { memcpy(&apdu[i], params, params_len); i += params_len; }
    apdu[i++] = 0x00;   /* Le */

    uint8_t rx[280];
    size_t rxl = 0;
    esp_err_t e = nfc_apdu_exchange(apdu, i, rx, sizeof(rx), &rxl);
    if (e != ESP_OK || rxl < 2) {
        s_last_status = 0xFF;
        return -1;
    }
    if (rx[rxl - 2] != 0x91) {
        ESP_LOGW(TAG, "unexpected SW1=0x%02x", rx[rxl - 2]);
        s_last_status = 0xFF;
        return -1;
    }
    s_last_status = rx[rxl - 1];

    size_t data_len = rxl - 2;
    if (data_len > resp_buf) {
        ESP_LOGW(TAG, "response %u bytes exceeds buffer %u", (unsigned)data_len, (unsigned)resp_buf);
        return -1;
    }
    if (data_len && resp_data) memcpy(resp_data, rx, data_len);
    if (resp_len) *resp_len = data_len;
    return s_last_status;
}

/* ------- CMAC-protected post-auth command wrapper ------- */

static esp_err_t cmac_append_and_send(uint8_t cmd, const uint8_t *header, size_t hlen,
                                      const uint8_t *data, size_t dlen,
                                      uint8_t *resp, size_t resp_buf, size_t *resp_len)
{
    uint8_t macbuf[1 + 2 + 4 + 256];
    if (hlen + dlen > 256) return ESP_ERR_INVALID_SIZE;

    size_t mi = 0;
    macbuf[mi++] = cmd;
    macbuf[mi++] = (uint8_t)(g_session.cmd_ctr & 0xFF);
    macbuf[mi++] = (uint8_t)(g_session.cmd_ctr >> 8);
    memcpy(&macbuf[mi], g_session.ti, 4); mi += 4;
    if (hlen) { memcpy(&macbuf[mi], header, hlen); mi += hlen; }
    if (dlen) { memcpy(&macbuf[mi], data,   dlen); mi += dlen; }

    uint8_t cmac_full[16];
    desfire_cmac(macbuf, mi, cmac_full);

    uint8_t mac8[8];
    for (int i = 0; i < 8; i++) mac8[i] = cmac_full[1 + 2 * i];

    uint8_t params[256];
    size_t pl = 0;
    if (hlen) { memcpy(&params[pl], header, hlen); pl += hlen; }
    if (dlen) { memcpy(&params[pl], data,   dlen); pl += dlen; }
    memcpy(&params[pl], mac8, 8); pl += 8;

    int st = desfire_xfer(cmd, params, pl, resp, resp_buf, resp_len);
    if (st < 0) return ESP_FAIL;
    if (st != DF_OPERATION_OK && st != DF_ADDITIONAL_FRAME) return ESP_FAIL;

    g_session.cmd_ctr++;

    if (resp_len && *resp_len >= 8) {
        size_t rd = *resp_len - 8;
        uint8_t verify[256];
        size_t vi = 0;
        verify[vi++] = (uint8_t)st;
        verify[vi++] = (uint8_t)(g_session.cmd_ctr & 0xFF);
        verify[vi++] = (uint8_t)(g_session.cmd_ctr >> 8);
        memcpy(&verify[vi], g_session.ti, 4); vi += 4;
        if (rd) { memcpy(&verify[vi], resp, rd); vi += rd; }

        uint8_t want_full[16], want8[8];
        desfire_cmac(verify, vi, want_full);
        for (int i = 0; i < 8; i++) want8[i] = want_full[1 + 2 * i];

        if (memcmp(&resp[rd], want8, 8) != 0) {
            ESP_LOGE(TAG, "response CMAC mismatch");
            return ESP_ERR_INVALID_CRC;
        }
        *resp_len = rd;
    }

    return ESP_OK;
}

/* ------- GetVersion (0x60) -------
 *
 * Three-frame exchange: HW info | SW info | UID + production info.
 * No authentication required — works on any DESFire card at PICC level.
 */
esp_err_t desfire_get_version(desfire_card_version_t *out)
{
    uint8_t resp[16]; size_t rl = 0;

    /* Frame 1: hardware information */
    int st = desfire_xfer(DF_CMD_GET_VERSION, NULL, 0, resp, sizeof(resp), &rl);
    if (st != DF_ADDITIONAL_FRAME || rl != 7) {
        ESP_LOGW(TAG, "GetVersion HW frame: st=0x%02x rl=%u", st, (unsigned)rl);
        return ESP_FAIL;
    }
    out->hw_vendor_id      = resp[0];
    out->hw_type           = resp[1];
    out->hw_subtype        = resp[2];
    out->hw_major_version  = resp[3];
    out->hw_minor_version  = resp[4];
    out->hw_storage_size   = resp[5];
    out->hw_protocol       = resp[6];

    /* Frame 2: software information */
    st = desfire_xfer(DF_CMD_ADDITIONAL_FRAME, NULL, 0, resp, sizeof(resp), &rl);
    if (st != DF_ADDITIONAL_FRAME || rl != 7) {
        ESP_LOGW(TAG, "GetVersion SW frame: st=0x%02x rl=%u", st, (unsigned)rl);
        return ESP_FAIL;
    }
    out->sw_vendor_id      = resp[0];
    out->sw_type           = resp[1];
    out->sw_subtype        = resp[2];
    out->sw_major_version  = resp[3];
    out->sw_minor_version  = resp[4];
    out->sw_storage_size   = resp[5];
    out->sw_protocol       = resp[6];

    /* Frame 3: UID (7 bytes) + batch number (5) + week (1) + year (1) */
    st = desfire_xfer(DF_CMD_ADDITIONAL_FRAME, NULL, 0, resp, sizeof(resp), &rl);
    if (st != DF_OPERATION_OK || rl != 14) {
        ESP_LOGW(TAG, "GetVersion UID frame: st=0x%02x rl=%u", st, (unsigned)rl);
        return ESP_FAIL;
    }
    memcpy(out->uid,      &resp[0], 7);
    memcpy(out->batch_no, &resp[7], 5);
    out->production_week = resp[12];
    out->production_year = resp[13];

    return ESP_OK;
}

/* ------- GetApplicationIDs (0x6A) -------
 *
 * Returns up to 28 AIDs. Handles the AF chaining case where the card
 * returns more AIDs than fit in one response frame.
 */
esp_err_t desfire_get_application_ids(uint8_t aids[][DESFIRE_AID_LEN],
                                      uint8_t *count, uint8_t max_count)
{
    uint8_t resp[28 * 3]; size_t rl = 0;
    *count = 0;

    int st = desfire_xfer(DF_CMD_GET_APPLICATION_IDS, NULL, 0,
                          resp, sizeof(resp), &rl);
    for (;;) {
        for (size_t i = 0; i + 3 <= rl && *count < max_count; i += 3) {
            memcpy(aids[*count], &resp[i], 3);
            (*count)++;
        }
        if (st == DF_OPERATION_OK) break;
        if (st != DF_ADDITIONAL_FRAME) return ESP_FAIL;

        st = desfire_xfer(DF_CMD_ADDITIONAL_FRAME, NULL, 0,
                          resp, sizeof(resp), &rl);
    }
    return ESP_OK;
}

/* ------- SelectApplication (0x5A) ------- */

esp_err_t desfire_select_application(const uint8_t aid[DESFIRE_AID_LEN])
{
    int st = desfire_xfer(DF_CMD_SELECT_APP, aid, 3, NULL, 0, NULL);
    if (st < 0 || st != DF_OPERATION_OK) return ESP_FAIL;
    g_session.authenticated = false;
    g_session.cmd_ctr = 0;
    return ESP_OK;
}

/* ------- GetFileIDs (0x6F) -------
 *
 * May return PERMISSION_DENIED (0x9D) if the app requires auth to list files.
 * In that case, callers should log the AID without file details.
 */
esp_err_t desfire_get_file_ids(uint8_t *file_ids, uint8_t *count,
                               uint8_t max_count)
{
    uint8_t resp[DESFIRE_MAX_FILES]; size_t rl = 0;
    *count = 0;

    int st = desfire_xfer(DF_CMD_GET_FILE_IDS, NULL, 0,
                          resp, sizeof(resp), &rl);
    if (st != DF_OPERATION_OK) return ESP_FAIL;

    for (size_t i = 0; i < rl && *count < max_count; i++) {
        file_ids[(*count)++] = resp[i];
    }
    return ESP_OK;
}

/* ------- GetFileSettings (0xF5) ------- */

esp_err_t desfire_get_file_settings(uint8_t file_no,
                                    desfire_file_settings_t *out)
{
    memset(out, 0, sizeof(*out));

    uint8_t resp[16]; size_t rl = 0;
    int st = desfire_xfer(DF_CMD_GET_FILE_SETTINGS, &file_no, 1,
                          resp, sizeof(resp), &rl);
    if (st != DF_OPERATION_OK || rl < 4) return ESP_FAIL;

    out->type         = resp[0];
    out->comm_mode    = (desfire_comm_mode_t)(resp[1] & 0x03);
    out->access_rights = (uint16_t)resp[2] | ((uint16_t)resp[3] << 8);

    /* File size is only meaningful for Std (0) and Backup (1) files */
    if ((out->type == 0 || out->type == 1) && rl >= 7) {
        out->size = (uint32_t)resp[4]
                  | ((uint32_t)resp[5] << 8)
                  | ((uint32_t)resp[6] << 16);
    }
    return ESP_OK;
}

/* ------- GetCardUID (0x51) — requires prior auth ------- */

esp_err_t desfire_get_uid(uint8_t uid[7])
{
    if (!g_session.authenticated) return ESP_ERR_INVALID_STATE;
    uint8_t buf[7]; size_t rl = 0;
    int st = desfire_enc_recv(DF_CMD_GET_UID, NULL, 0, 7, buf, sizeof(buf), &rl);
    if (st != DF_OPERATION_OK || rl != 7) return ESP_FAIL;
    memcpy(uid, buf, 7);
    return ESP_OK;
}

/* ------- FormatPICC (0xFC) — requires PICC master key auth ------- */

esp_err_t desfire_format_picc(void)
{
    if (!g_session.authenticated) return ESP_ERR_INVALID_STATE;
    return cmac_append_and_send(DF_CMD_FORMAT_PICC, NULL, 0, NULL, 0,
                                NULL, 0, NULL);
}

/* ------- CreateApplication (0xCA) ------- */

esp_err_t desfire_create_application(const uint8_t aid[DESFIRE_AID_LEN],
                                     uint8_t key_settings,
                                     uint8_t num_keys_and_flags)
{
    uint8_t hdr[5];
    memcpy(&hdr[0], aid, 3);
    hdr[3] = key_settings;
    hdr[4] = num_keys_and_flags;

    if (g_session.authenticated) {
        return cmac_append_and_send(DF_CMD_CREATE_APP, hdr, sizeof(hdr),
                                    NULL, 0, NULL, 0, NULL);
    }
    int st = desfire_xfer(DF_CMD_CREATE_APP, hdr, sizeof(hdr), NULL, 0, NULL);
    return (st == DF_OPERATION_OK) ? ESP_OK : ESP_FAIL;
}

/* ------- CreateStdDataFile (0xCD) ------- */

esp_err_t desfire_create_std_data_file(uint8_t file_no,
                                       desfire_comm_mode_t comm,
                                       uint16_t access_rights,
                                       uint32_t file_size)
{
    if (!g_session.authenticated) return ESP_ERR_INVALID_STATE;

    uint8_t hdr[7];
    hdr[0] = file_no;
    hdr[1] = (uint8_t)comm;
    hdr[2] = (uint8_t)(access_rights & 0xFF);
    hdr[3] = (uint8_t)(access_rights >> 8);
    hdr[4] = (uint8_t)(file_size & 0xFF);
    hdr[5] = (uint8_t)((file_size >> 8) & 0xFF);
    hdr[6] = (uint8_t)((file_size >> 16) & 0xFF);

    return cmac_append_and_send(DF_CMD_CREATE_STD_FILE, hdr, sizeof(hdr),
                                NULL, 0, NULL, 0, NULL);
}

/* ------- ReadData (0xBD) ------- */

esp_err_t desfire_read_data(uint8_t file_no,
                            uint32_t offset, uint32_t length,
                            desfire_comm_mode_t comm,
                            uint8_t *out, size_t out_buf, size_t *out_len)
{
    uint8_t hdr[7];
    hdr[0] = file_no;
    hdr[1] = (uint8_t)(offset & 0xFF);
    hdr[2] = (uint8_t)((offset >> 8) & 0xFF);
    hdr[3] = (uint8_t)((offset >> 16) & 0xFF);
    hdr[4] = (uint8_t)(length & 0xFF);
    hdr[5] = (uint8_t)((length >> 8) & 0xFF);
    hdr[6] = (uint8_t)((length >> 16) & 0xFF);

    if (comm == DESFIRE_COMM_PLAIN) {
        int st = desfire_xfer(DF_CMD_READ_DATA, hdr, sizeof(hdr),
                              out, out_buf, out_len);
        return (st == DF_OPERATION_OK) ? ESP_OK : ESP_FAIL;
    }
    if (comm == DESFIRE_COMM_CMAC) {
        if (!g_session.authenticated) return ESP_ERR_INVALID_STATE;
        return cmac_append_and_send(DF_CMD_READ_DATA, hdr, sizeof(hdr),
                                    NULL, 0, out, out_buf, out_len);
    }
    if (comm == DESFIRE_COMM_ENCRYPTED) {
        if (!g_session.authenticated) return ESP_ERR_INVALID_STATE;
        if (length == 0 || length > out_buf) return ESP_ERR_INVALID_SIZE;
        int st = desfire_enc_recv(DF_CMD_READ_DATA, hdr, sizeof(hdr),
                                  length, out, out_buf, out_len);
        return (st == DF_OPERATION_OK) ? ESP_OK : ESP_FAIL;
    }
    return ESP_FAIL;
}

/* ------- WriteData (0x3D) ------- */

esp_err_t desfire_write_data(uint8_t file_no,
                             uint32_t offset, uint32_t length,
                             desfire_comm_mode_t comm,
                             const uint8_t *data)
{
    uint8_t hdr[7];
    hdr[0] = file_no;
    hdr[1] = (uint8_t)(offset & 0xFF);
    hdr[2] = (uint8_t)((offset >> 8) & 0xFF);
    hdr[3] = (uint8_t)((offset >> 16) & 0xFF);
    hdr[4] = (uint8_t)(length & 0xFF);
    hdr[5] = (uint8_t)((length >> 8) & 0xFF);
    hdr[6] = (uint8_t)((length >> 16) & 0xFF);

    if (comm == DESFIRE_COMM_PLAIN) {
        uint8_t buf[256];
        if (sizeof(hdr) + length > sizeof(buf)) return ESP_ERR_INVALID_SIZE;
        memcpy(&buf[0], hdr, sizeof(hdr));
        memcpy(&buf[sizeof(hdr)], data, length);
        int st = desfire_xfer(DF_CMD_WRITE_DATA, buf, sizeof(hdr) + length,
                              NULL, 0, NULL);
        return (st == DF_OPERATION_OK) ? ESP_OK : ESP_FAIL;
    }
    if (comm == DESFIRE_COMM_CMAC) {
        if (!g_session.authenticated) return ESP_ERR_INVALID_STATE;
        return cmac_append_and_send(DF_CMD_WRITE_DATA, hdr, sizeof(hdr),
                                    data, length, NULL, 0, NULL);
    }
    if (comm == DESFIRE_COMM_ENCRYPTED) {
        if (!g_session.authenticated) return ESP_ERR_INVALID_STATE;
        int st = desfire_enc_send(DF_CMD_WRITE_DATA, hdr, sizeof(hdr),
                                  data, length, NULL, 0, NULL);
        return (st == DF_OPERATION_OK) ? ESP_OK : ESP_FAIL;
    }
    return ESP_FAIL;
}
