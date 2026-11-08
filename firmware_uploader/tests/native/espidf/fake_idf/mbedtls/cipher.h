#ifndef FAKE_MBEDTLS_CIPHER_H
#define FAKE_MBEDTLS_CIPHER_H

typedef enum { MBEDTLS_CIPHER_NONE = 0, MBEDTLS_CIPHER_AES_128_ECB = 1 } mbedtls_cipher_type_t;
typedef struct { mbedtls_cipher_type_t type; } mbedtls_cipher_info_t;

const mbedtls_cipher_info_t *mbedtls_cipher_info_from_type(mbedtls_cipher_type_t type);

#endif
