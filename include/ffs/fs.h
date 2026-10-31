#ifndef FFS_FS_H
#define FFS_FS_H
#include "ffs/format.h"
#ifdef FFS_PROFILE
#include "ffs/profile.h"
#endif

/* Error values are the corresponding AmigaDOS IoErr values. */
enum {
    FFS_OK = 0, FFS_NOMEM = 103, FFS_BAD_NAME = 210,
    FFS_NOT_FOUND = 205, FFS_EXISTS = 203, FFS_WRONG_TYPE = 212,
    FFS_NOT_EMPTY = 216, FFS_FULL = 221, FFS_CORRUPT = 213,
    FFS_WRITE_PROTECTED = 214, FFS_READ_PROTECTED = 224,
    FFS_DELETE_PROTECTED = 222, FFS_SEEK_ERROR = 219,
    FFS_IO = 225, FFS_NO_MORE = 232, FFS_SOFT_LINK = 233
};
struct ffs_io {
    void *context;
    int (*read)(void *, uint32_t, unsigned char *, uint32_t);
    int (*write)(void *, uint32_t, const unsigned char *, uint32_t);
    int (*flush)(void *);
    void *(*alloc)(void *, size_t);
    void (*free)(void *, void *, size_t);
    /* Optional contiguous transfers: start block, byte length. */
    int (*read_extent)(void *, uint32_t, unsigned char *, uint32_t);
    int (*write_extent)(void *, uint32_t, const unsigned char *, uint32_t);
    /* Optional: store the current days, minutes and ticks. Called only
     * before timestamps are written; otherwise callers maintain now[]. */
    void (*date)(void *, uint32_t *);
    /* Optional block-aligned write whose bytes stay unchanged until the next
     * flush, so it may complete asynchronously. Falls back to write. */
    int (*write_owned)(void *, uint32_t, const unsigned char *, uint32_t);
};
/* A borrowed run of full caller blocks. OFS runs hold payload bytes; their
 * headers are built during transfer from the file block index and the key
 * following the run's last block. */
struct ffs_data_run { uint32_t key, size, blocks, index, next; const unsigned char *data; };
/* Blocks freed by the current operation, retired when it succeeds. */
struct ffs_released { struct ffs_released *next; unsigned count; uint32_t keys[32]; };
/* A range of a pending block edited in place; its prior bytes are kept at
 * offset `at` of the volume's undo buffer until the operation ends. */
struct ffs_undo { struct ffs_change *block; uint16_t offset, length, at; };
struct ffs_volume {
    struct ffs_io io;
    struct ffs_format format;
    uint32_t block_size, blocks, reserved, root, dos_type, buckets;
    unsigned block_shift;
    unsigned mounted, writable, failed, write_requested, validated;
    struct ffs_change *changes, *pending;
    struct ffs_released *released, released_inline;
    struct ffs_data_run *direct;
    struct ffs_data_run direct_inline[4];
    uint32_t direct_count, direct_capacity;
    unsigned disk_dirty;
    unsigned write_borrowed;
    /* On-disk root bytes, kept only while memory replaces them before the
     * validity flag is cleared on disk. */
    unsigned char *root_image;
    unsigned owned_pending;
    struct ffs_change *cache_index[64], *clean, *clean_tail;
    unsigned clean_count, cache_limit;
    unsigned deferred, pending_count, pending_limit;
    uint32_t read_file, read_table, read_index;
    uint32_t bitmap_page, bitmap_block, bitmap_base, bitmap_ext_index, bitmap_ext_key;
    /* Free blocks in memory, and the current operation's uncommitted change. */
    uint32_t free_count, free_change;
    unsigned free_known;
    uint32_t now[3], created[3], alloc_hint, generation;
    /* Writes and resizes may not grow a file beyond this size. Larger files
     * remain readable and may be overwritten in place or shrunk. */
    uint32_t file_limit;
    unsigned in_place;
    struct ffs_undo undo[16];
    unsigned undo_count, undo_used;
    unsigned char *undo_bytes;
    /* The last entry found by name, and its hash, until the next mutation. */
    uint32_t hash_key, hash_generation;
    uint16_t hash_value;
#ifdef FFS_PROFILE
    struct ffs_profile profile;
    uint32_t (*clock)(void *);
#endif
};
/* Saved mapping is valid only for the recorded mutation generation. */
struct ffs_file_state {
    uint32_t key, table, table_start, generation, cache_block, cache_offset;
    unsigned defer_metadata;
};
/* generation is the volume's when the entry was decoded. comment_block is
 * the DOS\6/7 block holding a comment too long to share the name's field. */
