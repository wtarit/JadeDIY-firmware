#ifndef _SW_JADE_MBEDTLS_BASE64_H_
#define _SW_JADE_MBEDTLS_BASE64_H_ 1

#include <wally_core.h>

static inline int mbedtls_base64_decode(
    unsigned char* dst, size_t dlen, size_t* olen, const unsigned char* src, size_t slen)
{
    return wally_base64_n_to_bytes((const char*)src, slen, 0, dst, dlen, olen) == WALLY_OK ? 0 : 1;
}

static inline int mbedtls_base64_encode(
    unsigned char* dst, size_t dlen, size_t* olen, const unsigned char* src, size_t slen)
{
    char* output;
    int ret = wally_base64_from_bytes(src, slen, 0, &output);
    if (ret != WALLY_OK) {
        return 1;
    }
    *olen = strlen(output); // Doesn't include NUL byte!
    if (dlen >= *olen) {
        memcpy(dst, output, *olen);
    }
    wally_free_string(output);
    return dlen >= *olen ? 0 : 1;
}
#endif // _SW_JADE_MBEDTLS_BASE64_H_
