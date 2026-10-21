#include "ffs/fs.h"
#include <string.h>

#ifdef FFS_PROFILE
uint32_t ffs_profile_start(struct ffs_volume *v)
{
    return v->clock ? v->clock(v->io.context) : 0;
}
void ffs_profile_end(struct ffs_volume *v, unsigned id, uint32_t start)
{
    if (v->clock) { v->profile.ticks[id] += v->clock(v->io.context) - start; ++v->profile.calls[id]; }
}
#endif

enum { BLOCK_CLEAN, BLOCK_CHANGE, BLOCK_PENDING, BLOCK_READ };
struct ffs_change {
    struct ffs_change *next, *hash_next, *previous, *rollback;
    uint32_t key;
    int checksum;
    unsigned close_update, state, readers, verified;
    unsigned char bytes[];
};

static struct ffs_change *cached_block(struct ffs_volume *v, uint32_t key)
{
    struct ffs_change *c;
    for (c = v->cache_index[key % 64]; c; c = c->hash_next)
        if (c->key == key) return c;
    return NULL;
}
static struct ffs_change *pending_block(struct ffs_volume *v, uint32_t key)
{
    struct ffs_change *c = cached_block(v, key);
    return c && c->state == BLOCK_PENDING ? c : NULL;
}
static void index_block(struct ffs_volume *v, struct ffs_change *c)
{
    c->hash_next = v->cache_index[c->key % 64];
    v->cache_index[c->key % 64] = c;
}
static void unindex_block(struct ffs_volume *v, struct ffs_change *c)
{
    struct ffs_change **p;
    if (c->key == UINT32_MAX) return;
    for (p = &v->cache_index[c->key % 64]; *p && *p != c; p = &(*p)->hash_next) {}
    if (*p) *p = c->hash_next;
}
static void unlink_clean(struct ffs_volume *v, struct ffs_change *c)
{
    if (c->previous) c->previous->next = c->next; else v->clean = c->next;
    if (c->next) c->next->previous = c->previous; else v->clean_tail = c->previous;
    --v->clean_count;
}
static void link_clean(struct ffs_volume *v, struct ffs_change *c)
{
    c->state = BLOCK_CLEAN; c->previous = NULL; c->next = v->clean;
    if (c->next) c->next->previous = c; else v->clean_tail = c;
    v->clean = c; ++v->clean_count;
}
static void free_block(struct ffs_volume *v, struct ffs_change *c)
{
    v->io.free(v->io.context, c, sizeof(*c) + v->block_size);
}
static void drop_clean(struct ffs_volume *v)
{
    struct ffs_change *c;
    while ((c = v->clean_tail)) {
        unlink_clean(v, c); unindex_block(v, c); free_block(v, c);
    }
}
unsigned ffs_cache_resize(struct ffs_volume *v, unsigned count)
{
    struct ffs_change *c;
    if (count > 1024) count = 1024;
    while (v->clean_count > count) {
        c = v->clean_tail;
        unlink_clean(v, c); unindex_block(v, c); free_block(v, c);
    }
    while (v->clean_count < count) {
        c = v->io.alloc(v->io.context, sizeof(*c) + v->block_size);
        if (!c) break;
        c->key = UINT32_MAX; c->rollback = NULL;
        c->close_update = c->readers = c->verified = 0;
        link_clean(v, c);
    }
    v->cache_limit = v->clean_count;
    return v->cache_limit;
}
/* A detached cell has no lookup identity and cannot be observed by readers. */
static struct ffs_change *new_block(struct ffs_volume *v)
{
    struct ffs_change *c = v->clean_tail;
    /* Dirty growth must not continually evict a smaller clean working set.
     * Prefer spare storage within its budget; evict clean data on failure. */
    if (!c || (c->key != UINT32_MAX && v->clean_count < v->cache_limit)) {
        c = v->io.alloc(v->io.context, sizeof(*c) + v->block_size);
        if (c) { c->rollback = NULL; c->close_update = c->readers = c->verified = 0; return c; }
        c = v->clean_tail;
    }
    if (c) { unlink_clean(v, c); unindex_block(v, c); }
    if (c) { c->rollback = NULL; c->close_update = c->readers = c->verified = 0; }
    return c;
}
static void retain_clean(struct ffs_volume *v, struct ffs_change *c)
{
    if (!v->cache_limit) { unindex_block(v, c); free_block(v, c); return; }
    if (v->clean_count == v->cache_limit) {
        struct ffs_change *old = v->clean_tail;
        unlink_clean(v, old); unindex_block(v, old); free_block(v, old);
    }
    c->rollback = NULL; c->close_update = 0;
    link_clean(v, c);
}
static void unlink_pending(struct ffs_volume *v, struct ffs_change *c)
{
    if (c->previous) c->previous->next = c->next; else v->pending = c->next;
    if (c->next) c->next->previous = c->previous;
    --v->pending_count;
}
static unsigned deferred_metadata(struct ffs_volume *v,
                                  const struct ffs_file_state *state)
{
    return v->deferred && state && state->defer_metadata;
}

size_t ffs_change_size(struct ffs_volume *v)
{
    return sizeof(struct ffs_change) + v->block_size;
}

static uint32_t word(const unsigned char *p, uint32_t offset)
{
#if defined(__GNUC__) && defined(__BYTE_ORDER__) && defined(__ORDER_BIG_ENDIAN__) && \
    __BYTE_ORDER__ == __ORDER_BIG_ENDIAN__
    /* Core buffers are allocator-aligned; metadata fields are even-aligned,
     * including the compact directory records. GCC must know that the latter
     * need only two-byte alignment on targets with stricter longword access. */
    typedef uint32_t metadata_word __attribute__((__may_alias__, __aligned__(2)));
    return *(const metadata_word *)(p + offset);
#else
    return ffs_get_be32(p + offset);
#endif
}
/* Metadata fields are at least even-aligned, including cache records. */
static inline void put_word(unsigned char *p, uint32_t value)
{
#if defined(__GNUC__) && defined(__BYTE_ORDER__) && defined(__ORDER_BIG_ENDIAN__) && \
    __BYTE_ORDER__ == __ORDER_BIG_ENDIAN__
    typedef uint32_t metadata_word __attribute__((__may_alias__, __aligned__(2)));
    *(metadata_word *)p = value;
#else
    ffs_put_be32(p, value);
#endif
}
static int valid_key(const struct ffs_volume *v, uint32_t key)
{
    return key >= v->reserved && key < v->blocks;
}
static unsigned char *buffer(struct ffs_volume *v)
{
    return v->io.alloc(v->io.context, v->block_size);
}
static void release(struct ffs_volume *v, unsigned char *p)
{
    if (p) v->io.free(v->io.context, p, v->block_size);
}
static int check_block(struct ffs_volume *v, const unsigned char *p, unsigned checksum)
{
    uint32_t sum;
    if (checksum && (ffs_block_sum(p, v->block_size, &sum) || sum))
        return FFS_CORRUPT;
    return 0;
}
/* Clean bytes stay unchanged until eviction or writable acquisition. Raw
 * payload reads do not establish that they have a metadata checksum. */
static int check_cached(struct ffs_volume *v, struct ffs_change *c, unsigned checksum)
{
    int err;
    if (!checksum || c->verified) return 0;
    err = check_block(v, c->bytes, 1);
    if (!err) c->verified = 1;
    return err;
}
/* Views are local to read-only scopes. Pinned clean blocks stay indexed but
 * leave the eviction list until the last reader releases them. No view may
 * cross a mutation or escape through a file handle or a returned entry. */
struct block_view {
    struct ffs_change *block;
    const unsigned char *bytes;
};
static int read_view(struct ffs_volume *v, uint32_t key,
                      struct block_view *view, unsigned checksum)
{
    struct ffs_change *c = cached_block(v, key);
    int err;
    view->block = NULL; view->bytes = NULL;
    if (key >= v->blocks) return FFS_CORRUPT;
    if (!c) {
        c = new_block(v);
        if (!c) return FFS_NOMEM;
        if (v->io.read(v->io.context, key, c->bytes, v->block_size)) err = FFS_IO;
        else err = check_block(v, c->bytes, checksum);
        if (err) { free_block(v, c); return err; }
        c->key = key; c->state = BLOCK_READ; c->verified = !!checksum;
        index_block(v, c);
    } else if (c->state == BLOCK_CLEAN || c->state == BLOCK_READ) {
        err = check_cached(v, c, checksum);
        if (err) return err;
        if (c->state == BLOCK_CLEAN) {
            unlink_clean(v, c); c->state = BLOCK_READ;
        }
    }
    ++c->readers;
    view->block = c; view->bytes = c->bytes;
    return 0;
}
static void release_view(struct ffs_volume *v, struct block_view *view)
{
    struct ffs_change *c = view->block;
    if (!c) return;
    view->block = NULL; view->bytes = NULL;
    if (!--c->readers && c->state == BLOCK_READ) retain_clean(v, c);
}
static int read_block(struct ffs_volume *v, uint32_t key,
                       unsigned char *p, unsigned checksum)
{
    struct ffs_change *c = cached_block(v, key);
    int err;
    if (c) {
        if (c->state == BLOCK_CLEAN || c->state == BLOCK_READ) {
            if (c->state == BLOCK_CLEAN) { unlink_clean(v, c); link_clean(v, c); }
            err = check_cached(v, c, checksum);
            if (err) return err;
        }
        memcpy(p, c->bytes, v->block_size);
        return 0;
    }
    if (key >= v->blocks) return FFS_CORRUPT;
    if (v->io.read(v->io.context, key, p, v->block_size)) return FFS_IO;
    err = check_block(v, p, checksum);
    if (!err && v->cache_limit) {
        /* A cache miss must still work when optional storage is unavailable. */
        c = v->clean_count < v->cache_limit ?
            v->io.alloc(v->io.context, sizeof(*c) + v->block_size) : NULL;
        if (!c && v->clean_tail) {
            c = v->clean_tail; unlink_clean(v, c); unindex_block(v, c);
        }
        if (c) {
            c->key = key; c->rollback = NULL; c->close_update = c->readers = 0;
            c->verified = !!checksum;
            memcpy(c->bytes, p, v->block_size);
            index_block(v, c); link_clean(v, c);
        }
    }
    return err;
}
/* Retain the on-disk root when memory first replaces it, so clearing its
 * validity flag needs no device read. Allocation failure keeps that read. */
static void keep_root_image(struct ffs_volume *v, uint32_t key, const unsigned char *bytes)
{
    if (key == v->root && !v->disk_dirty && !v->root_image &&
        (v->root_image = buffer(v)))
        memcpy(v->root_image, bytes, v->block_size);
}
static void drop_root_image(struct ffs_volume *v)
{
    release(v, v->root_image); v->root_image = NULL;
}
/* Clean blocks become operation-owned in place. Pending blocks need an
 * unindexed rollback copy until the ordinary mutation succeeds. */
static int acquire_change(struct ffs_volume *v, uint32_t key,
                          struct ffs_change **result, int checksum,
                          unsigned clear, unsigned read)
{
    struct ffs_change *old, *c;
    int err = 0;
    if (!valid_key(v, key)) return FFS_CORRUPT;
    old = cached_block(v, key);
    if (old && old->readers) return FFS_CORRUPT;
    if (old && old->state == BLOCK_CHANGE) c = old;
    else {
        if (old && old->state == BLOCK_CLEAN) {
            c = old; unlink_clean(v, c);
            if (read && !clear) err = check_cached(v, c, checksum >= 0);
            if (!err) keep_root_image(v, key, c->bytes);
        } else {
            c = new_block(v);
            if (c && old) {
                c->close_update = old->close_update;
                if (read && !clear) memcpy(c->bytes, old->bytes, v->block_size);
            } else if (c && read && !clear) {
                if (v->io.read(v->io.context, key, c->bytes, v->block_size)) err = FFS_IO;
                else err = check_block(v, c->bytes, checksum >= 0);
                if (!err) keep_root_image(v, key, c->bytes);
            }
        }
        if (!c) return FFS_NOMEM;
        if (old) unindex_block(v, old);
        if (err) { free_block(v, c); return err; }
        c->rollback = old && old != c ? old : NULL;
        c->key = key; c->state = BLOCK_CHANGE;
        c->next = v->changes; v->changes = c;
        index_block(v, c);
    }
    if (clear) memset(c->bytes, 0, v->block_size);
    c->verified = 0; c->checksum = checksum; *result = c;
    return 0;
}
static int edit_block(struct ffs_volume *v, uint32_t key, unsigned char **bytes,
                      int checksum, unsigned clear)
{
    struct ffs_change *c;
    int err = acquire_change(v, key, &c, checksum, clear, 1);
    if (err) return err;
    *bytes = c->bytes;
    return 0;
}
/* Consume a sole read view when a scan finds the block to edit. A detached
 * read cell already owns its bytes; pending cells keep ordinary rollback. */
static int edit_view(struct ffs_volume *v, struct block_view *view,
                      unsigned char **bytes, int checksum)
{
    struct ffs_change *c = view->block;
    if (!c || c->readers != 1) return FFS_CORRUPT;
    c->readers = 0; view->block = NULL; view->bytes = NULL;
    if (c->state != BLOCK_READ) return edit_block(v, c->key, bytes, checksum, 0);
    c->state = BLOCK_CHANGE; c->checksum = checksum; c->verified = 0;
    c->next = v->changes; v->changes = c;
    *bytes = c->bytes;
    return 0;
}
/* Streaming writes acquire mutable metadata once. Operation-owned buffers
 * are already staged; borrowed pending buffers are already on writeback.
 * Preparation leaves borrowed bytes untouched until fallible work ends. */
