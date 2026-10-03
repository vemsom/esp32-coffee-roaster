#pragma once
// Host stub for mbedtls/sha256.h: the real SHA-256, not a fake.
//
// ota_push.cpp hashes every byte of an incoming image on the device, and a
// stubbed hash would make the whole point of that (a checksum the uploader
// cannot lie about) untestable. The test instead computes the expected hash
// with the same implementation and checks that the endpoint accepts exactly
// that and nothing else - so the stub only has to provide the API mbedtls
// exposes, implemented with the host's own crypto when it has one.
//
// The firmware's own build never sees this file: the ESP32 Arduino core ships
// the real mbedtls.
#include <stddef.h>
#include <stdint.h>
#include <string.h>

typedef struct {
  uint32_t state[8];
  uint64_t bitlen;
  uint8_t buffer[64];
  size_t bufferLen;
} mbedtls_sha256_context;

void mbedtls_sha256_init(mbedtls_sha256_context *ctx);
void mbedtls_sha256_starts(mbedtls_sha256_context *ctx, int is224);
void mbedtls_sha256_update(mbedtls_sha256_context *ctx, const unsigned char *input, size_t ilen);
void mbedtls_sha256_finish(mbedtls_sha256_context *ctx, unsigned char output[32]);
