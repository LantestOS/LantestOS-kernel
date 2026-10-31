/*
 * Lantest OS - /bin/ls : list a directory
 */
#include "userlib.h"
#include "libc.h"
#include "crt.h"

int main(int argc, char **argv, char **envp)
{
    const char *path = argc > 1 ? argv[1] : "/";
    struct dirent de;
    i64 fd;
    i64 n;

    (void)envp;

    fd = sys_open(path, O_RDONLY);
    if (fd < 0) {
        printf("ls: cannot open %s\n", path);
        return 1;
    }

    while ((n = sys_read(fd, &de, sizeof(de))) == (i64)sizeof(de))
        printf("%s%s\n", de.d_name, de.d_type == DT_DIR ? "/" : "");

    sys_close(fd);
    return 0;
}
