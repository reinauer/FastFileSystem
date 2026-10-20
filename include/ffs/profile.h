#ifndef FFS_PROFILE_H
#define FFS_PROFILE_H
/* Private diagnostic packet, present only in FFS_PROFILE builds. */
#define FFS_PROFILE_PACKET 0x46535053
/* Inclusive times: nested categories must not be added together. */
enum { FFS_P_WRITE, FFS_P_GROW, FFS_P_ALLOCATE, FFS_P_STAGE,
       FFS_P_CACHE, FFS_P_COMMIT, FFS_P_DEVICE, FFS_P_MEMORY, FFS_P_SYNC, FFS_P_WAIT,
       FFS_P_CHECKSUM, FFS_P_MEDIA, FFS_P_EDIT, FFS_P_TABLE,
       FFS_P_PLAN, FFS_P_UNDO, FFS_P_SORT, FFS_P_GATHER,
       FFS_P_ROOT, FFS_P_CLEANUP, FFS_P_INVALIDATE, FFS_P_COUNT };
struct ffs_profile {
    unsigned long ticks[FFS_P_COUNT], calls[FFS_P_COUNT];
    unsigned long memory_flags, address_mask, dma_sample, handler_address;
    /* Reserved counters from the former write plan. */
    unsigned long undo_words, undo_chunks;
};
#endif
