#include "desfire_internal.h"
#include "esp_log.h"
#include "esp_random.h"

#include <string.h>

static const char *TAG = "df_auth";

desfire_session_t g_session;
uint8_t g_session_mac_key_full[16];          /* used by cmac.c */

/* AuthenticateEV2First (cmd 0x71)
 *
 * Step 1: tx [0x71][KeyNo][LenCap=0x00]               -> rx 0xAF + E(RndB)
 * Step 2: tx [0xAF][E(RndA || RndB')]                 -> rx 0x00 + E(TI || RndA' || PDcap || PCDcap)
 *
 * RndB' = RndB <<< 8 (rotate left 1 byte)
 * RndA' = RndA <<< 8
 *
 * Session key derivation (NXP DESFire EV2 datasheet §7.1.2):
 *   sv1 = 0xA55A 00 01 00 80 || RndA[15..14] ||
 *         (RndA[13..8] XOR RndB[15..10]) || RndB[9..0] || RndA[7..0]
 *   sv2 = 0x5AA5 00 01 00 80 || RndA[15..14] ||
 *         (RndA[13..8] XOR RndB[15..10]) || RndB[9..0] || RndA[7..0]
 *   K_ses_auth_enc = AES-CMAC(K_master, sv1)  (full 16-byte CMAC output)
 *   K_ses_auth_mac = AES-CMAC(K_master, sv2)
 *
 * After auth: TI = the 4-byte transaction id from the response, cmd_ctr = 0.
 */

static void rotate_left_1(uint8_t buf[16])
{
    uint8_t t = buf[0];
    memmove(&buf[0], &buf[1], 15);
    buf[15] = t;
}

/* AES-CMAC over an arbitrary key — used during auth before the session key is
 * established. Mirrors desfire_cmac() but takes the key as a parameter. */
static void cmac_with_key(const uint8_t key[16], const uint8_t *msg, size_t len,
                          uint8_t out_full[16])
{
    uint8_t saved[16];
    memcpy(saved, g_session_mac_key_full, 16);
    memcpy(g_session_mac_key_full, key, 16);
    desfire_cmac(msg, len, out_full);
    memcpy(g_session_mac_key_full, saved, 16);
}

static void derive_session_keys(const uint8_t k_master[16],
                                const uint8_t rndA[16], const uint8_t rndB[16],
                                uint8_t k_enc[16], uint8_t k_mac[16])
{
    uint8_t sv[32];
    /* Header sv1 */
    sv[0] = 0xA5; sv[1] = 0x5A; sv[2] = 0x00; sv[3] = 0x01;
    sv[4] = 0x00; sv[5] = 0x80;
    /* RndA[15..14] */
    sv[6] = rndA[0]; sv[7] = rndA[1];
    /* RndA[13..8] XOR RndB[15..10] */
    for (int i = 0; i < 6; i++) sv[8 + i] = rndA[2 + i] ^ rndB[i];
    /* RndB[9..0] */
    for (int i = 0; i < 10; i++) sv[14 + i] = rndB[6 + i];
    /* RndA[7..0] */
    for (int i = 0; i < 8; i++) sv[24 + i] = rndA[8 + i];

    cmac_with_key(k_master, sv, 32, k_enc);

    /* sv2: same body, different header */
    sv[0] = 0x5A; sv[1] = 0xA5;
    cmac_with_key(k_master, sv, 32, k_mac);
}

esp_err_t desfire_auth_ev2_first(uint8_t key_no,
                                 const uint8_t key[DESFIRE_AES_KEY_LEN])
{
    /* --- Step 1 --- */
    uint8_t step1[3] = { key_no, 0x00, 0x00 };  /* LenCap = 0x00, no PCDcap2 */
    uint8_t resp[64]; size_t rl = 0;

    int st = desfire_xfer(DF_CMD_AUTH_EV2_FIRST, step1, sizeof(step1),
                          resp, sizeof(resp), &rl);
    if (st != DF_ADDITIONAL_FRAME || rl != 16) {
        ESP_LOGE(TAG, "auth step1 status=0x%02x rl=%u", st, (unsigned)rl);
        return ESP_FAIL;
    }

    uint8_t E_rndB[16], rndB[16], iv[16] = {0};
    memcpy(E_rndB, resp, 16);

    /* Decrypt RndB with K_master, IV = 0. After decrypt, IV becomes E_rndB. */
    desfire_aes_cbc_decrypt(key, iv, E_rndB, rndB, 16);

    /* Build RndA, RndB' (RndB rotated left by 1) */
    uint8_t rndA[16];
    esp_fill_random(rndA, 16);

    uint8_t rndBp[16];
    memcpy(rndBp, rndB, 16);
    rotate_left_1(rndBp);

    /* Encrypt (RndA || RndB') with K_master, IV continues from E_rndB */
    uint8_t to_enc[32], encrypted[32];
    memcpy(&to_enc[0],  rndA,  16);
    memcpy(&to_enc[16], rndBp, 16);

    uint8_t iv2[16];
    memcpy(iv2, E_rndB, 16);
    desfire_aes_cbc_encrypt(key, iv2, to_enc, encrypted, 32);

    /* --- Step 2 --- */
    st = desfire_xfer(DF_CMD_ADDITIONAL_FRAME, encrypted, 32,
                      resp, sizeof(resp), &rl);
    if (st != DF_OPERATION_OK || rl != 32) {
        ESP_LOGE(TAG, "auth step2 status=0x%02x rl=%u", st, (unsigned)rl);
        return ESP_FAIL;
    }

    /* Decrypt response with K_master, IV continues from last ciphertext block
     * we transmitted (encrypted[16..31]). */
    uint8_t iv3[16];
    memcpy(iv3, &encrypted[16], 16);
    uint8_t plain[32];
    desfire_aes_cbc_decrypt(key, iv3, resp, plain, 32);

    /* plain = TI(4) || RndA'(16) || PDcap2(6) || PCDcap2(6) */
    uint8_t rndAp_expected[16];
    memcpy(rndAp_expected, rndA, 16);
    rotate_left_1(rndAp_expected);

    if (memcmp(&plain[4], rndAp_expected, 16) != 0) {
        ESP_LOGE(TAG, "auth: RndA' mismatch — wrong key");
        return ESP_FAIL;
    }

    /* Capture session state */
    memcpy(g_session.ti, &plain[0], 4);
    g_session.key_no = key_no;
    g_session.cmd_ctr = 0;
    derive_session_keys(key, rndA, rndB,
                        g_session.k_ses_auth_enc, g_session.k_ses_auth_mac);
    memcpy(g_session_mac_key_full, g_session.k_ses_auth_mac, 16);
    g_session.authenticated = true;

    ESP_LOGI(TAG, "auth EV2First OK key=%u TI=%02x%02x%02x%02x",
             key_no, g_session.ti[0], g_session.ti[1], g_session.ti[2], g_session.ti[3]);
    return ESP_OK;
}
