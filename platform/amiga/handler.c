#define __USE_OLD_TIMEVAL__
#define __NOLIBBASE__
#define __NOGLOBALIFACE__
#include <exec/types.h>
#include <exec/execbase.h>
#include <exec/memory.h>
#include <exec/io.h>
#include <exec/errors.h>
#include <exec/interrupts.h>
#include <devices/timer.h>
#include <devices/trackdisk.h>
#include <devices/newstyle.h>
#include <dos/dos.h>
#include <dos/dosextens.h>
#include <dos/filehandler.h>
#include <dos/exall.h>
#include <dos/notify.h>
#include <utility/hooks.h>
#ifdef __amigaos4__
#include <dos/obsolete.h>
#pragma GCC diagnostic ignored "-Wdeprecated-declarations"
#include <interfaces/exec.h>
#include <interfaces/dos.h>
#include <interfaces/utility.h>
#endif
#include <proto/exec.h>
#include <proto/dos.h>
#include <proto/utility.h>
#include <string.h>
#include "ffs/fs.h"
#include "write_queue.h"
#ifndef ENVF_DISABLENSD
#define ENVF_DISABLENSD (1L << 18)
#endif
#include "dma.h"
#include "version.h"
#if defined(FFS_PROFILE) || !defined(__amigaos4__)
#include <proto/timer.h>
#endif

/* Found by the Version command; the Makefile supplies both fields. */
static const char version[] __attribute__((__used__)) =
    "$VER: FastFileSystem " FFS_VERSION " (" FFS_DATE ")";

/* OS3 packets carry positions and sizes as LONGs, so files grow only to
 * 2 GiB - 1. Larger files written elsewhere remain readable. */
#ifdef __amigaos4__
#define FILE_LIMIT UINT32_MAX
#else
#define FILE_LIMIT INT32_MAX
#endif

/* Device addressing for the partition. */
enum { IO32, IO64_TD64, IO64_NSD };
struct file_lock {
    struct FileLock dos;
    struct file_lock *next;
    struct ffs_scan scan;
};
struct file_handle {
    struct file_handle *next;
    struct file_lock *lock;
    uint32_t position;
    struct ffs_file_state mapping;
};
struct record_lock {
    struct record_lock *next;
    struct file_handle *file;
    uint32_t start, length;
    unsigned shared;
};
struct record_wait {
    struct record_wait *next;
    struct DosPacket *packet;
    uint32_t ticks;
};
struct watch {
    struct watch *next;
    struct NotifyRequest *request;
    uint32_t key;
    unsigned active, outstanding, pending;
    char path[256];
};
struct notice {
    struct NotifyMessage message;
    struct watch *watch;
};
struct core_slab { struct core_slab *next; ULONG size; };
struct core_pool { void *free; struct core_slab *slabs; };
struct handler {
    struct ExecBase *exec;
    struct DosLibrary *dos;
#ifdef __amigaos4__
    struct ExecIFace *iexec;
    struct DOSIFace *idos;
#endif
    struct MsgPort *port, *io_port;
    struct IOStdReq *request, *change_request;
    struct Interrupt change_interrupt;
    struct ffs_write_queue writes;
    struct DeviceNode *device;
    struct DosList *volume;
    struct ffs_volume fs;
    struct file_lock *locks;
    struct file_handle *files;
    struct watch *watches;
    struct MsgPort *notify_port;
    unsigned notify_pending;
    uint32_t changed[4];
    unsigned num_changed;
    struct record_lock *records;
    struct record_wait *waiting;
    struct MsgPort *timer_port;
    struct timerequest *timer;
    unsigned timer_open, timer_pending, write_ticks, media_ticks, timer_ticks;
    uint32_t minute_start, date_days, date_minutes;
    uint64_t origin;
    uint32_t transfer, memory, address_mask;
    unsigned io64;
    struct core_pool pools[2];
    unsigned cache_count, media_known;
    volatile unsigned media_changed;
    uint32_t media_number;
    unsigned opened, quitting, inhibited, user_protected, protected_writable;
    unsigned copy_ready, borrow_ready, motor_on, motor_idle;
    /* An ACTION_WRITE buffer stays valid until the packet's reply, so its
     * final transfer may still run while the core finishes the write. */
    struct IOStdReq *packet_request;
    uintptr_t packet_data;
    uint32_t packet_size, packet_length;
    unsigned packet_pending;
    void *spare_locks, *spare_files;
    LONG password;
};
#define SysBase (h->exec)
#define DOSBase (h->dos)
#ifdef __amigaos4__
#define IExec (h->iexec)
#define IDOS (h->idos)
#endif

#if defined(FFS_PROFILE) || !defined(__amigaos4__)
#define TimerBase ((struct Device *)h->timer->tr_node.io_Device)
#endif
#ifdef FFS_PROFILE
static uint32_t profile_clock(void *ctx)
{
    struct handler *h = ctx;
    struct EClockVal value;
    if (!h->timer_open) return 0;
    ReadEClock(&value); return value.ev_lo;
}
#endif
static int dma_address(struct handler *h, const void *p, uint32_t size)
{
    return ffs_dma_range(h->address_mask, (uint32_t)(uintptr_t)p, size);
}
static void *memory_alloc(void *ctx, size_t size)
{
    struct handler *h = ctx;
    void *p;
    PROFILE_START(&h->fs);
    p = AllocMem(size, MEMF_PUBLIC | MEMF_CLEAR);
    PROFILE_END(&h->fs, FFS_P_MEMORY);
    return p;
}
/* Only buffers handed to the device need the mount's DMA restrictions. */
static void *dma_alloc(struct handler *h, uint32_t size)
{
    void *p = AllocMem(size, MEMF_PUBLIC | h->memory);
    if (p && dma_address(h, p, size)) return p;
    if (p) FreeMem(p, size);
    if (h->memory & (MEMF_CHIP | MEMF_FAST)) return NULL;
    p = AllocMem(size, MEMF_PUBLIC | MEMF_CHIP);
    if (p && !dma_address(h, p, size)) { FreeMem(p, size); p = NULL; }
    return p;
}
static void memory_free(void *ctx, void *p, size_t size)
{
    struct handler *h = ctx;
    FreeMem(p, size);
}
/* Locks and file handles are recycled: Exec allocation dominates their
 * cost. Recycled storage is cleared like a new allocation. */
static void *spare_alloc(struct handler *h, void **spares, size_t size)
{
    void *p = *spares;
    if (!p) return memory_alloc(h, size);
    *spares = *(void **)p; memset(p, 0, size);
    return p;
}
static void spare_free(void **spares, void *p)
{
    *(void **)p = *spares; *spares = p;
}
static void free_spares(struct handler *h, void **spares, size_t size)
{
    void *p;
    while ((p = *spares)) { *spares = *(void **)p; memory_free(h, p, size); }
}
/* The portable core already initializes its allocations (host tests use
 * malloc). Recycle fixed-size scratch and staging cells without clearing. */
static unsigned core_class(struct handler *h, size_t size)
{
    if (size == h->fs.block_size) return 0;
    if (size == ffs_change_size(&h->fs)) return 1;
    return 2;
}
static void *core_alloc(void *ctx, size_t size)
{
    struct handler *h = ctx;
    unsigned which = core_class(h, size), i;
    struct core_pool *pool;
    struct core_slab *slab;
    unsigned char *cell;
    void *result;
    PROFILE_START(&h->fs);
    if (which == 2) result = AllocMem(size, MEMF_PUBLIC);
    else {
        pool = &h->pools[which];
        if (!pool->free) {
            ULONG total = sizeof(*slab) + 16 * size;
            slab = AllocMem(total, MEMF_PUBLIC);
            if (!slab) {
                /* One-cell slabs retain the same free path on low memory. */
                total = sizeof(*slab) + size;
                slab = AllocMem(total, MEMF_PUBLIC);
            }
            if (!slab) { PROFILE_END(&h->fs, FFS_P_MEMORY); return NULL; }
            slab->size = total; slab->next = pool->slabs; pool->slabs = slab;
            cell = (unsigned char *)(slab + 1);
            for (i = 0; i < (total - sizeof(*slab)) / size; ++i) {
                *(void **)cell = pool->free; pool->free = cell; cell += size;
            }
        }
        result = pool->free; pool->free = *(void **)result;
    }
    PROFILE_END(&h->fs, FFS_P_MEMORY);
    return result;
}
static void core_free(void *ctx, void *p, size_t size)
{
    struct handler *h = ctx;
    unsigned which = core_class(h, size);
    if (which == 2) FreeMem(p, size);
    else { *(void **)p = h->pools[which].free; h->pools[which].free = p; }
}
static void free_core_pools(struct handler *h)
{
    unsigned i;
    struct core_slab *slab;
    for (i = 0; i < 2; ++i) {
        while ((slab = h->pools[i].slabs)) {
            h->pools[i].slabs = slab->next; FreeMem(slab, slab->size);
        }
        h->pools[i].free = NULL;
    }
}
static void clear_cache(struct handler *h)
{
    (void)ffs_cache_resize(&h->fs, 0);
    h->fs.cache_limit = h->cache_count;
}
static unsigned resize_cache(struct handler *h, unsigned count)
{
    h->cache_count = ffs_cache_resize(&h->fs, count);
    return h->cache_count;
}
/* NDK 3.2 prefixes the query result fields with nsdqr_; the OS4 SDK does
 * not. */
#ifdef __amigaos4__
#define nsdqr_SizeAvailable SizeAvailable
#define nsdqr_DeviceType DeviceType
#define nsdqr_SupportedCommands SupportedCommands
#endif
/* Prefer NSD, which the device must list, then probe TD64 with a transfer
 * of zero bytes. */
