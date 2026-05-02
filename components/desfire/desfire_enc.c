#include "desfire.h"
#include "desfire_internal.h"
#include "esp_log.h"
#include <string.h>

static const char *TAG = "df_enc";

/* ------- IV derivation -------
 *
 * EV2 session-mode IV (NXP DESFire EV2 datasheet §7.3 / AN12343):
 *
 *   IV_input(host->card)  = 0xA5 0x5A || TI || cmd_ctr_LE(2) || 0x00 × 8
 *   IV_input(card->host)  = 0x5A 0xA5 || TI || cmd_ctr_LE(2) || 0x00 × 8
 *   IV                    = AES-ECB-Encrypt(K_ses_auth_enc, IV_input)
 *
 * The cmd_ctr used is the *current* value at the time the IV is derived —
 * it gets incremented after the round-trip completes. Both directions of a
 * single command-response use the same counter value with different magic.
 */

void desfire_enc_iv(uint8_t magic_hi, uint8_t magic_lo, uint8_t iv_out[16])
{
    uint8_t in[16] = {0};
    in[0] = magic_hi;
    in[1] = magic_lo;
    memcpy(&in[2], g_session.ti, 4);
    in[6] = (uint8_t)(g_session.cmd_ctr & 0xFF);
    in[7] = (uint8_t)(g_session.cmd_ctr >> 8);
    /* in[8..15] already zero */

    /* AES-ECB one block. CBC with zero-IV over a single block == ECB. */
    uint8_t zero_iv[16] = {0};
    desfire_aes_cbc_encrypt(g_session.k_ses_auth_enc, zero_iv, in, iv_out, 16);
}

/* ------- shared MAC helpers ------- */

static void mac_truncate(const uint8_t full[16], uint8_t out8[8])
{
    /* On-wire MAC is the 8 even-indexed bytes (1,3,5,...,15). */
    for (int i = 0; i < 8; i++) out8[i] = full[1 + 2 * i];
}

static void cmac_session(const uint8_t *msg, size_t len, uint8_t mac8[8])
{
    uint8_t full[16];
    desfire_cmac(msg, len, full);
    mac_truncate(full, mac8);
}

/* Pad to a 16-byte boundary using DESFire's "0x80 then zeroes" rule.
 * Special case: if input is already a multiple of 16 AND non-empty, append a
 * full block (0x80 followed by 15 zero bytes). For our usage we always have
 * at least the CRC32 inside, so we never hit the empty-input edge. */
static size_t pad_to_block(uint8_t *buf, size_t len, size_t buf_size)
{
    size_t rem = len % 16;
    size_t pad = (rem == 0) ? 0 : (16 - rem);
    /* DESFire EV2: only pad to fill the block; if already aligned, no extra
     * block is added (the CRC has already taken care of integrity). */
    if (pad == 0) return len;
    if (len + pad > buf_size) return 0;
    buf[len] = 0x80;
    memset(&buf[len + 1], 0, pad - 1);
    return len + pad;
}

/* ------- Shape A: encrypted send -------
 *
 * Layout sent on the wire:
 *   cmd | header | E_K(data || crc32_LE(cmd | header | data) || 0x80 pad...) | mac8
 *
 * The CRC32 input includes the cmd byte itself. The encrypted block sequence
 * length must be a multiple of 16 — we pad the (data || crc) tail.
 */