static int write_edit(struct ffs_volume *v, uint32_t key,
                      struct ffs_change **result, int checksum, unsigned clear)
{
    struct ffs_change *c;
    PROFILE_START(v);
    if (clear) {
        c = cached_block(v, key);
        if (c && c->state != BLOCK_CLEAN) {
            PROFILE_END(v, FFS_P_EDIT); return FFS_CORRUPT;
        }
    }
    if (v->write_borrowed && !clear) {
        c = cached_block(v, key);
        if (c && c->readers) { PROFILE_END(v, FFS_P_EDIT); return FFS_CORRUPT; }
        if (c && c->state != BLOCK_CLEAN) {
            *result = c; PROFILE_END(v, FFS_P_EDIT); return 0;
        }
    }
#ifdef FFS_PROFILE
    {
        int err = acquire_change(v, key, result, checksum, clear, 1);
        PROFILE_END(v, FFS_P_EDIT);
        return err;
    }
#else
    return acquire_change(v, key, result, checksum, clear, 1);
#endif
}
static int header(struct ffs_volume *v, uint32_t key, unsigned char *p)
{
    int err;
    if (!valid_key(v, key)) return FFS_CORRUPT;
    err = read_block(v, key, p, 1);
    if (err) return err;
    if (word(p, 0) != 2 || word(p, 4) != (key == v->root ? 0 : key))
        return FFS_CORRUPT;
    return 0;
}
static int header_view(struct ffs_volume *v, uint32_t key, struct block_view *view)
{
    int err;
    view->block = NULL; view->bytes = NULL;
    if (!valid_key(v, key)) return FFS_CORRUPT;
    err = read_view(v, key, view, 1);
    if (!err && (word(view->bytes, 0) != 2 ||
                 word(view->bytes, 4) != (key == v->root ? 0 : key))) {
        release_view(v, view); err = FFS_CORRUPT;
    }
    return err;
}
static int decode_entry(struct ffs_volume *v, uint32_t key,
                         const unsigned char *p, struct ffs_entry *e)
{
    uint32_t size = v->block_size;
    unsigned n = p[size - 80], c = p[size - 184];
    int32_t type = (int32_t)word(p, size - 4);
    if (n == 0 || n > 30 || (key != v->root && c > 79)) return FFS_CORRUPT;
    if (key == v->root) {
        if (type != 1 || word(p, 8) || word(p, 16) ||
            word(p, 12) != v->buckets || word(p, size - 12))
            return FFS_CORRUPT;
    } else if (type != -3 && type != 2 && type != 3 && type != -4 && type != 4)
        return FFS_CORRUPT;
    if (!e) return 0;
    /* Every field is assigned; strings end at their recorded lengths. */
    e->key = key;
    e->type = type;
    e->parent = word(p, size - 12);
    e->hash_next = word(p, size - 16);
    e->days = word(p, size - 92);
    e->minutes = word(p, size - 88);
    e->ticks = word(p, size - 84);
    memcpy(e->name, p + size - 79, n); e->name[n] = 0;
    if (key != v->root) {
        e->size = word(p, size - 188);
        e->protection = word(p, size - 192);
        e->link = word(p, size - 44);
        e->backlink = word(p, size - 40);
        e->owner = word(p, size - 196);
        if (c) memcpy(e->comment, p + size - 183, c);
    } else {
        e->size = e->protection = e->link = e->backlink = e->owner = 0;
        c = 0;
    }
    e->comment[c] = 0;
    return 0;
}
int ffs_mount(struct ffs_volume *v, const struct ffs_io *io, uint32_t size,
              uint32_t blocks, uint32_t reserved, unsigned writable)
{
    unsigned char *p;
    struct ffs_entry root;
    int err;
    if (!v || !io || !io->read || !io->alloc || !io->free ||
        size < 512 || size > 32768 || (size & (size - 1)) ||
        reserved < 1 || reserved >= blocks || blocks - reserved < 2)
        return FFS_CORRUPT;
    memset(v, 0, sizeof(*v));
    v->write_requested = writable && io->write && io->flush;
    v->io = *io; v->block_size = size; v->blocks = blocks;
    v->file_limit = UINT32_MAX;
    while ((UINT32_C(1) << v->block_shift) < size) ++v->block_shift;
    v->reserved = reserved;
    v->root = reserved + (blocks - reserved - 1) / 2;
    v->buckets = size / 4 - 56;
    p = buffer(v);
    if (!p) return FFS_NOMEM;
    err = read_block(v, 0, p, 0);
    if (!err) {
        v->dos_type = word(p, 0);
        if (ffs_decode_type(v->dos_type, &v->format)) err = FFS_CORRUPT;
    }
    if (!err) err = header(v, v->root, p);
    if (!err) err = decode_entry(v, v->root, p, &root);
    if (!err) {
        v->created[0] = word(p, size - 28);
        v->created[1] = word(p, size - 24);
        v->created[2] = word(p, size - 20);
        v->mounted = 1;
        v->validated = word(p, size - 200) == UINT32_MAX;
        v->writable = v->write_requested && v->validated;
    }
    release(v, p);
    /* A set bitmap-valid flag is trusted. Only an invalid flag needs the
     * structural scan and repair. */
    return err;
}
int ffs_stat(struct ffs_volume *v, uint32_t key, struct ffs_entry *entry)
{
    struct block_view view;
    int err;
    if (!v || !v->mounted || !entry) return FFS_CORRUPT;
    err = header_view(v, key, &view);
    if (!err) err = decode_entry(v, key, view.bytes, entry);
    release_view(v, &view);
    return err;
}
static int directory(struct ffs_volume *v, uint32_t key, unsigned char *p)
{
    int err = header(v, key, p);
    int32_t type;
    if (err) return err;
    type = (int32_t)word(p, v->block_size - 4);
    return type == 1 || type == 2 ? 0 : FFS_WRONG_TYPE;
}
static int same_name(struct ffs_volume *v, const unsigned char *a,
                      const unsigned char *b, size_t length)
{
    size_t i;
    for (i = 0; i < length; ++i)
        if (ffs_upper(a[i], v->format.international) !=
            ffs_upper(b[i], v->format.international)) return 0;
    return 1;
}
int ffs_lookup(struct ffs_volume *v, uint32_t dir, const unsigned char *name,
               size_t length, struct ffs_entry *e)
{
    struct block_view view;
    uint16_t hash;
    uint32_t key, visited = 0;
    int err, match;
    if (ffs_name_hash(name, length, v->format.international,
                      (uint16_t)v->buckets, &hash)) return FFS_BAD_NAME;
    err = header_view(v, dir, &view);
    if (!err && word(view.bytes, v->block_size - 4) != 1 &&
        word(view.bytes, v->block_size - 4) != 2) err = FFS_WRONG_TYPE;
    key = err ? 0 : word(view.bytes, 24 + 4 * hash);
    release_view(v, &view);
    while (!err && key) {
        if (++visited > v->blocks) { err = FFS_CORRUPT; break; }
        err = header_view(v, key, &view);
        if (!err) {
            const unsigned char *p = view.bytes;
            match = p[v->block_size - 80] == length &&
                    same_name(v, name, p + v->block_size - 79, length);
            /* Validate every chain member, but decode only the match. */
            err = decode_entry(v, key, p, match ? e : NULL);
            if (!err && word(p, v->block_size - 12) != dir) err = FFS_CORRUPT;
            if (!match) key = word(p, v->block_size - 16);
        }
        release_view(v, &view);
        if (err || match) break;
    }
    return err ? err : key ? 0 : FFS_NOT_FOUND;
}
/* Lookups validate their directory, so the starting entry is decoded only
 * for a parent step or an empty path. */
static int resolve_entry(struct ffs_volume *v, uint32_t dir, const char *path,
                          struct ffs_entry *e, uint32_t *parent, const char **leaf)
{
    const char *s, *colon;
    unsigned hops = 0, loaded = 0;
    int err = 0;
    if (!path) return FFS_BAD_NAME;
    colon = strchr(path, ':');
    if (colon) { dir = v->root; path = colon + 1; }
    while (!err && *path) {
        if (*path == '/') {
            if (!loaded) err = ffs_stat(v, dir, e);
            if (err) break;
            if (!e->parent) return FFS_NOT_FOUND;
            err = ffs_stat(v, e->parent, e);
            dir = e->key; loaded = 1;
            ++path;
            continue;
        }
        s = path;
        while (*path && *path != '/') ++path;
        if (loaded && e->type != 1 && e->type != 2) return FFS_WRONG_TYPE;
        err = ffs_lookup(v, dir, (const unsigned char *)s,
                         (size_t)(path - s), e);
        if (err == FFS_NOT_FOUND && !*path && leaf) {
            *parent = dir; *leaf = s;
        }
        while (!err && (e->type == -4 || e->type == 4)) {
            if (++hops > 64) return FFS_CORRUPT;
            err = ffs_stat(v, e->link, e);
        }
        if (!err && e->type == 3) return FFS_SOFT_LINK;
        dir = e->key; loaded = 1;
        if (*path == '/') ++path;
    }
    if (!err && !loaded) err = ffs_stat(v, dir, e);
    return err;
}
int ffs_resolve(struct ffs_volume *v, uint32_t dir, const char *path,
                struct ffs_entry *e)
{
    return resolve_entry(v, dir, path, e, NULL, NULL);
}
static int cache_record_length(struct ffs_volume *, const unsigned char *, uint32_t, uint32_t *);
/* The iterator resumes at its saved record. Each record is validated when
 * it is returned; iterators are invalidated by directory mutations. */
static int next_cached(struct ffs_volume *v, uint32_t dir, struct ffs_iterator *it,
                       struct ffs_entry *e)
{
    struct block_view view;
    const unsigned char *p;
    uint32_t key = it->next, offset = it->bucket, index = it->index, length, n, c;
    int err;
    if (!offset) {
        err = header_view(v, dir, &view);
        if (!err && word(view.bytes, v->block_size - 4) != 1 &&
            word(view.bytes, v->block_size - 4) != 2) err = FFS_WRONG_TYPE;
        key = err ? 0 : word(view.bytes, v->block_size - 8);
        release_view(v, &view);
        if (err) return err;
        offset = 24; index = 0;
    }
    while (key) {
        if (!valid_key(v, key) || ++it->visited > (uint64_t)v->blocks * 2) return FFS_CORRUPT;
        err = read_view(v, key, &view, 1);
        if (err) return err;
        p = view.bytes;
        if (word(p, 0) != 33 || word(p, 4) != key || word(p, 8) != dir ||
            index > word(p, 12)) err = FFS_CORRUPT;
        else if (index < word(p, 12)) {
            err = cache_record_length(v, p, offset, &length);
            if (!err) {
                e->key = word(p, offset); e->parent = dir;
                e->size = word(p, offset + 4); e->protection = word(p, offset + 8);
                e->owner = word(p, offset + 12);
                e->days = ((uint32_t)p[offset + 16] << 8) | p[offset + 17];
                e->minutes = ((uint32_t)p[offset + 18] << 8) | p[offset + 19];
                e->ticks = ((uint32_t)p[offset + 20] << 8) | p[offset + 21];
                e->type = (int8_t)p[offset + 22];
                e->link = e->backlink = e->hash_next = 0;
                n = p[offset + 23]; c = p[offset + 24 + n];
                memcpy(e->name, p + offset + 24, n); e->name[n] = 0;
                if (c) memcpy(e->comment, p + offset + 25 + n, c);
                e->comment[c] = 0;
                if (!valid_key(v, e->key) || (e->type != -3 && e->type != 2 && e->type != 3 &&
                    e->type != -4 && e->type != 4)) err = FFS_CORRUPT;
            }
            release_view(v, &view);
            if (err) return err;
            it->next = key; it->bucket = offset + length; it->index = index + 1;
            /* Links retain their own name/comment and resolve live metadata. */
            if (e->type == -4 || e->type == 4) return ffs_stat(v, e->key, e);
            return 0;
        }
        key = err ? 0 : word(p, 16); offset = 24; index = 0;
        release_view(v, &view);
        if (err) return err;
    }
    it->next = 0; it->bucket = 24; it->index = 0;
    return FFS_NO_MORE;
}
static int next_entry(struct ffs_volume *v, uint32_t dir, struct ffs_iterator *it,
                      struct ffs_entry *e, unsigned use_cache)
{
    struct block_view view;
    uint32_t key = it->next;
    int err;
    if (use_cache && v->format.dircache && v->validated && !v->changes) {
        err = next_cached(v, dir, it, e);
        goto links;
    }
    if (!key) {
        err = header_view(v, dir, &view);
        if (!err && word(view.bytes, v->block_size - 4) != 1 &&
            word(view.bytes, v->block_size - 4) != 2) err = FFS_WRONG_TYPE;
        while (!err && !key && it->bucket < v->buckets)
            key = word(view.bytes, 24 + 4 * it->bucket++);
        release_view(v, &view);
        if (err) return err;
        if (!key) return FFS_NO_MORE;
    }
    if (++it->visited > v->blocks) return FFS_CORRUPT;
    err = ffs_stat(v, key, e);
    if (!err && e->parent != dir) err = FFS_CORRUPT;
    if (!err) it->next = e->hash_next;
links:
    if (!err && (e->type == -4 || e->type == 4)) {
        struct ffs_entry target;
        err = ffs_stat(v, e->link, &target);
        if (!err && target.type != (e->type == -4 ? -3 : 2)) err = FFS_CORRUPT;
        if (!err) {
            e->size = target.size; e->protection = target.protection; e->owner = target.owner;
            e->days = target.days; e->minutes = target.minutes; e->ticks = target.ticks;
        }
    }
    return err;
}
int ffs_next(struct ffs_volume *v, uint32_t dir, struct ffs_iterator *it,
             struct ffs_entry *e)
{
    return next_entry(v, dir, it, e, 1);
}
static int table_view(struct ffs_volume *v, uint32_t key, uint32_t file,
                      struct block_view *view)
{
    int err;
    release_view(v, view);
    if (!valid_key(v, key)) return FFS_CORRUPT;
    err = read_view(v, key, view, 1);
    if (!err && (word(view->bytes, 0) != 16 || word(view->bytes, 4) != key ||
        word(view->bytes, v->block_size - 12) != file ||
        word(view->bytes, v->block_size - 4) != UINT32_C(0xfffffffd)))
        err = FFS_CORRUPT;
    return err;
}
int ffs_read(struct ffs_volume *v, uint32_t key, uint32_t offset, void *data,
             uint32_t length, uint32_t *actual)
{
    struct block_view table, payload = {0};
    const unsigned char *p, *block;
    unsigned char *out = data, *transfer = NULL;
    uint32_t capacity, transfer_blocks = 0, size;
    uint32_t index, within, skip, extension, seen = 0, n, amount, data_key;
    unsigned extent;
    int err;
    *actual = 0;
    if (!v || !v->mounted) return FFS_CORRUPT;
    capacity = v->block_size - (v->format.fast ? 0 : 24);
    err = header_view(v, key, &table);
    p = table.bytes;
    /* Validate the header; a read needs only its type, protection and size. */
    if (!err) err = decode_entry(v, key, p, NULL);
    if (err) goto done;
    if (word(p, v->block_size - 4) != UINT32_C(0xfffffffd)) { err = FFS_WRONG_TYPE; goto done; }
    if (word(p, v->block_size - 192) & 8) { err = FFS_READ_PROTECTED; goto done; }
    size = word(p, v->block_size - 188);
    if (offset >= size || !length) goto done;
    if (!data) { err = FFS_BAD_NAME; goto done; }
    if (length > size - offset) length = size - offset;
    /* FFS blocks hold a power of two bytes; only OFS needs the division. */
    if (v->format.fast) { index = offset >> v->block_shift; within = offset & (capacity - 1); }
    else { index = offset / capacity; within = offset % capacity; }
    skip = 0; n = index;
    if (n >= v->buckets) { skip = index / v->buckets; n = index - skip * v->buckets; }
    if (v->read_file == key && v->read_index && v->read_index <= skip) {
        extension = v->read_table;
        err = table_view(v, extension, key, &table);
        p = table.bytes;
        seen = v->read_index; skip -= seen;
    }
    while (!err && skip--) {
        extension = word(p, v->block_size - 8);
        if (!valid_key(v, extension) || ++seen > v->blocks) {
            err = FFS_CORRUPT; break;
        }
        err = table_view(v, extension, key, &table);
        p = table.bytes;
    }
    if (!err) {
        v->read_file = key; v->read_index = seen;
        v->read_table = seen ? word(p, 4) : key;
    }
    while (!err && length) {
        if (n >= word(p, 8) || word(p, 8) > v->buckets) { err = FFS_CORRUPT; break; }
        data_key = word(p, 24 + (v->buckets - 1 - n) * 4);
        if (!valid_key(v, data_key)) { err = FFS_CORRUPT; break; }
        extent = v->io.read_extent && !v->changes && !v->pending && !within && length >= capacity;
        /* OFS blocks carry headers, so their runs go through a transfer
         * buffer of at most 16 KiB and are checked block by block. */
        if (extent && !v->format.fast && !transfer) {
            /* Without the buffer, blocks are read one at a time. */
            if (!transfer_blocks) {
                uint32_t full = length / capacity;
                transfer_blocks = 16384u >> v->block_shift;
                if (!transfer_blocks) transfer_blocks = 1;
                if (transfer_blocks > full) transfer_blocks = full;
                transfer = v->io.alloc(v->io.context, transfer_blocks << v->block_shift);
            }
            if (!transfer) extent = 0;
        }
        if (extent) {
            const unsigned char *slot = p + 24 + (v->buckets - 2 - n) * 4;
            uint32_t run = 1, limit, i, sum, next = data_key + 1;
            limit = v->format.fast ? length >> v->block_shift : length / capacity;
            if (limit > word(p, 8) - n) limit = word(p, 8) - n;
            if (limit > v->blocks - data_key) limit = v->blocks - data_key;
            if (!v->format.fast && limit > transfer_blocks) limit = transfer_blocks;
            /* Table slots run downward; count successive keys. */
            while (run < limit && word(slot, 0) == next) { ++run; ++next; slot -= 4; }
            if (v->format.fast) {
                amount = run << v->block_shift;
                if (v->io.read_extent(v->io.context, data_key, out, amount)) {
                    err = FFS_IO; break;
                }
            } else {
                amount = run * capacity;
                if (v->io.read_extent(v->io.context, data_key, transfer, run << v->block_shift)) {
                    err = FFS_IO; break;
                }
                for (i = 0, block = transfer; !err && i < run; ++i, block += v->block_size) {
                    if (ffs_block_sum(block, v->block_size, &sum) || sum ||
                        word(block, 0) != 8 || word(block, 4) != key ||
                        word(block, 8) != index + i + 1 || word(block, 12) != capacity)
                        err = FFS_CORRUPT;
                    else memcpy(out + i * capacity, block + 24, capacity);
                }
                if (err) break;
            }
            index += run; n += run;
        } else {
            err = read_view(v, data_key, &payload, !v->format.fast);
            if (err) break;
            block = payload.bytes;
            amount = capacity - within;
            if (amount > length) amount = length;
            if (!v->format.fast && (word(block, 0) != 8 || word(block, 4) != key ||
                word(block, 8) != index + 1 || word(block, 12) > capacity ||
                word(block, 12) < within + amount)) {
                err = FFS_CORRUPT; break;
            }
            memcpy(out, block + (v->format.fast ? 0 : 24) + within, amount);
            release_view(v, &payload);
            ++index; ++n; within = 0;
        }
        out += amount; offset += amount; length -= amount; *actual += amount;
        if (length && n == v->buckets) {
            extension = word(p, v->block_size - 8);
            if (!valid_key(v, extension) || ++seen > v->blocks) { err = FFS_CORRUPT; break; }
            err = table_view(v, extension, key, &table);
            p = table.bytes;
            if (!err) { v->read_file = key; v->read_table = extension; v->read_index = seen; }
            n = 0;
        }
    }
done:
    release_view(v, &payload); release_view(v, &table);
    if (transfer) v->io.free(v->io.context, transfer, transfer_blocks << v->block_shift);
    return err;
}