static unsigned query_io64(struct handler *h, ULONG flags)
{
    struct NSDeviceQueryResult result;
    UWORD *command;
    unsigned reads = 0, writes = 0;
    if (!(flags & ENVF_DISABLENSD)) {
        memset(&result, 0, sizeof(result));
        h->request->io_Command = NSCMD_DEVICEQUERY;
        h->request->io_Data = &result; h->request->io_Length = sizeof(result);
        if (!DoIO((struct IORequest *)h->request) && h->request->io_Actual >= 16 &&
            result.nsdqr_SizeAvailable == h->request->io_Actual &&
            result.nsdqr_DeviceType == NSDEVTYPE_TRACKDISK && result.nsdqr_SupportedCommands)
            for (command = result.nsdqr_SupportedCommands; *command; ++command) {
                if (*command == NSCMD_TD_READ64) reads = 1;
                if (*command == NSCMD_TD_WRITE64) writes = 1;
            }
        if (reads && writes) return IO64_NSD;
    }
    h->request->io_Command = TD_READ64; h->request->io_Data = &result;
    h->request->io_Length = 0; h->request->io_Offset = 0; h->request->io_Actual = 0;
    if (!DoIO((struct IORequest *)h->request) ||
        h->request->io_Error != IOERR_NOCMD) return IO64_TD64;
    return IO32;
}
/* Mount write-protected media read-only. Devices without the command
 * report an error and stay writable. */
