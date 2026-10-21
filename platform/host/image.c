#define _DARWIN_C_SOURCE
#define _DEFAULT_SOURCE
#define _POSIX_C_SOURCE 200809L
#include "ffs/fs.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>
#include <time.h>

static int image_read(void *ctx, uint32_t key, unsigned char *p, uint32_t size)
{
    FILE *f = ctx;
    return fseeko(f, (off_t)key * size, SEEK_SET) || fread(p, 1, size, f) != size;
}
static int image_write(void *ctx, uint32_t key, const unsigned char *p, uint32_t size)
{
    FILE *f = ctx;
    return fseeko(f, (off_t)key * size, SEEK_SET) || fwrite(p, 1, size, f) != size;
}
static int image_flush(void *ctx)
{
    FILE *f = ctx;
    return fflush(f) || fsync(fileno(f));
}
static int parent_path(struct ffs_volume *v, const char *path, uint32_t *key,
                        char name[31])
{
    struct ffs_entry e;
    char *copy = malloc(strlen(path) + 1), *slash, *leaf;
    int err;
    if (!copy) return FFS_NOMEM;
    strcpy(copy, path);
    slash = strrchr(copy, '/');
    if (!slash) slash = strrchr(copy, ':');
    leaf = slash ? slash + 1 : copy;
    if (strlen(leaf) > 30) { free(copy); return FFS_BAD_NAME; }
    strcpy(name, leaf);
    if (slash) slash[1] = 0;
    else copy[0] = 0;
    err = ffs_resolve(v, v->root, copy, &e);
    if (!err) *key = e.key;
    free(copy); return err;
}
static void *image_alloc(void *ctx, size_t size) { (void)ctx; return malloc(size); }
static void image_free(void *ctx, void *p, size_t size)
{ (void)ctx; (void)size; free(p); }
static int number(const char *s, uint32_t *n)
{
    char *end;
    unsigned long value = strtoul(s, &end, 0);
    if (!*s || *s == '-' || *end || value > UINT32_MAX) return -1;
    *n = (uint32_t)value;
    return 0;
}
int main(int argc, char **argv)
{
    struct ffs_volume volume = {0};
    struct ffs_entry entry;
    struct ffs_iterator it = {0, 0, 0, 0};
    struct ffs_io io;
    struct stat st;
    unsigned char bytes[8192];
    uint32_t offset = 0, length = UINT32_MAX, count, got;
    uint32_t size = 512;
    FILE *f;
    int err = 0, writable;
    char name[31];
    uint32_t parent = 0;
    const char *env = getenv("FFS_BLOCK_SIZE");
    if (argc < 4 || (env && number(env, &size)) || !size) {
        fprintf(stderr, "usage: ffs-image image COMMAND PATH [ARGS]\ncommands: list; cat [offset length]; mkdir; put host-file; rm;\n          rename new-path; truncate size; comment text; check; repair;\n          format variant\n");
        return 2;
    }
    writable = strcmp(argv[2], "cat") && strcmp(argv[2], "list") && strcmp(argv[2], "check");
    f = fopen(argv[1], writable ? "r+b" : "rb");
    if (!f) { perror(argv[1]); return 1; }
    if (flock(fileno(f), LOCK_EX | LOCK_NB) || fstat(fileno(f), &st) ||
        st.st_size % size || (uint64_t)st.st_size / size > UINT32_MAX) {
        fprintf(stderr, "image is locked or has invalid size\n"); fclose(f); return 1;
    }
    memset(&io, 0, sizeof(io));
    io.context = f; io.read = image_read; io.alloc = image_alloc; io.free = image_free;
    io.write = image_write; io.flush = image_flush;
    err = ffs_mount(&volume, &io, size, (uint32_t)(st.st_size / size), 2, writable);
    if (!strcmp(argv[2], "format") && volume.io.read) err = 0;
    if (!err) {
        time_t now = time(NULL) - 252460800;
        (void)ffs_cache_resize(&volume, 32);
        volume.now[0] = (uint32_t)(now / 86400);
        volume.now[1] = (uint32_t)(now % 86400 / 60);
        volume.now[2] = (uint32_t)(now % 60 * 50);
    }
    if (!err && !strcmp(argv[2], "format") && argc == 5) {
        uint32_t variant;
        if (number(argv[4], &variant) || variant > 5) err = FFS_BAD_NAME;
        else err = ffs_format_volume(&volume, FFS_DOS_BASE + variant, argv[3]);
        goto done;
    }
    if (!err && (!strcmp(argv[2], "check") || !strcmp(argv[2], "repair"))) {
        err = ffs_validate(&volume, !strcmp(argv[2], "repair")); goto done;
    }
    if (!err && (!strcmp(argv[2], "mkdir") || !strcmp(argv[2], "put"))) {
        uint32_t key = 0;
        err = parent_path(&volume, argv[3], &parent, name);
        if (!err) err = ffs_create(&volume, parent, name, !strcmp(argv[2], "mkdir"), &key);
        if (!err && !strcmp(argv[2], "put")) {
            FILE *input = argc > 4 ? fopen(argv[4], "rb") : NULL;
            if (!input) err = FFS_IO;
            while (!err && (count = (uint32_t)fread(bytes, 1, sizeof(bytes), input))) {
                err = ffs_write(&volume, key, offset, bytes, count, &got);
                offset += got;
            }
            if (input) { if (ferror(input)) err = FFS_IO; fclose(input); }
        }
        goto done;
    }
    if (!err && (!strcmp(argv[2], "rm") || !strcmp(argv[2], "rename"))) {
        err = parent_path(&volume, argv[3], &parent, name);
        if (!err) err = ffs_lookup(&volume, parent, (const unsigned char *)name, strlen(name), &entry);
    } else if (!err) err = ffs_resolve(&volume, volume.root, argv[3], &entry);
    if (!err && !strcmp(argv[2], "list")) {
        uint32_t directory = entry.key;
        while (!(err = ffs_next(&volume, directory, &it, &entry)))
            puts((const char *)entry.name);
        if (err == FFS_NO_MORE) err = 0;
    } else if (!err && !strcmp(argv[2], "cat")) {
        if ((argc > 4 && number(argv[4], &offset)) ||
            (argc > 5 && number(argv[5], &length))) err = FFS_BAD_NAME;
        while (!err && length) {
            count = length > sizeof(bytes) ? sizeof(bytes) : length;
            err = ffs_read(&volume, entry.key, offset, bytes, count, &got);
            if (got && fwrite(bytes, 1, got, stdout) != got) err = FFS_IO;
            if (!got) break;
            offset += got; length -= got;
        }
    } else if (!err && !strcmp(argv[2], "rm")) err = ffs_remove(&volume, entry.key);
    else if (!err && !strcmp(argv[2], "truncate") && argc == 5) {
        if (number(argv[4], &length)) err = FFS_BAD_NAME;
        else err = ffs_resize(&volume, entry.key, length);
    } else if (!err && !strcmp(argv[2], "comment") && argc == 5) {
        if (strlen(argv[4]) >= sizeof(entry.comment)) err = FFS_BAD_NAME;
        else {
            strcpy((char *)entry.comment, argv[4]);
            err = ffs_set_metadata(&volume, entry.key, &entry);
        }
    } else if (!err && !strcmp(argv[2], "rename") && argc == 5) {
        err = parent_path(&volume, argv[4], &parent, name);
        if (!err) err = ffs_rename(&volume, entry.key, parent, name);
    } else if (!err) err = FFS_BAD_NAME;
done:
    ffs_discard(&volume);
    if (fclose(f) && !err) err = FFS_IO;
    if (err) fprintf(stderr, "filesystem error %d\n", err);
    return err ? 1 : 0;
}