static int stage_impl(struct ffs_volume *v, uint32_t key, unsigned char *p, int checksum)
{
    struct ffs_change *c;
    int err;
    if (!valid_key(v, key)) return FFS_CORRUPT;
    err = acquire_change(v, key, &c, checksum, 0, 0);
    if (err) return err;
    if (c->bytes != p) memcpy(c->bytes, p, v->block_size);
    return 0;
}
static int stage(struct ffs_volume *v, uint32_t key, unsigned char *p, int checksum)
{
    int result;
    PROFILE_START(v);
    result = stage_impl(v, key, p, checksum);
    PROFILE_END(v, FFS_P_STAGE);
    return result;
}

/* Bytes passed here stay unchanged until the following flush. */
static int write_owned(struct ffs_volume *v, uint32_t key, const unsigned char *p, uint32_t size)
{
    if (v->io.write_owned) {
        v->owned_pending = 1;
        return v->io.write_owned(v->io.context, key, p, size);
    }
    return size == v->block_size ? v->io.write(v->io.context, key, p, size) :
           v->io.write_extent(v->io.context, key, p, size);
}
static int flush_device(struct ffs_volume *v)
{
    v->owned_pending = 0;
    return v->io.flush(v->io.context);
}
/* After a failure, complete owned transfers before their bytes are freed. */
static void drain_owned(struct ffs_volume *v)
{
    if (v->owned_pending) (void)flush_device(v);
}
/* Retire freed overlay blocks only after the mutation succeeds. The first
 * record is part of the volume, so ordinary frees allocate nothing. */
static int record_release(struct ffs_volume *v, uint32_t key)
{
    struct ffs_released *r = v->released;
    if (!r || r->count == 32) {
        r = r ? v->io.alloc(v->io.context, sizeof(*r)) : &v->released_inline;
        if (!r) return FFS_NOMEM;
        r->count = 0; r->next = v->released; v->released = r;
    }
    r->keys[r->count++] = key;
    return 0;
}
static void cancel_release(struct ffs_volume *v, uint32_t key)
{
    struct ffs_released *r;
    unsigned i;
    for (r = v->released; r; r = r->next)
        for (i = 0; i < r->count; ++i)
            if (r->keys[i] == key) r->keys[i] = 0;
}
static void release_records(struct ffs_volume *v, unsigned discard_blocks)
{
    struct ffs_released *r;
    struct ffs_change **p, *c;
    unsigned i;
    while ((r = v->released)) {
        for (i = 0; discard_blocks && i < r->count; ++i) {
            uint32_t key = r->keys[i];
            /* Every cell of a block is indexed; owned cells replace theirs. */
            if (!key || !(c = cached_block(v, key))) continue;
            if (c->state == BLOCK_CHANGE) {
                for (p = &v->changes; *p && *p != c; p = &(*p)->next) {}
                if (*p) *p = c->next;
                if (c->rollback) {
                    unlink_pending(v, c->rollback); free_block(v, c->rollback);
                }
            } else if (c->state == BLOCK_CLEAN) unlink_clean(v, c);
            else unlink_pending(v, c);
            unindex_block(v, c); free_block(v, c);
        }
        v->released = r->next;
        if (r != &v->released_inline) v->io.free(v->io.context, r, sizeof(*r));
    }
}
/* An operation's bitmap changes count once it is published. */
static void publish_free_change(struct ffs_volume *v)
{
    v->free_count += v->free_change; v->free_change = 0;
}
static void abort_changes(struct ffs_volume *v)
{
    struct ffs_change *c;
    v->write_borrowed = 0; v->free_change = 0;
    release_records(v, 0);
    if (v->direct && v->direct != v->direct_inline) v->io.free(v->io.context, v->direct, v->direct_capacity * sizeof(*v->direct));
    v->direct = NULL; v->direct_count = v->direct_capacity = 0;
    while ((c = v->changes)) {
        v->changes = c->next;
        unindex_block(v, c);
        if (c->rollback) index_block(v, c->rollback);
        free_block(v, c);
    }
    if (!v->pending) drop_root_image(v);
}
static void refresh_date(struct ffs_volume *v)
{
    if (v->io.date) v->io.date(v->io.context, v->now);
}
static int begin_write(struct ffs_volume *v)
{
    v->read_file = 0;
    if (!v->mounted || v->failed || !v->validated) return FFS_CORRUPT;
    if (!v->writable) return FFS_WRITE_PROTECTED;
    if (v->changes) return FFS_CORRUPT;
    if (v->pending_limit && v->pending_count >= v->pending_limit)
        return ffs_flush(v);
    return 0;
}
/* Operations that stamp metadata take the date once, before mutating. */
static int begin_change(struct ffs_volume *v)
{
    refresh_date(v);
    return begin_write(v);
}
static void sort_changes(struct ffs_volume *v)
{
    struct ffs_change *sorted = NULL, *c, **p;
    while ((c = v->changes)) {
        v->changes = c->next;
        for (p = &sorted; *p && (*p)->key < c->key; p = &(*p)->next) {}
        c->next = *p; *p = c;
    }
    v->changes = sorted;
}
/* Contiguous runs are gathered into one owned batch, which the caller
 * frees after the following flush. */
static int write_changes(struct ffs_volume *v, unsigned char **gathered, uint32_t *gathered_size)
{
    struct ffs_change *c, *last, *part;
    uint32_t count, total = 0, limit = 16384u >> v->block_shift;
    unsigned char *batch = NULL, *out;
    int err = 0;
    if (v->io.write_extent && limit > 1) {
        {
            PROFILE_START(v);
            sort_changes(v);
            PROFILE_END(v, FFS_P_SORT);
        }
        PROFILE_START(v);
        /* Gather only real runs, and never include the validity root. */
        for (c = v->changes; c; c = last->next) {
            last = c; count = 1;
            if (c->key == v->root) continue;
            while (count < limit && last->next && last->next->key != v->root &&
                   last->next->key == last->key + 1) { last = last->next; ++count; }
            if (count > 1) total += count;
        }
        if (total) batch = v->io.alloc(v->io.context, total << v->block_shift);
        PROFILE_END(v, FFS_P_GATHER);
    }
    {
        PROFILE_START(v);
        for (c = v->changes; c; c = c->next) {
            /* The validity root is finalized separately after the barrier. */
            if (c->key == v->root) continue;
            if (c->checksum >= 0)
                (void)ffs_block_checksum(c->bytes, v->block_size, (size_t)c->checksum);
            c->verified = c->checksum >= 0;
        }
        PROFILE_END(v, FFS_P_CHECKSUM);
    }
    out = batch;
    for (c = v->changes; !err && c; c = last->next) {
        last = c;
        if (c->key == v->root) continue;
        count = 1;
        if (batch) {
            while (count < limit && last->next && last->next->key != v->root &&
                   last->next->key == last->key + 1) {
                last = last->next;
                ++count;
            }
        }
        if (count > 1) {
            unsigned char *run = out;
            PROFILE_START(v);
            for (part = c; part != last->next; part = part->next) {
                memcpy(out, part->bytes, v->block_size); out += v->block_size;
            }
            PROFILE_END(v, FFS_P_GATHER);
            err = write_owned(v, c->key, run, count << v->block_shift);
        }
        else err = write_owned(v, c->key, c->bytes, v->block_size);
    }
    *gathered = batch; *gathered_size = total << v->block_shift;
    return err ? FFS_IO : 0;
}
/* The OLD root bytes: a clean cached root, or the image kept when memory
 * replaced it. Otherwise the caller supplies scratch for a device read. */
static unsigned char *disk_root(struct ffs_volume *v)
{
    struct ffs_change *c = cached_block(v, v->root);
    if (v->root_image) return v->root_image;
    return c && c->state == BLOCK_CLEAN && c->verified ? c->bytes : NULL;
}
/* Clear validity on the OLD root before any metadata or direct data is
 * replaced; the following flush makes it durable first. A clean cached
 * root is edited in place and stays coherent. */
static int submit_invalid_root(struct ffs_volume *v, unsigned char *scratch)
{
    unsigned char *p = disk_root(v);
    if (!p && v->io.read(v->io.context, v->root, p = scratch, v->block_size)) return FFS_IO;
    put_word(p + v->block_size - 200, 0);
    (void)ffs_block_checksum(p, v->block_size, 20);
    if (write_owned(v, v->root, p, v->block_size)) {
        drain_owned(v); return FFS_IO;
    }
    return 0;
}
static int invalidate_root(struct ffs_volume *v, unsigned char *scratch)
{
    if (submit_invalid_root(v, scratch) || flush_device(v)) return FFS_IO;
    drop_root_image(v);
    return 0;
}
static int commit_changes_impl(struct ffs_volume *v, int err, unsigned char *root)
{
    struct ffs_change *c;
    unsigned char *published = NULL, *gathered = NULL;
    uint32_t gathered_size = 0;
    if (err || !v->changes) { release(v, root); abort_changes(v); return err; }
    release_records(v, 1);
    if (!root) root = buffer(v);
    if (!root) { abort_changes(v); return FFS_NOMEM; }
    if (!v->disk_dirty) err = invalidate_root(v, root);
    if (!err) err = write_changes(v, &gathered, &gathered_size);
    if (!err) {
        PROFILE_START(v);
        /* The commit owns cached metadata until cleanup. Use its final root
         * directly; scratch still holds the old root for invalidation. */
        c = cached_block(v, v->root);
        published = c ? c->bytes : root;
        if (!c) err = read_block(v, v->root, root, 0);
        PROFILE_END(v, FFS_P_ROOT);
    }
    /* The old root remains invalid on disk. Synchronize the other metadata
     * before publishing the updated valid root; writing an intermediate
     * invalid copy of that root adds no ordering guarantee. */
    if (!err && flush_device(v)) err = FFS_IO;
    if (!err) {
        PROFILE_START(v);
        put_word(published + v->block_size - 40, v->now[0]);
        put_word(published + v->block_size - 36, v->now[1]);
        put_word(published + v->block_size - 32, v->now[2]);
        put_word(published + v->block_size - 200, UINT32_MAX);
        (void)ffs_block_checksum(published, v->block_size, 20);
        PROFILE_END(v, FFS_P_ROOT);
        if (write_owned(v, v->root, published, v->block_size) ||
            flush_device(v)) err = FFS_IO;
    }
    {
        PROFILE_START(v);
        /* Owned writes may still be in flight after a failure. */
        if (err) { drain_owned(v); v->failed = 1; v->free_known = 0; drop_clean(v); }
        else {
            v->disk_dirty = 0;
            publish_free_change(v);
            c = cached_block(v, v->root);
            if (c) {
                if (c->bytes != published) memcpy(c->bytes, published, v->block_size);
                c->verified = 1;
            }
            while ((c = v->changes)) {
                v->changes = c->next;
                if (c->rollback) {
                    unlink_pending(v, c->rollback); free_block(v, c->rollback);
                }
                retain_clean(v, c);
            }
        }
        release(v, root); abort_changes(v);
        /* Gathered runs were borrowed by owned writes until the flushes. */
        if (gathered) v->io.free(v->io.context, gathered, gathered_size);
        PROFILE_END(v, FFS_P_CLEANUP);
    }
    return err;
}
static int commit_changes(struct ffs_volume *v, int err, unsigned char *root)
{
    int result;
    PROFILE_START(v);
    result = commit_changes_impl(v, err, root);
    PROFILE_END(v, FFS_P_COMMIT);
    return result;
}

/* All allocations and metadata preflight finish before borrowed caller data
 * is written. The caller buffer is never retained after the packet reply. */
static int write_ofs_run(struct ffs_volume *v, const struct ffs_data_run *run, uint32_t file,
                         unsigned char *transfer, uint32_t transfer_size)
{
    uint32_t cap = v->block_size - 24, limit = transfer_size >> v->block_shift, done, i, n;
    const unsigned char *src = run->data;
    unsigned char *p;
    for (done = 0; done < run->blocks; done += n) {
        n = run->blocks - done; if (n > limit) n = limit;
        for (i = 0, p = transfer; i < n; ++i, p += v->block_size, src += cap) {
            uint32_t at = done + i;
            put_word(p, 8); put_word(p + 4, file); put_word(p + 8, run->index + at + 1);
            put_word(p + 12, cap);
            put_word(p + 16, at + 1 < run->blocks ? run->key + at + 1 : run->next);
            put_word(p + 20, 0);
            memcpy(p + 24, src, cap);
            (void)ffs_block_checksum(p, v->block_size, 20);
        }
        if (v->io.write_extent(v->io.context, run->key + done, transfer, n << v->block_shift))
            return FFS_IO;
    }
    return 0;
}
static int write_direct(struct ffs_volume *v, unsigned char *root, uint32_t file,
                        unsigned char *transfer, uint32_t transfer_size)
{
    uint32_t i, count;
    struct ffs_change *c, *next;
    unsigned invalidating = !v->disk_dirty;
    if (!v->direct_count) return 0;
    for (i = 0; i < v->direct_count; ++i)
        if (v->root - v->direct[i].key < v->direct[i].blocks) return FFS_CORRUPT;
    /* The root write overlaps the cache scan below; the flush completes it
     * before any data is transferred. */
    if (invalidating) {
        v->disk_dirty = 1;
        if (submit_invalid_root(v, root)) return FFS_IO;
    }
    for (i = 0; i < v->direct_count; ++i) {
        struct ffs_data_run *run = &v->direct[i];
        /* Direct transfers supersede both clean and buffered old data. */
        {
            PROFILE_START(v);
            /* Visit each affected hash bucket once, even for long extents.
             * Hash collisions outside the transferred range remain cached. */
            uint32_t first = run->key, blocks = run->blocks;
            struct ffs_change **slot = &v->cache_index[first % 64];
            for (count = blocks < 64 ? blocks : 64; count; --count) {
                c = *slot++;
                if (slot == v->cache_index + 64) slot = v->cache_index;
                while (c) {
                    next = c->hash_next;
                    if (c->key - first < blocks) {
                        /* A damaged mapping must not free metadata held by the plan. */
                        if ((c->state == BLOCK_CHANGE || c->state == BLOCK_PENDING) &&
                            c->checksum >= 0) {
                            PROFILE_END(v, FFS_P_INVALIDATE); drain_owned(v); return FFS_CORRUPT;
                        }
                        if (c->state != BLOCK_CHANGE) {
                            if (c->state == BLOCK_CLEAN) unlink_clean(v, c);
                            else unlink_pending(v, c);
                            unindex_block(v, c); free_block(v, c);
                        }
                    }
                    c = next;
                }
            }
            PROFILE_END(v, FFS_P_INVALIDATE);
        }
    }
    if (invalidating) {
        if (flush_device(v)) return FFS_IO;
        drop_root_image(v);
    }
    for (i = 0; i < v->direct_count; ++i) {
        struct ffs_data_run *run = &v->direct[i];
        if (!v->format.fast) {
            if (write_ofs_run(v, run, file, transfer, transfer_size)) return FFS_IO;
        } else if (v->io.write_extent(v->io.context, run->key, run->data, run->size)) return FFS_IO;
    }
    return 0;
}
/* Keep the current operation separate until it succeeds, so allocation
 * failures cannot destroy previously acknowledged, buffered operations. */