int desfire_enc_send(uint8_t cmd,
                     const uint8_t *header, size_t hlen,
                     const uint8_t *data,   size_t dlen,
                     uint8_t *resp, size_t resp_buf, size_t *resp_len)
{
    if (!g_session.authenticated) return -1;

    /* 1. Build CRC input: cmd | header | data */
    uint8_t crc_buf[1 + 256];
    if (1 + hlen + dlen > sizeof(crc_buf)) return -1;
    size_t ci = 0;
    crc_buf[ci++] = cmd;
    if (hlen) { memcpy(&crc_buf[ci], header, hlen); ci += hlen; }
    if (dlen) { memcpy(&crc_buf[ci], data,   dlen); ci += dlen; }
    uint32_t crc = desfire_crc32(crc_buf, ci);

    /* 2. Build plaintext: data || crc32_LE || pad */
    uint8_t plain[256];
    if (dlen + 4 > sizeof(plain)) return -1;
    size_t pi = 0;
    if (dlen) { memcpy(&plain[pi], data, dlen); pi += dlen; }
    plain[pi++] = (uint8_t)(crc & 0xFF);
    plain[pi++] = (uint8_t)((crc >> 8) & 0xFF);
    plain[pi++] = (uint8_t)((crc >> 16) & 0xFF);
    plain[pi++] = (uint8_t)((crc >> 24) & 0xFF);
    pi = pad_to_block(plain, pi, sizeof(plain));
    if (pi == 0) return -1;

    /* 3. Encrypt with K_ses_enc, IV derived from TI+ctr (host->card magic). */
    uint8_t iv[16];
    desfire_enc_iv(0xA5, 0x5A, iv);

    uint8_t cipher[256];
    desfire_aes_cbc_encrypt(g_session.k_ses_auth_enc, iv, plain, cipher, pi);

    /* 4. Build MAC input: cmd | ctr_LE | TI | header | ciphertext */
    uint8_t macbuf[1 + 2 + 4 + 256];
    if (7 + hlen + pi > sizeof(macbuf)) return -1;
    size_t mi = 0;
    macbuf[mi++] = cmd;
    macbuf[mi++] = (uint8_t)(g_session.cmd_ctr & 0xFF);
    macbuf[mi++] = (uint8_t)(g_session.cmd_ctr >> 8);
    memcpy(&macbuf[mi], g_session.ti, 4); mi += 4;
    if (hlen) { memcpy(&macbuf[mi], header, hlen); mi += hlen; }
    memcpy(&macbuf[mi], cipher, pi); mi += pi;

    uint8_t mac8[8];
    cmac_session(macbuf, mi, mac8);

    /* 5. Wire params = header | ciphertext | mac8 */
    uint8_t params[256];
    if (hlen + pi + 8 > sizeof(params)) return -1;
    size_t wi = 0;
    if (hlen) { memcpy(&params[wi], header, hlen); wi += hlen; }
    memcpy(&params[wi], cipher, pi); wi += pi;
    memcpy(&params[wi], mac8, 8); wi += 8;

    /* 6. Send and check response. Most encrypted-send commands return only a
     * status byte + a response MAC (no data). We verify and strip. */
    uint8_t rx[64]; size_t rl = 0;
    int st = desfire_xfer(cmd, params, wi, rx, sizeof(rx), &rl);
    if (st < 0) return -1;
    if (st != DF_OPERATION_OK) {
        ESP_LOGW(TAG, "enc_send: card status 0x%02x", st);
        /* still bump counter? — no: NXP says counter only increments on OK */
        return st;
    }

    g_session.cmd_ctr++;

    /* Verify response MAC if present */
    if (rl >= 8) {
        size_t rd = rl - 8;
        uint8_t verify[1 + 2 + 4 + 256];
        if (7 + rd > sizeof(verify)) return -1;
        size_t vi = 0;
        verify[vi++] = (uint8_t)st;
        verify[vi++] = (uint8_t)(g_session.cmd_ctr & 0xFF);
        verify[vi++] = (uint8_t)(g_session.cmd_ctr >> 8);
        memcpy(&verify[vi], g_session.ti, 4); vi += 4;
        if (rd) { memcpy(&verify[vi], rx, rd); vi += rd; }

        uint8_t want8[8];
        cmac_session(verify, vi, want8);
        if (memcmp(&rx[rd], want8, 8) != 0) {
            ESP_LOGE(TAG, "enc_send: response MAC mismatch");
            return -1;
        }
        if (resp && rd) memcpy(resp, rx, rd);
        if (resp_len) *resp_len = rd;
    } else {
        if (resp_len) *resp_len = 0;
    }
    return DF_OPERATION_OK;
}

/* ------- Shape B: send CMAC'd, receive encrypted -------
 *
 * Wire:
 *   tx: cmd | header | mac8(over cmd|ctr|TI|header)
 *   rx: E_K(plaintext || crc32_LE(status|plaintext) || pad) | mac8(over status|ctr|TI|ciphertext)
 *
 * Note the response CRC is over [status_byte_OK(0x00) || plaintext], NOT
 * over the ciphertext. This is the EV2 convention for response integrity.
 */
