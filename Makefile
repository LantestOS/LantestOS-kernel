# Lantest OS - M4-2 build system
#
# Targets:
#   make                  build kernel.elf + lantest.iso (ELF64 user images embedded)
#   make run              boot the ISO in QEMU (graphical window + serial in terminal)
#   make run-nographic    boot the ISO in QEMU with serial only (no GUI/WSLg needed)
#   make debug            start QEMU paused, with a GDB stub on tcp::1234
#   make gdb              connect GDB to a running `make debug` session
#   make test             headless boot test (VFS/ramfs + fd + uaccess + ELF + lifecycle)
#   make test-exception   build with a deliberate page fault, verify the #PF panic
#   make clean            remove build outputs
#
# All commands are intended to run inside WSL2 / a Linux host.

CC       := gcc
LD       := ld
OBJCOPY  := objcopy
QEMU     := qemu-system-x86_64

C_SRCS   := kernel.c \
            libk/kprintf.c libk/panic.c \
            drivers/uart.c drivers/vga.c drivers/console.c \
            drivers/pci.c drivers/virtio_blk.c drivers/keyboard.c \
            fs/file.c fs/vfs.c fs/ramfs.c fs/lantestfs.c fs/pipe.c \
            mm/vma.c mm/page_cache.c \
            arch/x86_64/gdt.c arch/x86_64/idt.c arch/x86_64/irq.c \
            arch/x86_64/paging.c arch/x86_64/acpi.c \
            arch/x86_64/lapic.c arch/x86_64/smp.c arch/x86_64/tlb.c arch/x86_64/percpu.c \
            mm/multiboot2.c mm/pmm.c mm/kmalloc.c mm/elf.c mm/uaccess.c \
            sched/scheduler.c sched/signal.c \
            syscall/syscall.c
AS_SRCS  := boot.S arch/x86_64/isr_asm.S arch/x86_64/switch.S \
            arch/x86_64/usermode.S arch/x86_64/uaccess_asm.S \
            arch/x86_64/ap_trampoline.S
