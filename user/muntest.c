/*
 * Lantest OS - munmap / mprotect self-test
 */
#include "userlib.h"
#include "crt.h"

#define PROT_READ  1
#define PROT_WRITE 2

int main(int argc, char **argv, char **envp)
{
    volatile unsigned char *p;
    i64 addr;
    i64 pid;
    i64 st = 0;

    (void)argc;
    (void)argv;
    (void)envp;

    addr = sys_mmap(8192);
    if (addr < 0) {
        sys_write_str("muntest: mmap FAIL\n");
        return 1;
    }
    p = (volatile unsigned char *)addr;
    p[0] = 11;
    p[4096] = 22;

    if (sys_mprotect(addr, 4096, PROT_READ) != 0) {
        sys_write_str("muntest: mprotect RO FAIL\n");
        return 1;
    }

    pid = sys_fork();
    if (pid == 0) {
        p[0] = 33;          /* must SIGSEGV */
        sys_exit(0);
    }
    if (pid < 0 || sys_wait(&st) != pid || st != 139) {
        sys_write_str("muntest: child protection FAIL\n");
        return 1;
    }

    if (sys_mprotect(addr, 4096, PROT_READ | PROT_WRITE) != 0) {
        sys_write_str("muntest: mprotect RW FAIL\n");
        return 1;
    }
    p[0] = 44;
    if (p[0] != 44) {
        sys_write_str("muntest: write after RW FAIL\n");
        return 1;
    }

    if (sys_munmap(addr + 4096, 4096) != 0) {
        sys_write_str("muntest: munmap FAIL\n");
        return 1;
    }
    if (p[0] != 44) {
        sys_write_str("muntest: surviving page FAIL\n");
        return 1;
    }

    sys_write_str("muntest: PASS\n");
    return 0;
}
