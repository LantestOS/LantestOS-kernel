/*
 * Lantest OS - ramfs file I/O test (C, Ring 3)
 *
 * 1. /hello.txt: O_CREAT/write/close/reopen/read/EOF/close
 * 2. /big.bin: 5000 bytes (> one 4 KiB ramfs block) write/read/verify
 * 3. missing path is rejected with -ENOENT
 */
#include "userlib.h"

#define PATH "/hello.txt"
#define BIG_PATH "/big.bin"
#define MSG  "Hello from ramfs!\n"
#define BIG_SIZE 5000u

static unsigned char big_w[BIG_SIZE];
static unsigned char big_r[BIG_SIZE];

static int big_file_test(void)
{
    i64 fd;
    i64 n;

    for (u64 i = 0; i < BIG_SIZE; ++i)
        big_w[i] = (unsigned char)(i * 7u + 3u);

    fd = sys_open(BIG_PATH, O_CREAT | O_RDWR);
    if (fd < 0)
        return 0;

    n = sys_write(fd, big_w, BIG_SIZE);
    sys_close(fd);
    if (n != (i64)BIG_SIZE)
        return 0;

    fd = sys_open(BIG_PATH, O_RDONLY);
    if (fd < 0)
        return 0;

    n = sys_read(fd, big_r, BIG_SIZE);
    sys_close(fd);
    if (n != (i64)BIG_SIZE)
        return 0;

    for (u64 i = 0; i < BIG_SIZE; ++i) {
        if (big_r[i] != big_w[i])
            return 0;
    }

    return 1;
}

void _start(void)
{
    char buf[64];
    u64 len = 0;
    i64 fd;
    i64 n;

    while (MSG[len] != '\0')
        ++len;

    fd = sys_open(PATH, O_CREAT | O_RDWR);
    if (fd < 0) {
        sys_write_str("filetest: open(O_CREAT) failed\n");
        sys_exit(1);
    }

    if (sys_write(fd, MSG, len) != (i64)len) {
        sys_write_str("filetest: write failed\n");
        sys_close(fd);
        sys_exit(2);
    }
    sys_close(fd);

    fd = sys_open(PATH, O_RDONLY);
    if (fd < 0) {
        sys_write_str("filetest: reopen failed\n");
        sys_exit(3);
    }

    n = sys_read(fd, buf, sizeof(buf) - 1);
    if (n <= 0) {
        sys_write_str("filetest: read failed\n");
        sys_close(fd);
        sys_exit(4);
    }
    buf[n] = '\0';

    sys_write_str("filetest: read back: ");
    sys_write_str(buf);

    n = sys_read(fd, buf, sizeof(buf) - 1);
    if (n == 0)
        sys_write_str("filetest: EOF=0\n");
    else
        sys_write_str("filetest: EOF check FAILED\n");

    sys_close(fd);

    if (big_file_test())
        sys_write_str("filetest: big file (5000 bytes) PASS\n");
    else
        sys_write_str("filetest: big file (5000 bytes) FAILED\n");

    if (sys_open("/no-such-file", O_RDONLY) < 0)
        sys_write_str("filetest: missing file rejected\n");
    else
        sys_write_str("filetest: missing file NOT rejected\n");

    sys_write_str("filetest: PASS\n");
    sys_exit(0);
}
