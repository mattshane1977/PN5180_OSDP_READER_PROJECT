#pragma once

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include "esp_err.h"

/*
 * DESFire EV3 driver.
 *
 * Implemented:
 *   - GetVersion          (0x60) — card HW/SW info + real UID, no auth needed
 *   - GetApplicationIDs   (0x6A) — list all AIDs on PICC, no auth needed
 *   - SelectApplication   (0x5A) — switch to an AID context
 *   - GetFileIDs          (0x6F) — list file IDs in selected app
 *   - GetFileSettings     (0xF5) — type, comm mode, size, access rights per file
 *   - AuthenticateEV2First(0x71) — AES-128 mutual auth, derives session keys
 *   - ReadData            (0xBD) — plain / CMAC / encrypted
 *   - WriteData           (0x3D) — plain / CMAC / encrypted
 *   - CreateApplication, CreateStdDataFile, ChangeKey — for card provisioning
 *   - FormatPICC          (0xFC) — factory reset (requires PICC master key auth)
 *
 * All functions return ESP_OK on success.  On a DESFire error status other than
 * OPERATION_OK / ADDITIONAL_FRAME, ESP_FAIL is returned; the raw status byte is
 * available via desfire_last_status().
 *
 * GetFileIDs and GetFileSettings may return ESP_FAIL with status 0x9D
 * (PERMISSION_DENIED) if the application requires authentication — handle this
 * gracefully by reporting the AID without file details.
 */

#define DESFIRE_AES_KEY_LEN     16
#define DESFIRE_AID_LEN         3
#define DESFIRE_MAX_APPS        28
#define DESFIRE_MAX_FILES       32

typedef enum {
    DESFIRE_COMM_PLAIN     = 0,
    DESFIRE_COMM_CMAC      = 1,
    DESFIRE_COMM_ENCRYPTED = 3,
} desfire_comm_mode_t;

typedef struct {
    uint8_t  key_no;
    uint8_t  ti[4];
    uint8_t  k_ses_auth_enc[16];
    uint8_t  k_ses_auth_mac[16];
    uint16_t cmd_ctr;
    bool     authenticated;
} desfire_session_t;

/* GetVersion result */
typedef struct {
    uint8_t hw_vendor_id;
    uint8_t hw_type;
    uint8_t hw_subtype;
    uint8_t hw_major_version;
    uint8_t hw_minor_version;
    uint8_t hw_storage_size;    /* encoded: bytes = 2^(n>>1), approx */
    uint8_t hw_protocol;
    uint8_t sw_vendor_id;
    uint8_t sw_type;
    uint8_t sw_subtype;
    uint8_t sw_major_version;
    uint8_t sw_minor_version;
    uint8_t sw_storage_size;
    uint8_t sw_protocol;
    uint8_t uid[7];             /* real UID (even if random-UID mode is on) */
    uint8_t batch_no[5];
    uint8_t production_week;    /* BCD */
    uint8_t production_year;    /* BCD, offset from 2000 */
} desfire_card_version_t;

/* GetFileSettings result */
typedef struct {
    uint8_t            type;            /* 0=Std 1=Backup 2=Value 3=LinRec 4=CycRec */
    desfire_comm_mode_t comm_mode;
    uint16_t           access_rights;
    uint32_t           size;            /* bytes, for std/backup files */
} desfire_file_settings_t;

/* --- init / status --- */
void    desfire_init(void);
uint8_t desfire_last_status(void);

/* --- PICC-level commands (no SelectApplication needed) --- */
esp_err_t desfire_get_version(desfire_card_version_t *out);
esp_err_t desfire_get_application_ids(uint8_t aids[][DESFIRE_AID_LEN],
                                      uint8_t *count, uint8_t max_count);
esp_err_t desfire_format_picc(void);    /* requires PICC master key auth */

/* --- Application selection and file enumeration --- */
esp_err_t desfire_select_application(const uint8_t aid[DESFIRE_AID_LEN]);
esp_err_t desfire_get_file_ids(uint8_t *file_ids, uint8_t *count,
                               uint8_t max_count);
esp_err_t desfire_get_file_settings(uint8_t file_no, desfire_file_settings_t *out);

/* --- Authentication --- */
esp_err_t desfire_auth_ev2_first(uint8_t key_no,
                                 const uint8_t key[DESFIRE_AES_KEY_LEN]);

/* GetCardUID — requires auth; returns real UID even when random-UID is on */
esp_err_t desfire_get_uid(uint8_t uid[7]);

/* --- Data operations --- */
esp_err_t desfire_read_data(uint8_t file_no,
                            uint32_t offset, uint32_t length,
                            desfire_comm_mode_t comm,
                            uint8_t *out, size_t out_buf, size_t *out_len);

esp_err_t desfire_write_data(uint8_t file_no,
                             uint32_t offset, uint32_t length,
                             desfire_comm_mode_t comm,
                             const uint8_t *data);

/* --- Personalization (for Phase 2 / LEAF provisioning) --- */
esp_err_t desfire_create_application(const uint8_t aid[DESFIRE_AID_LEN],
                                     uint8_t key_settings,
                                     uint8_t num_keys_and_flags);

esp_err_t desfire_create_std_data_file(uint8_t file_no,
                                       desfire_comm_mode_t comm,
                                       uint16_t access_rights,
                                       uint32_t file_size);

esp_err_t desfire_change_key(uint8_t key_no,
                             const uint8_t old_key[DESFIRE_AES_KEY_LEN],
                             const uint8_t new_key[DESFIRE_AES_KEY_LEN],
                             uint8_t key_version);
