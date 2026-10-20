# FastFileSystem

A C AmigaDOS filesystem handler for AmigaOS 3 (68000+) and AmigaOS 4
(PowerPC), using GCC and a portable, host-tested core. It reads and writes
all six v40 formats: DOS\0/2/4 (OFS) and DOS\1/3/5 (FFS), including
international names and directory caches.

Implemented operations include directories, extension files, allocation,
resize, rename, metadata, hard/soft links, format, bitmap validation and
repair. The native handler adds locks, file handles, ExAll, record locks,
notifications, write protection, inhibit, cache resizing and device I/O.
State belongs to each mount; OS3 and OS4 share the packet implementation.


## Build 

```
make amigaos3
make amigaos4
```

Binaries are `build/os3/FastFileSystem` and `build/os4/FastFileSystem`.
Override `HOSTCC`, `HOSTCFLAGS`, `M68K_CC`, `PPC_CC` or `NDK_PATH` as needed.

## Mounting

Use a disposable image first. Adapt [the mount entry](platform/amiga/mountlist)
to the device, partition geometry and handler location. `ForceLoad = 1`
ensures Mount loads this binary instead of selecting a resident filesystem.
Never mount the same writable partition through two handlers simultaneously.

The host tool `build/ffs-image` supports `list`, `cat`, `mkdir`, `put`, `rm`,
`rename`, `truncate`, `check`, `repair` and `format` on existing image files.
Run it without arguments for command syntax. Set `FFS_BLOCK_SIZE` for blocks
other than 512 bytes. Image writes use an exclusive lock and flush to storage.

