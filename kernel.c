/*
 * Lantest OS - M3 kernel entry
 *
 * M1: long mode boot, UART/VGA console.
 * M2: GDT/TSS, IDT, exceptions, PIC/PIT, physical page allocator.
 * M3a: kmalloc/kfree heap, tasks, switch_to, round-robin scheduling.
 * M3b: per-task PML4/CR3, Ring 3 user tasks, int 0x80 syscalls.
 * M3c-1: exit -> ZOMBIE, wait() reaps children, idle orphan reaper.
 * M3c-2: ELF64 user programs, page-wise PT_LOAD loading.
 * M3c-3: copy_from_user/copy_to_user, page-fault fixup, SMAP.
 */
#include <types.h>
#include <kprintf.h>
#include <uart.h>
#include <keyboard.h>
#include <vga.h>
#include <panic.h>
#include <multiboot2.h>
#include <pmm.h>
#include <kmalloc.h>
#include <elf.h>
#include <task.h>
#include <uaccess.h>
#include <vfs.h>
#include <virtio_blk.h>
#include <lantestfs.h>
#include <x86_64/gdt.h>
#include <x86_64/idt.h>
#include <x86_64/isr.h>
#include <x86_64/irq.h>
#include <x86_64/paging.h>
#include <smp.h>
#include <percpu.h>
#include <x86_64/usermode.h>

/* Linker-provided boundaries of the kernel image (physical == virtual). */
extern u8 _kernel_start[];
extern u8 _kernel_end[];

/* Embedded ELF64 user images (linked by user_blobs.o, see Makefile). */
extern const u8 _binary_user_parent_elf_start[];
extern const u8 _binary_user_parent_elf_end[];
extern const u8 _binary_user_child_elf_start[];
extern const u8 _binary_user_child_elf_end[];
extern const u8 _binary_user_filetest_elf_start[];
extern const u8 _binary_user_filetest_elf_end[];
extern const u8 _binary_user_shell_elf_start[];
extern const u8 _binary_user_shell_elf_end[];
extern const u8 _binary_user_hello_elf_start[];
extern const u8 _binary_user_hello_elf_end[];
extern const u8 _binary_user_unlinktest_elf_start[];
extern const u8 _binary_user_unlinktest_elf_end[];
extern const u8 _binary_user_init_elf_start[];
extern const u8 _binary_user_init_elf_end[];
extern const u8 _binary_user_ls_elf_start[];
extern const u8 _binary_user_ls_elf_end[];
extern const u8 _binary_user_cat_elf_start[];
extern const u8 _binary_user_cat_elf_end[];
extern const u8 _binary_user_echo_elf_start[];
extern const u8 _binary_user_echo_elf_end[];
extern const u8 _binary_user_head_elf_start[];
extern const u8 _binary_user_head_elf_end[];
extern const u8 _binary_user_cowtest_elf_start[];
extern const u8 _binary_user_cowtest_elf_end[];
extern const u8 _binary_user_mmaptest_elf_start[];
extern const u8 _binary_user_mmaptest_elf_end[];
extern const u8 _binary_user_segvtest_elf_start[];
extern const u8 _binary_user_segvtest_elf_end[];
extern const u8 _binary_user_maptest_elf_start[];
extern const u8 _binary_user_maptest_elf_end[];
extern const u8 _binary_user_muntest_elf_start[];
extern const u8 _binary_user_muntest_elf_end[];
extern const u8 _binary_user_malloctest_elf_start[];
extern const u8 _binary_user_malloctest_elf_end[];
extern const u8 _binary_user_shmtest_elf_start[];
extern const u8 _binary_user_shmtest_elf_end[];
extern const u8 _binary_user_libctest_elf_start[];
extern const u8 _binary_user_libctest_elf_end[];
extern const u8 _binary_user_fiotest_elf_start[];
extern const u8 _binary_user_fiotest_elf_end[];
extern const u8 _binary_user_sjtest_elf_start[];
extern const u8 _binary_user_sjtest_elf_end[];
extern const u8 _binary_user_floattest_elf_start[];
extern const u8 _binary_user_floattest_elf_end[];
extern const u8 _binary_user_lua_elf_start[];
extern const u8 _binary_user_lua_elf_end[];
extern const u8 _binary_user_apuser_elf_start[];
extern const u8 _binary_user_apuser_elf_end[];
extern const u8 _binary_user_stress_elf_start[];
extern const u8 _binary_user_stress_elf_end[];


