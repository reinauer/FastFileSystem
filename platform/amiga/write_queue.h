#ifndef FFS_WRITE_QUEUE_H
#define FFS_WRITE_QUEUE_H

#include <stdint.h>
#include <string.h>

/* Buffers and requests belong to the caller and outlive drain. Copying
 * slots own a copy of submitted data; borrowing slots transfer the caller's
 * bytes, which must stay unchanged until drain. Completion errors remain
 * latched until reinitialization. */
#define FFS_QUEUE_COPIES 2
#define FFS_QUEUE_SLOTS 8
struct ffs_write_slot {
    void *request;
    unsigned char *bytes;
    const unsigned char *data;
    uint64_t offset;
    uint32_t length;
    unsigned pending;
};
struct ffs_write_queue {
    struct ffs_write_slot slots[FFS_QUEUE_SLOTS];
    void *context;
    void (*start)(void *, struct ffs_write_slot *);
    int (*finish)(void *, struct ffs_write_slot *);
    uint32_t capacity;
    unsigned next, next_borrowed, failed, busy;
};
static int ffs_queue_finish(struct ffs_write_queue *q, struct ffs_write_slot *s)
{
    if (s->pending) {
        if (q->finish(q->context, s)) q->failed = 1;
        s->pending = 0; --q->busy;
    }
    return q->failed ? -1 : 0;
}
static int ffs_queue_drain(struct ffs_write_queue *q)
{
    unsigned i;
    for (i = 0; q->busy && i < FFS_QUEUE_SLOTS; ++i) (void)ffs_queue_finish(q, &q->slots[i]);
    return q->failed ? -1 : 0;
}
/* Drivers may reorder requests. Serialize overlapping ranges. */
static int ffs_queue_submit(struct ffs_write_queue *q, struct ffs_write_slot *s,
                            uint64_t offset, uint32_t length)
{
    unsigned i;
    if (!length || length > UINT64_MAX - offset || ffs_queue_finish(q, s)) return -1;
    for (i = 0; q->busy && i < FFS_QUEUE_SLOTS; ++i) {
        struct ffs_write_slot *other = &q->slots[i];
        if (other->pending && offset < other->offset + other->length &&
            other->offset < offset + length && ffs_queue_finish(q, other)) return -1;
    }
    s->offset = offset; s->length = length;
    return 0;
}
static int ffs_queue_write(struct ffs_write_queue *q, uint64_t offset,
                           const unsigned char *data, uint32_t length)
{
    struct ffs_write_slot *s = &q->slots[q->next];
    if (length > q->capacity || ffs_queue_submit(q, s, offset, length)) return -1;
    memcpy(s->bytes, data, length);
    s->data = s->bytes;
    q->start(q->context, s); s->pending = 1; ++q->busy;
    q->next = (q->next + 1) % FFS_QUEUE_COPIES;
    return 0;
}
static int ffs_queue_borrow(struct ffs_write_queue *q, uint64_t offset,
                            const unsigned char *data, uint32_t length)
{
    struct ffs_write_slot *s = &q->slots[FFS_QUEUE_COPIES + q->next_borrowed];
    if (ffs_queue_submit(q, s, offset, length)) return -1;
    s->data = data;
    q->start(q->context, s); s->pending = 1; ++q->busy;
    q->next_borrowed = (q->next_borrowed + 1) % (FFS_QUEUE_SLOTS - FFS_QUEUE_COPIES);
    return 0;
}
#endif
