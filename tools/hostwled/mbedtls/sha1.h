// 桩：util.cpp 里 SHA1 只给 OTA 用，与灯效无关。
#pragma once
#include <stdint.h>
#include <stddef.h>
typedef struct { int unused; } mbedtls_sha1_context;
static inline void mbedtls_sha1_init(mbedtls_sha1_context *) {}
static inline void mbedtls_sha1_free(mbedtls_sha1_context *) {}
static inline int  mbedtls_sha1_starts_ret(mbedtls_sha1_context *) { return 0; }
static inline int  mbedtls_sha1_update_ret(mbedtls_sha1_context *, const unsigned char *, size_t) { return 0; }
static inline int  mbedtls_sha1_finish_ret(mbedtls_sha1_context *, unsigned char *) { return 0; }
static inline int  mbedtls_sha1_starts(mbedtls_sha1_context *) { return 0; }
static inline int  mbedtls_sha1_update(mbedtls_sha1_context *, const unsigned char *, size_t) { return 0; }
static inline int  mbedtls_sha1_finish(mbedtls_sha1_context *, unsigned char *) { return 0; }
