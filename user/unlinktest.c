/*
 * Lantest OS - unlink/delete-on-close test (Ring 3)
 *
 * Creates a file, keeps two fds open, unlinks the path, verifies:
 *   - the path is gone for new opens
 *   - data is still readable through the already-open fd
 *   - after closing all fds, the file is truly gone
 */
#include "userlib.h"

#define PATH "/u.txt"
#define MSG "hello"

void _start(void)
{
    char buf[8];
    i64 fd1;
    i64 fd2;
    i64 n;

    fd1 = sys_open(PATH, O_RDWR | O_CREAT | O_TRUNC);
    if (fd1 < 0)
        goto fail;

    if (sys_write(fd1, MSG, 5) != 5)
        goto fail;

    fd2 = sys_open(PATH, O_RDONLY);
    if (fd2 < 0)
        goto fail;

    if (sys_unlink(PATH) != 0)
        goto fail;

    /* New opens must fail immediately after unlink. */
    if (sys_open(PATH, O_RDONLY) >= 0)
        goto fail;

    /* The inode is kept alive by the open fds: data still readable. */
    n = sys_read(fd2, buf, 5);
    if (n != 5)
        goto fail;
    for (u64 i = 0; i < 5; ++i) {
        if (buf[i] != MSG[i])
            goto fail;
    }

    sys_close(fd2);
    sys_close(fd1);

    /* Last close released the inode: path still absent. */
    if (sys_open(PATH, O_RDONLY) >= 0)
        goto fail;

    sys_write_str("unlinktest: delete-on-close PASS\n");
    sys_exit(0);

fail:
    sys_write_str("unlinktest: FAIL\n");
    sys_exit(1);
}
