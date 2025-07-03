#ifndef _SW_JADE_SODIUM_CRYPTO_VERIFY_32_H_
#define _SW_JADE_SODIUM_CRYPTO_VERIFY_32_H_ 1

static inline int crypto_verify_32(const unsigned char* x, const unsigned char* y) { return memcmp(x, y, 32); }

#endif // _SW_JADE_SODIUM_CRYPTO_VERIFY_32_H_