static int finish_change(struct ffs_volume *v, int err)
{
    struct ffs_change *c;
    /* In-place close updates move no table or cache record, so cursors and
     * iterators remain valid. */
    if (!err && (v->changes || v->write_borrowed) && !v->in_place) ++v->generation;
    v->in_place = 0;
    if (!v->deferred) return commit_changes(v, err, NULL);
    if (err || (!v->changes && !v->write_borrowed)) { abort_changes(v); return err; }
    /* Streaming writes have already submitted data and applied metadata.
     * Publishing their owned cells must perform no fallible work. */
    v->write_borrowed = 0;
    publish_free_change(v);
    release_records(v, 1);
    if (v->direct && v->direct != v->direct_inline) v->io.free(v->io.context, v->direct, v->direct_capacity * sizeof(*v->direct));
    v->direct = NULL; v->direct_count = v->direct_capacity = 0;
    while ((c = v->changes)) {
        v->changes = c->next;
        if (c->rollback) {
            unlink_pending(v, c->rollback); free_block(v, c->rollback);
            c->rollback = NULL;
        }
        ++v->pending_count; c->state = BLOCK_PENDING;
        c->previous = NULL; c->next = v->pending;
        if (c->next) c->next->previous = c;
        v->pending = c;
    }
    return 0;
}
static int finish_file(struct ffs_volume *, uint32_t);
int ffs_flush(struct ffs_volume *v)
{
    unsigned char *root;
    struct ffs_change *c;
    int err;
    if (v->failed || v->changes) return FFS_IO;
    if (!v->pending) return 0;
    refresh_date(v);
    for (;;) {
        for (c = v->pending; c && !c->close_update; c = c->next) {}
        if (!c) break;
        err = finish_file(v, c->key);
        if (err) return err;
    }
    /* Allocate before detaching acknowledged operations: low memory must
     * leave them available for reads and a later flush retry. */
    root = buffer(v);
    if (!root) return FFS_NOMEM;
    v->changes = v->pending; v->pending = NULL; v->pending_count = 0;
    for (c = v->changes; c; c = c->next) c->state = BLOCK_CHANGE;
    err = commit_changes(v, 0, root);
    if (err) v->failed = 1;
    return err;
}
void ffs_discard(struct ffs_volume *v)
{
    /* Direct data may already be on disk without its allocation metadata. */
    if (v->disk_dirty) v->failed = 1;
    abort_changes(v);
    v->changes = v->pending; v->pending = NULL; v->pending_count = 0;
    v->read_file = 0; v->bitmap_block = v->bitmap_ext_key = 0; v->free_known = 0;
    abort_changes(v);
    drop_clean(v);
}
/* Pages beyond the root's 25 are listed in a chain of extension blocks.
 * A cursor keeps the last extension found, so ascending lookups never
 * rewalk the chain from the root. */
static int bitmap_key(struct ffs_volume *v, uint32_t page, uint32_t *key)
{
    struct block_view view;
    uint32_t slots = v->block_size / 4 - 1, index, next, seen;
    int err;
    if (v->bitmap_block && v->bitmap_page == page) {
        *key = v->bitmap_block; return 0;
    }
    err = header_view(v, v->root, &view);
    if (err) return err;
    if (page < 25) {
        *key = word(view.bytes, v->block_size - 196 + page * 4);
        release_view(v, &view);
    } else {
        index = (page - 25) / slots;
        if (v->bitmap_ext_key && v->bitmap_ext_index <= index) {
            next = v->bitmap_ext_key; seen = v->bitmap_ext_index;
        } else { next = word(view.bytes, v->block_size - 96); seen = 0; }
        release_view(v, &view);
        for (;;) {
            if (!valid_key(v, next)) return FFS_CORRUPT;
            err = read_view(v, next, &view, 0);
            if (err) return err;
            if (seen == index) {
                *key = word(view.bytes, (page - 25) % slots * 4);
                v->bitmap_ext_key = next; v->bitmap_ext_index = index;
                release_view(v, &view);
                break;
            }
            next = word(view.bytes, v->block_size - 4);
            release_view(v, &view); ++seen;
        }
    }
    if (!valid_key(v, *key) || *key == v->root) return FFS_CORRUPT;
    v->bitmap_page = page; v->bitmap_block = *key;
    v->bitmap_base = v->reserved + page * ((v->block_size - 4) * 8);
    return 0;
}
static int bitmap_change(struct ffs_volume *v, uint32_t key, unsigned make_free)
{
    uint32_t page, mapkey = 0, bit, value, offset;
    unsigned char *p;
    int err;
    if (!valid_key(v, key) || key == v->root) return FFS_CORRUPT;
    bit = key - v->reserved;
    page = bit / ((v->block_size - 4) * 8);
    bit %= (v->block_size - 4) * 8;
    err = bitmap_key(v, page, &mapkey);
    if (err) return err;
    if (key == mapkey) return FFS_CORRUPT;
    err = edit_block(v, mapkey, &p, 0, 0);
    offset = 4 + bit / 32 * 4;
    if (!err) {
        value = word(p, offset);
        if (!!(value & (UINT32_C(1) << (bit % 32))) == !!make_free) err = FFS_CORRUPT;
        else {
            value ^= UINT32_C(1) << (bit % 32);
            put_word(p + offset, value);
            v->free_change += make_free ? 1 : UINT32_MAX;
        }
    }
    if (!err && make_free) err = record_release(v, key);
    return err;
}
static int allocate_block_impl(struct ffs_volume *v, uint32_t *key)
{
    struct block_view view = {NULL, NULL};
    unsigned char *p;
    uint32_t bits = (v->block_size - 4) * 8, page, mapkey = 0;
    uint32_t start = valid_key(v, v->alloc_hint) ? v->alloc_hint : v->reserved;
    uint32_t candidate, limit, bit, loaded = UINT32_MAX;
    unsigned pass;
    int err = 0;
    for (pass = 0; !err && pass < 2; ++pass) {
        candidate = pass ? v->reserved : start;
        limit = pass ? start : v->blocks;
        for (; !err && candidate < limit; ++candidate) {
            bit = candidate - v->reserved; page = bit / bits; bit %= bits;
            if (loaded != page) {
                release_view(v, &view);
                err = bitmap_key(v, page, &mapkey);
                if (!err) err = read_view(v, mapkey, &view, 1);
                loaded = page;
            }
            if (!err && (word(view.bytes, 4 + bit / 32 * 4) & (UINT32_C(1) << (bit % 32)))) {
                if (candidate == mapkey || candidate == v->root) err = FFS_CORRUPT;
                else err = edit_view(v, &view, &p, 0);
                if (!err) put_word(p + 4 + bit / 32 * 4,
                    word(p, 4 + bit / 32 * 4) & ~(UINT32_C(1) << (bit % 32)));
                if (!err) {
                    cancel_release(v, candidate); *key = candidate; v->alloc_hint = candidate + 1;
                    --v->free_change;
                }
                release_view(v, &view); return err;
            }
        }
    }
    release_view(v, &view);
    return err ? err : FFS_FULL;
}
static int allocate_block(struct ffs_volume *v, uint32_t *key)
{
    int result;
    PROFILE_START(v);
    result = allocate_block_impl(v, key);
    PROFILE_END(v, FFS_P_ALLOCATE);
    return result;
}


static uint32_t bit_count(uint32_t x)
{
    x -= (x >> 1) & 0x55555555u;
    x = (x & 0x33333333u) + ((x >> 2) & 0x33333333u);
    x = (x + (x >> 4)) & 0x0f0f0f0fu;
    x += x >> 8; x += x >> 16;
    return x & 0x3f;
}
/* The count is taken from the bitmap once, then follows each operation's
 * allocations and frees as it is published. */
int ffs_free_blocks(struct ffs_volume *v, uint32_t *count)
{
    struct block_view view;
    uint32_t bits = (v->block_size - 4) * 8, page, left, n, i, free_blocks = 0, key = 0;
    int err = 0;
    if (!v->free_known) {
        left = v->blocks - v->reserved;
        for (page = 0; !err && left; ++page, left -= n) {
            n = left < bits ? left : bits;
            err = bitmap_key(v, page, &key);
            if (!err) err = read_view(v, key, &view, 1);
            for (i = 0; !err && i < n; i += 32) {
                uint32_t value = word(view.bytes, 4 + i / 8);
                if (n - i < 32) value &= (UINT32_C(1) << (n - i)) - 1;
                free_blocks += bit_count(value);
            }
            release_view(v, &view);
        }
        if (err) return err;
        v->free_count = free_blocks; v->free_known = 1;
    }
    *count = v->free_count;
    return 0;
}
static void stamp(struct ffs_volume *v, unsigned char *p)
{
    put_word(p + v->block_size - 92, v->now[0]);
    put_word(p + v->block_size - 88, v->now[1]);
    put_word(p + v->block_size - 84, v->now[2]);
}
static int component(const char *name)
{
    size_t n = strlen(name), i;
    if (!n || n > 30) return FFS_BAD_NAME;
    for (i = 0; i < n; ++i) {
        unsigned ch = (unsigned char)name[i] & 0x7f;
        if (ch < 32 || ch == '/' || ch == ':') return FFS_BAD_NAME;
    }
    return 0;
}
static int directory_cache(struct ffs_volume *v, uint32_t dir)
{
    unsigned char *p, *cache;
    struct ffs_iterator it = {0, 0, 0, 0};
    struct ffs_entry entry;
    uint32_t old, next, seen = 0, first = 0, current = 0, offset = 24, entries = 0;
    size_t n, c, length;
    int err;
    if (!v->format.dircache) return 0;
    /* Rebuilding moves records: positions saved by cursors are stale. */
    v->in_place = 0;
    p = buffer(v); cache = buffer(v);
    if (!p || !cache) { release(v, p); release(v, cache); return FFS_NOMEM; }
    err = directory(v, dir, p);
    old = err ? 0 : word(p, v->block_size - 8);
    while (!err && old) {
        if (++seen > v->blocks || !valid_key(v, old)) { err = FFS_CORRUPT; break; }
        err = read_block(v, old, cache, 1);
        if (err) break;
        if (word(cache, 0) != 33 || word(cache, 4) != old || word(cache, 8) != dir) {
            err = FFS_CORRUPT; break;
        }
        next = word(cache, 16);
        err = bitmap_change(v, old, 1); old = next;
    }
    if (!err) err = allocate_block(v, &first);
    current = first;
    memset(cache, 0, v->block_size);
    put_word(cache, 33); put_word(cache + 4, current); put_word(cache + 8, dir);
    while (!err && !(err = next_entry(v, dir, &it, &entry, 0))) {
        n = strlen((char *)entry.name); c = strlen((char *)entry.comment);
        length = (25 + n + c + 1) & ~(size_t)1;
        if (offset + length > v->block_size) {
            err = allocate_block(v, &next);
            if (err) break;
            put_word(cache + 12, entries); put_word(cache + 16, next);
            err = stage(v, current, cache, 20);
            if (err) break;
            current = next; offset = 24; entries = 0;
            memset(cache, 0, v->block_size);
            put_word(cache, 33); put_word(cache + 4, current); put_word(cache + 8, dir);
        }
        put_word(cache + offset, entry.key);
        put_word(cache + offset + 4, entry.size);
        put_word(cache + offset + 8, entry.protection);
        put_word(cache + offset + 12, entry.owner);
        cache[offset + 16] = (unsigned char)(entry.days >> 8);
        cache[offset + 17] = (unsigned char)entry.days;
        cache[offset + 18] = (unsigned char)(entry.minutes >> 8);
        cache[offset + 19] = (unsigned char)entry.minutes;
        cache[offset + 20] = (unsigned char)(entry.ticks >> 8);
        cache[offset + 21] = (unsigned char)entry.ticks;
        cache[offset + 22] = (unsigned char)entry.type;
        cache[offset + 23] = (unsigned char)n;
        memcpy(cache + offset + 24, entry.name, n);
        cache[offset + 24 + n] = (unsigned char)c;
        memcpy(cache + offset + 25 + n, entry.comment, c);
        offset += (uint32_t)length; ++entries;
    }
    if (err == FFS_NO_MORE) err = 0;
    if (!err) {
        put_word(cache + 12, entries);
        err = stage(v, current, cache, 20);
    }
    if (!err) {
        put_word(p + v->block_size - 8, first);
        stamp(v, p); err = stage(v, dir, p, 20);
    }
    release(v, p); release(v, cache); return err;
}
static int cache_record_length(struct ffs_volume *v, const unsigned char *p,
                                uint32_t offset, uint32_t *length)
{
    uint32_t n, c;
    if (offset > v->block_size - 25) return FFS_CORRUPT;
    n = p[offset + 23];
    if (!n || n > 30 || n > v->block_size - offset - 25) return FFS_CORRUPT;
    c = p[offset + 24 + n];
    *length = (25 + n + c + 1) & ~1u;
    return c > 79 || *length > v->block_size - offset ? FFS_CORRUPT : 0;
}
/* Ordinary insert/delete and fixed-size updates touch only one list block.
 * Structural renames and comment growth retain the rebuilding fallback. */
static int change_cache_position(struct ffs_volume *v, uint32_t dir, uint32_t key, int mode,
                                 struct ffs_file_state *state)
{
    struct ffs_entry e, target;
    struct block_view view = {NULL, NULL};
    const unsigned char *scan;
    unsigned char record[134], *p;
    uint32_t current, seen = 0, offset, length, entries, i, found, found_length;
    uint32_t used = 0, record_length = 0, n, c;
    int err;
    if (!v->format.dircache) return 0;
    if (mode >= 0) {
        err = ffs_stat(v, key, &e);
        if (err) return err;
        if (e.type == -4 || e.type == 4) {
            err = ffs_stat(v, e.link, &target);
            if (err) return err;
            e.size = target.size; e.protection = target.protection; e.owner = target.owner;
            e.days = target.days; e.minutes = target.minutes; e.ticks = target.ticks;
        }
        n = (uint32_t)strlen((char *)e.name); c = (uint32_t)strlen((char *)e.comment);
        record_length = (25 + n + c + 1) & ~1u;
        memset(record, 0, sizeof(record));
        put_word(record, key); put_word(record + 4, e.size);
        put_word(record + 8, e.protection); put_word(record + 12, e.owner);
        record[16] = (unsigned char)(e.days >> 8); record[17] = (unsigned char)e.days;
        record[18] = (unsigned char)(e.minutes >> 8); record[19] = (unsigned char)e.minutes;
        record[20] = (unsigned char)(e.ticks >> 8); record[21] = (unsigned char)e.ticks;
        record[22] = (unsigned char)e.type; record[23] = (unsigned char)n;
        memcpy(record + 24, e.name, n); record[24 + n] = (unsigned char)c;
        memcpy(record + 25 + n, e.comment, c);
    }
    err = header_view(v, dir, &view);
    if (!err && word(view.bytes, v->block_size - 4) != 1 &&
        word(view.bytes, v->block_size - 4) != 2) err = FFS_WRONG_TYPE;
    current = err ? 0 : word(view.bytes, v->block_size - 8);
    release_view(v, &view);
    while (!err && current) {
        if (!valid_key(v, current) || ++seen > v->blocks) { err = FFS_CORRUPT; break; }
        err = read_view(v, current, &view, 1);
        if (err) break;
        scan = view.bytes;
        if (word(scan, 0) != 33 || word(scan, 4) != current || word(scan, 8) != dir) {
            err = FFS_CORRUPT; break;
        }
        entries = word(scan, 12); offset = 24; found = found_length = 0;
        for (i = 0; !err && i < entries; ++i) {
            err = cache_record_length(v, scan, offset, &length);
            if (!err) {
                if (word(scan, offset) == key) { found = offset; found_length = length; }
                offset += length;
            }
        }
        if (err) break;
        used = offset;
        if (found) {
            if (mode >= 0 && record_length != found_length) break;
            err = edit_view(v, &view, &p, 20);
            if (err) break;
            if (mode < 0) {
                /* Records are even-aligned and longer than a word. */
                for (i = found; i + 4 <= used - found_length; i += 4)
                    put_word(p + i, word(p, i + found_length));
                for (; i < used - found_length; ++i) p[i] = p[i + found_length];
                memset(p + used - found_length, 0, found_length);
                put_word(p + 12, entries - 1);
            } else memcpy(p + found, record, record_length);
            if (state) { state->cache_block = current; state->cache_offset = found; }
            return 0;
        }
        if (mode > 0 && record_length <= v->block_size - used) {
            err = edit_view(v, &view, &p, 20);
            if (err) break;
            memcpy(p + used, record, record_length);
            put_word(p + 12, entries + 1);
            return 0;
        }
        current = word(scan, 16);
        release_view(v, &view);
    }
    release_view(v, &view);
    return err ? err : directory_cache(v, dir);
}
static int change_cache(struct ffs_volume *v, uint32_t dir, uint32_t key, int mode)
{
    return change_cache_position(v, dir, key, mode, NULL);
}
static int insert_entry(struct ffs_volume *v, uint32_t dir, uint32_t key, unsigned char *p)
{
    unsigned char *parent;
    uint16_t hash = 0;
    int err = edit_block(v, dir, &parent, 20, 0);
    if (!err && (word(parent, 0) != 2 || word(parent, 4) != (dir == v->root ? 0 : dir)))
        err = FFS_CORRUPT;
    if (!err && word(parent, v->block_size - 4) != 1 &&
        word(parent, v->block_size - 4) != 2) err = FFS_WRONG_TYPE;
    if (!err) {
        (void)ffs_name_hash(p + v->block_size - 79, p[v->block_size - 80],
                           v->format.international, (uint16_t)v->buckets, &hash);
        put_word(p + v->block_size - 16, word(parent, 24 + hash * 4));
        put_word(p + v->block_size - 12, dir);
        err = stage(v, key, p, 20);
        put_word(parent + 24 + hash * 4, key);
        stamp(v, parent);
    }
    return err;
}
static int create_entry(struct ffs_volume *v, uint32_t dir, const char *name,
                         int32_t type, const char *target, uint32_t link_key, uint32_t *key,
                         unsigned absent, struct ffs_entry *result)
{
    unsigned char *p;
    struct ffs_entry existing;
    uint32_t newkey = 0;
    int err = begin_change(v);
    if (err) return err;
    err = component(name);
    if (!err && target && strlen(target) >= v->buckets * 4) err = FFS_BAD_NAME;
    if (!err && !absent) {
        err = ffs_lookup(v, dir, (const unsigned char *)name, strlen(name), &existing);
        if (!err) err = FFS_EXISTS;
        else if (err == FFS_NOT_FOUND) err = 0;
    }
    if (!err) err = allocate_block(v, &newkey);
    if (!err) err = edit_block(v, newkey, &p, 20, 1);
    if (!err) {
        put_word(p, 2); put_word(p + 4, newkey);
        put_word(p + v->block_size - 4, (uint32_t)type);
        if (target) memcpy(p + 24, target, strlen(target) + 1);
        p[v->block_size - 80] = (unsigned char)strlen(name);
        memcpy(p + v->block_size - 79, name, strlen(name)); stamp(v, p);
        if (link_key) {
            unsigned char *linked = buffer(v);
            if (!linked) err = FFS_NOMEM;
            else {
                err = header(v, link_key, linked);
                if (!err) {
                    put_word(p + v->block_size - 44, link_key);
                    put_word(p + v->block_size - 40, word(linked, v->block_size - 40));
                    put_word(linked + v->block_size - 40, newkey);
                    err = stage(v, link_key, linked, 20);
                }
                release(v, linked);
            }
        }
        if (!err) err = insert_entry(v, dir, newkey, p);
        if (!err && type == 2) err = directory_cache(v, newkey);
        if (!err) err = change_cache(v, dir, newkey, 1);
        if (!err && result) err = decode_entry(v, newkey, p, result);
    }
    err = finish_change(v, err);
    if (!err) *key = newkey;
    return err;
}

