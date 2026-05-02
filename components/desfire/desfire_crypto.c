#include "desfire_internal.h"
#include "mbedtls/aes.h"

#include <string.h>

/* ---- AES-128 CBC, IV continued across calls (caller-managed) ---- */

void desfire_aes_cbc_encrypt(const uint8_t key[16], uint8_t iv[16],
                             const uint8_t *in, uint8_t *out, size_t len)
{
    mbedtls_aes_context ctx;
    mbedtls_aes_init(&ctx);
    mbedtls_aes_setkey_enc(&ctx, key, 128);
    mbedtls_aes_crypt_cbc(&ctx, MBEDTLS_AES_ENCRYPT, len, iv, in, out);
    mbedtls_aes_free(&ctx);
}

void desfire_aes_cbc_decrypt(const uint8_t key[16], uint8_t iv[16],
                             const uint8_t *in, uint8_t *out, size_t len)
{
    mbedtls_aes_context ctx;
    mbedtls_aes_init(&ctx);
    mbedtls_aes_setkey_dec(&ctx, key, 128);
    mbedtls_aes_crypt_cbc(&ctx, MBEDTLS_AES_DECRYPT, len, iv, in, out);
    mbedtls_aes_free(&ctx);
}

/* ---- CMAC (NIST SP 800-38B) over AES-128 ----
 *
 * DESFire EV2 uses the MAC subkeys derived from K_ses_auth_mac. We compute the
 * full 16-byte CMAC every call and let the caller truncate as needed (EV2 uses
 * the 8 even-indexed bytes for the on-wire MAC).
 */

static void aes_ecb_encrypt_block(const uint8_t key[16], const uint8_t in[16], uint8_t out[16])
{
    mbedtls_aes_context ctx;
    mbedtls_aes_init(&ctx);
    mbedtls_aes_setkey_enc(&ctx, key, 128);
    mbedtls_aes_crypt_ecb(&ctx, MBEDTLS_AES_ENCRYPT, in, out);
    mbedtls_aes_free(&ctx);
}

static void left_shift_one(const uint8_t in[16], uint8_t out[16])
{
    uint8_t carry = 0;
    for (int i = 15; i >= 0; i--) {
        out[i] = (uint8_t)((in[i] << 1) | carry);
        carry = (in[i] & 0x80) ? 1 : 0;
    }
}

static void cmac_subkeys(const uint8_t key[16], uint8_t k1[16], uint8_t k2[16])
{
    static const uint8_t Rb = 0x87;
    uint8_t L[16] = {0};
    uint8_t zero[16] = {0};
    aes_ecb_encrypt_block(key, zero, L);

    left_shift_one(L, k1);
    if (L[0] & 0x80) k1[15] ^= Rb;

    left_shift_one(k1, k2);
    if (k1[0] & 0x80) k2[15] ^= Rb;
}

void desfire_cmac(const uint8_t *msg, size_t len, uint8_t out_full[16])
{
    extern uint8_t g_session_mac_key_full[16]; /* set in desfire_auth.c */
    const uint8_t *key = g_session_mac_key_full;

    uint8_t k1[16], k2[16];
    cmac_subkeys(key, k1, k2);

    /* Number of complete blocks; the last block may be partial. */
    size_t n = (len == 0) ? 1 : (len + 15) / 16;
    bool last_complete = (len != 0 && (len % 16) == 0);

    uint8_t M_last[16];
    if (last_complete) {
        memcpy(M_last, &msg[(n - 1) * 16], 16);
        for (int i = 0; i < 16; i++) M_last[i] ^= k1[i];
    } else {
        size_t rem = len - (n - 1) * 16;
        memset(M_last, 0, 16);
        if (rem) memcpy(M_last, &msg[(n - 1) * 16], rem);
        M_last[rem] = 0x80;                /* padding */
        for (int i = 0; i < 16; i++) M_last[i] ^= k2[i];
    }

    uint8_t X[16] = {0};
    uint8_t Y[16];
    for (size_t i = 0; i < n - 1; i++) {
        for (int j = 0; j < 16; j++) Y[j] = X[j] ^ msg[i * 16 + j];
        aes_ecb_encrypt_block(key, Y, X);
    }
    for (int j = 0; j < 16; j++) Y[j] = X[j] ^ M_last[j];
    aes_ecb_encrypt_block(key, Y, out_full);
}

/* ---- CRC32 ---- 
 *
 * DESFire uses ISO/IEC 13239 CRC32 (polynomial 0xEDB88320, init 0xFFFFFFFF,
 * no final xor, reflected). This is the same as zlib CRC32 *without* the
 * final xor — i.e. ~zlib(x) where zlib does the xor. We implement directly
 * to avoid pulling in zlib for one routine.
 */

uint32_t desfire_crc32(const uint8_t *data, size_t len)
{
    uint32_t crc = 0xFFFFFFFF;
    for (size_t i = 0; i < len; i++) {
        crc ^= data[i];
        for (int b = 0; b < 8; b++) {
            crc = (crc >> 1) ^ (0xEDB88320u & -(int32_t)(crc & 1));
        }
    }
    return crc;   /* no final XOR — matches NXP datasheet */
}
