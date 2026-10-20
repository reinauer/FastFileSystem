#include <stddef.h>
#include <stdint.h>
typedef uint32_t memory_word __attribute__((__may_alias__));
/* Every 68k CPU moves longwords at even addresses; others need longword
 * alignment for full-speed copies. */
#ifdef __mc68000__
#define WORD_ALIGNMENT 1u
#else
#define WORD_ALIGNMENT 3u
#endif
void *memcpy(void *dst, const void *src, size_t n)
{
    unsigned char *d = dst;
    const unsigned char *s = src;
    if ((((uintptr_t)d ^ (uintptr_t)s) & WORD_ALIGNMENT) == 0) {
        while (n && ((uintptr_t)d & 3u)) { *d++ = *s++; --n; }
        while (n >= 32) {
            memory_word *dw = (memory_word *)d;
            const memory_word *sw = (const memory_word *)s;
            dw[0] = sw[0]; dw[1] = sw[1]; dw[2] = sw[2]; dw[3] = sw[3];
            dw[4] = sw[4]; dw[5] = sw[5]; dw[6] = sw[6]; dw[7] = sw[7];
            d += 32; s += 32; n -= 32;
        }
        while (n >= 4) {
            *(memory_word *)d = *(const memory_word *)s;
            d += 4; s += 4; n -= 4;
        }
    }
    while (n--) *d++ = *s++;
    return dst;
}
void *memset(void *dst, int ch, size_t n)
{
    unsigned char *d = dst;
    uint32_t value = (unsigned char)ch;
    value |= value << 8; value |= value << 16;
    while (n && ((uintptr_t)d & 3u)) { *d++ = (unsigned char)ch; --n; }
    while (n >= 32) {
        memory_word *dw = (memory_word *)d;
        dw[0] = value; dw[1] = value; dw[2] = value; dw[3] = value;
        dw[4] = value; dw[5] = value; dw[6] = value; dw[7] = value;
        d += 32; n -= 32;
    }
    while (n >= 4) { *(memory_word *)d = value; d += 4; n -= 4; }
    while (n--) *d++ = (unsigned char)ch;
    return dst;
}
size_t strlen(const char *s)
{
    const char *p = s;
    while (*p) ++p;
    return (size_t)(p - s);
}
char *strchr(const char *s, int ch)
{
    do { if (*s == (char)ch) return (char *)s; } while (*s++);
    return NULL;
}