int desfire_enc_recv(uint8_t cmd,
                     const uint8_t *header, size_t hlen,
                     size_t payload_len,
                     uint8_t *resp, size_t resp_buf, size_t *resp_len)
{
    if (!g_session.authenticated) return -1;
    if (payload_len > resp_buf) return -1;

    /* 1. Build MAC input over the request: cmd | ctr | TI | header */
    uint8_t macbuf[1 + 2 + 4 + 64];
    if (7 + hlen > sizeof(macbuf)) return -1;
    size_t mi = 0;
    macbuf[mi++] = cmd;
    macbuf[mi++] = (uint8_t)(g_session.cmd_ctr & 0xFF);
    macbuf[mi++] = (uint8_t)(g_session.cmd_ctr >> 8);
    memcpy(&macbuf[mi], g_session.ti, 4); mi += 4;
    if (hlen) { memcpy(&macbuf[mi], header, hlen); mi += hlen; }

    uint8_t mac8[8];
    cmac_session(macbuf, mi, mac8);

    /* 2. Send: header | mac8 */
    uint8_t params[64 + 8];
    if (hlen + 8 > sizeof(params)) return -1;
    size_t wi = 0;
    if (hlen) { memcpy(&params[wi], header, hlen); wi += hlen; }
    memcpy(&params[wi], mac8, 8); wi += 8;

    uint8_t rx[280]; size_t rl = 0;
    int st = desfire_xfer(cmd, params, wi, rx, sizeof(rx), &rl);
    if (st < 0) return -1;
    if (st != DF_OPERATION_OK) {
        ESP_LOGW(TAG, "enc_recv: card status 0x%02x", st);
        return st;
    }
    if (rl < 8) return -1;

    /* 3. Verify response MAC over [status | ctr_after | TI | ciphertext].
     * Counter is bumped *before* MAC verification — DESFire treats the
     * cmd_ctr as having advanced for response-side computations. */
    g_session.cmd_ctr++;

    size_t cipher_len = rl - 8;
    uint8_t verify[1 + 2 + 4 + 280];
    if (7 + cipher_len > sizeof(verify)) return -1;
    size_t vi = 0;
    verify[vi++] = (uint8_t)st;
    verify[vi++] = (uint8_t)(g_session.cmd_ctr & 0xFF);
    verify[vi++] = (uint8_t)(g_session.cmd_ctr >> 8);
    memcpy(&verify[vi], g_session.ti, 4); vi += 4;
    memcpy(&verify[vi], rx, cipher_len); vi += cipher_len;

    uint8_t want8[8];
    cmac_session(verify, vi, want8);
    if (memcmp(&rx[cipher_len], want8, 8) != 0) {
        ESP_LOGE(TAG, "enc_recv: response MAC mismatch");
        return -1;
    }

    /* 4. Decrypt with K_ses_enc, IV from TI+ctr (card->host magic).
     * The counter was already bumped above, so the IV reflects the post-
     * increment value — this matches the NXP spec's "response counter" usage.
     * Some open implementations derive the response IV pre-increment; both
     * conventions work as long as host and card agree, but the spec wording
     * favours post-increment for the response. */
    uint8_t iv[16];
    desfire_enc_iv(0x5A, 0xA5, iv);

    if (cipher_len % 16 != 0) {
        ESP_LOGE(TAG, "enc_recv: cipher len %u not block-aligned", (unsigned)cipher_len);
        return -1;
    }
    uint8_t plain[280];
    if (cipher_len > sizeof(plain)) return -1;
    desfire_aes_cbc_decrypt(g_session.k_ses_auth_enc, iv, rx, plain, cipher_len);

    /* 5. Verify CRC32 over [0x00 status_OK | plaintext_payload] */
    if (cipher_len < payload_len + 4) {
        ESP_LOGE(TAG, "enc_recv: cipher_len %u too small for payload+crc",
                 (unsigned)cipher_len);
        return -1;
    }
    uint8_t crc_in[1 + 256];
    if (1 + payload_len > sizeof(crc_in)) return -1;
    crc_in[0] = 0x00;
    memcpy(&crc_in[1], plain, payload_len);
    uint32_t crc_calc = desfire_crc32(crc_in, 1 + payload_len);

    uint32_t crc_recv =
        (uint32_t)plain[payload_len]            |
        ((uint32_t)plain[payload_len + 1] << 8) |
        ((uint32_t)plain[payload_len + 2] << 16)|
        ((uint32_t)plain[payload_len + 3] << 24);

    if (crc_calc != crc_recv) {
        ESP_LOGE(TAG, "enc_recv: CRC mismatch (calc=0x%08x recv=0x%08x)",
                 (unsigned)crc_calc, (unsigned)crc_recv);
        return -1;
    }

    if (resp && payload_len) memcpy(resp, plain, payload_len);
    if (resp_len) *resp_len = payload_len;
    return DF_OPERATION_OK;
}