int ffs_create(struct ffs_volume *v, uint32_t dir, const char *name,
                int is_dir, uint32_t *key)
{
    return create_entry(v, dir, name, is_dir ? 2 : -3, NULL, 0, key, 0, NULL);
}
int ffs_resolve_create(struct ffs_volume *v, uint32_t dir, const char *path,
                       struct ffs_entry *e, unsigned *created)
{
    const char *leaf = NULL;
    uint32_t parent = 0, key;
    int err;
    *created = 0;
    err = resolve_entry(v, dir, path, e, &parent, &leaf);
    if (err != FFS_NOT_FOUND || !leaf) return err;
    /* Resolution and creation are one synchronous core operation. No other
     * namespace mutation can invalidate the final-component miss. */
    err = create_entry(v, parent, leaf, -3, NULL, 0, &key, 1, e);
    if (!err) *created = 1;
    return err;
}
int ffs_symlink(struct ffs_volume *v, uint32_t dir, const char *name,
                 const char *target, uint32_t *key)
{
    if (!target || !*target) return FFS_BAD_NAME;
    return create_entry(v, dir, name, 3, target, 0, key, 0, NULL);
}
int ffs_hardlink(struct ffs_volume *v, uint32_t dir, const char *name,
                  uint32_t target, uint32_t *key)
{
    struct ffs_entry e, parent;
    uint32_t ancestor = dir, seen = 0;
    int err = ffs_stat(v, target, &e);
    if (err) return err;
    if (e.type != -3 && e.type != 2) return FFS_WRONG_TYPE;
    if (e.type == 2) while (ancestor) {
        if (ancestor == target || ++seen > v->blocks) return FFS_BAD_NAME;
        err = ffs_stat(v, ancestor, &parent);
        if (err) return err;
        ancestor = parent.parent;
    }
    return create_entry(v, dir, name, e.type == -3 ? -4 : 4, NULL, target, key, 0, NULL);
}
static int object_caches_impl(struct ffs_volume *v, uint32_t key)
{
    struct ffs_entry e;
    uint32_t next, seen = 0;
    int err;
    if (!v->format.dircache) return 0;
    err = ffs_stat(v, key, &e);
    if (err) return err;
    next = e.backlink;
    err = e.parent ? change_cache(v, e.parent, key, 0) : 0;
    while (!err && next) {
        if (++seen > v->blocks) return FFS_CORRUPT;
        err = ffs_stat(v, next, &e);
        if (!err && (e.link != key || (e.type != -4 && e.type != 4))) err = FFS_CORRUPT;
        if (!err) err = change_cache(v, e.parent, e.key, 0);
        next = e.backlink;
    }
    return err;
}
static int object_caches(struct ffs_volume *v, uint32_t key)
{
    int result;
    PROFILE_START(v);
    result = object_caches_impl(v, key);
    PROFILE_END(v, FFS_P_CACHE);
    return result;
}

/* Callers take the date once for the whole operation. */
static int finish_file(struct ffs_volume *v, uint32_t key)
{
    struct ffs_change *c = pending_block(v, key);
    unsigned char *p;
    int err;
    if (v->failed || v->changes) return FFS_IO;
    if (!c || !c->close_update) return 0;
    v->in_place = 1;
    err = edit_block(v, key, &p, 20, 0);
    if (!err) {
        put_word(p + v->block_size - 192,
                     word(p, v->block_size - 192) & ~16u);
        stamp(v, p);
        v->changes->close_update = 0;
        err = object_caches(v, key);
    }
    return finish_change(v, err);
}
int ffs_finish_file(struct ffs_volume *v, uint32_t key)
{
    struct ffs_change *c = pending_block(v, key);
    if (c && c->close_update) refresh_date(v);
    return finish_file(v, key);
}

/* A sequential writer can retain its fixed-size directory record location.
 * Any intervening mutation invalidates it with the extension-table cursor. */
static int write_cache(struct ffs_volume *v, struct ffs_file_state *state,
                       const struct ffs_entry *e, const unsigned char *head)
{
    unsigned char *p;
    uint32_t offset, length, i;
    int err;
    PROFILE_START(v);
    if (!v->format.dircache) return 0;
    if (!state || e->backlink) return object_caches(v, e->key);
    if (!state->cache_block) {
        err = change_cache_position(v, e->parent, e->key, 0, state);
    } else {
        offset = state->cache_offset;
        err = edit_block(v, state->cache_block, &p, 20, 0);
        if (!err && (word(p, 0) != 33 || word(p, 4) != state->cache_block ||
            word(p, 8) != e->parent || offset < 24)) err = FFS_CORRUPT;
        if (!err) err = cache_record_length(v, p, offset, &length);
        if (!err && word(p, offset) != e->key) err = FFS_CORRUPT;
        if (!err) {
            put_word(p + offset + 4, word(head, v->block_size - 188));
            put_word(p + offset + 8, word(head, v->block_size - 192));
            for (i = 0; i < 3; ++i) {
                p[offset + 16 + i * 2] = (unsigned char)(v->now[i] >> 8);
                p[offset + 17 + i * 2] = (unsigned char)v->now[i];
            }
        }
    }
    PROFILE_END(v, FFS_P_CACHE);
    return err;
}

int ffs_readlink(struct ffs_volume *v, uint32_t key, char *target,
                  uint32_t capacity, uint32_t *length)
{
    unsigned char *p = buffer(v);
    uint32_t n = 0;
    int err;
    if (!p) return FFS_NOMEM;
    err = header(v, key, p);
    if (!err && word(p, v->block_size - 4) != 3) err = FFS_WRONG_TYPE;
    if (!err) {
        while (n < v->buckets * 4 && p[24 + n]) ++n;
        if (n == v->buckets * 4) err = FFS_CORRUPT;
        else if (n >= capacity) err = 120;
        else { memcpy(target, p + 24, n + 1); *length = n; }
    }
    release(v, p); return err;
}

/* Data blocks holding size bytes; FFS blocks need no division. */
static uint32_t data_blocks(const struct ffs_volume *v, uint32_t size)
{
    uint32_t capacity = v->block_size - 24;
    if (v->format.fast) return (size >> v->block_shift) + ((size & (v->block_size - 1)) != 0);
    return size ? size / capacity + (size % capacity != 0) : 0;
}
static int truncate_file(struct ffs_volume *, uint32_t, uint32_t, uint32_t, unsigned);
static int ffs_write_impl(struct ffs_volume *, struct ffs_file_state *, uint32_t, uint32_t,
                          const void *, uint32_t, uint32_t *);
/* Shrinking walks the tables once. Growth writes zeros through the
 * streaming planner in bounded chunks, each its own operation, after a
 * free-space check, so a file that cannot fit is left unchanged. */
int ffs_resize(struct ffs_volume *v, uint32_t key, uint32_t size)
{
    struct ffs_entry e;
    unsigned char *p;
    uint32_t old_count, count, tables, old_tables, free_blocks, actual, n;
    int err = begin_change(v);
    if (!err) err = ffs_stat(v, key, &e);
    if (!err && e.type != -3) err = FFS_WRONG_TYPE;
    if (!err && (e.protection & 4)) err = 223;
    if (!err && size > e.size && size > v->file_limit) err = 207;
    if (err) return err;
    if (size > e.size) {
        struct ffs_file_state state;
        unsigned char *zero;
        old_count = data_blocks(v, e.size);
        count = data_blocks(v, size);
        old_tables = old_count ? (old_count - 1) / v->buckets + 1 : 1;
        tables = (count - 1) / v->buckets + 1;
        err = ffs_free_blocks(v, &free_blocks);
        if (!err && free_blocks < (count - old_count) + (tables - old_tables)) err = FFS_FULL;
        zero = err ? NULL : v->io.alloc(v->io.context, 65536);
        if (!err && !zero) err = FFS_NOMEM;
        if (err) return err;
        memset(zero, 0, 65536);
        memset(&state, 0, sizeof(state));
        for (; !err && e.size < size; e.size += n) {
            n = size - e.size < 65536 ? size - e.size : 65536;
            err = ffs_write_impl(v, &state, key, e.size, zero, n, &actual);
        }
        v->io.free(v->io.context, zero, 65536);
        return err;
    }
    PROFILE_START(v);
    if (size < e.size) err = truncate_file(v, key, e.size, size, 0);
    if (!err) err = edit_block(v, key, &p, 20, 0);
    if (!err) {
        put_word(p + v->block_size - 188, size);
        put_word(p + v->block_size - 192, e.protection & ~16u);
        stamp(v, p);
    }
    PROFILE_END(v, FFS_P_GROW);
    if (!err) err = object_caches(v, key);
    return finish_change(v, err);
}
/* One cursor visits each candidate at most once, so planned reservations
 * cannot overlap even though borrowed bitmap words remain unchanged. */
struct write_allocator {
    struct ffs_change *block;
    uint32_t base, start, cursor, limit;
    unsigned wrapped;
};
struct write_reservation {
    struct ffs_change *bitmap, *table;
    uint32_t first, count, slot, bit;
};
struct write_plan {
    struct ffs_change **tables;
    struct ffs_change *inline_tables[4];
    uint32_t table_count;
    struct write_reservation *runs;
    struct write_reservation inline_runs[8];
    uint32_t count, capacity;
};
static int reserve_write_run(struct ffs_volume *v, struct write_plan *plan,
                              struct write_allocator *a, struct ffs_change *table,
                              uint32_t first, uint32_t count, uint32_t slot)
{
    struct write_reservation *run;
    if (plan->count == plan->capacity) {
        uint32_t capacity = plan->capacity * 2;
        struct write_reservation *larger;
        if (capacity < plan->capacity || capacity > UINT32_MAX / sizeof(*larger))
            return FFS_NOMEM;
        larger = v->io.alloc(v->io.context, capacity * sizeof(*larger));
        if (!larger) return FFS_NOMEM;
        memcpy(larger, plan->runs, plan->count * sizeof(*larger));
        if (plan->runs != plan->inline_runs)
            v->io.free(v->io.context, plan->runs, plan->capacity * sizeof(*larger));
        plan->runs = larger; plan->capacity = capacity;
    }
    run = &plan->runs[plan->count++];
    run->bitmap = a->block; run->table = table; run->bit = first - a->base;
    run->first = first; run->count = count; run->slot = slot;
    return 0;
}
/* Find a run without reserving its bits. A run stops at a hole, page end or
 * cursor limit. Both table and data allocation consume this same cursor. */
static int write_allocate_impl(struct ffs_volume *v, struct write_allocator *a,
                               uint32_t wanted, uint32_t *first, uint32_t *count)
{
    uint32_t bits = (v->block_size - 4) * 8;
    uint32_t candidate = a->cursor, bit, page, key, value, mask, offset, skip;
    int err;
    for (;;) {
        if (candidate == a->limit) {
            if (a->wrapped) return FFS_FULL;
            candidate = v->reserved; a->limit = a->start; a->wrapped = 1;
            if (candidate == a->limit) return FFS_FULL;
        }
        if (!a->block || candidate < a->base || candidate - a->base >= bits) {
            if (v->bitmap_block && candidate >= v->bitmap_base &&
                candidate - v->bitmap_base < bits) {
                key = v->bitmap_block; err = 0;
            } else {
                page = (candidate - v->reserved) / bits;
                err = bitmap_key(v, page, &key);
            }
            if (!err) err = write_edit(v, key, &a->block, 0, 0);
            if (err) return err;
            a->base = v->bitmap_base;
        }
        bit = candidate - a->base; offset = 4 + bit / 32 * 4;
        value = word(a->block->bytes, offset); mask = UINT32_C(1) << (bit % 32);
        if (value & (UINT32_MAX << (bit % 32))) {
            while (!(value & mask)) { ++candidate; mask <<= 1; }
            if (candidate < a->limit) break;
            candidate = a->limit;
        } else {
            skip = 32 - bit % 32;
            if (skip > a->limit - candidate) skip = a->limit - candidate;
            candidate += skip;
        }
    }
    *first = candidate; *count = 0;
    for (;;) {
        if ((value | (mask - 1)) == UINT32_MAX) {
            /* The remaining word is free. Consume it without inspecting
             * individual bits, capped by the request and cursor boundary. */
            skip = 32 - (candidate - a->base) % 32;
            if (skip > wanted - *count) skip = wanted - *count;
            if (skip > a->limit - candidate) skip = a->limit - candidate;
            candidate += skip; *count += skip;
        } else {
            do {
                ++candidate; ++*count; mask <<= 1;
            } while (*count < wanted && candidate < a->limit && (value & mask));
            if (mask) break;
        }
        if (*count == wanted || candidate == a->limit ||
            candidate - a->base >= bits) break;
        offset += 4; mask = 1; value = word(a->block->bytes, offset);
        if (!(value & mask)) break;
    }
    /* Check the complete selected range once, including word-sized steps. */
    if (v->root - *first < *count || a->block->key - *first < *count)
        return FFS_CORRUPT;
    a->cursor = candidate;
    return 0;
}
static int write_allocate(struct ffs_volume *v, struct write_allocator *a,
                          uint32_t wanted, uint32_t *first, uint32_t *count)
{
    int result;
    PROFILE_START(v);
    result = write_allocate_impl(v, a, wanted, first, count);
    PROFILE_END(v, FFS_P_ALLOCATE);
    return result;
}

