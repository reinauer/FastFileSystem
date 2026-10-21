#ifndef FFS_FORMAT_H
#define FFS_FORMAT_H

#include <stddef.h>
#include <stdint.h>

#define FFS_DOS_BASE UINT32_C(0x444f5300)
/* DOS\6/7 names reach 107 bytes, the DOS limit; FastFileSystem 47.4
 * creates them only up to 106. Other formats keep 30. */
#define FFS_NAME_MAX 107u
#define FFS_LONG_NAME_MAX 106u
#define FFS_SHORT_NAME_MAX 30u

struct ffs_format {
    unsigned fast;
    unsigned international;
    unsigned dircache;
    unsigned long_names;
};

/* Return 0 on success, -1 for invalid arguments; leave outputs untouched. */
int ffs_decode_type(uint32_t type, struct ffs_format *format);
uint32_t ffs_get_be32(const unsigned char *bytes);
void ffs_put_be32(unsigned char *bytes, uint32_t value);
static inline unsigned char ffs_upper(unsigned char ch, unsigned international)
{
    /* International folding leaves 0xf7 and 0xff unchanged. */
    if ((ch >= 'a' && ch <= 'z') ||
        (international && ch >= 0xe0 && ch <= 0xfe && ch != 0xf7))
        return (unsigned char)(ch - 0x20);
    return ch;
}
/* Names are byte strings, not UTF-8. Hash returns a bucket, not an offset. */
int ffs_name_hash(const unsigned char *name, size_t length,
                  unsigned international, uint16_t buckets, uint16_t *hash);
/* Ordinary additive checksum, NOT the end-around-carry boot checksum. */
int ffs_block_sum(const unsigned char *block, size_t size, uint32_t *sum);
/* checksum_offset is 20 for headers/OFS data, 0 for bitmap blocks. */
int ffs_block_checksum(unsigned char *block, size_t size,
                       size_t checksum_offset);

#endif