static unsigned media_protected(struct handler *h)
{
    h->request->io_Command = TD_PROTSTATUS;
    return !DoIO((struct IORequest *)h->request) && h->request->io_Actual;
}
static int remount_volume(struct handler *h)
{
    struct ffs_io io = h->fs.io;
    uint32_t size = h->fs.block_size, blocks = h->fs.blocks, reserved = h->fs.reserved;
    int err;
    ffs_discard(&h->fs);
    err = ffs_mount(&h->fs, &io, size, blocks, reserved,
                    !h->user_protected && !media_protected(h));
    h->fs.cache_limit = h->cache_count;
    h->fs.file_limit = FILE_LIMIT;
    return err;
}
/* The callback only invalidates the medium and wakes its owning task. */
#ifdef __amigaos4__
static void media_interrupt(LONG unused, struct ExecBase *exec, APTR data)
{
    struct handler *h = data;
    (void)unused; (void)exec;
#else
static LONG media_interrupt(APTR data __asm("a1"))
{
    struct handler *h = data;
#endif
    h->media_changed = 1;
    Signal(h->port->mp_SigTask, 1UL << h->port->mp_SigBit);
#ifndef __amigaos4__
    return 0;
#endif
}
static void install_media_interrupt(struct handler *h)
{
    struct IOStdReq *r;
    r = (struct IOStdReq *)CreateIORequest(h->io_port, sizeof(*r));
    if (!r) return;
    r->io_Device = h->request->io_Device; r->io_Unit = h->request->io_Unit;
    h->change_interrupt.is_Node.ln_Type = NT_INTERRUPT;
    h->change_interrupt.is_Node.ln_Name = (char *)"ffs-media";
    h->change_interrupt.is_Data = h;
    h->change_interrupt.is_Code = (void (*)(void))(APTR)media_interrupt;
    r->io_Command = TD_ADDCHANGEINT;
    r->io_Data = &h->change_interrupt; r->io_Length = sizeof(h->change_interrupt);
    h->change_request = r;
    SendIO((struct IORequest *)r);
}
static unsigned media_interrupt_active(struct handler *h)
{
    if (!h->change_request) return 0;
    if (!CheckIO((struct IORequest *)h->change_request)) return 1;
    /* Unsupported commands complete instead of retaining the request. */
    WaitIO((struct IORequest *)h->change_request);
    DeleteIORequest((struct IORequest *)h->change_request);
    h->change_request = NULL;
    return 0;
}
static void remove_media_interrupt(struct handler *h)
{
    if (!media_interrupt_active(h)) return;
    h->change_request->io_Command = TD_REMCHANGEINT;
    DoIO((struct IORequest *)h->change_request);
    DeleteIORequest((struct IORequest *)h->change_request);
    h->change_request = NULL;
}
/* Partitions ending beyond 4 GiB use NSD or TD64 commands, which take the
 * offset's high 32 bits in io_Actual. */
static void prepare_transfer(struct handler *h, struct IOStdReq *r, UWORD command, uint64_t offset)
{
    unsigned write = command == CMD_WRITE;
    r->io_Command = h->io64 == IO64_NSD ? (write ? NSCMD_TD_WRITE64 : NSCMD_TD_READ64) :
                    h->io64 == IO64_TD64 ? (write ? TD_WRITE64 : TD_READ64) : command;
    r->io_Offset = (ULONG)offset; r->io_Actual = (ULONG)(offset >> 32);
}
static void start_write(void *context, struct ffs_write_slot *s)
{
    struct handler *h = context;
    struct IOStdReq *r = s->request;
    prepare_transfer(h, r, CMD_WRITE, s->offset);
    r->io_Data = (APTR)s->data; r->io_Length = s->length;
    SendIO((struct IORequest *)r);
}
static int finish_write(void *context, struct ffs_write_slot *s)
{
    struct handler *h = context;
    struct IOStdReq *r = s->request;
    int err;
    PROFILE_START(&h->fs);
    if (h->media_changed && !CheckIO((struct IORequest *)r)) AbortIO((struct IORequest *)r);
    err = WaitIO((struct IORequest *)r) || r->io_Actual != s->length || h->media_changed;
    PROFILE_END(&h->fs, FFS_P_WAIT);
    return err ? -1 : 0;
}
static int finish_packet_write(struct handler *h)
{
    struct IOStdReq *r = h->packet_request;
    int err;
    if (!h->packet_pending) return 0;
    h->packet_pending = 0;
    PROFILE_START(&h->fs);
    if (h->media_changed && !CheckIO((struct IORequest *)r)) AbortIO((struct IORequest *)r);
    err = WaitIO((struct IORequest *)r) || r->io_Actual != h->packet_length || h->media_changed;
    PROFILE_END(&h->fs, FFS_P_WAIT);
    /* The core already counts the data as written. */
    if (err) h->fs.failed = 1;
    return err ? -1 : 0;
}
static int drain_writes(struct handler *h)
{
    if (ffs_queue_drain(&h->writes)) h->fs.failed = 1;
    return h->writes.failed || h->media_changed;
}
static void init_write_requests(struct handler *h, uint32_t block_size)
{
    unsigned i;
    h->writes.capacity = block_size > 16384 ? block_size : 16384;
    h->writes.context = h; h->writes.start = start_write; h->writes.finish = finish_write;
    h->copy_ready = h->borrow_ready = 1;
    for (i = 0; i < FFS_QUEUE_SLOTS; ++i) {
        struct ffs_write_slot *s = &h->writes.slots[i];
        struct IOStdReq *r = (struct IOStdReq *)CreateIORequest(h->io_port, sizeof(*r));
        s->request = r;
        if (i < FFS_QUEUE_COPIES) s->bytes = dma_alloc(h, h->writes.capacity);
        if (r) {
            r->io_Device = h->request->io_Device; r->io_Unit = h->request->io_Unit;
        }
        if (i < FFS_QUEUE_COPIES && (!r || !s->bytes)) h->copy_ready = 0;
        if (i >= FFS_QUEUE_COPIES && !r) h->borrow_ready = 0;
    }
    h->packet_request = (struct IOStdReq *)CreateIORequest(h->io_port, sizeof(*h->packet_request));
    if (h->packet_request) {
        h->packet_request->io_Device = h->request->io_Device;
        h->packet_request->io_Unit = h->request->io_Unit;
    }
}
static void free_write_requests(struct handler *h)
{
    unsigned i;
    (void)finish_packet_write(h); (void)drain_writes(h);
    if (h->packet_request) DeleteIORequest((struct IORequest *)h->packet_request);
    for (i = 0; i < FFS_QUEUE_SLOTS; ++i) {
        struct ffs_write_slot *s = &h->writes.slots[i];
        if (s->request) DeleteIORequest((struct IORequest *)s->request);
        if (s->bytes) memory_free(h, s->bytes, h->writes.capacity);
    }
}

static int check_media(struct handler *h)
{
    PROFILE_START(&h->fs);
    h->request->io_Command = TD_CHANGENUM;
    if (!DoIO((struct IORequest *)h->request)) {
        if (h->media_known && h->media_number != h->request->io_Actual) h->media_changed = 1;
        h->media_number = h->request->io_Actual; h->media_known = 1;
    }
    if (h->media_changed) {
        clear_cache(h); PROFILE_END(&h->fs, FFS_P_MEDIA); return -1;
    }
    PROFILE_END(&h->fs, FFS_P_MEDIA);
    return 0;
}
/* Switch a floppy motor off once transfers have stopped. Devices without
 * the command ignore it or fail harmlessly. */
static void motor_off(struct handler *h)
{
    if (!h->motor_on || drain_writes(h)) return;
    h->request->io_Command = TD_MOTOR; h->request->io_Length = 0;
    (void)DoIO((struct IORequest *)h->request);
    h->motor_on = h->motor_idle = 0;
}
/* Transfer modes: copy through owned DMA buffers, transfer the caller's
 * DMA-compatible buffer synchronously, borrow it until the next drain, or
 * leave a write packet's final transfer running until the next device
 * access or the reply. */
enum { IO_COPY, IO_DIRECT, IO_BORROW, IO_PACKET };
static int device_io_impl(struct handler *h, uint32_t block, void *data,
                      uint32_t size, UWORD command, unsigned mode)
{
    uint64_t absolute;
    unsigned char *p = data;
    unsigned direct = mode == IO_DIRECT || mode == IO_PACKET;
    if (mode == IO_BORROW && !h->borrow_ready) mode = IO_COPY;
    if ((h->packet_pending && finish_packet_write(h)) || h->media_changed || h->writes.failed)
        return -1;
    if (!media_interrupt_active(h) && check_media(h)) return -1;
    h->motor_on = 1; h->motor_idle = 0;
    /* Idle queues need no drain: failures are latched and checked above. */
    if ((direct || command != CMD_WRITE) && h->writes.busy && drain_writes(h)) return -1;
    /* Startup checked the partition against the device's addressing. */
    if (block >= h->fs.blocks ||
        size > ((uint64_t)(h->fs.blocks - block) << h->fs.block_shift)) return -1;
    absolute = h->origin + ((uint64_t)block << h->fs.block_shift);
    while (size) {
        uint32_t n = size > h->transfer ? h->transfer : size;
        if (h->media_changed) return -1;
        if (mode == IO_BORROW) {
            if (ffs_queue_borrow(&h->writes, absolute, p, n)) return -1;
            p += n; size -= n; absolute += n;
            continue;
        }
        if (!direct && command == CMD_WRITE && h->copy_ready) {
            if (n > h->writes.capacity) n = h->writes.capacity;
            if (ffs_queue_write(&h->writes, absolute, p, n)) return -1;
            p += n; size -= n; absolute += n;
            continue;
        }
        {
            unsigned char *dma = direct ? p : h->writes.slots[0].bytes;
            struct IOStdReq *r = mode == IO_PACKET && n == size ? h->packet_request : h->request;
            unsigned allocated = 0;
            int err;
            if (!direct && n > h->writes.capacity) n = h->writes.capacity;
            if (!dma) {
                n = n > h->fs.block_size ? h->fs.block_size : n;
                dma = dma_alloc(h, n); allocated = 1;
            }
            if (!dma) return -1;
            if (!direct && command == CMD_WRITE) memcpy(dma, p, n);
            prepare_transfer(h, r, command, absolute);
            r->io_Data = dma;
            r->io_Length = n;
            if (r == h->packet_request) {
                SendIO((struct IORequest *)r);
                h->packet_length = n; h->packet_pending = 1;
                return 0;
            }
            if (direct) {
                SendIO((struct IORequest *)r);
                err = WaitIO((struct IORequest *)r);
            } else err = DoIO((struct IORequest *)r);
            err = err || r->io_Actual != n || h->media_changed;
            if (!err && !direct && command == CMD_READ) memcpy(p, dma, n);
            if (allocated) FreeMem(dma, n);
            if (err) return -1;
        }
        p += n; size -= n; absolute += n;
    }
    return 0;
}
static int device_io(struct handler *h, uint32_t block, void *data, uint32_t size,
                     UWORD command, unsigned mode)
{
    int err;
    PROFILE_START(&h->fs);
    err = device_io_impl(h, block, data, size, command, mode);
    PROFILE_END(&h->fs, FFS_P_DEVICE);
    return err;
}
/* Reads transfer directly into DMA-compatible destinations, including cache
 * cells; other destinations use the reusable DMA buffer. */
static int device_read(void *ctx, uint32_t block, unsigned char *data, uint32_t size)
{
    return device_io(ctx, block, data, size, CMD_READ,
                     dma_address(ctx, data, size) ? IO_DIRECT : IO_COPY);
}
static int device_write(void *ctx, uint32_t block, const unsigned char *data, uint32_t size)
{
    return device_io(ctx, block, (void *)data, size, CMD_WRITE, IO_COPY);
}
/* Transfer a DMA-compatible extent directly instead of copying it into Chip
 * RAM; owned-buffer queuing remains the fallback. Data inside the current
 * write packet's buffer may finish after the call. */
static int device_write_extent(void *ctx, uint32_t block, const unsigned char *data, uint32_t size)
{
    struct handler *h = ctx;
    uintptr_t offset = (uintptr_t)data - h->packet_data;
    unsigned mode = dma_address(ctx, data, size) ? IO_DIRECT : IO_COPY;
    if (mode == IO_DIRECT && h->packet_request && offset < h->packet_size &&
        size <= h->packet_size - offset) mode = IO_PACKET;
    return device_io(ctx, block, (void *)data, size, CMD_WRITE, mode);
}
/* The core keeps these bytes unchanged until its next flush, so several
 * metadata writes can be in flight before one drain. */
static int device_write_owned(void *ctx, uint32_t block, const unsigned char *data, uint32_t size)
{
    return device_io(ctx, block, (void *)data, size, CMD_WRITE,
                     dma_address(ctx, data, size) ? IO_BORROW : IO_COPY);
}
static int device_flush(void *ctx)
{
    struct handler *h = ctx;
    int err;
    PROFILE_START(&h->fs);
    err = finish_packet_write(h) || drain_writes(h);
    if (!err) {
        h->request->io_Command = CMD_UPDATE;
        err = DoIO((struct IORequest *)h->request) != 0;
    }
    PROFILE_END(&h->fs, FFS_P_SYNC);
    return err ? -1 : 0;
}
static void reply(struct handler *h, struct DosPacket *pkt, LONG result, LONG error)
{
    struct MsgPort *destination = pkt->dp_Port;
    pkt->dp_Res1 = result; pkt->dp_Res2 = error;
    pkt->dp_Port = h->port;
    pkt->dp_Link->mn_Node.ln_Name = (char *)pkt;
    PutMsg(destination, pkt->dp_Link);
}
#ifdef __amigaos4__
/* 64-bit packets keep dp_Res0, so DOS can tell that this handler knew them. */
static void reply64(struct handler *h, struct DosPacket *pkt, int64_t result, LONG error)
{
    struct DosPacket64 *wide = (struct DosPacket64 *)pkt;
    struct MsgPort *destination = wide->dp_Port;
    wide->dp_Res1 = result; wide->dp_Res2 = error;
    wide->dp_Port = h->port;
    wide->dp_Link->mn_Node.ln_Name = (char *)pkt;
    PutMsg(destination, wide->dp_Link);
}
#endif
static void from_bstr(char *dst, BPTR str)
{
    const unsigned char *p = BADDR(str);
    if (!p) { dst[0] = 0; return; }
    memcpy(dst, p + 1, *p); dst[*p] = 0;
}
static struct file_lock *get_lock(struct handler *h, BPTR bptr)
{
    struct file_lock *p;
    for (p = h->locks; p; p = p->next)
        if (MKBADDR(&p->dos) == bptr) return p;
    return NULL;
}
static int lock_key(struct handler *h, BPTR bptr, uint32_t *key)
{
    struct file_lock *p;
    if (!bptr) { *key = h->fs.root; return 0; }
    p = get_lock(h, bptr);
    if (!p) return ERROR_INVALID_LOCK;
    *key = p->dos.fl_Key;
    return 0;
}
/* Whether an outstanding lock conflicts with access of the given mode. */
static int in_use(struct handler *h, uint32_t key, LONG mode)
{
    struct file_lock *p;
    for (p = h->locks; p; p = p->next)
        if ((uint32_t)p->dos.fl_Key == key &&
            (mode == EXCLUSIVE_LOCK || p->dos.fl_Access == EXCLUSIVE_LOCK)) return 1;
    return 0;
}
static int new_lock(struct handler *h, uint32_t key, LONG mode, BPTR *result)
{
    struct file_lock *p;
    if (mode != SHARED_LOCK && mode != EXCLUSIVE_LOCK) return ERROR_INVALID_LOCK;
    if (in_use(h, key, mode)) return ERROR_OBJECT_IN_USE;
    p = spare_alloc(h, &h->spare_locks, sizeof(*p));
    if (!p) return ERROR_NO_FREE_STORE;
    p->dos.fl_Key = key; p->dos.fl_Access = mode;
    p->dos.fl_Task = h->port; p->dos.fl_Volume = MKBADDR(h->volume);
    p->next = h->locks;
    p->dos.fl_Link = h->locks ? MKBADDR(&h->locks->dos) : 0;
    h->locks = p;
    h->volume->dol_misc.dol_volume.dol_LockList = MKBADDR(&p->dos);
    *result = MKBADDR(&p->dos);
    return 0;
}
static int free_lock(struct handler *h, BPTR lock)
{
    struct file_lock **p = &h->locks, *found;
    if (!lock) return 0;
    while (*p && MKBADDR(&(*p)->dos) != lock) p = &(*p)->next;
    if (!*p) return ERROR_INVALID_LOCK;
    found = *p; *p = found->next;
    {
        struct file_lock *it;
        for (it = h->locks; it; it = it->next)
            it->dos.fl_Link = it->next ? MKBADDR(&it->next->dos) : 0;
        h->volume->dol_misc.dol_volume.dol_LockList = h->locks ? MKBADDR(&h->locks->dos) : 0;
    }
    ffs_scan_end(&h->fs, &found->scan);
    spare_free(&h->spare_locks, found);
    return 0;
}
static struct file_handle *get_file(struct handler *h, LONG arg)
{
    struct file_handle *p;
    for (p = h->files; p; p = p->next) if ((LONG)p == arg) return p;
    return NULL;
}
static int acquire_record(struct handler *h, struct DosPacket *pkt)
{
    struct file_handle *file = get_file(h, pkt->dp_Arg1);
    struct record_lock *record;
    uint32_t start = pkt->dp_Arg2, length = pkt->dp_Arg3;
    unsigned shared = ((ULONG)pkt->dp_Arg4 & 2) != 0;
    if (!file) return ERROR_INVALID_LOCK;
    if (!length || pkt->dp_Arg4 < 0 || pkt->dp_Arg4 > 3) return ERROR_BAD_NUMBER;
    for (record = h->records; record; record = record->next) {
        if (record->file == file || record->file->lock->dos.fl_Key != file->lock->dos.fl_Key) continue;
        if ((!record->shared || !shared) && (uint64_t)start < (uint64_t)record->start + record->length &&
            (uint64_t)record->start < (uint64_t)start + length) return ERROR_LOCK_COLLISION;
    }
    record = memory_alloc(h, sizeof(*record));
    if (!record) return ERROR_NO_FREE_STORE;
    record->file = file; record->start = start; record->length = length;
    record->shared = shared; record->next = h->records; h->records = record;
    return 0;
}
static int start_record_timer(struct handler *h)
{
    if (!h->timer) {
        if (!h->timer_port) h->timer_port = CreateMsgPort();
        if (!h->timer_port) return ERROR_NO_FREE_STORE;
        h->timer = (struct timerequest *)CreateIORequest(h->timer_port, sizeof(*h->timer));
        if (!h->timer) return ERROR_NO_FREE_STORE;
        if (OpenDevice((CONST_STRPTR)TIMERNAME, UNIT_VBLANK, (struct IORequest *)h->timer, 0))
            return ERROR_DEVICE_NOT_MOUNTED;
        h->timer_open = 1;
    }
    if (!h->timer_open) return ERROR_DEVICE_NOT_MOUNTED;
    if (h->timer_pending && h->waiting && h->timer_ticks > 1) {
        AbortIO((struct IORequest *)h->timer);
        WaitIO((struct IORequest *)h->timer);
        h->timer_pending = 0;
    }
    if (!h->timer_pending && h->opened) {
        h->timer_ticks = h->waiting ? 1 : 10;
        h->timer->tr_node.io_Command = TR_ADDREQUEST;
        h->timer->tr_time.tv_secs = 0; h->timer->tr_time.tv_micro = h->timer_ticks * 20000;
        SendIO((struct IORequest *)h->timer); h->timer_pending = 1;
    }
    return 0;
}
static void service_records(struct handler *h, unsigned tick)
{
    struct record_wait **p = &h->waiting;
    while (*p) {
        struct record_wait *wait = *p;
        int err = acquire_record(h, wait->packet);
        if (err == ERROR_LOCK_COLLISION) {
            if (tick && wait->ticks) --wait->ticks;
            if (wait->ticks) { p = &wait->next; continue; }
            err = ERROR_LOCK_TIMEOUT;
        }
        *p = wait->next;
        reply(h, wait->packet, err ? DOSFALSE : DOSTRUE, err);
        memory_free(h, wait, sizeof(*wait));
    }
    if (h->waiting) (void)start_record_timer(h);
}
static void release_records(struct handler *h, struct file_handle *file)
{
    struct record_lock **p = &h->records;
    struct record_wait **w = &h->waiting;
    while (*p) {
        struct record_lock *record = *p;
        if (record->file != file) { p = &record->next; continue; }
        *p = record->next; memory_free(h, record, sizeof(*record));
    }
    while (*w) {
        struct record_wait *wait = *w;
        if (wait->packet->dp_Arg1 != (LONG)file) { w = &wait->next; continue; }
        *w = wait->next;
        reply(h, wait->packet, DOSFALSE, ERROR_INVALID_LOCK);
        memory_free(h, wait, sizeof(*wait));
    }
    service_records(h, 0);
}
/* Owner and reserved fields belong to the caller: DOS clears them before
 * examining, and OS4 keeps private context there. */
static void fill_fib(const struct ffs_entry *entry, struct FileInfoBlock *fib)
{
    size_t length;
    fib->fib_DiskKey = entry->key;
    fib->fib_DirEntryType = entry->type;
#ifdef __amigaos4__
    fib->fib_Obsolete = entry->type;
#else
    fib->fib_EntryType = entry->type;
#endif
    fib->fib_Size = entry->size;
    fib->fib_Protection = entry->protection;
    fib->fib_OwnerUID = (UWORD)(entry->owner >> 16);
    fib->fib_OwnerGID = (UWORD)entry->owner;
    fib->fib_NumBlocks = (entry->size >> 9) + ((entry->size & 511u) != 0);
    fib->fib_Date.ds_Days = entry->days;
    fib->fib_Date.ds_Minute = entry->minutes;
    fib->fib_Date.ds_Tick = entry->ticks;
    /* BSTRs followed by their terminators. */
    length = strlen((const char *)entry->name);
    fib->fib_FileName[0] = (char)length;
    /* A 107-byte name fills the field; its terminator would not fit. */
    memcpy(fib->fib_FileName + 1, entry->name,
           length < sizeof(fib->fib_FileName) - 1 ? length + 1 : length);
    length = strlen((const char *)entry->comment);
    fib->fib_Comment[0] = (char)length;
    /* So does a 79-byte comment. */
    memcpy(fib->fib_Comment + 1, entry->comment,
           length < sizeof(fib->fib_Comment) - 1 ? length + 1 : length);
}
static int parent_name(struct handler *h, BPTR lock, char *path,
                         uint32_t *parent, char **name)
{
    struct ffs_entry e;
    char *p = path, *last = NULL;
    int err = lock_key(h, lock, parent);
    while (*p) { if (*p == '/' || *p == ':') last = p; ++p; }
    *name = last ? last + 1 : path;
    if (!err && last) {
        /* A trailing slash can be a parent component, not just a separator.
         * Keep it in the prefix so /leaf and dir//leaf retain their meaning. */
        char save = last[1];
        last[1] = 0;
        err = ffs_resolve(&h->fs, *parent, path, &e);
        last[1] = save;
        if (!err) *parent = e.key;
    }
    return err;
}
static void send_notice(struct handler *h, struct watch *watch)
{
    struct NotifyRequest *request = watch->request;
    if (request->nr_Flags & NRF_SEND_SIGNAL) {
        if (request->nr_stuff.nr_Signal.nr_SignalNum < 32)
            Signal(request->nr_stuff.nr_Signal.nr_Task, 1UL << request->nr_stuff.nr_Signal.nr_SignalNum);
    }
    if (request->nr_Flags & NRF_SEND_MESSAGE) {
        struct notice *notice;
        if ((request->nr_Flags & NRF_WAIT_REPLY) && watch->outstanding) { watch->pending = 1; return; }
        notice = memory_alloc(h, sizeof(*notice));
        if (!notice) { watch->pending = 1; return; }
        notice->watch = watch;
        notice->message.nm_Class = NOTIFY_CLASS; notice->message.nm_Code = NOTIFY_CODE;
        notice->message.nm_NReq = request;
        notice->message.nm_ExecMessage.mn_Node.ln_Type = NT_MESSAGE;
        notice->message.nm_ExecMessage.mn_ReplyPort = h->notify_port;
        notice->message.nm_ExecMessage.mn_Length = sizeof(struct NotifyMessage);
        ++watch->outstanding; ++h->notify_pending; ++request->nr_MsgCount;
        PutMsg(request->nr_stuff.nr_Msg.nr_Port, &notice->message.nm_ExecMessage);
    }
}
static void returned_notices(struct handler *h)
{
    struct Message *message;
    while ((message = GetMsg(h->notify_port))) {
        struct notice *notice = (struct notice *)message;
        struct watch *watch = notice->watch;
        --watch->outstanding; --h->notify_pending;
        if (watch->active) {
            --watch->request->nr_MsgCount;
            if (watch->pending) { watch->pending = 0; send_notice(h, watch); }
        } else if (!watch->outstanding) memory_free(h, watch, sizeof(*watch));
        memory_free(h, notice, sizeof(*notice));
    }
}
static void changed(struct handler *h, uint32_t key)
{
    struct ffs_entry e;
    if (!h->watches) return;
    if (h->num_changed < 4) h->changed[h->num_changed++] = key;
    if (!ffs_stat(&h->fs, key, &e) && e.parent && h->num_changed < 4)
        h->changed[h->num_changed++] = e.parent;
}
static void notify_changes(struct handler *h)
{
    struct watch *watch;
    for (watch = h->watches; watch; watch = watch->next) {
        struct ffs_entry e;
        uint32_t key = ffs_resolve(&h->fs, h->fs.root, watch->path, &e) ? 0 : e.key;
        unsigned i;
        for (i = 0; i < h->num_changed; ++i)
            if (h->changed[i] == key || h->changed[i] == watch->key) { send_notice(h, watch); break; }
        watch->key = key;
    }
}
static int call_match(struct handler *h, struct Hook *hook, LONG *type,
                       struct ExAllData *data)
{
    struct Library *UtilityBase = OpenLibrary((CONST_STRPTR)"utility.library", 37);
    ULONG result = 0;
#ifdef __amigaos4__
    struct UtilityIFace *IUtility = UtilityBase ?
        (struct UtilityIFace *)GetInterface(UtilityBase, "main", 1, NULL) : NULL;
    if (IUtility) {
        result = CallHookPkt(hook, type, data);
        DropInterface((struct Interface *)IUtility);
    }
#else
    if (UtilityBase) result = CallHookPkt(hook, type, data);
#endif
    if (UtilityBase) CloseLibrary(UtilityBase);
    return result != 0;
}
static int examine_all(struct handler *h, uint32_t dir, struct ffs_scan *scan,
                       struct DosPacket *pkt)
{
    struct ExAllControl *control = (void *)pkt->dp_Arg5;
    struct ExAllData item, *last = NULL, *dest;
    struct ffs_entry e;
    unsigned char *out = (void *)pkt->dp_Arg2;
    uint32_t left = pkt->dp_Arg3, fixed, need, names, comments;
    uint32_t previous = control->eac_LastKey ? control->eac_LastKey : dir;
    int err;
    const uint32_t sizes[] = {0, 8, 12, 16, 20, 32, 36, 40};
    control->eac_Entries = 0;
    if (pkt->dp_Arg4 < ED_NAME || pkt->dp_Arg4 > ED_OWNER) return ERROR_BAD_NUMBER;
    fixed = sizes[pkt->dp_Arg4];
    while (!(err = ffs_scan_next(&h->fs, dir, scan, previous, &e))) {
        memset(&item, 0, sizeof(item));
        item.ed_Name = (STRPTR)e.name; item.ed_Type = e.type;
        item.ed_Size = e.size; item.ed_Prot = e.protection;
        item.ed_Days = e.days; item.ed_Mins = e.minutes; item.ed_Ticks = e.ticks;
        item.ed_Comment = (STRPTR)e.comment;
        item.ed_OwnerUID = (UWORD)(e.owner >> 16); item.ed_OwnerGID = (UWORD)e.owner;
        if ((control->eac_MatchString && !MatchPatternNoCase(control->eac_MatchString, item.ed_Name)) ||
            (control->eac_MatchFunc && !call_match(h, control->eac_MatchFunc, &pkt->dp_Arg4, &item))) {
            control->eac_LastKey = previous = e.key; continue;
        }
        names = (uint32_t)strlen((char *)e.name) + 1;
        comments = pkt->dp_Arg4 >= ED_COMMENT ? (uint32_t)strlen((char *)e.comment) + 1 : 0;
        need = (fixed + names + comments + 3) & ~3u;
        if (need > left) {
            ffs_scan_again(scan, previous);
            return control->eac_Entries ? 0 : ERROR_NO_FREE_STORE;
        }
        dest = (void *)out;
        item.ed_Name = (STRPTR)(out + fixed);
        item.ed_Comment = comments ? (STRPTR)(out + fixed + names) : NULL;
        memcpy(out, &item, fixed); memcpy(out + fixed, e.name, names);
        if (comments) memcpy(out + fixed + names, e.comment, comments);
        if (last) last->ed_Next = dest;
        last = dest; out += need; left -= need;
        control->eac_LastKey = previous = e.key; ++control->eac_Entries;
    }
    return err;
}
static void unpublish_volume(struct handler *h)
{
    if (h->volume) {
        RemDosEntry(h->volume);
        ((unsigned char *)BADDR(h->volume->dol_Name))[0] = 30;
        FreeDosEntry(h->volume); h->volume = NULL;
    }
}
static int publish_volume(struct handler *h)
{
    struct ffs_entry root;
    char reserved_name[31];
    int err = ffs_stat(&h->fs, h->fs.root, &root);
    if (err) return err;
    /* Reserve the maximum volume-name capacity so relabeling does not replace
     * a DosList node referenced by existing FileLocks. Restore this
     * allocation length before FreeDosEntry for implementations using it. */
    memset(reserved_name, ' ', 30); reserved_name[30] = 0;
    memcpy(reserved_name, root.name, strlen((char *)root.name));
    h->volume = MakeDosEntry((CONST_STRPTR)reserved_name, DLT_VOLUME);
    if (!h->volume) return ERROR_NO_FREE_STORE;
    h->volume->dol_Type = DLT_VOLUME;
    {
        unsigned char *bstr = BADDR(h->volume->dol_Name);
        memset(bstr, 0, 32);
        bstr[0] = (UBYTE)strlen((char *)root.name);
        memcpy(bstr + 1, root.name, bstr[0]);
    }
    h->volume->dol_Task = h->port;
    h->volume->dol_misc.dol_volume.dol_DiskType = h->fs.dos_type;
    h->volume->dol_misc.dol_volume.dol_VolumeDate.ds_Days = h->fs.created[0];
    h->volume->dol_misc.dol_volume.dol_VolumeDate.ds_Minute = h->fs.created[1];
    h->volume->dol_misc.dol_volume.dol_VolumeDate.ds_Tick = h->fs.created[2];
    if (!AddDosEntry(h->volume)) {
        ((unsigned char *)BADDR(h->volume->dol_Name))[0] = 30;
        FreeDosEntry(h->volume); h->volume = NULL; return ERROR_OBJECT_EXISTS;
    }
    return 0;
}
/* The core asks only before writing timestamps. On OS3, the timer's system
 * time gives DateStamp's value at a fraction of its cost: 32-bit division
 * is a library call, so the current minute is recomputed only when left. */
static void current_date(void *ctx, uint32_t *now)
{
    struct handler *h = ctx;
    struct DateStamp date;
#ifndef __amigaos4__
    if (h->timer_open) {
        struct timeval time;
        uint32_t second;
        GetSysTime(&time);
        second = time.tv_secs - h->minute_start;
        if (time.tv_secs < h->minute_start || second >= 60 || !h->minute_start) {
            h->date_days = time.tv_secs / 86400;
            h->date_minutes = time.tv_secs % 86400 / 60;
            second = time.tv_secs % 60;
            h->minute_start = time.tv_secs - second;
        }
        now[0] = h->date_days; now[1] = h->date_minutes;
        /* 20000 us per tick: both operands fit a 16-bit division. */
        now[2] = second * TICKS_PER_SECOND + (uint16_t)(time.tv_micro >> 5) / (uint16_t)625;
        return;
    }
#endif
    DateStamp(&date);
    now[0] = date.ds_Days; now[1] = date.ds_Minute; now[2] = date.ds_Tick;
}
/* Seek or resize through a file handle. Positions span the unsigned 32-bit
 * size field; growth stops at FILE_LIMIT. A seek yields the previous
 * position; a resize yields the new size. */
static int move_file(struct handler *h, struct file_handle *file, int64_t offset,
                     LONG mode, unsigned resize, uint32_t *value)
{
    struct ffs_entry entry;
    struct file_handle *other;
    int64_t position = offset;
    int err = ffs_stat(&h->fs, file->lock->dos.fl_Key, &entry);
    if (err) return err;
    if (mode == OFFSET_CURRENT) position += file->position;
    else if (mode == OFFSET_END) position += entry.size;
    else if (mode != OFFSET_BEGINNING) return ERROR_SEEK_ERROR;
    if (position < 0 || position > UINT32_MAX || (!resize && position > entry.size))
        return ERROR_SEEK_ERROR;
    if (!resize) { *value = file->position; file->position = (uint32_t)position; return 0; }
    /* Never truncate below another open handle's position. */
    for (other = h->files; other; other = other->next)
        if (other != file && other->lock->dos.fl_Key == file->lock->dos.fl_Key &&
            other->position > position) position = other->position;
    err = ffs_resize(&h->fs, file->lock->dos.fl_Key, (uint32_t)position);
    if (err) return err;
    changed(h, file->lock->dos.fl_Key);
    if (file->position > position) file->position = (uint32_t)position;
    *value = (uint32_t)position;
    return 0;
}
static void dispatch(struct handler *h, struct DosPacket *pkt)
{
    struct ffs_entry entry;
    struct file_handle *file;
    uint32_t key = 0, got = 0;
    BPTR lock = 0;
    LONG result = DOSTRUE;
    int err = 0;
    char name[256];
#ifdef __amigaos4__
    int64_t result64 = 0;
    unsigned packet64 = 0;
#endif
    h->num_changed = 0;
    /* The timer provides a fallback for devices without change interrupts. */
    if (h->opened && (!h->timer_open || pkt->dp_Type == ACTION_DISK_CHANGE))
        (void)check_media(h);
    if (h->media_changed) {
        (void)drain_writes(h);
        (void)check_media(h);
        ffs_discard(&h->fs); h->fs.failed = 1;
    }
    if (h->media_changed && !h->locks && !h->inhibited) {
        unpublish_volume(h); clear_cache(h); h->media_changed = 0; h->writes.failed = 0;
        (void)check_media(h);
        err = remount_volume(h);
        if (!err) err = publish_volume(h);
        err = 0;
    }
    if (h->media_changed && pkt->dp_Type != ACTION_FREE_LOCK && pkt->dp_Type != ACTION_END &&
        pkt->dp_Type != ACTION_DIE && pkt->dp_Type != ACTION_DISK_INFO &&
        pkt->dp_Type != ACTION_REMOVE_NOTIFY) {
        reply(h, pkt, DOSFALSE, ERROR_NO_DISK); return;
    }
    h->fs.deferred = 1; h->fs.pending_limit = 128;
    if ((h->inhibited || !h->fs.mounted) && pkt->dp_Type != ACTION_INHIBIT && pkt->dp_Type != ACTION_DIE &&
        pkt->dp_Type != ACTION_IS_FILESYSTEM && pkt->dp_Type != ACTION_FLUSH &&
        pkt->dp_Type != ACTION_FORMAT && pkt->dp_Type != ACTION_DISK_INFO &&
        pkt->dp_Type != ACTION_CURRENT_VOLUME && pkt->dp_Type != ACTION_REMOVE_NOTIFY) {
        reply(h, pkt, 0, ERROR_NO_DISK); return;
    }
#ifdef FFS_PROFILE
    h->fs.clock = profile_clock;
    if (pkt->dp_Type == FFS_PROFILE_PACKET) {
        if (pkt->dp_Arg1 && pkt->dp_Arg2 != sizeof(h->fs.profile)) {
            reply(h, pkt, DOSFALSE, ERROR_BAD_NUMBER); return;
        }
        h->fs.profile.memory_flags = h->memory;
        h->fs.profile.address_mask = h->address_mask;
        h->fs.profile.dma_sample = (uintptr_t)h->writes.slots[0].bytes;
        h->fs.profile.handler_address = (uintptr_t)h;
        if (pkt->dp_Arg1) memcpy((void *)pkt->dp_Arg1, &h->fs.profile, sizeof(h->fs.profile));
        memset(&h->fs.profile, 0, sizeof(h->fs.profile));
        reply(h, pkt, DOSTRUE, 0); return;
    }
#endif
    switch (pkt->dp_Type) {
    case ACTION_MORE_CACHE: {
        int64_t count = (int64_t)h->cache_count + pkt->dp_Arg1;
        if (count < 1) count = 1;
        if (count > 1024) count = 1024;
        result = (LONG)resize_cache(h, (unsigned)count);
        break;
    }
    case ACTION_DISK_CHANGE:
        if (h->locks) err = ERROR_OBJECT_IN_USE;
        else {
            err = ffs_flush(&h->fs);
            if (err) break;
            unpublish_volume(h); clear_cache(h); h->media_changed = 0; h->writes.failed = 0;
            (void)check_media(h);
            err = remount_volume(h);
            if (!err && !h->inhibited) err = publish_volume(h);
        }
        break;
    case ACTION_IS_FILESYSTEM: break;
    case ACTION_CURRENT_VOLUME:
        result = MKBADDR(h->volume); break;
    case ACTION_LOCATE_OBJECT:
        err = lock_key(h, pkt->dp_Arg1, &key);
        from_bstr(name, pkt->dp_Arg2);
        if (!err) err = ffs_resolve(&h->fs, key, name, &entry);
        if (!err) err = new_lock(h, entry.key, pkt->dp_Arg3, &lock);
        result = lock;
        break;
    case ACTION_FREE_LOCK:
        err = free_lock(h, pkt->dp_Arg1); break;
    case ACTION_COPY_DIR:
    case ACTION_PARENT:
    case ACTION_COPY_DIR_FH:
    case ACTION_PARENT_FH:
        if (pkt->dp_Type == ACTION_COPY_DIR_FH || pkt->dp_Type == ACTION_PARENT_FH) {
            file = get_file(h, pkt->dp_Arg1);
            if (!file) err = ERROR_INVALID_LOCK;
            else key = file->lock->dos.fl_Key;
        } else err = lock_key(h, pkt->dp_Arg1, &key);
        if (!err) err = ffs_stat(&h->fs, key, &entry);
        if (!err && (pkt->dp_Type == ACTION_PARENT || pkt->dp_Type == ACTION_PARENT_FH)) key = entry.parent;
        if (!err && key) err = new_lock(h, key, SHARED_LOCK, &lock);
        result = lock;
        break;
    case ACTION_SAME_LOCK: {
        uint32_t other;
        err = lock_key(h, pkt->dp_Arg1, &key);
        if (!err) err = lock_key(h, pkt->dp_Arg2, &other);
        result = !err && key == other ? LOCK_SAME : LOCK_SAME_VOLUME;
        break;
    }
    case ACTION_EXAMINE_ALL: {
        /* Without a lock there is nowhere to keep a scan between packets. */
        struct file_lock *dirlock = pkt->dp_Arg1 ? get_lock(h, pkt->dp_Arg1) : NULL;
        struct ffs_scan scan = {0};
        err = lock_key(h, pkt->dp_Arg1, &key);
        if (!err) err = examine_all(h, key, dirlock ? &dirlock->scan : &scan, pkt);
        ffs_scan_end(&h->fs, &scan);
        break;
    }
    case ACTION_EXAMINE_ALL_END: {
        struct file_lock *dirlock = pkt->dp_Arg1 ? get_lock(h, pkt->dp_Arg1) : NULL;
        if (dirlock) ffs_scan_end(&h->fs, &dirlock->scan);
        ((struct ExAllControl *)pkt->dp_Arg5)->eac_LastKey = 0;
        break;
    }
    case ACTION_EXAMINE_OBJECT:
    case ACTION_EXAMINE_FH:
        if (pkt->dp_Type == ACTION_EXAMINE_FH) {
            file = get_file(h, pkt->dp_Arg1);
            if (!file) err = ERROR_INVALID_LOCK;
            else key = file->lock->dos.fl_Key;
        } else err = lock_key(h, pkt->dp_Arg1, &key);
        if (!err) err = ffs_stat(&h->fs, key, &entry);
        if (!err) fill_fib(&entry, BADDR(pkt->dp_Arg2));
        break;
    case ACTION_EXAMINE_NEXT: {
        struct FileInfoBlock *fib = BADDR(pkt->dp_Arg2);
        struct file_lock *dirlock = pkt->dp_Arg1 ? get_lock(h, pkt->dp_Arg1) : NULL;
        struct ffs_scan scan = {0};
        key = dirlock ? (uint32_t)dirlock->dos.fl_Key : h->fs.root;
        if (pkt->dp_Arg1 && !dirlock) err = ERROR_INVALID_LOCK;
        else err = ffs_scan_next(&h->fs, key, dirlock ? &dirlock->scan : &scan,
                                 fib->fib_DiskKey, &entry);
        ffs_scan_end(&h->fs, &scan);
        if (!err) fill_fib(&entry, fib);
        break;
    }
    case ACTION_FH_FROM_LOCK: {
        struct FileHandle *fh = BADDR(pkt->dp_Arg1);
        struct file_lock *owned = get_lock(h, pkt->dp_Arg2);
        if (!owned) { err = ERROR_INVALID_LOCK; break; }
        err = ffs_stat(&h->fs, owned->dos.fl_Key, &entry);
        if (!err && entry.type != -3) err = ERROR_OBJECT_WRONG_TYPE;
        file = err ? NULL : spare_alloc(h, &h->spare_files, sizeof(*file));
        if (!err && !file) err = ERROR_NO_FREE_STORE;
        if (!err) {
            file->lock = owned; file->next = h->files; h->files = file;
            file->mapping.defer_metadata = 1;
            fh->fh_Arg1 = (LONG)file; fh->fh_Type = h->port;
        }
        break;
    }
    case ACTION_CHANGE_MODE: {
        struct file_lock *owned = NULL, *other;
        if (pkt->dp_Arg1 == CHANGE_FH) {
            file = get_file(h, pkt->dp_Arg2);
            if (file) owned = file->lock;
        } else if (pkt->dp_Arg1 == CHANGE_LOCK) owned = get_lock(h, pkt->dp_Arg2);
        if (!owned || (pkt->dp_Arg3 != SHARED_LOCK && pkt->dp_Arg3 != EXCLUSIVE_LOCK))
            err = ERROR_INVALID_LOCK;
        if (!err && pkt->dp_Arg3 == EXCLUSIVE_LOCK)
            for (other = h->locks; other; other = other->next)
                if (other != owned && other->dos.fl_Key == owned->dos.fl_Key) err = ERROR_OBJECT_IN_USE;
        if (!err) owned->dos.fl_Access = pkt->dp_Arg3;
        break;
    }
    case ACTION_FINDINPUT:
    case ACTION_FINDOUTPUT:
    case ACTION_FINDUPDATE: {
        struct FileHandle *fh = BADDR(pkt->dp_Arg1);
        unsigned created = 0;
        err = lock_key(h, pkt->dp_Arg2, &key);
        from_bstr(name, pkt->dp_Arg3);
        if (!err) {
            if (pkt->dp_Type == ACTION_FINDINPUT)
                err = ffs_resolve(&h->fs, key, name, &entry);
            else err = ffs_resolve_create(&h->fs, key, name, &entry, &created);
        }
        if (!err && entry.type != -3) err = ERROR_OBJECT_WRONG_TYPE;
        if (!err && pkt->dp_Type == ACTION_FINDINPUT && (entry.protection & 8)) err = ERROR_READ_PROTECTED;
        if (!err && pkt->dp_Type == ACTION_FINDOUTPUT && (entry.protection & 1)) err = ERROR_DELETE_PROTECTED;
        if (!err) err = new_lock(h, entry.key,
                                  pkt->dp_Type == ACTION_FINDOUTPUT ? EXCLUSIVE_LOCK : SHARED_LOCK, &lock);
        if (!err && pkt->dp_Type == ACTION_FINDOUTPUT && !created) {
            err = ffs_resize(&h->fs, entry.key, 0);
            if (err) free_lock(h, lock);
        }
        file = NULL;
        if (!err) {
            file = spare_alloc(h, &h->spare_files, sizeof(*file));
            if (!file) { free_lock(h, lock); err = ERROR_NO_FREE_STORE; }
        }
        if (!err) {
            if (pkt->dp_Type != ACTION_FINDINPUT) changed(h, entry.key);
            file->lock = get_lock(h, lock);
            file->next = h->files; h->files = file;
            file->mapping.defer_metadata = 1;
            fh->fh_Arg1 = (LONG)file; fh->fh_Type = h->port;
        }
        break;
    }
    case ACTION_END: {
        struct file_handle **p = &h->files;
        while (*p && (LONG)*p != pkt->dp_Arg1) p = &(*p)->next;
        if (!*p) { err = ERROR_INVALID_LOCK; break; }
        file = *p;
        if (!h->media_changed) err = ffs_finish_file(&h->fs, file->lock->dos.fl_Key);
        release_records(h, file); *p = file->next;
        free_lock(h, MKBADDR(&file->lock->dos));
        spare_free(&h->spare_files, file);
        break;
    }
    case ACTION_LOCK_RECORD:
        err = acquire_record(h, pkt);
        if (err == ERROR_LOCK_COLLISION && !(pkt->dp_Arg4 & 1)) {
            struct record_wait *wait;
            if (!pkt->dp_Arg5) { err = ERROR_LOCK_TIMEOUT; break; }
            wait = memory_alloc(h, sizeof(*wait));
            if (!wait) { err = ERROR_NO_FREE_STORE; break; }
            wait->packet = pkt; wait->ticks = pkt->dp_Arg5;
            wait->next = h->waiting; h->waiting = wait;
            err = start_record_timer(h);
            if (!err) return; /* The packet remains owned by the wait queue. */
            h->waiting = wait->next; memory_free(h, wait, sizeof(*wait));
        }
        break;
    case ACTION_FREE_RECORD: {
        struct record_lock **p = &h->records, *record;
        file = get_file(h, pkt->dp_Arg1);
        if (!file) { err = ERROR_INVALID_LOCK; break; }
        while (*p && ((*p)->file != file || (*p)->start != (uint32_t)pkt->dp_Arg2 ||
                       (*p)->length != (uint32_t)pkt->dp_Arg3)) p = &(*p)->next;
        if (!*p) { err = ERROR_RECORD_NOT_LOCKED; break; }
        record = *p; *p = record->next; memory_free(h, record, sizeof(*record));
        service_records(h, 0);
        break;
    }
    case ACTION_READ:
    case ACTION_WRITE:
        file = get_file(h, pkt->dp_Arg1);
        if (!file) err = ERROR_INVALID_LOCK;
        else if (pkt->dp_Arg3 < 0) err = ERROR_BAD_NUMBER;
        else {
            if (pkt->dp_Type == ACTION_READ)
                err = ffs_read(&h->fs, file->lock->dos.fl_Key, file->position,
                               (void *)pkt->dp_Arg2, pkt->dp_Arg3, &got);
            else {
                h->packet_data = (uintptr_t)pkt->dp_Arg2; h->packet_size = pkt->dp_Arg3;
                err = ffs_write_file(&h->fs, &file->mapping, file->lock->dos.fl_Key, file->position,
                                     (const void *)pkt->dp_Arg2, pkt->dp_Arg3, &got);
                h->packet_size = 0;
                if (finish_packet_write(h) && !err) { err = FFS_IO; got = 0; }
            }
            if (!err && pkt->dp_Type == ACTION_WRITE) changed(h, file->lock->dos.fl_Key);
            file->position += got;
        }
        result = err ? -1 : (LONG)got;
        break;
    case ACTION_SEEK:
    case ACTION_SET_FILE_SIZE: {
        uint32_t value = 0;
        file = get_file(h, pkt->dp_Arg1);
        err = file ? move_file(h, file, (int32_t)pkt->dp_Arg2, pkt->dp_Arg3,
                               pkt->dp_Type == ACTION_SET_FILE_SIZE, &value) : ERROR_INVALID_LOCK;
        /* LONG results cannot express 2 GiB or more: report -1 without an
         * error, as for any value this interface cannot represent. */
        result = err || value > INT32_MAX ? -1 : (LONG)value;
        break;
    }
#ifdef __amigaos4__
    case ACTION_CHANGE_FILE_POSITION64:
    case ACTION_CHANGE_FILE_SIZE64:
    case ACTION_GET_FILE_POSITION64:
    case ACTION_GET_FILE_SIZE64: {
        struct DosPacket64 *wide = (struct DosPacket64 *)pkt;
        uint32_t value = 0;
        packet64 = 1;
        file = get_file(h, wide->dp_Arg1);
        if (!file) err = ERROR_INVALID_LOCK;
        else if (pkt->dp_Type == ACTION_GET_FILE_POSITION64) value = file->position;
        else if (pkt->dp_Type == ACTION_GET_FILE_SIZE64) {
            err = ffs_stat(&h->fs, file->lock->dos.fl_Key, &entry);
            value = entry.size;
        } else err = move_file(h, file, wide->dp_Arg2, wide->dp_Arg3,
                               pkt->dp_Type == ACTION_CHANGE_FILE_SIZE64, &value);
        if (pkt->dp_Type == ACTION_GET_FILE_POSITION64 || pkt->dp_Type == ACTION_GET_FILE_SIZE64)
            result64 = err ? -1 : (int64_t)value;
        else result64 = err ? DOSFALSE : DOSTRUE;
        break;
    }
#endif
    case ACTION_INFO:
    case ACTION_DISK_INFO: {
        struct InfoData *info = BADDR(pkt->dp_Type == ACTION_INFO ? pkt->dp_Arg2 : pkt->dp_Arg1);
        if (pkt->dp_Type == ACTION_INFO) err = lock_key(h, pkt->dp_Arg1, &key);
        if (!err) {
            memset(info, 0, sizeof(*info));
            uint32_t free_blocks = 0;
            if (h->fs.mounted) err = ffs_free_blocks(&h->fs, &free_blocks);
            info->id_DiskState = !h->fs.validated ? ID_VALIDATING : h->fs.writable ? ID_VALIDATED : ID_WRITE_PROTECTED;
            /* Reserved boot blocks are not part of the volume's capacity. */
            info->id_NumBlocks = h->fs.blocks - h->fs.reserved;
            info->id_NumBlocksUsed = h->fs.blocks - h->fs.reserved - free_blocks;
            info->id_BytesPerBlock = h->fs.block_size;
            info->id_DiskType = h->fs.mounted ? h->fs.dos_type : ID_NOT_REALLY_DOS;
            info->id_VolumeNode = MKBADDR(h->volume);
            info->id_InUse = h->locks != NULL;
        }
        break;
    }
    case ACTION_CREATE_DIR: {
        char *leaf;
        from_bstr(name, pkt->dp_Arg2);
        err = parent_name(h, pkt->dp_Arg1, name, &key, &leaf);
        if (!err) err = ffs_create(&h->fs, key, leaf, 1, &key);
        if (!err) changed(h, key);
        if (!err) err = new_lock(h, key, EXCLUSIVE_LOCK, &lock);
        result = lock;
        break;
    }
    case ACTION_DELETE_OBJECT:
    case ACTION_RENAME_OBJECT: {
        char *leaf;
        from_bstr(name, pkt->dp_Arg2);
        /* parent_name resolves the lock itself. */
        err = parent_name(h, pkt->dp_Arg1, name, &key, &leaf);
        if (!err) err = ffs_lookup(&h->fs, key, (unsigned char *)leaf, strlen(leaf), &entry);
        /* Locks hold the header key, not the name. As in FastFileSystem 47,
         * any lock prevents deletion, but only an exclusive one renaming. */
        if (!err && in_use(h, entry.key,
                           pkt->dp_Type == ACTION_DELETE_OBJECT ? EXCLUSIVE_LOCK : SHARED_LOCK))
            err = ERROR_OBJECT_IN_USE;
        if (!err) changed(h, entry.key);
        if (!err && pkt->dp_Type == ACTION_DELETE_OBJECT) err = ffs_remove_entry(&h->fs, &entry);
        else if (!err) {
            from_bstr(name, pkt->dp_Arg4);
            err = parent_name(h, pkt->dp_Arg3, name, &key, &leaf);
            if (!err) err = ffs_rename(&h->fs, entry.key, key, leaf);
            if (!err) changed(h, entry.key);
        }
        break;
    }
    case ACTION_SET_PROTECT:
    case ACTION_SET_COMMENT:
    case ACTION_SET_DATE:
        err = lock_key(h, pkt->dp_Arg2, &key);
        from_bstr(name, pkt->dp_Arg3);
        if (!err && pkt->dp_Type == ACTION_SET_COMMENT) {
            /* A hard link keeps its own comment, as in FastFileSystem 47;
             * protection and dates belong to its target. */
            char *leaf;
            uint32_t dir;
            err = parent_name(h, pkt->dp_Arg2, name, &dir, &leaf);
            if (!err && *leaf) {
                err = ffs_lookup(&h->fs, dir, (const unsigned char *)leaf, strlen(leaf), &entry);
                if (!err && entry.type == 3) err = FFS_SOFT_LINK;
            } else if (!err) err = ffs_resolve(&h->fs, key, name, &entry);
        } else if (!err) err = ffs_resolve(&h->fs, key, name, &entry);
        if (!err) {
            if (pkt->dp_Type == ACTION_SET_PROTECT) entry.protection = pkt->dp_Arg4;
            else if (pkt->dp_Type == ACTION_SET_DATE) {
                struct DateStamp *ds = (void *)pkt->dp_Arg4;
                entry.days = ds->ds_Days; entry.minutes = ds->ds_Minute; entry.ticks = ds->ds_Tick;
            } else {
                from_bstr(name, pkt->dp_Arg4);
                if (strlen(name) > 79) err = ERROR_COMMENT_TOO_BIG;
                else memcpy(entry.comment, name, strlen(name) + 1);
            }
            if (!err) err = ffs_set_metadata(&h->fs, entry.key, &entry);
            if (!err) changed(h, entry.key);
        }
        break;
    case ACTION_MAKE_LINK: {
        char *leaf;
        from_bstr(name, pkt->dp_Arg2);
        err = parent_name(h, pkt->dp_Arg1, name, &key, &leaf);
        if (!err && pkt->dp_Arg4) err = ffs_symlink(&h->fs, key, leaf, (const char *)pkt->dp_Arg3, &key);
        else if (!err) {
            uint32_t target;
            err = lock_key(h, pkt->dp_Arg3, &target);
            if (!err) err = ffs_hardlink(&h->fs, key, leaf, target, &key);
        }
        if (!err) changed(h, key);
        break;
    }
    case ACTION_READ_LINK: {
        const char *path = (const char *)pkt->dp_Arg2, *start, *colon;
        char component_name[FFS_NAME_MAX + 1];
        uint32_t length = 0, capacity = pkt->dp_Arg4;
        char *target = (char *)pkt->dp_Arg3;
        err = lock_key(h, pkt->dp_Arg1, &key);
        colon = strchr(path, ':');
        if (colon) { key = h->fs.root; path = colon + 1; }
        while (!err && *path) {
            size_t n;
            start = path; while (*path && *path != '/') ++path;
            /* Lookups truncate longer names to the volume's limit anyway. */
            n = (size_t)(path - start) < FFS_NAME_MAX ? (size_t)(path - start) : FFS_NAME_MAX;
            memcpy(component_name, start, n); component_name[n] = 0;
            if (!*component_name) {
                err = ffs_stat(&h->fs, key, &entry);
                if (!err) key = entry.parent;
                if (!key && !err) err = ERROR_OBJECT_NOT_FOUND;
            } else {
                err = ffs_resolve(&h->fs, key, component_name, &entry);
                if (err == FFS_SOFT_LINK) {
                    err = ffs_readlink(&h->fs, entry.key, target, capacity, &length);
                    if (*path == '/') ++path;
                    if (!err && *path) {
                        uint32_t rest = (uint32_t)strlen(path);
                        unsigned slash = length && target[length - 1] != ':';
                        if (rest >= capacity - length || slash >= capacity - length - rest) err = ERROR_LINE_TOO_LONG;
                        else {
                            if (slash) target[length++] = '/';
                            memcpy(target + length, path, rest + 1); length += rest;
                        }
                    }
                    break;
                }
                if (!err) key = entry.key;
            }
            if (*path == '/') ++path;
        }
        if (!err && !length) err = ERROR_OBJECT_NOT_FOUND;
        result = err ? -1 : (LONG)length;
        if (err == ERROR_LINE_TOO_LONG) { err = 0; result = -2; }
        break;
    }
    case ACTION_WRITE_PROTECT:
        if (pkt->dp_Arg1) {
            if (h->user_protected) err = ERROR_DISK_WRITE_PROTECTED;
            else if ((err = ffs_flush(&h->fs))) break;
            else { h->protected_writable = h->fs.writable; h->user_protected = 1; h->password = pkt->dp_Arg2; h->fs.writable = 0; }
        } else if (h->password && h->password != pkt->dp_Arg2) err = ERROR_INVALID_COMPONENT_NAME;
        else {
            h->user_protected = 0; h->password = 0;
            if (h->protected_writable && !h->fs.failed) h->fs.writable = 1;
        }
        break;
    case ACTION_INHIBIT:
        if (pkt->dp_Arg1) {
            if (h->locks) err = ERROR_OBJECT_IN_USE;
            else if (!(err = ffs_flush(&h->fs)) && !h->inhibited++) {
                unpublish_volume(h); motor_off(h);
            }
        } else if (h->inhibited && !--h->inhibited) {
            clear_cache(h); h->media_changed = 0;
            err = remount_volume(h);
            if (!err) err = publish_volume(h);
        }
        break;
    case ACTION_ADD_NOTIFY: {
        struct NotifyRequest *request = (void *)pkt->dp_Arg1;
        struct watch *watch;
        const char *path = (const char *)(request->nr_FullName ? request->nr_FullName : request->nr_Name);
        if (!path || strlen(path) > 255 || !(request->nr_Flags & (NRF_SEND_MESSAGE | NRF_SEND_SIGNAL))) {
            err = ERROR_INVALID_COMPONENT_NAME; break;
        }
        if (!h->notify_port) h->notify_port = CreateMsgPort();
        if (!h->notify_port) { err = ERROR_NO_FREE_STORE; break; }
        for (watch = h->watches; watch; watch = watch->next)
            if (watch->request == request) { err = ERROR_OBJECT_IN_USE; break; }
        if (err) break;
        watch = memory_alloc(h, sizeof(*watch));
        if (!watch) { err = ERROR_NO_FREE_STORE; break; }
        memcpy(watch->path, path, strlen(path) + 1);
        watch->request = request; watch->active = 1;
        if (!ffs_resolve(&h->fs, h->fs.root, path, &entry)) watch->key = entry.key;
        watch->next = h->watches; h->watches = watch;
        request->nr_Handler = h->port; request->nr_MsgCount = 0;
        if (request->nr_Flags & NRF_NOTIFY_INITIAL) send_notice(h, watch);
        break;
    }
    case ACTION_REMOVE_NOTIFY: {
        struct watch **p = &h->watches, *watch;
        while (*p && (*p)->request != (void *)pkt->dp_Arg1) p = &(*p)->next;
        if (!*p) { err = ERROR_OBJECT_NOT_FOUND; break; }
        watch = *p; *p = watch->next;
        watch->active = 0; watch->request->nr_Handler = NULL;
        if (!watch->outstanding) memory_free(h, watch, sizeof(*watch));
        break;
    }
    case ACTION_FLUSH: {
        unsigned pending = h->fs.pending != NULL;
        err = ffs_flush(&h->fs);
        /* A commit already waits for the final valid-root device update. */
        if (!err && !pending && device_flush(h)) err = FFS_IO;
        break;
    }
    case ACTION_DIE:
        if (h->locks || h->files || h->watches || h->notify_pending) err = ERROR_OBJECT_IN_USE;
        else if (!(err = ffs_flush(&h->fs))) h->quitting = 1;
        break;
    case ACTION_FORMAT:
        if (h->locks) err = ERROR_OBJECT_IN_USE;
        else if (h->user_protected) err = ERROR_DISK_WRITE_PROTECTED;
        else {
            from_bstr(name, pkt->dp_Arg1);
            err = ffs_format_volume(&h->fs, (uint32_t)pkt->dp_Arg2, name);
            if (!err) { unpublish_volume(h); if (!h->inhibited) err = publish_volume(h); }
        }
        break;
    case ACTION_RENAME_DISK:
        from_bstr(name, pkt->dp_Arg1);
        if (!err) err = ffs_relabel(&h->fs, name);
        if (!err) {
            unsigned char *bstr = BADDR(h->volume->dol_Name);
            size_t length = strlen(name);
            LockDosList(LDF_VOLUMES | LDF_WRITE);
            memset(bstr, 0, 32); bstr[0] = (UBYTE)length;
            memcpy(bstr + 1, name, length);
            UnLockDosList(LDF_VOLUMES | LDF_WRITE);
        }
        break;
    default: err = ERROR_ACTION_NOT_KNOWN; break;
    }
    if (!h->fs.pending) h->write_ticks = 0;
    else if (!h->write_ticks) h->write_ticks = 100;
    if (h->fs.pending && !h->timer_pending) {
        if (start_record_timer(h)) {
            int flush_error = ffs_flush(&h->fs);
            if (!err) err = flush_error;
        }
    }
    if (err && result != -1) result = 0;
    /* DOSTRUE is also -1; only stream operations return -1 on failure. */
    if (err && pkt->dp_Type != ACTION_READ && pkt->dp_Type != ACTION_WRITE &&
        pkt->dp_Type != ACTION_SEEK && pkt->dp_Type != ACTION_SET_FILE_SIZE && pkt->dp_Type != ACTION_READ_LINK) result = 0;
    if (!err && h->num_changed) notify_changes(h);
#ifdef __amigaos4__
    if (packet64) { reply64(h, pkt, result64, err); return; }
#endif
    reply(h, pkt, result, err);
}
int ffs_handler_main(void);
int ffs_handler_main(void)
{
    struct handler state, *h = &state;
    struct Process *process;
    struct Message *message;
    struct DosPacket *startup;
    struct FileSysStartupMsg *fssm;
    struct DosEnvec *env;
    struct ffs_io io;
    uint64_t sectors, origin, total, block_size;
    char device[256];
    int err = ERROR_NO_FREE_STORE;
    memset(h, 0, sizeof(*h));
    h->exec = *(struct ExecBase **)4UL;
#ifdef __amigaos4__
    h->iexec = (struct ExecIFace *)h->exec->MainInterface;
#endif
    process = (struct Process *)FindTask(NULL);
    h->port = &process->pr_MsgPort;
    WaitPort(h->port); message = GetMsg(h->port);
    startup = (struct DosPacket *)message->mn_Node.ln_Name;
    h->dos = (struct DosLibrary *)OpenLibrary((CONST_STRPTR)"dos.library", 37);
#ifdef __amigaos4__
    if (h->dos) h->idos = (struct DOSIFace *)GetInterface((struct Library *)h->dos, "main", 1, NULL);
    if (!h->idos) goto done;
#endif
    if (!h->dos || !startup->dp_Arg2 || !startup->dp_Arg3) goto done;
    fssm = BADDR(startup->dp_Arg2);
    h->device = BADDR(startup->dp_Arg3);
    env = BADDR(fssm->fssm_Environ);
    err = ERROR_NOT_A_DOS_DISK;
    if (!env || env->de_TableSize < DE_UPPERCYL || env->de_HighCyl < env->de_LowCyl ||
        !env->de_Surfaces || !env->de_BlocksPerTrack || !env->de_SizeBlock ||
        !env->de_SectorPerBlock) goto done;
    sectors = (uint64_t)env->de_Surfaces * env->de_BlocksPerTrack;
    block_size = (uint64_t)env->de_SizeBlock * 4 * env->de_SectorPerBlock;
    origin = sectors * env->de_LowCyl;
    total = sectors * ((uint64_t)env->de_HighCyl - env->de_LowCyl + 1);
    /* Keys are 32-bit; byte offsets beyond 4 GiB need 64-bit commands. */
    if (block_size < 512 || block_size > 32768 || (block_size & (block_size - 1)) ||
        origin % env->de_SectorPerBlock || total % env->de_SectorPerBlock ||
        total / env->de_SectorPerBlock > UINT32_MAX) goto done;
    h->origin = origin * env->de_SizeBlock * 4;
    h->transfer = env->de_TableSize >= DE_MAXTRANSFER ? env->de_MaxTransfer : 0x1fe00;
    h->transfer -= h->transfer % (env->de_SizeBlock * 4);
    if (!h->transfer) goto done;
    h->memory = env->de_TableSize >= DE_BUFMEMTYPE ? env->de_BufMemType : MEMF_PUBLIC;
    h->address_mask = env->de_TableSize >= DE_MASK ? env->de_Mask : 0xfffffffeUL;

    h->io_port = CreateMsgPort();
    if (!h->io_port) { err = ERROR_NO_FREE_STORE; goto done; }
    h->request = (struct IOStdReq *)CreateIORequest(h->io_port, sizeof(*h->request));
    if (!h->request) { err = ERROR_NO_FREE_STORE; goto done; }
    from_bstr(device, fssm->fssm_Device);
    if (OpenDevice((CONST_STRPTR)device, fssm->fssm_Unit,
                    (struct IORequest *)h->request, fssm->fssm_Flags)) goto done;
    h->opened = 1;
    if ((origin + total) * env->de_SizeBlock * 4 > UINT32_MAX) {
        h->io64 = query_io64(h, env->de_TableSize >= DE_INTERLEAVE ? env->de_Interleave : 0);
        if (h->io64 == IO32) goto done;
    }
    install_media_interrupt(h);
    (void)check_media(h);
    init_write_requests(h, (uint32_t)block_size);
    memset(&io, 0, sizeof(io));
    io.context = h; io.read = device_read; io.write = device_write;
    io.read_extent = device_read; io.write_extent = device_write_extent;
    io.flush = device_flush; io.alloc = core_alloc; io.free = core_free;
    io.date = current_date; io.write_owned = device_write_owned;
    err = ffs_mount(&h->fs, &io, (uint32_t)block_size,
                    (uint32_t)(total / env->de_SectorPerBlock), env->de_Reserved,
                    !media_protected(h));
    h->fs.file_limit = FILE_LIMIT;
    if (err == ERROR_NO_FREE_STORE) goto done;
    if (!err) {
        if (!h->fs.writable && h->fs.write_requested) (void)ffs_validate(&h->fs, 1);
        err = publish_volume(h);
        if (err) goto done;
    }
    (void)resize_cache(h, env->de_TableSize >= DE_NUMBUFFERS && env->de_NumBuffers ? env->de_NumBuffers : 3);
    (void)start_record_timer(h);
    h->device->dn_Task = h->port;
    reply(h, startup, DOSTRUE, 0);
    startup = NULL;
    while (!h->quitting) {
        ULONG signals = 1UL << h->port->mp_SigBit;
        if (h->notify_port) signals |= 1UL << h->notify_port->mp_SigBit;
        if (h->timer_port) signals |= 1UL << h->timer_port->mp_SigBit;
        Wait(signals);
        if (h->media_changed) {
            (void)drain_writes(h);
            clear_cache(h); ffs_discard(&h->fs); h->fs.failed = 1;
        }
        /* Ports with nothing outstanding cannot hold a reply. */
        if (h->notify_pending) returned_notices(h);
        if (h->timer_pending && GetMsg(h->timer_port)) {
            h->timer_pending = 0;
            if (h->media_ticks <= h->timer_ticks) h->media_ticks = 0;
            else h->media_ticks -= h->timer_ticks;
            if (!h->media_ticks) {
                (void)check_media(h); h->media_ticks = 10;
                if (h->media_changed) { ffs_discard(&h->fs); h->fs.failed = 1; }
            }
            if (h->write_ticks <= h->timer_ticks) h->write_ticks = 0;
            else h->write_ticks -= h->timer_ticks;
            if (h->fs.pending && !h->write_ticks) {
                (void)ffs_flush(&h->fs);
                if (h->fs.pending) h->write_ticks = 100;
            }
            /* Two idle seconds without pending writes stop the motor. */
            if (h->motor_on && !h->fs.pending && (h->motor_idle += h->timer_ticks) >= 100)
                motor_off(h);
            service_records(h, 1);
            (void)start_record_timer(h);
        }
        while (!h->quitting && (message = GetMsg(h->port)))
            dispatch(h, (struct DosPacket *)message->mn_Node.ln_Name);
    }
    h->device->dn_Task = NULL;
    motor_off(h);
done:
    remove_media_interrupt(h);
    free_write_requests(h);
    free_spares(h, &h->spare_locks, sizeof(struct file_lock));
    free_spares(h, &h->spare_files, sizeof(struct file_handle));
    ffs_discard(&h->fs);
    free_core_pools(h);
    (void)resize_cache(h, 0);
    if (h->notify_port) DeleteMsgPort(h->notify_port);
    if (h->timer_pending) { AbortIO((struct IORequest *)h->timer); WaitIO((struct IORequest *)h->timer); }
    if (h->timer_open) CloseDevice((struct IORequest *)h->timer);
    if (h->timer) DeleteIORequest((struct IORequest *)h->timer);
    if (h->timer_port) DeleteMsgPort(h->timer_port);
    unpublish_volume(h);
    if (h->opened) CloseDevice((struct IORequest *)h->request);
    if (h->request) DeleteIORequest((struct IORequest *)h->request);
    if (h->io_port) DeleteMsgPort(h->io_port);
#ifdef __amigaos4__
    if (h->idos) DropInterface((struct Interface *)h->idos);
#endif
    if (h->dos) CloseLibrary((struct Library *)h->dos);
    if (startup) reply(h, startup, DOSFALSE, err);
    return 0;
}
