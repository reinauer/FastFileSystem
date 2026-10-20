HOSTCC ?= cc
M68K_CC ?= m68k-amigaos-gcc
PPC_CC ?= ppc-amigaos-gcc
NDK_PATH ?= $(dir $(shell command -v $(M68K_CC)))/../m68k-amigaos/ndk-include
WARN = -Wall -Wextra -Wpedantic -Werror -Wshadow -Wstrict-prototypes
CFLAGS_COMMON = -std=c99 $(WARN) -Iinclude
HOSTCFLAGS ?= -O2 -g
CORE = core/format.c core/fs.c
HEADERS = include/ffs/format.h include/ffs/fs.h

.PHONY: all clean distclean
all: amigaos3 amigaos4 ffs-image

clean:
	rm build/os3/FastFileSystem
	rm build/os4/FastFileSystem
	rm build/ffs-image

distclean:
	rm -rf build

AMIGA_SOURCES = platform/amiga/handler.c platform/amiga/libc.c $(CORE)
AMIGA_FLAGS = -Wno-array-bounds -std=gnu99 -O2 -Wall -Wextra -Werror -Iinclude -fno-builtin -ffreestanding

.PHONY: amigaos3 amigaos4 ffs-image
amigaos3: build/os3/FastFileSystem
amigaos4: build/os4/FastFileSystem
ffs-image: build/ffs-image

ffs-image:
build/ffs-image: platform/host/image.c $(CORE) $(HEADERS) Makefile
	mkdir -p $(@D)
	$(HOSTCC) $(CFLAGS_COMMON) $(HOSTCFLAGS) platform/host/image.c $(CORE) -o $@


build/os3/FastFileSystem: platform/amiga/start.S $(AMIGA_SOURCES) $(HEADERS) Makefile
	mkdir -p $(@D)
	$(M68K_CC) $(AMIGA_FLAGS) -m68000 -noixemul -nostdlib -nostartfiles \
		-I$(NDK_PATH) \
		platform/amiga/start.S $(AMIGA_SOURCES) -lgcc -lc -lgcc -o $@

build/os4/FastFileSystem: platform/amiga/start4.c $(AMIGA_SOURCES) $(HEADERS) Makefile
	mkdir -p $(@D)
	$(PPC_CC) $(AMIGA_FLAGS) -D__USE_INLINE__ -D__USE_BASETYPE__ \
		-mcrt=newlib -fno-asynchronous-unwind-tables -nostdlib -nostartfiles -static \
		platform/amiga/start4.c $(AMIGA_SOURCES) -lgcc -o $@

build/os3/FastFileSystem build/os4/FastFileSystem: platform/amiga/write_queue.h platform/amiga/dma.h
