// Заместител на mbedtls/aes.h (AES-128 върху верифицираната реализация aes128.h)
#ifndef FAKE_MBEDTLS_AES_H
#define FAKE_MBEDTLS_AES_H

#include <stddef.h>
#include <stdint.h>

typedef struct { uint8_t key[16]; int keyed; } mbedtls_aes_context;

void mbedtls_aes_init(mbedtls_aes_context *ctx);
void mbedtls_aes_free(mbedtls_aes_context *ctx);
int mbedtls_aes_setkey_enc(mbedtls_aes_context *ctx, const unsigned char *key, unsigned int keybits);
int mbedtls_aes_crypt_ecb_enc(mbedtls_aes_context *ctx, const unsigned char in[16], unsigned char out[16]);
int mbedtls_aes_crypt_ctr(mbedtls_aes_context *ctx, size_t length, size_t *nc_off, unsigned char nonce_counter[16],
                          unsigned char stream_block[16], const unsigned char *input, unsigned char *output);

#endif
