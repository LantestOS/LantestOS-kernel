/*
 * Lantest OS - COW fork self-test
 *
 * A writable global is shared by fork; the child writes first, then the
 * parent verifies its own value is unchanged, and finally writes itself.
 */
#include "userlib.h"
#include "crt.h"

static volatile u64 cow_value = 0x1111;

int main(int argc, char **argv, char **envp)
{
    i64 pid;
    i64 st = 0;
    i64 r;

    (void)argc;
    (void)argv;
    (void)envp;

    pid = sys_fork();
    if (pid == 0) {
        cow_value = 0x2222;
        if (cow_value != 0x2222) {
            sys_write_str("cowtest: child FAIL\n");
            sys_exit(1);
        }
        sys_exit(0);
    }
    if (pid < 0) {
        sys_write_str("cowtest: fork FAIL\n");
        return 1;
    }

    r = sys_wait(&st);
    if (r != pid || st != 0) {
        sys_write_str("cowtest: wait FAIL\n");
        return 1;
    }
    if (cow_value != 0x1111) {
        sys_write_str("cowtest: parent isolation FAIL\n");
        return 1;
    }

    cow_value = 0x3333;
    if (cow_value != 0x3333) {
        sys_write_str("cowtest: parent write FAIL\n");
        return 1;
    }

    sys_write_str("cowtest: PASS\n");
    return 0;
}