struct ffs_entry {
    uint32_t key, parent, size, protection, days, minutes, ticks;
    uint32_t link, hash_next, backlink, owner, generation, comment_block;
    int32_t type;
    unsigned char name[FFS_NAME_MAX + 1], comment[80];
};
/* The iterator is invalidated by directory mutations. */
struct ffs_iterator { uint32_t bucket, next, visited, index; };
/* A scan for ExNext and ExAll, which outlives changes to the volume: the
 * caller may move or delete what it has been given. Until a change the
 * iterator runs and keys collects what it returned. A change voids the
 * iterator, so the scan is fixed: the keys still to return follow those. */
struct ffs_scan {
    struct ffs_iterator it, before;
    uint32_t *keys;
    uint32_t capacity, count, position, generation, previous;
    unsigned fixed;
    int status;
};
#ifdef FFS_PROFILE
uint32_t ffs_profile_start(struct ffs_volume *);
void ffs_profile_end(struct ffs_volume *, unsigned, uint32_t);
#define PROFILE_START(v) uint32_t profile_start = ffs_profile_start(v)
#define PROFILE_END(v, id) ffs_profile_end(v, id, profile_start)
#else
#define PROFILE_START(v) ((void)0)
#define PROFILE_END(v, id) ((void)0)
#endif
int ffs_mount(struct ffs_volume *, const struct ffs_io *, uint32_t block_size,
              uint32_t blocks, uint32_t reserved, unsigned writable);
/* Size used by native scratch pools; callers must not inspect the nodes. */
size_t ffs_change_size(struct ffs_volume *);
/* Reserve clean cache buffers. Dirty buffers are never evicted by resizing. */
unsigned ffs_cache_resize(struct ffs_volume *, unsigned count);
/* Deferred writes remain readable until flush; discard on media loss. */
int ffs_flush(struct ffs_volume *);
/* Complete deferred file timestamps, archive flags and directory records. */
int ffs_finish_file(struct ffs_volume *, uint32_t key);
void ffs_discard(struct ffs_volume *);
int ffs_stat(struct ffs_volume *, uint32_t key, struct ffs_entry *);
int ffs_lookup(struct ffs_volume *, uint32_t directory,
               const unsigned char *, size_t, struct ffs_entry *);
int ffs_resolve(struct ffs_volume *, uint32_t directory, const char *,
                struct ffs_entry *);
/* Resolve a path, creating only a missing final file component. */
int ffs_resolve_create(struct ffs_volume *, uint32_t directory, const char *,
                       struct ffs_entry *, unsigned *created);
int ffs_next(struct ffs_volume *, uint32_t directory,
             struct ffs_iterator *, struct ffs_entry *);
/* Return the entry after key previous, or the first if previous is the
 * directory. Any entry may be resumed from, but only the one the scan last
 * returned is resumed from without rereading the directory. */
int ffs_scan_next(struct ffs_volume *, uint32_t directory, struct ffs_scan *,
                  uint32_t previous, struct ffs_entry *);
/* Have the next call return the last entry again, after previous. */
void ffs_scan_again(struct ffs_scan *, uint32_t previous);
void ffs_scan_end(struct ffs_volume *, struct ffs_scan *);
int ffs_read(struct ffs_volume *, uint32_t key, uint32_t offset, void *,
             uint32_t length, uint32_t *actual);
int ffs_create(struct ffs_volume *, uint32_t directory, const char *name,
                int directory_type, uint32_t *key);
int ffs_write(struct ffs_volume *, uint32_t key, uint32_t offset,
               const void *, uint32_t length, uint32_t *actual);
int ffs_write_file(struct ffs_volume *, struct ffs_file_state *, uint32_t key,
                   uint32_t offset, const void *, uint32_t length, uint32_t *actual);
int ffs_resize(struct ffs_volume *, uint32_t key, uint32_t size);
int ffs_remove(struct ffs_volume *, uint32_t key);
/* Removes a looked-up entry, decoding its header again only if the volume
 * changed since the lookup. */
int ffs_remove_entry(struct ffs_volume *, const struct ffs_entry *);
int ffs_rename(struct ffs_volume *, uint32_t key, uint32_t directory, const char *name);
int ffs_set_metadata(struct ffs_volume *, uint32_t key, const struct ffs_entry *);
int ffs_free_blocks(struct ffs_volume *, uint32_t *count);
int ffs_validate(struct ffs_volume *, unsigned repair);
int ffs_symlink(struct ffs_volume *, uint32_t directory, const char *name,
                 const char *target, uint32_t *key);
int ffs_readlink(struct ffs_volume *, uint32_t key, char *, uint32_t capacity,
                  uint32_t *length);
int ffs_format_volume(struct ffs_volume *, uint32_t dos_type, const char *name);
int ffs_relabel(struct ffs_volume *, const char *name);
int ffs_hardlink(struct ffs_volume *, uint32_t directory, const char *name,
                  uint32_t target, uint32_t *key);
#endif