/* ------- ChangeKey -------
 *
 * Two cases, distinguished by whether the target key matches the
 * currently-authenticated key.
 *
 * Wire (always): cmd=0xC4, params = [key_no | E_K(plain) | mac8]
 *
 * Same-key plaintext (24 bytes, padded to 32):
 *     new_key(16) | key_ver(1) | crc32_LE(cmd | key_no | new_key | key_ver) | pad
 *
 * Different-key plaintext (28 bytes, padded to 32):
 *     (new_key XOR old_key)(16) | key_ver(1)
 *       | crc32_LE(cmd | key_no | (new_key XOR old_key) | key_ver)
 *       | crc32_LE(new_key)
 *       | pad
 *
 * IV is the EV2 host->card IV. The MAC is the standard
 *   cmac(cmd | ctr | TI | key_no | ciphertext).
 *
 * Counter rules: increments on success, regardless of branch.
 *
 * Important caveat: changing key 0 (app master) of the *current* application
 * de-authenticates the session immediately. Any post-change MAC in the
 * response uses the *old* session keys, but you cannot run further commands
 * without re-authenticating. We handle that by tolerating either a present
 * or absent response MAC for the same-key key-0 case.
 */

esp_err_t desfire_change_key(uint8_t key_no,
                             const uint8_t old_key[DESFIRE_AES_KEY_LEN],
                             const uint8_t new_key[DESFIRE_AES_KEY_LEN],
                             uint8_t key_version)
{
    if (!g_session.authenticated) return ESP_ERR_INVALID_STATE;

    /* Determine same-key vs different-key. The "same key" case is when
     * key_no equals the key we authenticated with AND we're at the same
     * application level (which we must be — auth doesn't survive
     * SelectApplication). */
    bool same_key = (key_no == g_session.key_no);

    /* Build the encrypted blob */
    uint8_t plain[32];   /* 32 covers both 24-byte same-key and 28-byte diff-key, padded */
    size_t  pi = 0;

    if (same_key) {
        /* new_key | key_ver | crc32(cmd | key_no | new_key | key_ver) */
        memcpy(&plain[pi], new_key, 16); pi += 16;
        plain[pi++] = key_version;

        uint8_t crc_in[1 + 1 + 16 + 1];
        size_t  ci = 0;
        crc_in[ci++] = DF_CMD_CHANGE_KEY;
        crc_in[ci++] = key_no;
        memcpy(&crc_in[ci], new_key, 16); ci += 16;
        crc_in[ci++] = key_version;
        uint32_t crc = desfire_crc32(crc_in, ci);
        plain[pi++] = (uint8_t)(crc & 0xFF);
        plain[pi++] = (uint8_t)((crc >> 8) & 0xFF);
        plain[pi++] = (uint8_t)((crc >> 16) & 0xFF);
        plain[pi++] = (uint8_t)((crc >> 24) & 0xFF);
    } else {
        /* (new XOR old) | key_ver
         *   | crc32(cmd | key_no | (new XOR old) | key_ver)
         *   | crc32(new_key)
         */
        for (int i = 0; i < 16; i++) plain[pi + i] = new_key[i] ^ old_key[i];
        pi += 16;
        plain[pi++] = key_version;

        uint8_t crc_in[1 + 1 + 16 + 1];
        size_t  ci = 0;
        crc_in[ci++] = DF_CMD_CHANGE_KEY;
        crc_in[ci++] = key_no;
        for (int i = 0; i < 16; i++) crc_in[ci + i] = new_key[i] ^ old_key[i];
        ci += 16;
        crc_in[ci++] = key_version;
        uint32_t crc1 = desfire_crc32(crc_in, ci);
        plain[pi++] = (uint8_t)(crc1 & 0xFF);
        plain[pi++] = (uint8_t)((crc1 >> 8) & 0xFF);
        plain[pi++] = (uint8_t)((crc1 >> 16) & 0xFF);
        plain[pi++] = (uint8_t)((crc1 >> 24) & 0xFF);

        uint32_t crc2 = desfire_crc32(new_key, 16);
        plain[pi++] = (uint8_t)(crc2 & 0xFF);
        plain[pi++] = (uint8_t)((crc2 >> 8) & 0xFF);
        plain[pi++] = (uint8_t)((crc2 >> 16) & 0xFF);
        plain[pi++] = (uint8_t)((crc2 >> 24) & 0xFF);
    }

    /* Pad to 32-byte boundary (both branches end at 21 or 25 bytes, plus
     * 0x80 + zeroes). */
    pi = pad_to_block(plain, pi, sizeof(plain));
    if (pi == 0) return ESP_ERR_INVALID_SIZE;

    /* Encrypt */
    uint8_t iv[16];
    desfire_enc_iv(0xA5, 0x5A, iv);

    uint8_t cipher[32];
    desfire_aes_cbc_encrypt(g_session.k_ses_auth_enc, iv, plain, cipher, pi);

    /* MAC over: cmd | ctr | TI | key_no | ciphertext */
    uint8_t macbuf[1 + 2 + 4 + 1 + 32];
    size_t  mi = 0;
    macbuf[mi++] = DF_CMD_CHANGE_KEY;
    macbuf[mi++] = (uint8_t)(g_session.cmd_ctr & 0xFF);
    macbuf[mi++] = (uint8_t)(g_session.cmd_ctr >> 8);
    memcpy(&macbuf[mi], g_session.ti, 4); mi += 4;
    macbuf[mi++] = key_no;
    memcpy(&macbuf[mi], cipher, pi); mi += pi;

    uint8_t mac8[8];
    cmac_session(macbuf, mi, mac8);

    /* Wire params = key_no | ciphertext | mac8 */
    uint8_t params[1 + 32 + 8];
    size_t wi = 0;
    params[wi++] = key_no;
    memcpy(&params[wi], cipher, pi); wi += pi;
    memcpy(&params[wi], mac8, 8); wi += 8;

    uint8_t rx[16]; size_t rl = 0;
    int st = desfire_xfer(DF_CMD_CHANGE_KEY, params, wi, rx, sizeof(rx), &rl);
    if (st != DF_OPERATION_OK) {
        ESP_LOGW(TAG, "ChangeKey card status 0x%02x", st);
        return ESP_FAIL;
    }

    /* If we just changed the key we authenticated with, the session is dead.
     * Mark it so the next op forces re-auth. */
    if (same_key) {
        g_session.authenticated = false;
        ESP_LOGI(TAG, "ChangeKey same-key: session de-authenticated");
        return ESP_OK;
    }

    g_session.cmd_ctr++;

    /* Verify the response MAC if present (different-key branch returns one) */
    if (rl >= 8) {
        size_t rd = rl - 8;
        uint8_t verify[1 + 2 + 4 + 16];
        size_t vi = 0;
        verify[vi++] = (uint8_t)st;
        verify[vi++] = (uint8_t)(g_session.cmd_ctr & 0xFF);
        verify[vi++] = (uint8_t)(g_session.cmd_ctr >> 8);
        memcpy(&verify[vi], g_session.ti, 4); vi += 4;
        if (rd) { memcpy(&verify[vi], rx, rd); vi += rd; }

        uint8_t want8[8];
        cmac_session(verify, vi, want8);
        if (memcmp(&rx[rd], want8, 8) != 0) {
            ESP_LOGE(TAG, "ChangeKey: response MAC mismatch");
            return ESP_ERR_INVALID_CRC;
        }
    }
    return ESP_OK;
}
