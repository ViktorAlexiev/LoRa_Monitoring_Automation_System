#ifndef FAKE_MBEDTLS_CMAC_H
#define FAKE_MBEDTLS_CMAC_H

#include <stddef.h>
#include "cipher.h"

int mbedtls_cipher_cmac(const mbedtls_cipher_info_t *cipher_info, const unsigned char *key, size_t keylen,
                        const unsigned char *input, size_t ilen, unsigned char *output);

#endif