OBJS     := $(C_SRCS:.c=.o) $(AS_SRCS:.S=.o)
HDRS     := $(wildcard include/*.h include/x86_64/*.h)
USER_HDRS := user/userlib.h user/user.ld

CFLAGS   := -m64 -std=gnu11 -ffreestanding -fno-pie -fno-pic -fno-stack-protector \
            -fno-builtin -fno-asynchronous-unwind-tables -nostdinc \
            -mno-red-zone -mgeneral-regs-only -fno-omit-frame-pointer \
            -Iinclude -Wall -Wextra -O2 -g
CFLAGS   += $(EXTRA_CFLAGS)
ASFLAGS  := -m64 -g
# -z noexecstack: user_blobs.o (binary-embedded ELFs) carries no
# .note.GNU-stack, and runner binutils 2.42 then warns "implies
# executable stack" and marks PT_GNU_STACK RWX. Declaring non-exec
# stack is true for this kernel and silences both.
LDFLAGS  := -m elf_x86_64 -T linker.ld --build-id=none \
            -z max-page-size=0x1000 -z noexecstack -Map=kernel.map

USER_CFLAGS := -m64 -std=gnu11 -ffreestanding -fno-pie -fno-pic \
               -mcmodel=large \
               -fno-stack-protector -fno-builtin -fno-asynchronous-unwind-tables \
               -mgeneral-regs-only -nostdinc -Iuser -O2 -g -Wall -Wextra
USER_FLOAT_CFLAGS := $(filter-out -mgeneral-regs-only,$(USER_CFLAGS))
USER_LDFLAGS := -m elf_x86_64 -s -T user/user.ld -z max-page-size=0x1000 \
                --no-warn-rwx-segments

# -cpu max exposes SMAP/SMEP so the M3c-3 hardening is actually exercised.
DISK_IMG := disk.img

# Wall-clock budget for one QEMU boot during tests.
# Local KVM runs need ~40 s; CI runners boot under TCG (several times
# slower), so the workflow overrides this with e.g. QEMU_TIMEOUT=240.
QEMU_TIMEOUT ?= 40

QEMU_DISK := -drive file=$(DISK_IMG),if=none,id=disk,format=raw \
             -device virtio-blk-pci,drive=disk

QEMUFLAGS := -cpu max -smp 4 -m 512M -cdrom lantest.iso -boot d -no-reboot \
             $(QEMU_DISK) -serial stdio
QEMU_HEADLESS := -cpu max -smp 4 -m 512M -cdrom lantest.iso -boot d -no-reboot \
                  $(QEMU_DISK) -display none -monitor none

USER_PROGS := parent child filetest shell hello unlinktest init ls cat echo head cowtest mmaptest maptest muntest malloctest shmtest libctest fiotest sjtest floattest apuser stress segvtest
USER_ELFS  := $(addprefix user/,$(addsuffix .elf,$(USER_PROGS)))

.PHONY: all run run-nographic debug gdb test test-exception clean

all: lantest.iso

$(OBJS): $(HDRS)

kernel.elf: $(OBJS) user_blobs.o linker.ld
	$(LD) $(LDFLAGS) -o $@ $(OBJS) user_blobs.o

%.o: %.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c $< -o $@

%.o: %.S
	@mkdir -p $(dir $@)
	$(CC) $(ASFLAGS) -c $< -o $@


# ---- Lua 5.4 port ----------------------------------------------------------
LUA_SRCS := lapi.c lcode.c lctype.c ldebug.c ldo.c ldump.c lfunc.c lgc.c \
            llex.c lmem.c lobject.c lopcodes.c lparser.c lstate.c lstring.c \
            ltable.c ltm.c lundump.c lvm.c lzio.c lauxlib.c lbaselib.c \
            ltablib.c lstrlib.c lutf8lib.c lmathlib.c liolib.c loslib.c
LUA_OBJS := $(addprefix user/lua_port/obj/,$(LUA_SRCS:.c=.o))
LUA_HDRS := $(wildcard user/lua_port/include/*.h user/lua_port/src/*.h)
LUA_CFLAGS := $(filter-out -Iuser,$(filter-out -mgeneral-regs-only,$(USER_CFLAGS))) \
              -Iuser/lua_port/include -Iuser/lua_port/src -Iuser \
              -DLUA_USE_C89 -include stdio.h

$(LUA_OBJS) user/lua_port/obj/libc.o user/lua_port/obj/lua_main.o: $(LUA_HDRS)

user/lua_port/obj/%.o: user/lua_port/src/%.c
	@mkdir -p $(dir $@)
	$(CC) $(LUA_CFLAGS) -c $< -o $@

user/lua_port/obj/libc.o: user/lua_port/libc.c
	@mkdir -p $(dir $@)
	$(CC) $(LUA_CFLAGS) -c $< -o $@

user/lua_port/obj/lua_main.o: user/lua_port/lua_main.c
	@mkdir -p $(dir $@)
	$(CC) $(LUA_CFLAGS) -Iuser -c $< -o $@

user/lua.elf: $(LUA_OBJS) user/lua_port/obj/libc.o user/lua_port/obj/lua_main.o user/user.ld
	$(LD) $(USER_LDFLAGS) -o $@ $(LUA_OBJS) user/lua_port/obj/libc.o user/lua_port/obj/lua_main.o

USER_ELFS += user/lua.elf

# ---- user programs: C -> ELF64 -> embedded raw blob ------------------------
user/floattest.o: user/floattest.c user/userlib.h user/crt.h user/libc.h user/stdio.h
	$(CC) $(USER_FLOAT_CFLAGS) -c $< -o $@

user/%.o: user/%.c user/userlib.h user/crt.h user/libc.h user/stdio.h
	@mkdir -p $(dir $@)
	$(CC) $(USER_CFLAGS) -c $< -o $@

user/%.elf: user/%.o user/libc.o user/user.ld
	$(LD) $(USER_LDFLAGS) -o $@ $< user/libc.o

user_blobs.o: $(USER_ELFS)
	$(LD) -r -b binary -o $@ $^

# ---- ISO ----------------------------------------------------------------
lantest.iso: kernel.elf grub.cfg
	mkdir -p iso/boot/grub
	cp kernel.elf iso/boot/kernel.elf
	cp grub.cfg iso/boot/grub/grub.cfg
	grub-mkrescue -o $@ iso

run: lantest.iso disk.img
	$(QEMU) $(QEMUFLAGS)

run-nographic: lantest.iso disk.img
	$(QEMU) -cpu max -m 512M -cdrom lantest.iso -boot d -no-reboot \
		$(QEMU_DISK) -nographic

debug: lantest.iso disk.img
	@echo "QEMU is paused. In another terminal run: make gdb"
	$(QEMU) $(QEMUFLAGS) -s -S

gdb: kernel.elf
	gdb -q kernel.elf \
		-ex 'set confirm off' \
		-ex 'set pagination off' \
		-ex 'target remote localhost:1234' \
		-ex 'hbreak kernel_main' \
		-ex 'continue'

# Headless M5-2 boot test:
#   - VFS core + ramfs root initialized
#   - user filetest: open(O_CREAT)/write/close/open/read/EOF/close, missing path
#   - fd table, uaccess, ELF, Ring 3 and lifecycle still work
test: lantest.iso disk.img
	rm -f serial.log
	timeout $(QEMU_TIMEOUT)s $(QEMU) $(QEMU_HEADLESS) -serial file:serial.log || true
	@a=$$(grep -c '\[task\] A' serial.log 2>/dev/null || true); \
	b=$$(grep -c '\[task\] B' serial.log 2>/dev/null || true); \
	ok=0; \
	if grep -q 'pmm] self-test: PASS' serial.log && \
	   grep -q 'kmalloc] self-test: PASS' serial.log && \
	   grep -q 'uaccess] self-test: PASS' serial.log && \
	   grep -q 'SMAP supported, enabled' serial.log && \
	   grep -q 'CR3 switch self-test: PASS' serial.log && \
	   grep -q '\[smp\] MADT:' serial.log && \
	   grep -q 'spin counter=300000' serial.log && \
	   grep -q 'percpu ok=3' serial.log && \
	   grep -q 'AP vector 0x30 handled' serial.log && \
	   grep -q 'timer ticks=' serial.log && \
	   grep -q 'name=user-apuser cpu=' serial.log && \
	   grep -q 'name=user-apuser.*status=42' serial.log && \
	   grep -q 'name=user-apuser2 cpu=2 status=42' serial.log && \
	   grep -q 'name=user-apuser3 cpu=3 status=42' serial.log && \
	   grep -q 'stress: worker PASS' serial.log && \
	   [ $$(grep -c 'stress: worker PASS' serial.log) -ge 3 ] && \
	   grep -q 'M4] VFS: superblock' serial.log && \
	   grep -q 'Lantest shell' serial.log && \
	   grep -q 'keyboard IRQ1 enabled' serial.log && \
	   grep -q '/bin/sh' serial.log && \
	   grep -q 'virtio-blk ready' serial.log && \
	   grep -q 'virtio] self-test: PASS' serial.log && \
	   grep -q 'lfs] self-test: PASS' serial.log && \
	   grep -q "hello from an exec'd ELF!" serial.log && \
	   grep -q 'argc=1' serial.log && \
	   grep -q 'cowtest: PASS' serial.log && \
	   grep -q 'mmaptest: PASS' serial.log && \
	   grep -q 'segvtest: about to fault' serial.log && \
	   grep -q 'signal=11' serial.log && \
	   grep -q 'muntest: PASS' serial.log && \
	   grep -q 'malloctest: PASS' serial.log && \
	   grep -q 'shmtest: PASS' serial.log && \
	   grep -q 'libctest: PASS' serial.log && \
	   grep -q 'fiotest: PASS' serial.log && \
	   grep -q 'sjtest: PASS' serial.log && \
	   grep -q 'floattest: PASS' serial.log && \
	   grep -q 'Hello from Lantest OS!' serial.log && \
	   grep -q 'HELLO LUA FROM LANTEST !' serial.log && \
	   grep -q 'File written.' serial.log && \
	   grep -q 'Written by Lua in Lantest OS!' serial.log && \
	   grep -q 'maptest: PASS' serial.log && \
	   grep -q 'pipe-ok' serial.log && \
	   grep -q 'unlinktest: delete-on-close PASS' serial.log && \
	   grep -q 'run: child exited' serial.log && \
	   grep -q 'Lantest shell' serial.log && \
	   grep -q 'keyboard IRQ1 enabled' serial.log && \
	   grep -q '/bin/sh' serial.log && \
	   grep -q 'M4] console fd table' serial.log && \
	   grep -q 'scheduler running:' serial.log && \
	   grep -q '\[elf\] PT_LOAD' serial.log && \
	   grep -q 'child: C ELF' serial.log && \
	   grep -q 'parent: C ELF' serial.log && \
	   grep -q 'kernel pointer rejected' serial.log && \
	   grep -q 'unmapped user pointer rejected' serial.log && \
	   grep -q 'bad fd rejected' serial.log && \
	   grep -q 'filetest: read back: Hello from ramfs!' serial.log && \
	   grep -q 'filetest: EOF=0' serial.log && \
	   grep -q 'filetest: big file (5000 bytes) PASS' serial.log && \
	   grep -q 'filetest: missing file rejected' serial.log && \
	   grep -q 'filetest: PASS' serial.log && \
	   grep -q 'proc] wait: parent' serial.log && \
	   grep -q 'status=42' serial.log && \
	   grep -q 'reap] orphan' serial.log && \
	   grep -q 'timer IRQ from Ring 3' serial.log && \
	   [ "$$a" -ge 2 ] && [ "$$b" -ge 2 ]; then ok=1; fi; \
	if [ $$ok -eq 1 ]; then \
		echo "M5-2 boot test: PASS (task A=$$a, task B=$$b, VFS/ramfs + fd + uaccess ok)"; \
	else \
		echo "M5-2 boot test: FAIL (task A=$$a, task B=$$b)"; \
		cat serial.log 2>/dev/null; exit 1; \
	fi

test-exception: disk.img
	$(MAKE) clean
	$(MAKE) EXTRA_CFLAGS=-DLANTEST_EXCEPTION_TEST lantest.iso
	rm -f serial.log
	timeout $(QEMU_TIMEOUT)s $(QEMU) $(QEMU_HEADLESS) -serial file:serial.log || true
	@ok=0; \
	if grep -q 'CPU EXCEPTION 14' serial.log && \
	   grep -q 'page fault' serial.log && \
	   grep -q 'KERNEL PANIC' serial.log; then ok=1; fi; \
	cat serial.log 2>/dev/null; \
	$(MAKE) clean >/dev/null 2>&1; \
	if [ $$ok -eq 1 ]; then \
		echo 'M5-2 exception test: PASS'; \
	else \
		echo 'M4-3 exception test: FAIL'; exit 1; \
	fi

disk.img:
	truncate -s 16M $@

clean-disk:
	rm -f disk.img

# Two boots against the same disk.img: proves the virtio-blk sector write
# survives a reboot (boot_count increments).
test-persist: lantest.iso
	rm -f disk.img persist1.log persist2.log
	truncate -s 16M disk.img
	timeout $(QEMU_TIMEOUT)s $(QEMU) $(QEMU_HEADLESS) -serial file:persist1.log || true
	timeout $(QEMU_TIMEOUT)s $(QEMU) $(QEMU_HEADLESS) -serial file:persist2.log || true
	@if grep -q 'virtio] self-test: PASS (disk boot_count=1)' persist1.log && \
	   grep -q 'lfs] self-test: PASS (boot_count=1)' persist1.log && \
	   grep -q 'virtio] self-test: PASS (disk boot_count=2)' persist2.log && \
	   grep -q 'lfs] self-test: PASS (boot_count=2)' persist2.log && \
	   grep -q '/bin/hello already on disk' persist2.log && \
	   grep -q '/bin/sh already on disk' persist2.log; then \
		echo 'M5-2 persistence test: PASS (virtio + LFS boot_count 1 -> 2)'; \
	else \
		echo 'M5-2 persistence test: FAIL'; \
		grep -a 'virtio' persist1.log persist2.log || true; exit 1; \
	fi

clean:
	rm -f kernel.elf lantest.iso kernel.map serial.log user_blobs.o persist1.log persist2.log
	rm -f user/*.elf user/lua.elf
	rm -rf user/lua_port/obj
	rm -rf iso
	find . -name '*.o' -delete
