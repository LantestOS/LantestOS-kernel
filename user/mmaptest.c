/*
 * Lantest OS - mmap/VMA self-test
 */
#include "userlib.h"
#include "crt.h"

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
        sys_write_str("mmaptest: mmap FAIL\n");
        return 1;
    }
    p = (volatile unsigned char *)addr;

    for (int i = 0; i < 8192; ++i)
        p[i] = (unsigned char)(i & 0xff);
    for (int i = 0; i < 8192; ++i) {
        if (p[i] != (unsigned char)(i & 0xff)) {
            sys_write_str("mmaptest: write FAIL\n");
            return 1;
        }
    }

    pid = sys_fork();
    if (pid == 0) {
        p[0] = 0xAA;
        if (p[0] != 0xAA) {
            sys_write_str("mmaptest: child COW FAIL\n");
            sys_exit(1);
        }
        sys_exit(0);
    }
    if (pid < 0 || sys_wait(&st) != pid || st != 0) {
        sys_write_str("mmaptest: wait FAIL\n");
        return 1;
    }

    if (p[0] != 0) {
        sys_write_str("mmaptest: parent isolation FAIL\n");
        return 1;
    }

    p[0] = 0x55;
    if (p[0] != 0x55) {
        sys_write_str("mmaptest: parent COW FAIL\n");
        return 1;
    }

    sys_write_str("mmaptest: PASS\n");
    return 0;
}
