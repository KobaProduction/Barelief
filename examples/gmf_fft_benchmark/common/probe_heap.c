#include "esp_gmf_fft_heap.h"
#include "gmf_probe.h"
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#if defined(ESP_PLATFORM)
#include "esp_heap_caps.h"
#elif defined(_WIN32)
#include <malloc.h>
#endif
/* Three upstream allocations plus caller data. Guards and bookkeeping are
 * measurement infrastructure, reported separately from payload. */
typedef struct { void *base; unsigned char *data; size_t size; } allocation;
static allocation slots[16];
static size_t live_bytes, calls;
static int good = 1;
void *esp_gmf_fft_calloc_aligned(size_t n, size_t elem, size_t alignment)
{
    if (n && elem > SIZE_MAX / n) return NULL;
    size_t size = n * elem;
    if (size > SIZE_MAX - 32) return NULL;
    if (alignment < 16) alignment = 16;
    /* Upstream requests 4/16. Offset 16 preserves its alignment. */
    if (alignment > 16) return NULL;
    size_t slot = 0;
    while (slot < 16 && slots[slot].base) ++slot;
    if (slot == 16) return NULL;
#if defined(ESP_PLATFORM)
    void *base = heap_caps_aligned_alloc(alignment, size + 32, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
#elif defined(_WIN32)
    void *base = _aligned_malloc(size + 32, alignment);
#else
    void *base = NULL;
    if (posix_memalign(&base, alignment, size + 32)) return NULL;
#endif
    if (!base) return NULL;
    unsigned char *data = (unsigned char *)base + 16;
    memset(base, 0xa5, 16);
    memset(data, 0, size);
    memset(data + size, 0xa5, 16);
    slots[slot] = (allocation){base, data, size};
    live_bytes += size;
    ++calls;
    return data;
}
int gmf_probe_guards_ok(void)
{
    for (size_t s = 0; s < 16; ++s) if (slots[s].base) {
        unsigned char *base = slots[s].base;
        for (size_t i = 0; i < 16; ++i)
            if (base[i] != 0xa5 || slots[s].data[slots[s].size+i] != 0xa5) good = 0;
    }
    return good;
}
void esp_gmf_fft_free_aligned(void *ptr)
{
    if (!ptr) return;
    gmf_probe_guards_ok();
    for (size_t s = 0; s < 16; ++s) if (slots[s].data == ptr) {
#if defined(ESP_PLATFORM)
        heap_caps_free(slots[s].base);
#elif defined(_WIN32)
        _aligned_free(slots[s].base);
#else
        free(slots[s].base);
#endif
        live_bytes -= slots[s].size;
        slots[s] = (allocation){0};
        return;
    }
    good = 0;
}
size_t gmf_probe_live_bytes(void) { return live_bytes; }
size_t gmf_probe_allocations(void) { return calls; }