static int write_table(struct ffs_volume *v, uint32_t key, uint32_t block,
                        struct ffs_change **table)
{
    uint32_t size = v->block_size;
    int err;
    if (!valid_key(v, block)) return FFS_CORRUPT;
    err = write_edit(v, block, table, 20, 0);
    if (!err && (word((*table)->bytes, 0) != 16 || word((*table)->bytes, 4) != block ||
        word((*table)->bytes, size - 12) != key || word((*table)->bytes, size - 4) != (uint32_t)-3))
        err = FFS_CORRUPT;
    return err;
}

/* Acquire the complete chain before choosing data blocks. The plan owns
 * only references; new cells are owned by the operation until publication. */
static int prepare_write_tables(struct ffs_volume *v, struct write_allocator *a,
                                 struct write_plan *plan, uint32_t key,
                                 uint32_t old_count, uint32_t last,
                                 uint32_t table_start, struct ffs_change *table)
{
    uint32_t size = v->block_size, next, count, allocated, i;
    struct ffs_change *child;
    int err;
    plan->table_count = 1;
    if (last - table_start >= v->buckets)
        plan->table_count += (last - table_start) / v->buckets;
    if (plan->table_count > 4) {
        if (plan->table_count > UINT32_MAX / sizeof(*plan->tables)) return FFS_NOMEM;
        plan->tables = v->io.alloc(v->io.context, plan->table_count * sizeof(*plan->tables));
        if (!plan->tables) return FFS_NOMEM;
    }
    plan->tables[0] = table;
    for (i = 1; i < plan->table_count; ++i) {
        count = word(table->bytes, 8);
        if (count > v->buckets) return FFS_CORRUPT;
        next = word(table->bytes, size - 8);
        if (next) err = write_table(v, key, next, &child);
        else {
            /* Missing links may extend EOF, but must not hide old data. */
            if (table_start < old_count && old_count - table_start > count)
                return FFS_CORRUPT;
            err = write_allocate(v, a, 1, &next, &allocated);
            if (!err) err = reserve_write_run(v, plan, a, NULL, next, 1, 0);
            if (!err) err = write_edit(v, next, &child, 20, 1);
            if (!err) {
                put_word(child->bytes, 16); put_word(child->bytes + 4, next);
                put_word(child->bytes + size - 12, key);
                put_word(child->bytes + size - 4, (uint32_t)-3);
            }
        }
        if (err) return err;
        plan->tables[i] = table = child; table_start += v->buckets;
    }
    return 0;
}
/* No fallible calls, allocation or reads follow successful data submission.
 * Apply bitmap runs by word and data pointers by table slot. */
static void apply_write_plan(struct ffs_volume *v, struct write_plan *plan)
{
    uint32_t i, n, bit, offset, left, amount, mask, first;
    PROFILE_START(v);
    for (i = 0; i < plan->count; ++i) {
        struct write_reservation *run = &plan->runs[i];
        bit = run->bit;
        offset = 4 + bit / 32 * 4; bit %= 32; left = run->count;
        while (left) {
            amount = 32 - bit; if (amount > left) amount = left;
            mask = (UINT32_MAX >> (32 - amount)) << bit;
            put_word(run->bitmap->bytes + offset, word(run->bitmap->bytes, offset) & ~mask);
            left -= amount; bit = 0; offset += 4;
        }
        v->free_change -= run->count;
        if (run->table) {
            /* Locals keep aliasing stores from reloading the run. */
            unsigned char *slot = run->table->bytes + 24 + (v->buckets - run->slot) * 4;
            first = run->first;
            for (n = run->count; n; --n) { slot -= 4; put_word(slot, first++); }
            put_word(run->table->bytes + 8, run->slot + run->count);
            if (!run->slot && run->table == plan->tables[0] &&
                word(run->table->bytes, 0) == 2)
                put_word(run->table->bytes + 16, run->first);
        }
    }
    for (i = 1; i < plan->table_count; ++i)
        put_word(plan->tables[i - 1]->bytes + v->block_size - 8, plan->tables[i]->key);
    PROFILE_END(v, FFS_P_TABLE);
}

static int collect_write_run(struct ffs_volume *v, uint32_t key, uint32_t blocks,
                             const unsigned char *data, uint32_t index)
{
    struct ffs_data_run *run = v->direct_count ? &v->direct[v->direct_count - 1] : NULL;
    uint32_t size = v->format.fast ? blocks << v->block_shift : blocks * (v->block_size - 24);
    if (run && run->key + run->blocks == key && run->data + run->size == data) {
        run->size += size; run->blocks += blocks;
        return 0;
    }
    if (v->direct_count == v->direct_capacity) {
        uint32_t capacity = v->direct_capacity * 2;
        struct ffs_data_run *larger;
        if (capacity < v->direct_capacity || capacity > UINT32_MAX / sizeof(*larger))
            return FFS_NOMEM;
        larger = v->io.alloc(v->io.context, capacity * sizeof(*larger));
        if (!larger) return FFS_NOMEM;
        memcpy(larger, v->direct, v->direct_count * sizeof(*larger));
        if (v->direct != v->direct_inline)
            v->io.free(v->io.context, v->direct, v->direct_capacity * sizeof(*larger));
        v->direct = larger; v->direct_capacity = capacity;
    }
    run = &v->direct[v->direct_count++];
    run->key = key; run->size = size; run->blocks = blocks; run->data = data;
    run->index = index; run->next = 0;
    return 0;
}

/* Plan a contiguous run once. Only partial edges and sparse gaps need
 * individual buffers; full caller blocks stay a single borrowed extent. */
static int prepare_write_run(struct ffs_volume *v, uint32_t block,
                              uint32_t blocks, uint32_t base, uint32_t old_size,
                              uint32_t offset, uint32_t end,
                              const unsigned char *src, unsigned char **scratch)
{
    uint32_t size = v->block_size, from, to, count;
    int err;
    while (blocks) {
        if (v->direct && base >= offset && end - base >= size) {
            count = (end - base) >> v->block_shift;
            if (count > blocks) count = blocks;
            err = collect_write_run(v, block, count, src + (base - offset), 0);
            if (err) return err;
            base += count << v->block_shift; block += count; blocks -= count;
            continue;
        }
        from = offset > base ? offset - base : 0;
        if (from > size) from = size;
        to = end - base; if (to > size) to = size;
        {
            struct ffs_change *c = cached_block(v, block);
            if (c && (c->state == BLOCK_CHANGE || c->state == BLOCK_PENDING) &&
                c->checksum >= 0) return FFS_CORRUPT;
        }
        if (!from && to == size) err = stage(v, block, (unsigned char *)src + (base - offset), -1);
        else {
            unsigned char *data = *scratch;
            if (!data) data = *scratch = buffer(v);
            if (!data) return FFS_NOMEM;
            if (base < old_size) err = read_block(v, block, data, 0);
            else { memset(data, 0, size); err = 0; }
            if (err) return err;
            if (end > old_size && (base >= old_size || old_size - base < size)) {
                uint32_t zero = old_size > base ? old_size - base : 0;
                memset(data + zero, 0, size - zero);
            }
            if (to > from) memcpy(data + from, src + (base + from - offset), to - from);
            err = stage(v, block, data, -1);
        }
        if (err) return err;
        ++block; --blocks; base += size;
    }
    return 0;
}

/* OFS data blocks carry headers and a successor link. Full blocks become
 * direct runs whose headers are built during transfer. Edges, and blocks
 * that already have pending cells, are edited as owned checksummed cells.
 * Each planned block receives the key of the block planned after it. */
struct ofs_link { uint32_t run; struct ffs_change *cell; };
static void link_ofs(struct ffs_volume *v, struct ofs_link *link, uint32_t key)
{
    if (link->run) v->direct[link->run - 1].next = key;
    else if (link->cell) put_word(link->cell->bytes + 16, key);
    link->run = 0; link->cell = NULL;
}
static int prepare_ofs_run(struct ffs_volume *v, uint32_t file, uint32_t block,
                           uint32_t blocks, uint32_t n, uint32_t last, uint32_t old_count,
                           uint32_t old_size, uint32_t offset, uint32_t end,
                           const unsigned char *src, struct ofs_link *link)
{
    uint32_t cap = v->block_size - 24, base = n * cap, from, to, used, zero;
    struct ffs_change *c;
    unsigned char *p;
    int err;
    for (; blocks; ++block, --blocks, ++n, base += cap) {
        link_ofs(v, link, block);
        c = cached_block(v, block);
        /* A full final overwrite keeps its existing successor via its cell. */
        if (v->direct && base >= offset && end - base >= cap &&
            (n < last || n + 1 >= old_count) &&
            (!c || c->state == BLOCK_CLEAN)) {
            err = collect_write_run(v, block, 1, src + (base - offset), n);
            if (err) return err;
            /* An extended run's last block is now this one. */
            link->run = v->direct_count; v->direct[link->run - 1].next = 0;
            continue;
        }
        /* A cell this operation already owns can only be aliased metadata. */
        if (c && (c->state == BLOCK_CHANGE || (base >= old_size && c->state != BLOCK_CLEAN)))
            return FFS_CORRUPT;
        err = acquire_change(v, block, &c, 20, base >= old_size, base < old_size);
        if (err) return err;
        p = c->bytes;
        if (base < old_size && (word(p, 0) != 8 || word(p, 4) != file ||
            word(p, 8) != n + 1 || word(p, 12) > cap)) return FFS_CORRUPT;
        from = offset > base ? offset - base : 0;
        if (from > cap) from = cap;
        to = end - base; if (to > cap) to = cap;
        /* Never expose bytes past the previous EOF when extending a file. */
        if (end > old_size && base < old_size) {
            zero = old_size - base;
            if (zero < cap) memset(p + 24 + zero, 0, cap - zero);
        }
        if (to > from) memcpy(p + 24 + from, src + (base + from - offset), to - from);
        used = (end > old_size ? end : old_size) - base;
        put_word(p, 8); put_word(p + 4, file); put_word(p + 8, n + 1);
        put_word(p + 12, used > cap ? cap : used);
        link->cell = c;
    }
    return 0;
}

static int write_fast_impl(struct ffs_volume *v, struct ffs_file_state *state,
                       uint32_t key, uint32_t offset, const unsigned char *src,
                       uint32_t length)
{
    struct write_allocator allocator = {0};
    struct write_plan plan;
    struct ffs_entry entry_storage, *entry = &entry_storage;
    struct ffs_change *head = NULL, *table = NULL;
    struct ofs_link link = {0, NULL};
    unsigned char *data = NULL, *root = NULL, *transfer = NULL;
    uint32_t size = v->block_size, end = offset + length, cap = size - 24;
    uint32_t first, table_index, transfer_size = 0;
    uint32_t last = v->format.fast ? (end - 1) >> v->block_shift : (end - 1) / cap;
    uint32_t table_start = 0, current = key, next;
    uint32_t block, n, pos, count, old_count, run_count, wanted;
    int err = 0;
    plan.tables = plan.inline_tables; plan.table_count = 0;
    plan.runs = plan.inline_runs; plan.count = 0; plan.capacity = 8;
    allocator.start = valid_key(v, v->alloc_hint) ? v->alloc_hint : v->reserved;
    allocator.cursor = allocator.start; allocator.limit = v->blocks;
    if (state && (state->key != key || state->generation != v->generation))
        state->cache_block = 0;
    err = write_edit(v, key, &head, 20, 0);
    if (!err && (word(head->bytes, 0) != 2 || word(head->bytes, 4) != key)) err = FFS_CORRUPT;
    if (!err && deferred_metadata(v, state)) {
        /* The streaming path needs no name, comment or directory snapshot. */
        unsigned name_length = head->bytes[size - 80], comment_length = head->bytes[size - 184];
        entry->type = (int32_t)word(head->bytes, size - 4);
        if (!name_length || name_length > 30 || comment_length > 79 ||
            (entry->type != -3 && entry->type != 2 && entry->type != 3 &&
             entry->type != -4 && entry->type != 4)) err = FFS_CORRUPT;
        entry->size = word(head->bytes, size - 188);
        entry->protection = word(head->bytes, size - 192);
    } else if (!err) err = decode_entry(v, key, head->bytes, entry);
    if (!err && entry->type != -3) err = FFS_WRONG_TYPE;
    if (!err && (entry->protection & 4)) err = 223;
    if (!err && end > entry->size && end > v->file_limit) err = 207;
    if (err) goto done;
    if (v->format.fast) {
        first = (offset < entry->size ? offset : entry->size) >> v->block_shift;
        old_count = (entry->size >> v->block_shift) + ((entry->size & (size - 1)) != 0);
    } else {
        first = (offset < entry->size ? offset : entry->size) / cap;
        old_count = entry->size / cap + (entry->size % cap != 0);
        /* Growth from a block boundary must link the previous last block. */
        if (old_count && first == old_count) --first;
    }
    table = head;
    if (last >= v->blocks) { err = FFS_FULL; goto done; }
    if (v->deferred && v->io.write_extent) {
        v->direct_capacity = 4;
        v->direct = v->direct_inline;
    }
    if (state && state->key == key && state->generation == v->generation &&
        state->table_start && state->table_start <= first) {
        table_start = state->table_start; current = state->table;
    }
    /* Tables before the first written block stay unchanged. Walking them as
     * views keeps a write deep into a large file from holding the chain. */
    if (first - table_start >= v->buckets) {
        struct block_view view = {0};
        const unsigned char *p = head->bytes;
        if (current != key) { err = table_view(v, current, key, &view); p = view.bytes; }
        while (!err && first - table_start >= v->buckets && (next = word(p, size - 8))) {
            err = table_view(v, next, key, &view);
            p = view.bytes; current = next; table_start += v->buckets;
        }
        release_view(v, &view);
    }
    if (!err && current != key) err = write_table(v, key, current, &table);
    if (!err) err = prepare_write_tables(v, &allocator, &plan, key, old_count,
                                         last, table_start, table);
    if (err) goto done;
    table_index = 0;
    if (first - table_start >= v->buckets)
        table_index = (first - table_start) / v->buckets;
    table_start += table_index * v->buckets;
    table = plan.tables[table_index]; current = table->key;
    pos = first - table_start; count = word(table->bytes, 8);
    for (n = first; n <= last; ) {
        uint32_t slots = 24 + (v->buckets - pos) * 4;
        if (count > v->buckets || pos > count) { err = FFS_CORRUPT; break; }
        wanted = last - n + 1;
        if (wanted > v->buckets - pos) wanted = v->buckets - pos;
        if (n < old_count) {
            if (pos >= count) { err = FFS_CORRUPT; break; }
            if (wanted > count - pos) wanted = count - pos;
            if (wanted > old_count - n) wanted = old_count - n;
            block = word(table->bytes, slots - 4);
            if (!valid_key(v, block) || block == key) { err = FFS_CORRUPT; break; }
            for (run_count = 1; run_count < wanted; ++run_count) {
                next = word(table->bytes, slots - (run_count + 1) * 4);
                if (next != block + run_count) break;
                if (!valid_key(v, next) || next == key) { err = FFS_CORRUPT; break; }
            }
        } else {
            err = write_allocate(v, &allocator, wanted, &block, &run_count);
            if (!err) err = reserve_write_run(v, &plan, &allocator, table, block, run_count, pos);
            if (!err) count = pos + run_count;
        }
        if (err) break;
        {
            PROFILE_START(v);
            if (v->format.fast)
                err = prepare_write_run(v, block, run_count, n << v->block_shift,
                                        entry->size, offset, end, src, &data);
            else err = prepare_ofs_run(v, key, block, run_count, n, last, old_count,
                                       entry->size, offset, end, src, &link);
            PROFILE_END(v, FFS_P_PLAN);
        }
        n += run_count; pos += run_count;
        if (err || n > last) break;
        if (pos == v->buckets) {
            table_start += v->buckets; pos = 0;
            table = plan.tables[++table_index]; current = table->key;
            count = word(table->bytes, 8);
        }
    }
    if (err) goto done;
    if (!deferred_metadata(v, state)) {
        /* This header is owned, so directory-cache preparation may use the
         * proposed size while borrowed table and bitmap bytes stay intact. */
        if (end > entry->size) put_word(head->bytes + size - 188, end);
        put_word(head->bytes + size - 192, entry->protection & ~16u);
        stamp(v, head->bytes);
        err = write_cache(v, state, entry, head->bytes);
        if (err) goto done;
    }
    if (v->direct_count && !v->disk_dirty && !disk_root(v)) {
        root = buffer(v);
        if (!root) { err = FFS_NOMEM; goto done; }
    }
    if (v->direct_count && !v->format.fast) {
        /* OFS headers need an owned transfer buffer, reused per extent. */
        uint32_t i, blocks = 0, limit = 16384u >> v->block_shift;
        for (i = 0; i < v->direct_count; ++i) blocks += v->direct[i].blocks;
        if (!limit) limit = 1;
        transfer_size = (blocks < limit ? blocks : limit) << v->block_shift;
        transfer = v->io.alloc(v->io.context, transfer_size);
        if (!transfer) { err = FFS_NOMEM; goto done; }
    }
    err = write_direct(v, root, key, transfer, transfer_size);
    if (err) { v->failed = 1; drop_clean(v); goto done; }
    /* There is no failure path after this point. */
    apply_write_plan(v, &plan);
    if (plan.count) v->alloc_hint = allocator.cursor;
    if (end > entry->size) put_word(head->bytes + size - 188, end);
    if (state) {
        state->key = key; state->table = current; state->table_start = table_start;
    }
done:
    release(v, root); release(v, data);
    if (transfer) v->io.free(v->io.context, transfer, transfer_size);
    if (plan.tables && plan.tables != plan.inline_tables)
        v->io.free(v->io.context, plan.tables, plan.table_count * sizeof(*plan.tables));
    if (plan.runs != plan.inline_runs)
        v->io.free(v->io.context, plan.runs, plan.capacity * sizeof(*plan.runs));
    return err;
}
static int write_fast(struct ffs_volume *v, struct ffs_file_state *state, uint32_t key, uint32_t offset, const unsigned char *src, uint32_t length)
{
    int result;
    PROFILE_START(v);
    result = write_fast_impl(v, state, key, offset, src, length);
    PROFILE_END(v, FFS_P_GROW);
    return result;
}


