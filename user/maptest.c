/*
 * Lantest OS - file-backed mmap test
 */
#include "userlib.h"
#include "crt.h"

static const char text[] = "file-backed mmap works";

int main(int argc, char **argv, char **envp)
{
    volatile const char *p;
    i64 fd;
    i64 addr;

    (void)argc;
    (void)argv;
    (void)envp;

    fd = sys_open("/maptest.txt", O_CREAT | O_TRUNC | O_RDWR);
    if (fd < 0) {
        sys_write_str("maptest: create FAIL\n");
        return 1;
    }
    sys_write(fd, text, sizeof(text) - 1);
    sys_close(fd);

    fd = sys_open("/maptest.txt", O_RDONLY);
    if (fd < 0) {
        sys_write_str("maptest: open FAIL\n");
        return 1;
    }

    addr = sys_mmap_file(fd, 0, 4096);
    sys_close(fd);
    if (addr < 0) {
        sys_write_str("maptest: mmap_file FAIL\n");
        return 1;
    }

    p = (volatile const char *)addr;
    for (u64 i = 0; i < sizeof(text) - 1; ++i) {
        if (p[i] != text[i]) {
            sys_write_str("maptest: content FAIL\n");
            return 1;
        }
    }
    if (p[sizeof(text) - 1] != 0) {
        sys_write_str("maptest: tail not zero\n");
        return 1;
    }

    sys_write_str("maptest: PASS\n");
    return 0;
}
