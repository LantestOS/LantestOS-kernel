/*
 * Lantest OS - /bin/cat : file or stdin -> stdout
 */
#include "userlib.h"
#include "stdio.h"
#include "crt.h"

int main(int argc, char **argv, char **envp)
{
    FILE *in = stdin;
    unsigned char buf[128];
    u64 n;

    (void)envp;

    if (argc > 1) {
        in = fopen(argv[1], "r");
        if (in == NULL) {
            printf("cat: cannot open %s\n", argv[1]);
            return 1;
        }
    }

    while ((n = fread(buf, 1, sizeof(buf), in)) > 0)
        fwrite(buf, 1, n, stdout);

    if (in != stdin)
        fclose(in);
    return 0;
}