static int blob_equal(const u8 *a, const u8 *b, u64 n)
{
    for (u64 i = 0; i < n; ++i) {
        if (a[i] != b[i])
            return 0;
    }
    return 1;
}

static void install_binary(const char *path, const u8 *start, const u8 *end)
{
    u64 size = (u64)(end - start);
    struct dentry *d = vfs_lookup(path);

    if (d != NULL && d->d_inode != NULL && d->d_inode->i_size == size) {
        u8 *tmp = (u8 *)kmalloc(size);

        if (tmp != NULL) {
            u64 got = 0;
            int same = 0;

            if (vfs_read_file(path, tmp, size, &got) == 0 &&
                got == size && blob_equal(tmp, start, size))
                same = 1;
            kfree(tmp);
            if (same) {
                kprintf("[M4] %s already on disk (%lu bytes)\n", path, size);
                return;
            }
        }
    }

    if (vfs_write_blob(path, start, size) == 0)
        kprintf("[M4] %s %s (%lu bytes)\n",
                d == NULL ? "installed" : "updated", path, size);
    else
        kprintf("[M4] WARNING: failed to install %s\n", path);
}

/* The register frame layout is shared with arch/x86_64/isr_asm.S. */
static_assert(sizeof(registers_t) == 22 * 8, "registers_t must match isr_asm.S");

#define THREAD_PERIOD_TICKS 20u   /* print roughly every 200 ms */
#define REAP_PERIOD_TICKS   100u  /* orphan reaper runs about once a second */

static void wait_ticks(u64 ticks)
{
    u64 start = timer_ticks();

    while (timer_ticks() - start < ticks)
        __asm__ volatile ("hlt");
}

static void kthread_a(void)
{
    for (;;) {
        kprintf("[task] A pid=%lu tick=%lu cr3=%p rsp0=%p\n",
                task_current()->pid, timer_ticks(),
                (void *)paging_read_cr3(), (void *)gdt_tss_rsp0());
        wait_ticks(THREAD_PERIOD_TICKS);
    }
}

static void kthread_b(void)
{
    for (;;) {
        kprintf("[task] B pid=%lu tick=%lu cr3=%p rsp0=%p\n",
                task_current()->pid, timer_ticks(),
                (void *)paging_read_cr3(), (void *)gdt_tss_rsp0());
        wait_ticks(THREAD_PERIOD_TICKS);
    }
}

static int cr3_switch_self_test(struct task *a, struct task *b)
{
    struct task *tasks[2];
    volatile u64 *probe;
    u64 original;
    int ok = 1;

    probe = (volatile u64 *)kmalloc(64);
    if (probe == NULL)
        return 0;

    *probe = 0x0123456789ABCDEFULL;
    original = paging_read_cr3();
    tasks[0] = a;
    tasks[1] = b;

    for (u32 i = 0; i < 2; ++i) {
        if (tasks[i]->cr3 == 0 || tasks[i]->cr3 == original)
            ok = 0;

        paging_write_cr3(tasks[i]->cr3);
        if (paging_read_cr3() != tasks[i]->cr3)
            ok = 0;
        if (*probe != 0x0123456789ABCDEFULL)
            ok = 0;

        kprintf("[M3b] CR3 -> %s (%p): identity probe %s\n",
                tasks[i]->name, (void *)tasks[i]->cr3,
                ok ? "ok" : "FAILED");
    }

    paging_write_cr3(original);
    if (paging_read_cr3() != original)
        ok = 0;

    kfree((void *)probe);
    return ok;
}

