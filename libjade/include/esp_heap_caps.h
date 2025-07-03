#ifndef _SW_JADE_ESP_HEAP_CAPS_H
#define _SW_JADE_ESP_HEAP_CAPS_H 1

#define MALLOC_CAP_DEFAULT 0
#define MALLOC_CAP_SPIRAM 0
#define MALLOC_CAP_INTERNAL 0

static inline void* heap_caps_malloc(size_t size, uint32_t caps) { return malloc(size); }
static inline void* heap_caps_malloc_prefer(size_t size, uint32_t caps, ...) { return malloc(size); }
static inline void* heap_caps_calloc(size_t num_elems, size_t size, uint32_t caps) { return calloc(num_elems, size); }
static inline void* heap_caps_calloc_prefer(size_t num_elems, size_t size, uint32_t caps, ...)
{
    return calloc(num_elems, size);
}
static inline void* heap_caps_aligned_alloc(size_t alignment, size_t size, uint32_t caps) { return malloc(size); }
static inline uint32_t heap_caps_get_free_size(uint32_t caps) { return 0xffffffff; }
static inline uint32_t heap_caps_get_largest_free_block(uint32_t caps) { return 0xffffffff; }

#endif // _SW_JADE_ESP_HEAP_CAPS_H
