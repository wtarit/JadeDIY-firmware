#ifndef _SW_JADE_MINIZ_H_
#define _SW_JADE_MINIZ_H_ 1
#include <zlib.h>

#define TDEFL_DEFAULT_MAX_PROBES 0
#define TINFL_FLAG_USING_NON_WRAPPING_OUTPUT_BUF 0
#define TDEFL_FINISH 0

typedef struct tdefl_compressor
{
    int unused;
} tdefl_compressor;

typedef struct tdefl_compressor tinfl_decompressor;

typedef int tdefl_status;
typedef int tinfl_status;

#define TDEFL_STATUS_OKAY Z_OK
#define TDEFL_STATUS_DONE Z_OK
#define TINFL_STATUS_DONE Z_OK

static inline tdefl_status tdefl_init(void* ctx, void* unused, void* unused2, int flags)
{
    return TDEFL_STATUS_OKAY;
}

static inline tdefl_status tdefl_compress(tdefl_compressor* ctx, const uint8_t* data, size_t *data_len, uint8_t* output, size_t *output_len, int flags)
{
    // Defer to zlib
    return compress2(output, output_len, data, *data_len, Z_BEST_COMPRESSION);
}

static inline tinfl_status tinfl_init(tinfl_decompressor* ctx)
{
    return TDEFL_STATUS_OKAY;
}

static inline tinfl_status tinfl_decompress(tinfl_decompressor* ctx, const uint8_t* data, size_t *data_len, uint8_t* output, uint8_t* output_nex, size_t *output_len, int flags)
{
    return uncompress2(output, output_len, data, data_len);
}

#endif // _SW_JADE_MINIZ_H_
