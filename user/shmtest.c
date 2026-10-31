/*
 * Lantest OS - MAP_SHARED + msync test
 */
#include "userlib.h"
#include "crt.h"

int main(int argc, char **argv, char **envp)
{
    volatile char *p;
    char c = 'A';
    char out = 0;
    i64 fd;
    i64 addr;
    i64 pid;
    i64 st = 0;

    (void)argc;
    (void)argv;
    (void)envp;

    fd = sys_open("/shmtest", O_CREAT | O_TRUNC | O_RDWR);
    if (fd < 0) {
        sys_write_str("shmtest: create FAIL\n");
        return 1;
    }
    sys_write(fd, &c, 1);
    sys_close(fd);

    fd = sys_open("/shmtest", O_RDWR);
    if (fd < 0) {
        sys_write_str("shmtest: open FAIL\n");
        return 1;
    }
    addr = sys_mmap_shared(fd, 0, 4096);
    sys_close(fd);
    if (addr < 0) {
        sys_write_str("shmtest: mmap_shared FAIL\n");
        return 1;
    }

    p = (volatile char *)addr;
    if (p[0] != 'A') {
        sys_write_str("shmtest: initial read FAIL\n");
        return 1;
    }

    pid = sys_fork();
    if (pid == 0) {
        p[0] = 'B';
        sys_exit(0);
    }
    if (pid < 0 || sys_wait(&st) != pid || st != 0) {
        sys_write_str("shmtest: child FAIL\n");
        return 1;
    }

    if (p[0] != 'B') {
        sys_write_str("shmtest: shared visibility FAIL\n");
        return 1;
    }

    if (sys_msync(addr, 4096) != 0) {
        sys_write_str("shmtest: msync FAIL\n");
        return 1;
    }

    fd = sys_open("/shmtest", O_RDONLY);
    if (fd < 0 || sys_read(fd, &out, 1) != 1) {
        sys_write_str("shmtest: reopen FAIL\n");
        return 1;
    }
    sys_close(fd);
    if (out != 'B') {
        sys_write_str("shmtest: writeback FAIL\n");
        return 1;
    }

    sys_write_str("shmtest: PASS\n");
    return 0;
}
