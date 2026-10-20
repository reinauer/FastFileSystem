#ifndef FFS_DMA_H
#define FFS_DMA_H
#include <stdint.h>
/* The start must satisfy Mask, and the entire byte range must stay inside
 * addressable memory. Bit zero is allowed inside an even-aligned transfer. */
static int ffs_dma_range(uint32_t mask, uint32_t start, uint32_t size)
{
    uint32_t end = start + size - 1, varying;
    if (!size || end < start || (start & ~mask)) return 0;
    varying = start ^ end;
    varying |= varying >> 1; varying |= varying >> 2; varying |= varying >> 4;
    varying |= varying >> 8; varying |= varying >> 16;
    return !((start | varying) & ~(mask | 1u));
}
#endif
