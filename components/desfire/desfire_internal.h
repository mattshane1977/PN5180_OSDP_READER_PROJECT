#pragma once

#include <stdint.h>
#include <stddef.h>
#include "desfire.h"

/* DESFire native status codes */
#define DF_OPERATION_OK         0x00
#define DF_NO_CHANGES           0x0C
#define DF_ILLEGAL_COMMAND      0x1C
#define DF_INTEGRITY_ERROR      0x1E
#define DF_NO_SUCH_KEY          0x40
#define DF_LENGTH_ERROR         0x7E
#define DF_PERMISSION_DENIED    0x9D
#define DF_PARAMETER_ERROR      0x9E
#define DF_AUTHENTICATION_ERROR 0xAE
#define DF_ADDITIONAL_FRAME     0xAF
#define DF_BOUNDARY_ERROR       0xBE
#define DF_COMMAND_ABORTED      0xCA
#define DF_DUPLICATE_ERROR      0xDE

/* Native command codes */
#define DF_CMD_GET_VERSION          0x60
#define DF_CMD_GET_APPLICATION_IDS  0x6A
#define DF_CMD_GET_FILE_IDS         0x6F
#define DF_CMD_GET_FILE_SETTINGS    0xF5
#define DF_CMD_AUTH_EV2_FIRST       0x71
#define DF_CMD_AUTH_EV2_NON_FIRST   0x77
#define DF_CMD_SELECT_APP           0x5A
#define DF_CMD_GET_UID              0x51
#define DF_CMD_FORMAT_PICC          0xFC
#define DF_CMD_CREATE_APP           0xCA
#define DF_CMD_CREATE_STD_FILE      0xCD
#define DF_CMD_CHANGE_KEY           0xC4
#define DF_CMD_READ_DATA            0xBD
#define DF_CMD_WRITE_DATA           0x3D
#define DF_CMD_ADDITIONAL_FRAME     0xAF

/* APDU exchange — returns DESFire status byte, or -1 on transport error.
 * resp_data does NOT include the DESFire status byte. */
int desfire_xfer(uint8_t cmd, const uint8_t *params, size_t params_len,
                 uint8_t *resp_data, size_t resp_buf, size_t *resp_len);

/* CMAC over msg using the current session MAC key. Output: 16 bytes. */
void desfire_cmac(const uint8_t *msg, size_t len, uint8_t out_full[16]);

/* CRC32 (ISO 13239, no final XOR — matches NXP datasheet) */
uint32_t desfire_crc32(const uint8_t *data, size_t len);

/* AES-128 CBC, caller manages IV */
void desfire_aes_cbc_encrypt(const uint8_t key[16], uint8_t iv[16],
                             const uint8_t *in, uint8_t *out, size_t len);
void desfire_aes_cbc_decrypt(const uint8_t key[16], uint8_t iv[16],
                             const uint8_t *in, uint8_t *out, size_t len);

extern desfire_session_t g_session;

/* Encrypted-mode helpers (desfire_enc.c) */
void desfire_enc_iv(uint8_t magic_hi, uint8_t magic_lo, uint8_t iv_out[16]);

int desfire_enc_send(uint8_t cmd,
                     const uint8_t *header, size_t hlen,
                     const uint8_t *data,   size_t dlen,
                     uint8_t *resp, size_t resp_buf, size_t *resp_len);

int desfire_enc_recv(uint8_t cmd,
                     const uint8_t *header, size_t hlen,
                     size_t payload_len,
                     uint8_t *resp, size_t resp_buf, size_t *resp_len);
