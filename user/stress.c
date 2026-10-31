/*
 * Lantest OS - M4-46 multi-AP stress worker
 *
 * Each worker runs on its own AP (cpu=1/2/3), forks children (children inherit
 * the worker's CPU affinity), children touch memory + create/write a file,
 * parent waits for all children. Kernel then reaps the workers.
 */
#include "userlib.h"
#include "libc.h"
#include "malloc.h"
#include "stdio.h"
#include "crt.h"

#define STRESS_CHILDREN 3

static void child_work(int idx)
{
    char path[48];
    char *mem;
    i64 fd;

    snprintf(path, sizeof(path), "/stress_%d_%d.txt",
             (int)sys_getpid(), idx);

    fd = sys_open(path, O_CREAT | O_WRONLY | O_TRUNC);
    if (fd < 0)
        sys_exit(3);

    mem = (char *)malloc(65536);
    if (mem == NULL) {
        sys_write(fd, "NOMEM\n", 6);
        sys_close(fd);
        sys_exit(4);
    }

    for (int i = 0; i < 65536; i += 4096)
        mem[i] = (char)('A' + idx);
    mem[65535] = (char)('a' + idx);

    sys_write(fd, mem, 512);
    free(mem);
    sys_close(fd);
    sys_exit(7);
}

int main(int argc, char **argv, char **envp)
{
    int ok = 0;

    (void)argc;
    (void)argv;
    (void)envp;

    for (int i = 0; i < STRESS_CHILDREN; ++i) {
        i64 pid = sys_fork();

        if (pid == 0)
            child_work(i);
    }

    for (int i = 0; i < STRESS_CHILDREN; ++i) {
        i64 st = 0;
        i64 pid = sys_wait(&st);

        if (pid > 0)
            ++ok;
    }

    if (ok == STRESS_CHILDREN)
        printf("stress: worker PASS children=%d\n", ok);
    else
        printf("stress: worker FAIL children=%d/%d\n", ok, STRESS_CHILDREN);

    return ok == STRESS_CHILDREN ? 0 : 1;
}