void kernel_main(u64 mb2_info, u32 mb2_magic)
{
    int have_uart = uart_init();

    vga_init();

    kprintf("\n");
    kprintf("==================================================\n");
    kprintf("  Lantest OS  -  M3c: uaccess + ELF + lifecycle\n");
    kprintf("  independent x86_64 kernel, no Linux code/ABI\n");
    kprintf("==================================================\n");
    kprintf("\n");
    kprintf("[boot] long mode active, kernel_main() running\n");

    if (mb2_magic != MB2_BOOTLOADER_MAGIC) {
        kprintf("[boot] ERROR: bad Multiboot2 magic %x\n", mb2_magic);
        panic("invalid boot information");
    }

    u32 info_size = *(volatile u32 *)mb2_info;
    kprintf("[boot] Multiboot2 info at %p, size %lu bytes\n",
            (void *)mb2_info, (u64)info_size);
    kprintf("[dev ] serial: %s\n",
            have_uart ? "COM1 present (115200 8N1)" : "absent, VGA only");
    kprintf("[dev ] display: VGA text 80x25 at 0xB8000\n");
    kprintf("[dev ] kernel image: %p - %p\n",
            (void *)_kernel_start, (void *)_kernel_end);

    /* ---- M2: CPU tables, interrupts and physical memory ---- */
    gdt_init();
    kprintf("[M2] GDT reloaded: TSS at selector 0x28, rsp0=%p\n",
            (void *)gdt_tss_rsp0());
    kprintf("[M2] GDT user segments: CS=0x1B, SS=0x23 (DPL 3)\n");

    idt_init();
    kprintf("[M2] IDT loaded: 256 gates, int 0x80 gate DPL=3\n");

    pic_remap();
    kprintf("[M2] PIC remapped: IRQ0-7 -> 0x20-0x27, IRQ8-15 -> 0x28-0x2F\n");

    pit_init(100);
    kprintf("[M2] PIT programmed: 100 Hz on IRQ0\n");

    keyboard_init();

    kprintf("\n");
    pmm_init(mb2_info, (u64)_kernel_start, (u64)_kernel_end);
    pmm_self_test();
    kprintf("\n");

    kmalloc_init();
    kmalloc_self_test();
    kprintf("\n");

    smp_init(mb2_info);
    kprintf("\n");

    /* ---- M3c-3: user memory access hardening ---- */
    kprintf("[M3c3] user-access hardening: copyin/copyout + fixup + SMAP\n");
    if (!uaccess_init())
        panic("uaccess self-test failed");
    kprintf("\n");

    kprintf("[M5] virtio-blk: probing legacy PCI device...\n");
    if (virtio_blk_init()) {
        if (!virtio_blk_selftest())
            kprintf("[M5] WARNING: virtio-blk self-test failed\n");
    } else {
        kprintf("[M5] virtio-blk unavailable; RAM-only mode\n");
    }
    kprintf("\n");

    kprintf("[M4] console fd table: fd0=stdin fd1=stdout fd2=stderr\n");
    kprintf("[M4] file_operations registered for the console char device\n");

    vfs_init();
    if (virtio_blk_present() && lfs_mounted()) {
        if (!lfs_selftest())
            kprintf("[M5] WARNING: LantestFS self-test failed\n");
    }

    vfs_mkdir("/bin", 0755);
    install_binary("/bin/hello",
                   _binary_user_hello_elf_start, _binary_user_hello_elf_end);
    install_binary("/bin/sh",
                   _binary_user_shell_elf_start, _binary_user_shell_elf_end);
    install_binary("/bin/ls",
                   _binary_user_ls_elf_start, _binary_user_ls_elf_end);
    install_binary("/bin/cat",
                   _binary_user_cat_elf_start, _binary_user_cat_elf_end);
    install_binary("/bin/echo",
                   _binary_user_echo_elf_start, _binary_user_echo_elf_end);
    install_binary("/bin/head",
                   _binary_user_head_elf_start, _binary_user_head_elf_end);
    install_binary("/bin/lua",
                   _binary_user_lua_elf_start, _binary_user_lua_elf_end);

#ifdef LANTEST_EXCEPTION_TEST
    kprintf("[test] deliberate page fault at 0x200000000 (expect #PF panic)\n");
    *(volatile u64 *)0x200000000ULL = 0xDEADBEEFULL;
#endif

    /* ---- M3a: tasks + round-robin ---- */
    scheduler_init();
    struct task *ta = task_create_kernel("kthread-a", kthread_a);

    u64 apuser_size =
        (u64)(_binary_user_apuser_elf_end - _binary_user_apuser_elf_start);
    struct task *apuser = task_create_user_elf("user-apuser",
                                               _binary_user_apuser_elf_start,
                                               apuser_size);
    for (u32 cpu = 1; cpu <= 3; ++cpu) {
        struct task *t = (cpu == 1) ? apuser :
            task_create_user_elf(cpu == 2 ? "user-apuser2" : "user-apuser3",
                                 _binary_user_apuser_elf_start,
                                 apuser_size);
        t->cpu = cpu;
        t->state = TASK_READY;
        percpu_enqueue_task(cpu, t);
        kprintf("[smp] AP user task pid=%lu queued on CPU%u\n", t->pid, cpu);
    }

    {
        u64 stress_size =
            (u64)(_binary_user_stress_elf_end - _binary_user_stress_elf_start);

        for (u32 cpu = 1; cpu <= 3; ++cpu) {
            char nm[TASK_NAME_MAX];

            nm[0] = 's'; nm[1] = 't'; nm[2] = 'r'; nm[3] = 'e';
            nm[4] = 's'; nm[5] = 's'; nm[6] = (char)('0' + cpu); nm[7] = 0;
            struct task *t = task_create_user_elf(nm,
                _binary_user_stress_elf_start, stress_size);
            t->cpu = cpu;
            t->state = TASK_READY;
            percpu_enqueue_task(cpu, t);
            kprintf("[smp] stress worker pid=%lu queued on CPU%u\n", t->pid, cpu);
        }
    }
    struct task *tb = task_create_kernel("kthread-b", kthread_b);

    kprintf("[M3] scheduler initialized: %lu tasks in the run queue\n",
            task_count());
    kprintf("[M3b] per-task PML4: boot=%p, %s=%p, %s=%p\n",
            (void *)task_current()->cr3,
            ta->name, (void *)ta->cr3,
            tb->name, (void *)tb->cr3);

    if (!cr3_switch_self_test(ta, tb))
        panic("CR3 switching self-test failed");
    kprintf("[M3b] CR3 switch self-test: PASS\n");

    /* ---- M4-6: init process (execs /bin/sh from LantestFS) ---- */
    u64 init_size =
        (u64)(_binary_user_init_elf_end - _binary_user_init_elf_start);
    struct task *tinit = task_create_user_elf("user-init",
                                              _binary_user_init_elf_start,
                                              init_size);
    kprintf("[M4] init task pid=%lu (execs /bin/sh from LantestFS)\n",
            tinit->pid);

    /* ---- M3c-2: ELF self-check + Ring 3 tasks ---- */
    kprintf("\n");
    kprintf("[M3c2] ELF loader self-check: embedded ELF64 user images:\n");

    u64 parent_size =
        (u64)(_binary_user_parent_elf_end - _binary_user_parent_elf_start);
    u64 child_size =
        (u64)(_binary_user_child_elf_end - _binary_user_child_elf_start);

    struct task *tparent = task_create_user_elf("user-parent",
                                                _binary_user_parent_elf_start,
                                                parent_size);
    struct task *tchild = task_create_user_elf("user-child",
                                               _binary_user_child_elf_start,
                                               child_size);
    tchild->parent = tparent;

    u64 filetest_size =
        (u64)(_binary_user_filetest_elf_end - _binary_user_filetest_elf_start);
    struct task *tfile = task_create_user_elf("user-filetest",
                                              _binary_user_filetest_elf_start,
                                              filetest_size);
    kprintf("[M4] filetest task: pid=%lu\n", tfile->pid);

    u64 unlinktest_size =
        (u64)(_binary_user_unlinktest_elf_end - _binary_user_unlinktest_elf_start);
    struct task *tunlink = task_create_user_elf("user-unlinktest",
                                                _binary_user_unlinktest_elf_start,
                                                unlinktest_size);
    kprintf("[M4] unlinktest task: pid=%lu\n", tunlink->pid);

    u64 cowtest_size =
        (u64)(_binary_user_cowtest_elf_end - _binary_user_cowtest_elf_start);
    struct task *tcow = task_create_user_elf("user-cowtest",
                                             _binary_user_cowtest_elf_start,
                                             cowtest_size);
    kprintf("[M4] cowtest task: pid=%lu\n", tcow->pid);

    u64 mmaptest_size =
        (u64)(_binary_user_mmaptest_elf_end - _binary_user_mmaptest_elf_start);
    struct task *tmmap = task_create_user_elf("user-mmaptest",
                                              _binary_user_mmaptest_elf_start,
                                              mmaptest_size);
    kprintf("[M4] mmaptest task: pid=%lu\n", tmmap->pid);

    u64 segvtest_size =
        (u64)(_binary_user_segvtest_elf_end - _binary_user_segvtest_elf_start);
    struct task *tsegv = task_create_user_elf("user-segvtest",
                                              _binary_user_segvtest_elf_start,
                                              segvtest_size);
    kprintf("[M4] segvtest task: pid=%lu\n", tsegv->pid);

    u64 maptest_size =
        (u64)(_binary_user_maptest_elf_end - _binary_user_maptest_elf_start);
    struct task *tmap = task_create_user_elf("user-maptest",
                                             _binary_user_maptest_elf_start,
                                             maptest_size);
    kprintf("[M4] maptest task: pid=%lu\n", tmap->pid);

    u64 muntest_size =
        (u64)(_binary_user_muntest_elf_end - _binary_user_muntest_elf_start);
    struct task *tmun = task_create_user_elf("user-muntest",
                                             _binary_user_muntest_elf_start,
                                             muntest_size);
    kprintf("[M4] muntest task: pid=%lu\n", tmun->pid);

    u64 malloctest_size =
        (u64)(_binary_user_malloctest_elf_end - _binary_user_malloctest_elf_start);
    struct task *tmalloc = task_create_user_elf("user-malloctest",
                                                _binary_user_malloctest_elf_start,
                                                malloctest_size);
    kprintf("[M4] malloctest task: pid=%lu\n", tmalloc->pid);

    u64 shmtest_size =
        (u64)(_binary_user_shmtest_elf_end - _binary_user_shmtest_elf_start);
    struct task *tshm = task_create_user_elf("user-shmtest",
                                             _binary_user_shmtest_elf_start,
                                             shmtest_size);
    kprintf("[M4] shmtest task: pid=%lu\n", tshm->pid);

    u64 libctest_size =
        (u64)(_binary_user_libctest_elf_end - _binary_user_libctest_elf_start);
    struct task *tlibc = task_create_user_elf("user-libctest",
                                              _binary_user_libctest_elf_start,
                                              libctest_size);
    kprintf("[M4] libctest task: pid=%lu\n", tlibc->pid);

    u64 fiotest_size =
        (u64)(_binary_user_fiotest_elf_end - _binary_user_fiotest_elf_start);
    struct task *tfio = task_create_user_elf("user-fiotest",
                                             _binary_user_fiotest_elf_start,
                                             fiotest_size);
    kprintf("[M4] fiotest task: pid=%lu\n", tfio->pid);

    u64 sjtest_size =
        (u64)(_binary_user_sjtest_elf_end - _binary_user_sjtest_elf_start);
    struct task *tsj = task_create_user_elf("user-sjtest",
                                            _binary_user_sjtest_elf_start,
                                            sjtest_size);
    kprintf("[M4] sjtest task: pid=%lu\n", tsj->pid);

    u64 floattest_size =
        (u64)(_binary_user_floattest_elf_end - _binary_user_floattest_elf_start);
    struct task *tfloat = task_create_user_elf("user-floattest",
                                               _binary_user_floattest_elf_start,
                                               floattest_size);
    kprintf("[M4] floattest task: pid=%lu\n", tfloat->pid);

    u64 lua_size =
        (u64)(_binary_user_lua_elf_end - _binary_user_lua_elf_start);
    struct task *tlua = task_create_user_elf("user-lua",
                                             _binary_user_lua_elf_start,
                                             lua_size);
    kprintf("[M4] lua task: pid=%lu (%lu bytes)\n", tlua->pid, lua_size);

    kprintf("[M3c2] ELF tasks: parent pid=%lu entry=%p, child pid=%lu entry=%p\n",
            tparent->pid, (void *)tparent->user_entry,
            tchild->pid, (void *)tchild->user_entry);

    kprintf("[M3c] lifecycle: exit -> ZOMBIE, parent wait() reaps child, "
            "orphans reaped by idle task\n");
    kprintf("[M3] starting round-robin: PIT slice = 100 ms, "
            "2 kernel threads + 2 user tasks\n");

    __asm__ volatile ("sti");
    schedule();   /* first switch; returns to the boot task via the scheduler */

    /*
     * Idle loop for the boot task: re-enable interrupts, sleep, and
     * periodically reap orphaned zombies (children whose parent is gone).
     */
    u64 last_reap = 0;
    for (;;) {
        __asm__ volatile ("sti");
        __asm__ volatile ("hlt");

        u64 now = timer_ticks();
        if (now - last_reap >= REAP_PERIOD_TICKS) {
            last_reap = now;
            task_reap_orphans();
            task_reap_deferred();
        }
    }
}