static int ffs_write_impl(struct ffs_volume *v, struct ffs_file_state *state, uint32_t key, uint32_t offset,
                          const void *src, uint32_t length, uint32_t *actual)
{
    int err = begin_write(v);
    *actual = 0;
    if (err) return err;
    /* Deferred streaming writes take their timestamps at close. */
    if (!deferred_metadata(v, state)) refresh_date(v);
    /* Header sizes are unsigned 32-bit: files end at 4 GiB - 1. */
    if (length > UINT32_MAX - offset) return 207;
    if (!length) return 0;
    v->write_borrowed = deferred_metadata(v, state);
    err = write_fast(v, state, key, offset, src, length);
    err = finish_change(v, err);
    if (!err) {
        *actual = length;
        if (state) state->generation = v->generation;
        if (deferred_metadata(v, state)) pending_block(v, key)->close_update = 1;
    }
    else if (state) state->key = 0;
    return err;
}
int ffs_write(struct ffs_volume *v, uint32_t key, uint32_t offset, const void *src, uint32_t length, uint32_t *actual)
{
    int result;
    PROFILE_START(v);
    result = ffs_write_impl(v, NULL, key, offset, src, length, actual);
    PROFILE_END(v, FFS_P_WRITE);
    return result;
}


int ffs_write_file(struct ffs_volume *v, struct ffs_file_state *state, uint32_t key,
                   uint32_t offset, const void *src, uint32_t length, uint32_t *actual)
{
    int result;
    PROFILE_START(v);
    result = ffs_write_impl(v, state, key, offset, src, length, actual);
    PROFILE_END(v, FFS_P_WRITE);
    return result;
}

static int unlink_entry(struct ffs_volume *v, const struct ffs_entry *e)
{
    struct block_view view;
    unsigned char *p;
    uint16_t hash = 0;
    uint32_t owner = e->parent, offset, next, seen = 0;
    int err;
    (void)ffs_name_hash(e->name, strlen((char *)e->name), v->format.international,
                       (uint16_t)v->buckets, &hash);
    offset = 24 + 4 * hash;
    err = header_view(v, owner, &view);
    if (!err && word(view.bytes, v->block_size - 4) != 1 &&
        word(view.bytes, v->block_size - 4) != 2) err = FFS_WRONG_TYPE;
    while (!err) {
        next = word(view.bytes, offset);
        if (next == e->key) {
            err = edit_view(v, &view, &p, 20);
            if (!err) {
                put_word(p + offset, e->hash_next);
                if (owner == e->parent) stamp(v, p);
            }
            break;
        }
        release_view(v, &view);
        if (!valid_key(v, next) || ++seen > v->blocks) { err = FFS_CORRUPT; break; }
        owner = next; offset = v->block_size - 16;
        err = header_view(v, owner, &view);
    }
    release_view(v, &view);
    return err;
}
/* Cells of freed blocks are retired when the operation succeeds. Blocks
 * without a cell need no record, which bounds memory for large frees. */
static int release_cached(struct ffs_volume *v, uint32_t first, uint32_t count)
{
    struct ffs_change **slot = &v->cache_index[first % 64], *c;
    uint32_t buckets = count < 64 ? count : 64;
    int err = 0;
    for (; !err && buckets; --buckets) {
        for (c = *slot++; !err && c; c = c->hash_next)
            if (c->key - first < count) err = record_release(v, c->key);
        if (slot == v->cache_index + 64) slot = v->cache_index;
    }
    return err;
}
/* Free a run of allocated blocks by bitmap word. */
static int free_run(struct ffs_volume *v, uint32_t first, uint32_t count)
{
    uint32_t bits = (v->block_size - 4) * 8, bit, n, mapkey = 0, offset, amount, mask, value;
    unsigned char *p;
    int err;
    if (first < v->reserved || first >= v->blocks || count > v->blocks - first ||
        v->root - first < count) return FFS_CORRUPT;
    while (count) {
        bit = (first - v->reserved) % bits;
        n = bits - bit < count ? bits - bit : count;
        err = bitmap_key(v, (first - v->reserved) / bits, &mapkey);
        if (!err && mapkey - first < n) err = FFS_CORRUPT;
        if (!err) err = edit_block(v, mapkey, &p, 0, 0);
        if (err) return err;
        for (offset = 4 + bit / 32 * 4, bit %= 32, amount = 0; amount < n; offset += 4, bit = 0) {
            uint32_t width = 32 - bit < n - amount ? 32 - bit : n - amount;
            mask = (UINT32_MAX >> (32 - width)) << bit;
            value = word(p, offset);
            if (value & mask) return FFS_CORRUPT;
            put_word(p + offset, value | mask);
            amount += width;
        }
        v->free_change += n;
        err = release_cached(v, first, n);
        if (err) return err;
        first += n; count -= n;
    }
    return 0;
}
/* Free a file's blocks beyond size in one walk of its tables, freeing data
 * in runs and tables wholly beyond the new end. Only the table holding the
 * end and an OFS final data block are edited; a removed file (discard)
 * edits nothing. The header's size is left to the caller. */
static int truncate_file(struct ffs_volume *v, uint32_t key, uint32_t old_size,
                         uint32_t size, unsigned discard)
{
    struct block_view view;
    unsigned char *p;
    uint32_t capacity = v->block_size - (v->format.fast ? 0 : 24);
    uint32_t old_count = data_blocks(v, old_size), keep = data_blocks(v, size), last = 0;
    uint32_t base = 0, table = key, n, kept, k, first = 0, count, next;
    int err = header_view(v, key, &view);
    for (; !err; base += v->buckets) {
        const unsigned char *t = view.bytes;
        n = old_count - base < v->buckets ? old_count - base : v->buckets;
        if (word(t, 0) != (table == key ? 2u : 16u) || word(t, 4) != table ||
            word(t, v->block_size - 4) != UINT32_C(0xfffffffd) ||
            (table != key && word(t, v->block_size - 12) != key) || word(t, 8) != n) {
            err = FFS_CORRUPT; break;
        }
        kept = keep > base ? (keep - base < n ? keep - base : n) : 0;
        if (kept && base + kept == keep) last = word(t, 24 + (v->buckets - kept) * 4);
        for (k = kept, count = 0; !err && k < n; ++k) {
            uint32_t data = word(t, 24 + (v->buckets - 1 - k) * 4);
            if (data == key || !valid_key(v, data)) err = FFS_CORRUPT;
            else if (count && data == first + count) ++count;
            else {
                if (count) err = free_run(v, first, count);
                first = data; count = 1;
            }
        }
        if (!err && count) err = free_run(v, first, count);
        next = word(t, v->block_size - 8);
        release_view(v, &view);
        if (err) break;
        if (table != key && !kept) err = free_run(v, table, 1);
        else if (!discard && (kept < n || (next && keep <= base + v->buckets))) {
            /* The table holding the new end loses its later entries. */
            err = edit_block(v, table, &p, 20, 0);
            if (!err) {
                memset(p + 24 + (v->buckets - n) * 4, 0, (n - kept) * 4);
                put_word(p + 8, kept);
                if (keep <= base + v->buckets) put_word(p + v->block_size - 8, 0);
                if (table == key && !kept) put_word(p + 16, 0);
            }
        }
        if (err || !next) break;
        if (!valid_key(v, next) || next == key || old_count - base <= v->buckets) {
            err = FFS_CORRUPT; break;
        }
        table = next;
        err = read_view(v, table, &view, 1);
    }
    if (!err && old_count - base > v->buckets) err = FFS_CORRUPT;
    release_view(v, &view);
    /* An OFS file ends at its final data block. */
    if (!err && !discard && last && !v->format.fast) {
        err = edit_block(v, last, &p, 20, 0);
        if (!err && (word(p, 0) != 8 || word(p, 4) != key || word(p, 8) != keep)) err = FFS_CORRUPT;
        if (!err) { put_word(p + 12, size - (keep - 1) * capacity); put_word(p + 16, 0); }
    }
    return err;
}
int ffs_remove(struct ffs_volume *v, uint32_t key)
{
    struct ffs_entry e, child;
    struct ffs_iterator it = {0, 0, 0, 0};
    unsigned char *p = NULL;
    uint32_t cache = 0, next, seen = 0;
    int err;
    refresh_date(v);
    err = finish_file(v, key);
    if (!err) err = begin_write(v);
    if (err) return err;
    err = ffs_stat(v, key, &e);
    if (!err && (key == v->root || (e.protection & 1))) err = FFS_DELETE_PROTECTED;
    if (!err && (e.type == -4 || e.type == 4 || e.backlink)) {
        struct ffs_entry other;
        unsigned char *linked = buffer(v);
        uint32_t owner = e.link;
        p = buffer(v);
        if (!linked || !p) err = FFS_NOMEM;
        if (!err && e.link) {
            /* Unlink a hard-link header from its target's backlink list. */
            for (seen = 0; !err; ++seen) {
                if (seen >= v->blocks) { err = FFS_CORRUPT; break; }
                err = header(v, owner, p);
                if (err) break;
                if (word(p, v->block_size - 40) == key) {
                    put_word(p + v->block_size - 40, e.backlink);
                    err = stage(v, owner, p, 20); break;
                }
                owner = word(p, v->block_size - 40);
            }
            if (!err) err = unlink_entry(v, &e);
            if (!err) err = bitmap_change(v, key, 1);
            if (!err) err = change_cache(v, e.parent, key, -1);
        } else if (!err) {
            /* Keep the real header key (OFS data and children refer to it),
             * and move it into the first link's directory/name slot. */
            err = ffs_stat(v, e.backlink, &other);
            if (!err && other.link != key) err = FFS_CORRUPT;
            if (!err) err = header(v, other.key, linked);
            if (!err) err = unlink_entry(v, &e);
            if (!err) err = unlink_entry(v, &other);
            if (!err) err = header(v, key, p);
            if (!err) {
                memcpy(p + v->block_size - 80, linked + v->block_size - 80, 32);
                memcpy(p + v->block_size - 184, linked + v->block_size - 184, 80);
                put_word(p + v->block_size - 192, other.protection);
                put_word(p + v->block_size - 196, other.owner);
                put_word(p + v->block_size - 40, other.backlink);
                err = insert_entry(v, other.parent, key, p);
            }
            if (!err) err = bitmap_change(v, other.key, 1);
            if (!err) err = directory_cache(v, e.parent);
            if (!err) err = object_caches(v, key);
        }
        release(v, linked); release(v, p);
        return finish_change(v, err);
    }
    if (!err && e.type == 2) {
        err = ffs_next(v, key, &it, &child);
        if (!err) err = FFS_NOT_EMPTY;
        else if (err == FFS_NO_MORE) err = 0;
        p = buffer(v);
        if (!p && !err) err = FFS_NOMEM;
        if (!err) err = header(v, key, p);
        if (!err && v->format.dircache) cache = word(p, v->block_size - 8);
        while (!err && cache) {
            if (!valid_key(v, cache) || ++seen > v->blocks) { err = FFS_CORRUPT; break; }
            err = read_block(v, cache, p, 1);
            if (err) break;
            if (word(p, 0) != 33 || word(p, 4) != cache || word(p, 8) != key) {
                err = FFS_CORRUPT; break;
            }
            next = word(p, 16); err = bitmap_change(v, cache, 1); cache = next;
        }
    } else if (!err && e.type == -3) err = truncate_file(v, key, e.size, 0, 1);
    else if (!err && e.type != 3) err = FFS_WRONG_TYPE;
    if (!err) err = unlink_entry(v, &e);
    if (!err) err = bitmap_change(v, key, 1);
    if (!err) err = change_cache(v, e.parent, key, -1);
    release(v, p);
    return finish_change(v, err);
}
int ffs_rename(struct ffs_volume *v, uint32_t key, uint32_t dir, const char *name)
{
    struct ffs_entry e, target;
    unsigned char *p = NULL;
    uint32_t ancestor = dir, seen = 0;
    int err = begin_change(v);
    if (err) return err;
    err = component(name);
    if (!err) err = ffs_stat(v, key, &e);
    if (!err && (key == v->root || (e.protection & 1))) err = FFS_DELETE_PROTECTED;
    if (!err) {
        err = ffs_lookup(v, dir, (const unsigned char *)name, strlen(name), &target);
        if (!err && target.key != key) err = FFS_EXISTS;
        else if (err == FFS_NOT_FOUND) err = 0;
    }
    while (!err && ancestor) {
        if (ancestor == key || ++seen > v->blocks) { err = FFS_BAD_NAME; break; }
        err = ffs_stat(v, ancestor, &target);
        if (!err) ancestor = target.parent;
    }
    if (!err) err = unlink_entry(v, &e);
    p = buffer(v);
    if (!p && !err) err = FFS_NOMEM;
    if (!err) err = header(v, key, p);
    if (!err) {
        memset(p + v->block_size - 80, 0, 32);
        p[v->block_size - 80] = (unsigned char)strlen(name);
        memcpy(p + v->block_size - 79, name, strlen(name));
        err = insert_entry(v, dir, key, p);
    }
    if (!err) err = directory_cache(v, e.parent);
    if (!err && dir != e.parent) err = directory_cache(v, dir);
    release(v, p);
    return finish_change(v, err);
}
int ffs_set_metadata(struct ffs_volume *v, uint32_t key, const struct ffs_entry *e)
{
    unsigned char *p;
    size_t length = 0;
    uint32_t parent = 0;
    int err = begin_change(v);
    if (err) return err;
    while (length < sizeof(e->comment) && e->comment[length]) ++length;
    if (length > 79) return 220;
    p = buffer(v);
    if (!p) return FFS_NOMEM;
    err = header(v, key, p);
    if (!err) {
        parent = word(p, v->block_size - 12);
        if (key != v->root) {
            put_word(p + v->block_size - 192, e->protection);
            memset(p + v->block_size - 184, 0, 80);
            p[v->block_size - 184] = (unsigned char)length;
            memcpy(p + v->block_size - 183, e->comment, length);
        }
        put_word(p + v->block_size - 92, e->days);
        put_word(p + v->block_size - 88, e->minutes);
        put_word(p + v->block_size - 84, e->ticks);
        err = stage(v, key, p, 20);
    }
    if (!err && parent) err = object_caches(v, key);
    release(v, p);
    return finish_change(v, err);
}

/* The validator's map of reachable blocks takes one bit per block, in
 * 64 KiB chunks so that large volumes need no large contiguous allocation.
 * Object keys are kept in a list that grows with the number of objects. */
#define USED_SHIFT 19
struct used_map { unsigned char **chunks; uint32_t count; };
struct key_list { uint32_t *keys; uint32_t count, capacity; };
static void free_used(struct ffs_volume *v, struct used_map *m)
{
    uint32_t i;
    for (i = 0; m->chunks && i < m->count; ++i)
        if (m->chunks[i]) v->io.free(v->io.context, m->chunks[i], 1u << (USED_SHIFT - 3));
    if (m->chunks) v->io.free(v->io.context, m->chunks, m->count * sizeof(*m->chunks));
    m->chunks = NULL; m->count = 0;
}
static int alloc_used(struct ffs_volume *v, struct used_map *m)
{
    uint32_t i;
    m->count = (v->blocks >> USED_SHIFT) + 1;
    m->chunks = v->io.alloc(v->io.context, m->count * sizeof(*m->chunks));
    if (!m->chunks) { m->count = 0; return FFS_NOMEM; }
    for (i = 0; i < m->count; ++i) m->chunks[i] = NULL;
    for (i = 0; i < m->count; ++i) {
        if (!(m->chunks[i] = v->io.alloc(v->io.context, 1u << (USED_SHIFT - 3)))) return FFS_NOMEM;
        memset(m->chunks[i], 0, 1u << (USED_SHIFT - 3));
    }
    return 0;
}
static int is_used(const struct used_map *m, uint32_t key)
{
    return (m->chunks[key >> USED_SHIFT][(key & ((1u << USED_SHIFT) - 1)) >> 3] >> (key & 7)) & 1;
}
static int mark_used(struct ffs_volume *v, struct used_map *m, uint32_t key)
{
    if (!valid_key(v, key) || is_used(m, key)) return FFS_CORRUPT;
    m->chunks[key >> USED_SHIFT][(key & ((1u << USED_SHIFT) - 1)) >> 3] |= (unsigned char)(1u << (key & 7));
    return 0;
}
/* Bits of a bitmap word, whose bit j describes key first + j, for the keys
 * in [low, high). */
static uint32_t range_bits(uint64_t first, uint64_t low, uint64_t high)
{
    uint64_t a = low > first ? low - first : 0, b = high > first ? high - first : 0;
    if (a > 32) a = 32;
    if (b > 32) b = 32;
    if (b <= a) return 0;
    return (b - a == 32 ? UINT32_MAX : (UINT32_C(1) << (b - a)) - 1) << a;
}
/* Used bits of the 32 keys from first; keys beyond the map count as used. */
static uint32_t used_bits(const struct used_map *m, uint64_t first)
{
    uint64_t byte = first >> 3, limit = (uint64_t)m->count << (USED_SHIFT - 3), value = 0;
    unsigned i;
    for (i = 0; i < 5; ++i, ++byte) {
        uint32_t b = byte < limit ? m->chunks[byte >> (USED_SHIFT - 3)][byte & ((1u << (USED_SHIFT - 3)) - 1)] : 0xff;
        value |= (uint64_t)b << (8 * i);
    }
    return (uint32_t)(value >> (first & 7));
}
/* Write a bitmap page directly. Callers clear validity first; the page has
 * no cached cell while this volume rebuilds its bitmap. */
static int write_bitmap_page(struct ffs_volume *v, uint32_t key, unsigned char *p)
{
    put_word(p, 0);
    (void)ffs_block_checksum(p, v->block_size, 0);
    return v->io.write(v->io.context, key, p, v->block_size) ? FFS_IO : 0;
}
static int push_key(struct ffs_volume *v, struct key_list *l, uint32_t key)
{
    if (l->count == l->capacity) {
        uint32_t capacity = l->capacity ? l->capacity * 2 : 64;
        uint32_t *larger;
        if (capacity < l->capacity || capacity > UINT32_MAX / sizeof(*larger)) return FFS_NOMEM;
        larger = v->io.alloc(v->io.context, capacity * sizeof(*larger));
        if (!larger) return FFS_NOMEM;
        if (l->keys) {
            memcpy(larger, l->keys, l->count * sizeof(*larger));
            v->io.free(v->io.context, l->keys, l->capacity * sizeof(*larger));
        }
        l->keys = larger; l->capacity = capacity;
    }
    l->keys[l->count++] = key;
    return 0;
}
int ffs_validate(struct ffs_volume *v, unsigned repair)
{
    unsigned char *p = NULL, *data = NULL;
    struct used_map used = {NULL, 0};
    struct key_list queue = {NULL, 0, 0};
    uint32_t head = 0, key, i, page, mapkey = 0;
    uint32_t next, count, n, j, block, sequence, bits, value;
    struct ffs_iterator it;
    struct ffs_entry entry;
    int err = 0;
    if (!v->mounted || v->failed || v->changes) return FFS_CORRUPT;
    if (repair && !v->write_requested) return FFS_WRITE_PROTECTED;
    /* Rebuilt pages replace the bitmap on disk, so commit pending work. */
    if (repair && (err = ffs_flush(v))) return err;
    if (repair) refresh_date(v);
    /* Validation must re-read committed blocks, while retaining dirty data. */
    drop_clean(v);
    v->bitmap_block = v->bitmap_ext_key = 0; v->free_known = 0;
    p = buffer(v); data = buffer(v);
    if (!p || !data) { err = FFS_NOMEM; goto done; }
    err = alloc_used(v, &used);
    if (!err) err = push_key(v, &queue, v->root);
    /* Root, directory/file headers, extension blocks, data and caches. */
    while (!err && head < queue.count) {
        key = queue.keys[head++];
        err = mark_used(v, &used, key);
        if (!err) err = ffs_stat(v, key, &entry);
        if (err) break;
        if (entry.type == -4 || entry.type == 4) {
            struct ffs_entry target;
            err = ffs_stat(v, entry.link, &target);
            if (!err && target.type != (entry.type == -4 ? -3 : 2)) err = FFS_CORRUPT;
            continue;
        }
        if (entry.type == 1 || entry.type == 2) {
            memset(&it, 0, sizeof(it));
            while (!(err = next_entry(v, key, &it, &entry, 0))) {
                if (queue.count >= v->blocks) { err = FFS_CORRUPT; break; }
                err = push_key(v, &queue, entry.key);
                if (err) break;
            }
            if (err == FFS_NO_MORE) err = 0;
            if (!err && v->format.dircache) {
                err = header(v, key, p);
                next = err ? 0 : word(p, v->block_size - 8);
                while (!err && next) {
                    err = mark_used(v, &used, next);
                    if (!err) err = read_block(v, next, p, 1);
                    if (!err && (word(p, 0) != 33 || word(p, 4) != next || word(p, 8) != key))
                        err = FFS_CORRUPT;
                    if (!err) next = word(p, 16);
                }
            }
        } else if (entry.type == -3) {
            count = data_blocks(v, entry.size);
            sequence = 0; next = key;
            while (!err) {
                if (next != key) err = mark_used(v, &used, next);
                if (!err) err = read_block(v, next, p, 1);
                if (err) break;
                if (word(p, 0) != (next == key ? 2u : 16u) || word(p, 4) != next ||
                    word(p, v->block_size - 4) != UINT32_C(0xfffffffd) ||
                    (next != key && word(p, v->block_size - 12) != key)) {
                    err = FFS_CORRUPT; break;
                }
                n = count > v->buckets ? v->buckets : count;
                if (word(p, 8) != n) { err = FFS_CORRUPT; break; }
                for (j = 0; !err && j < n; ++j) {
                    block = word(p, 24 + (v->buckets - 1 - j) * 4);
                    err = mark_used(v, &used, block);
                    ++sequence;
                    if (!err && !v->format.fast) {
                        err = read_block(v, block, data, 1);
                        if (!err && (word(data, 0) != 8 || word(data, 4) != key ||
                            word(data, 8) != sequence || word(data, 12) > v->block_size - 24)) err = FFS_CORRUPT;
                    }
                }
                count -= n; next = word(p, v->block_size - 8);
                if (!err && (!count != !next)) err = FFS_CORRUPT;
                if (err || !next) break;
            }
        }
    }
    /* Every hard link must target a normally reachable real header. */
    for (i = 0; !err && i < queue.count; ++i) {
        err = ffs_stat(v, queue.keys[i], &entry);
        if (!err && entry.link && (!valid_key(v, entry.link) || !is_used(&used, entry.link)))
            err = FFS_CORRUPT;
    }
    /* Check backlink membership before a repair can free anything. */
    for (i = 0; !err && i < queue.count; ++i) {
        struct ffs_entry target, link;
        uint32_t seen = 0, chain;
        err = ffs_stat(v, queue.keys[i], &entry);
        if (err || !entry.link) continue;
        err = ffs_stat(v, entry.link, &target);
        chain = err ? 0 : target.backlink;
        while (!err && chain && chain != entry.key) {
            if (++seen > v->blocks) { err = FFS_CORRUPT; break; }
            err = ffs_stat(v, chain, &link);
            if (!err && link.link != entry.link) err = FFS_CORRUPT;
            if (!err) chain = link.backlink;
        }
        if (!err && chain != entry.key) err = FFS_CORRUPT;
    }
    /* Bitmap extension blocks have no checksum and end with a next pointer. */
    if (!err) err = header(v, v->root, p);
    next = err ? 0 : word(p, v->block_size - 96);
    while (!err && next) {
        err = mark_used(v, &used, next);
        if (!err) err = read_block(v, next, p, 0);
        if (!err) next = word(p, v->block_size - 4);
    }
    bits = (v->block_size - 4) * 8;
    for (page = 0; !err && page <= (v->blocks - v->reserved - 1) / bits; ++page) {
        err = bitmap_key(v, page, &mapkey);
        if (!err) err = mark_used(v, &used, mapkey);
    }
    /* Repair writes rebuilt pages directly, after clearing validity. */
    if (!err && repair && !v->disk_dirty) {
        err = invalidate_root(v, data);
        if (!err) v->disk_dirty = 1;
    }
    for (page = 0; !err && page <= (v->blocks - v->reserved - 1) / bits; ++page) {
        err = bitmap_key(v, page, &mapkey);
        if (!err && !repair) err = read_block(v, mapkey, p, 1);
        for (i = 0; !err && i < bits; i += 32) {
            /* The final page may extend past the highest 32-bit key. */
            uint64_t first = (uint64_t)v->reserved + (uint64_t)page * bits + i;
            uint32_t mask = range_bits(first, 0, v->blocks);
            value = mask ? ~used_bits(&used, first) & mask : 0;
            if (repair) put_word(p + 4 + i / 8, value);
            else if ((word(p, 4 + i / 8) & mask) != value) err = FFS_CORRUPT;
        }
        if (!err && repair) err = write_bitmap_page(v, mapkey, p);
    }
    if (!err && repair) {
        /* Rebuild cache records from their authoritative object headers. */
        for (i = 0; !err && i < queue.count; ++i) {
            err = ffs_stat(v, queue.keys[i], &entry);
            if (!err && (entry.type == 1 || entry.type == 2)) err = directory_cache(v, queue.keys[i]);
        }
    }
done:
    free_used(v, &used);
    if (queue.keys) v->io.free(v->io.context, queue.keys, queue.capacity * sizeof(*queue.keys));
    release(v, p); release(v, data);
    if (repair) {
        err = finish_change(v, err);
        if (!err) v->writable = v->validated = 1;
    }
    return err;
}

int ffs_relabel(struct ffs_volume *v, const char *name)
{
    unsigned char *p;
    int err = begin_change(v);
    if (err) return err;
    err = component(name);
    if (err) return err;
    p = buffer(v);
    if (!p) return FFS_NOMEM;
    err = header(v, v->root, p);
    if (!err) {
        memset(p + v->block_size - 80, 0, 32);
        p[v->block_size - 80] = (unsigned char)strlen(name);
        memcpy(p + v->block_size - 79, name, strlen(name));
        stamp(v, p); err = stage(v, v->root, p, 20);
    }
    release(v, p); return finish_change(v, err);
}
int ffs_format_volume(struct ffs_volume *v, uint32_t dos_type, const char *name)
{
    struct ffs_format format, previous_format = v->format;
    uint32_t previous_type = v->dos_type;
    unsigned char *p;
    uint32_t pages, extensions, first_map, first_ext, first_cache, last, page, i;
    uint32_t bits = (v->block_size - 4) * 8, slots = v->block_size / 4 - 1;
    int err;
    if (!v->write_requested || v->failed || v->changes) return FFS_WRITE_PROTECTED;
    err = ffs_flush(v);
    if (err) return err;
    refresh_date(v);
    err = component(name);
    if (err || ffs_decode_type(dos_type, &format)) return FFS_BAD_NAME;
    pages = (v->blocks - v->reserved - 1) / bits + 1;
    extensions = pages > 25 ? (pages - 26) / slots + 1 : 0;
    first_map = v->root + 1;
    first_ext = first_map + pages;
    first_cache = first_ext + extensions;
    last = first_cache + (format.dircache ? 1 : 0);
    if (last >= v->blocks || last < v->root) return FFS_FULL;
    p = buffer(v);
    if (!p) return FFS_NOMEM;
    v->read_file = 0;
    v->bitmap_block = v->bitmap_ext_key = 0; v->free_known = 0;
    drop_clean(v);
    v->format = format; v->dos_type = dos_type;
    /* Clear validity, then write the bitmap directly instead of staging
     * every page. Pages without the root, the new metadata or the volume
     * end are entirely free. */
    if (!v->disk_dirty) {
        err = invalidate_root(v, p);
        if (!err) v->disk_dirty = 1;
    }
    for (page = 0; !err && page < pages; ++page) {
        uint64_t start = (uint64_t)v->reserved + (uint64_t)page * bits;
        if (start + bits <= v->blocks && (start + bits <= v->root || start >= last))
            memset(p + 4, 0xff, v->block_size - 4);
        else for (i = 0; i < bits; i += 32)
            put_word(p + 4 + i / 8, range_bits(start + i, v->reserved, v->root) |
                                    range_bits(start + i, last, v->blocks));
        err = write_bitmap_page(v, first_map + page, p);
    }
    for (page = 0; !err && page < extensions; ++page) {
        memset(p, 0, v->block_size);
        for (i = 0; i < slots && 25 + page * slots + i < pages; ++i)
            put_word(p + i * 4, first_map + 25 + page * slots + i);
        put_word(p + v->block_size - 4, page + 1 < extensions ? first_ext + page + 1 : 0);
        if (v->io.write(v->io.context, first_ext + page, p, v->block_size)) err = FFS_IO;
    }
    if (err) v->failed = 1;
    if (!err && format.dircache) {
        memset(p, 0, v->block_size);
        put_word(p, 33); put_word(p + 4, first_cache); put_word(p + 8, v->root);
        err = stage(v, first_cache, p, 20);
    }
    if (!err) {
        memset(p, 0, v->block_size);
        put_word(p, 2); put_word(p + 12, v->buckets);
        for (page = 0; page < pages && page < 25; ++page)
            put_word(p + v->block_size - 196 + page * 4, first_map + page);
        put_word(p + v->block_size - 96, extensions ? first_ext : 0);
        put_word(p + v->block_size - 8, format.dircache ? first_cache : 0);
        put_word(p + v->block_size - 4, 1);
        p[v->block_size - 80] = (unsigned char)strlen(name);
        memcpy(p + v->block_size - 79, name, strlen(name)); stamp(v, p);
        for (i = 0; i < 3; ++i) put_word(p + v->block_size - 28 + i * 4, v->now[i]);
        err = stage(v, v->root, p, 20);
    }
    err = commit_changes(v, err, NULL);
    if (!err) {
        /* Non-bootable boot block: formatting must discard old boot code. */
        memset(p, 0, v->block_size);
        if (v->reserved > 1 && v->io.write(v->io.context, 1, p, v->block_size)) err = FFS_IO;
        put_word(p, dos_type); put_word(p + 8, v->root);
        if (!err && (v->io.write(v->io.context, 0, p, v->block_size) ||
                     v->io.flush(v->io.context))) err = FFS_IO;
        if (err) v->failed = 1;
        else {
            v->mounted = v->writable = v->validated = 1;
            for (i = 0; i < 3; ++i) v->created[i] = v->now[i];
        }
    }
    if (err && !v->failed) { v->format = previous_format; v->dos_type = previous_type; }
    release(v, p); return err;
}
