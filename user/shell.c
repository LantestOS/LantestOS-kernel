/*
 * Lantest OS - init / shell (M4-4)
 *
 * Built-in commands plus external program execution:
 *   run <path>  -> fork() + execve() + wait()
 *   rm <path>   -> unlink() (delete-on-close)
 */
#include "userlib.h"
#include "libc.h"

#define LINE_MAX 128
#define MAX_STAGES 8
#define MAX_JOBS 8
#define SIGCONT 18
#define SIGTSTP 20

static void run_command(char *line);
static void copy_str(char *d, const char *s);

static int bg_mode;
static void job_new(int pgid, const int *pids, int npids, const char *cmd);

static u64 slen(const char *s)
{
    u64 n = 0;
    while (s[n] != '\0')
        ++n;
    return n;
}

static int scmp(const char *a, const char *b)
{
    while (*a != '\0' && *a == *b) {
        ++a;
        ++b;
    }
    return (int)((unsigned char)*a - (unsigned char)*b);
}

static void print(const char *s)
{
    printf("%s", s);
}

static char *skip_spaces(char *s)
{
    while (*s == ' ' || *s == '\t')
        ++s;
    return s;
}

static void trim(char *s)
{
    u64 n = slen(s);
    while (n > 0 && (s[n - 1] == ' ' || s[n - 1] == '\t' ||
                     s[n - 1] == '\n' || s[n - 1] == '\r'))
        s[--n] = '\0';
}

static void __attribute__((unused)) cmd_ls(const char *path)
{
    struct dirent de;
    i64 fd;
    i64 n;

    fd = sys_open(path, O_RDONLY);
    if (fd < 0) {
        print("ls: cannot open ");
        print(path);
        print("\n");
        return;
    }

    while ((n = sys_read(fd, &de, sizeof(de))) > 0) {
        if (n != (i64)sizeof(de))
            break;
        print(de.d_name);
        if (de.d_type == DT_DIR)
            print("/");
        print("\n");
    }

    sys_close(fd);
}

static void __attribute__((unused)) cmd_cat(const char *path)
{
    char buf[128];
    i64 fd;
    i64 n;

    if (*path != '\0') {
        fd = sys_open(path, O_RDONLY);
        if (fd < 0) {
            print("cat: cannot open ");
            print(path);
            print("\n");
            return;
        }
    } else {
        fd = STDIN_FD;
    }

    while ((n = sys_read(fd, buf, sizeof(buf))) > 0)
        sys_write(STDOUT_FD, buf, (u64)n);

    if (*path != '\0')
        sys_close(fd);
}

static void __attribute__((unused)) cmd_echo(char *arg)
{
    char *redir = 0;
    char *path;

    for (char *p = arg; *p != '\0'; ++p) {
        if (*p == '>') {
            redir = p;
            break;
        }
    }

    if (redir == 0) {
        print(arg);
        print("\n");
        return;
    }

    *redir = '\0';
    trim(arg);
    path = skip_spaces(redir + 1);
    trim(path);
    if (*path == '\0') {
        print("echo: missing redirect file\n");
        return;
    }

    i64 fd = sys_open(path, O_WRONLY | O_CREAT | O_TRUNC);
    if (fd < 0) {
        print("echo: cannot create ");
        print(path);
        print("\n");
        return;
    }

    sys_write(fd, arg, slen(arg));
    sys_write(fd, "\n", 1);
    sys_close(fd);

    print("echo: wrote ");
    print(path);
    print("\n");
}

static void cmd_touch(const char *path)
{
    i64 fd = sys_open(path, O_RDWR | O_CREAT);

    if (fd < 0) {
        print("touch: cannot create ");
        print(path);
        print("\n");
        return;
    }
    sys_close(fd);
}

static void cmd_rm(const char *path)
{
    if (sys_unlink(path) == 0) {
        print("rm: removed ");
        print(path);
        print("\n");
    } else {
        print("rm: cannot remove ");
        print(path);
        print("\n");
    }
}

static void cmd_run(char *argline)
{
    char *argv[8];
    int argc = 0;
    char *p = argline;

    while (*p != '\0' && argc < 8) {
        while (*p == ' ' || *p == '\t')
            ++p;
        if (*p == '\0')
            break;
        argv[argc++] = p;
        while (*p != '\0' && *p != ' ' && *p != '\t')
            ++p;
        if (*p != '\0')
            *p++ = '\0';
    }

    if (argc == 0) {
        print("run: missing file\n");
        return;
    }
    if (argc < 8)
        argv[argc] = 0;

    i64 pid = sys_fork();

    if (pid == 0) {
        sys_execve(argv[0], argv, 0);
        print("run: exec failed\n");
        sys_exit(127);
    } else if (pid > 0) {
        i64 status = 0;
        sys_wait(&status);
        print("run: child exited\n");
    } else {
        print("run: fork failed\n");
    }
}

static const char *command_path(const char *cmd)
{
    if (scmp(cmd, "ls") == 0)
        return "/bin/ls";
    if (scmp(cmd, "cat") == 0)
        return "/bin/cat";
    if (scmp(cmd, "echo") == 0)
        return "/bin/echo";
    if (scmp(cmd, "head") == 0)
        return "/bin/head";
    return 0;
}

static int parse_argv(char *s, char *argv[], int max)
{
    int argc = 0;

    while (*s != '\0' && argc < max) {
        while (*s == ' ' || *s == '\t')
            ++s;
        if (*s == '\0')
            break;
        argv[argc++] = s;
        while (*s != '\0' && *s != ' ' && *s != '\t')
            ++s;
        if (*s != '\0')
            *s++ = '\0';
    }

    return argc;
}

static void run_external_cmd(const char *cmd, char *arg)
{
    const char *path = command_path(cmd);
    char *redir = 0;
    char *redir_path = 0;
    char *argv[16];
    int argc = 0;
    i64 out = -1;
    i64 pid;
    char label[64];
    int label_len = 0;

    for (int i = 0; cmd[i] != '\0' && label_len < 60; ++i)
        label[label_len++] = cmd[i];
    label[label_len++] = ' ';
    for (int i = 0; arg[i] != '\0' && label_len < 63; ++i)
        label[label_len++] = arg[i];
    label[label_len] = '\0';

    for (char *q = arg; *q != '\0'; ++q) {
        if (*q == '>') {
            redir = q;
            break;
        }
    }
    if (redir != 0) {
        *redir = '\0';
        redir_path = skip_spaces(redir + 1);
        trim(redir_path);
        trim(arg);
        if (*redir_path == '\0') {
            print("shell: missing redirect file\n");
            return;
        }
    }

    argv[argc++] = (char *)cmd;
    argc += parse_argv(arg, &argv[argc], 15);
    if (argc < 16)
        argv[argc] = 0;

    if (path == 0) {
        print("shell: unknown external command\n");
        return;
    }

    if (redir_path != 0) {
        out = sys_open(redir_path, O_WRONLY | O_CREAT | O_TRUNC);
        if (out < 0) {
            print("shell: cannot create ");
            print(redir_path);
            print("\n");
            return;
        }
    }

    pid = sys_fork();
    if (pid == 0) {
        if (!bg_mode)
            sys_setpgid(0, 0);
        if (out >= 0) {
            sys_dup2((int)out, STDOUT_FD);
            sys_close((int)out);
        }
        sys_execve(path, argv, 0);
        print("shell: exec failed\n");
        sys_exit(127);
    }

    if (out >= 0)
        sys_close((int)out);

    if (pid > 0) {
        if (!bg_mode)
            sys_set_fg((int)pid);

        {
            i64 st = 0;
            i64 r = sys_waitpid((int)pid, &st);

            if (!bg_mode && r == pid && (st & 0xff) == 0x7f) {
                int one = (int)pid;
                job_new((int)pid, &one, 1, label);
            }
        }

        if (!bg_mode)
            sys_set_fg(0);
    } else {
        print("shell: fork failed\n");
    }
}

static void exec_stage(char *line)
{
    char copy[LINE_MAX];
    char *p = skip_spaces(line);
    char *cmd;
    char *arg;

    copy_str(copy, line);
    trim(p);
    if (*p == '\0')
        sys_exit(0);

    cmd = p;
    while (*p != '\0' && *p != ' ' && *p != '\t')
        ++p;
    if (*p != '\0')
        *p++ = '\0';
    arg = skip_spaces(p);
    trim(arg);

    {
        const char *path = command_path(cmd);

        if (path != 0) {
            char *argv[16];
            int argc = 0;

            argv[argc++] = cmd;
            argc += parse_argv(arg, &argv[argc], 15);
            if (argc < 16)
                argv[argc] = 0;
            sys_execve(path, argv, 0);
            print("shell: exec failed\n");
            sys_exit(127);
        }
    }

    run_command(copy);
    sys_exit(0);
}

struct job {
    int used;
    int pgid;
    int pids[MAX_STAGES];
    int npids;
    int stopped;
    char cmd[64];
};

static struct job jobs[MAX_JOBS];

static void print_int(int v)
{
    printf("%d", v);
}

static struct job *job_select(const char *arg)
{
    int n = 0;
    const char *p = arg;

    if (*p == '\0') {
        for (int i = MAX_JOBS - 1; i >= 0; --i)
            if (jobs[i].used)
                return &jobs[i];
        return 0;
    }

    while (*p >= '0' && *p <= '9') {
        n = n * 10 + (*p - '0');
        ++p;
    }
    if (n < 1 || n > MAX_JOBS || !jobs[n - 1].used)
        return 0;
    return &jobs[n - 1];
}

static void job_new(int pgid, const int *pids, int npids, const char *cmd)
{
    struct job *j = 0;

    for (int i = 0; i < MAX_JOBS; ++i) {
        if (jobs[i].used && jobs[i].pgid == pgid) {
            j = &jobs[i];
            break;
        }
    }
    if (j == 0) {
        for (int i = 0; i < MAX_JOBS; ++i) {
            if (!jobs[i].used) {
                j = &jobs[i];
                break;
            }
        }
    }
    if (j == 0) {
        print("jobs: table full\n");
        return;
    }

    j->used = 1;
    j->pgid = pgid;
    j->npids = npids;
    j->stopped = 1;
    for (int i = 0; i < npids && i < MAX_STAGES; ++i)
        j->pids[i] = pids[i];

    {
        int i = 0;
        while (cmd[i] != '\0' && i < 63) {
            j->cmd[i] = cmd[i];
            ++i;
        }
        j->cmd[i] = '\0';
    }
}

static void cmd_jobs(void)
{
    for (int i = 0; i < MAX_JOBS; ++i) {
        if (!jobs[i].used)
            continue;
        print("[");
        print_int(i + 1);
        print("] ");
        print(jobs[i].stopped ? "Stopped" : "Running");
        print(" pgid=");
        print_int(jobs[i].pgid);
        print("  ");
        print(jobs[i].cmd);
        print("\n");
    }
}

static void cmd_fg(char *arg)
{
    struct job *j = job_select(arg);
    int still_stopped = 0;

    if (j == 0) {
        print("fg: no such job\n");
        return;
    }

    sys_set_fg(j->pgid);
    sys_kill_pgrp(j->pgid, SIGCONT);

    for (int i = 0; i < j->npids; ++i) {
        i64 st = 0;
        i64 r = sys_waitpid(j->pids[i], &st);

        if (r == j->pids[i] && (st & 0xff) == 0x7f)
            still_stopped = 1;
    }

    sys_set_fg(0);

    if (still_stopped)
        j->stopped = 1;
    else
        j->used = 0;
}

static void cmd_bg(char *arg)
{
    struct job *j = job_select(arg);

    if (j == 0) {
        print("bg: no such job\n");
        return;
    }

    sys_kill_pgrp(j->pgid, SIGCONT);
    j->stopped = 0;
    print("bg: continued pgid=");
    print_int(j->pgid);
    print("\n");
}

static void cmd_pipeline(char *line)
{
    char *stages[MAX_STAGES];
    int pipes[MAX_STAGES][2];
    i64 pids[MAX_STAGES];
    int nstages = 0;
    int npipes = 0;
    int forked = 0;
    i64 first_pid = -1;
    char *p = line;
    char label[64];
    int label_len = 0;

    while (line[label_len] != '\0' && label_len < 63) {
        label[label_len] = line[label_len];
        ++label_len;
    }
    label[label_len] = '\0';

    for (;;) {
        char *bar = 0;
        char *q;

        for (q = p; *q != '\0'; ++q) {
            if (*q == '|') {
                bar = q;
                break;
            }
        }

        if (bar != 0)
            *bar = '\0';

        p = skip_spaces(p);
        trim(p);
        if (*p == '\0') {
            print("pipe: empty command\n");
            return;
        }
        if (nstages >= MAX_STAGES) {
            print("pipe: too many stages\n");
            return;
        }
        stages[nstages++] = p;

        if (bar == 0)
            break;
        p = bar + 1;
    }

    npipes = nstages - 1;

    for (int i = 0; i < npipes; ++i) {
        if (sys_pipe(pipes[i]) < 0) {
            print("pipe: allocation failed\n");
            return;
        }
    }

    for (int i = 0; i < nstages; ++i) {
        pids[i] = sys_fork();

        if (pids[i] == 0) {
            if (i == 0)
                sys_setpgid(0, 0);
            else
                sys_setpgid(0, (int)first_pid);

            if (i > 0)
                sys_dup2(pipes[i - 1][0], STDIN_FD);
            if (i < npipes)
                sys_dup2(pipes[i][1], STDOUT_FD);

            for (int j = 0; j < npipes; ++j) {
                sys_close(pipes[j][0]);
                sys_close(pipes[j][1]);
            }

            exec_stage(stages[i]);
            sys_exit(0);
        }

        if (pids[i] < 0) {
            print("pipe: fork failed\n");
            break;
        }
        if (i == 0)
            first_pid = pids[0];
        ++forked;
    }

    for (int i = 0; i < npipes; ++i) {
        sys_close(pipes[i][0]);
        sys_close(pipes[i][1]);
    }

    if (!bg_mode && first_pid > 0)
        sys_set_fg((int)first_pid);

    {
        int stopped = 0;

        for (int i = 0; i < forked; ++i) {
            i64 st = 0;
            i64 r = sys_waitpid((int)pids[i], &st);

            if (r == pids[i] && (st & 0xff) == 0x7f)
                stopped = 1;
        }

        if (!bg_mode && stopped && first_pid > 0) {
            int tmp[MAX_STAGES];
            for (int i = 0; i < forked; ++i)
                tmp[i] = (int)pids[i];
            job_new((int)first_pid, tmp, forked, label);
        }
    }

    if (!bg_mode)
        sys_set_fg(0);
}

static void help(void)
{
    print("commands:\n");
    print("  help                 this help\n");
    print("  ls [path]            list directory (default /)\n");
    print("  cat <file>           print file contents\n");
    print("  head                 print the first line of stdin\n");
    print("  mkdir <path>         create directory\n");
    print("  touch <file>         create empty file\n");
    print("  rm <file>            remove file\n");
    print("  echo <text> [> file] print text or write it to a file\n");
    print("  run <file> [args...]  fork + execve an ELF file (argv)\n");
    print("  cmd1 | cmd2 | ...    pipeline (up to 8 stages)\n");
    print("  cmd &                run in background\n");
    print("  jobs                 list stopped/running jobs\n");
    print("  fg [n] / bg [n]      resume job in foreground/background\n");
    print("  exit                 leave the shell\n");
}

static void run_command(char *line)
{
    char *p = skip_spaces(line);
    char *cmd;
    char *arg;

    trim(p);
    if (*p == '\0')
        return;

    {
        char *bar = 0;
        for (char *q = p; *q != '\0'; ++q) {
            if (*q == '|') {
                bar = q;
                break;
            }
        }
        if (bar != 0) {
            cmd_pipeline(p);
            return;
        }
    }

    cmd = p;
    while (*p != '\0' && *p != ' ' && *p != '\t')
        ++p;
    if (*p != '\0')
        *p++ = '\0';
    arg = skip_spaces(p);
    trim(arg);

    if (command_path(cmd) != 0) {
        run_external_cmd(cmd, arg);
        return;
    }

    if (scmp(cmd, "help") == 0) {
        help();
    } else if (scmp(cmd, "mkdir") == 0) {
        if (*arg == '\0') {
            print("mkdir: missing path\n");
        } else if (sys_mkdir(arg, 0755) == 0) {
            print("mkdir: ok\n");
        } else {
            print("mkdir: failed (exists or bad path)\n");
        }
    } else if (scmp(cmd, "touch") == 0) {
        if (*arg == '\0')
            print("touch: missing file\n");
        else
            cmd_touch(arg);
    } else if (scmp(cmd, "rm") == 0) {
        if (*arg == '\0')
            print("rm: missing file\n");
        else
            cmd_rm(arg);
    } else if (scmp(cmd, "jobs") == 0) {
        cmd_jobs();
    } else if (scmp(cmd, "fg") == 0) {
        cmd_fg(arg);
    } else if (scmp(cmd, "bg") == 0) {
        cmd_bg(arg);
    } else if (scmp(cmd, "run") == 0) {
        if (*arg == '\0')
            print("run: missing file\n");
        else
            cmd_run(arg);
    } else if (scmp(cmd, "exit") == 0) {
        sys_exit(0);
    } else {
        print("shell: unknown command: ");
        print(cmd);
        print("\n");
    }
}

#define HIST_MAX 8
#define CAND_MAX 16
#define CAND_LEN LINE_MAX
#define PROMPT "lantest> "

static char line_buf[LINE_MAX];
static char history[HIST_MAX][LINE_MAX];
static int history_count;
static char candidates[CAND_MAX][CAND_LEN];

static int prefix_of(const char *s, const char *p)
{
    while (*p != '\0') {
        if (*s != *p)
            return 0;
        ++s;
        ++p;
    }
    return 1;
}

static void copy_str(char *d, const char *s)
{
    while (*s != '\0')
        *d++ = *s++;
    *d = '\0';
}

static void erase_n(int n)
{
    while (n-- > 0)
        sys_write(STDOUT_FD, "\b \b", 3);
}

static void redraw_line(int old_len, int new_len)
{
    erase_n(old_len);
    sys_write(STDOUT_FD, line_buf, (u64)new_len);
}

static void history_add(const char *s)
{
    if (s[0] == '\0')
        return;
    if (history_count > 0 &&
        scmp(history[history_count - 1], s) == 0)
        return;

    if (history_count < HIST_MAX) {
        copy_str(history[history_count], s);
        history_count++;
    } else {
        for (int i = 1; i < HIST_MAX; ++i)
            copy_str(history[i - 1], history[i]);
        copy_str(history[HIST_MAX - 1], s);
    }
}

static const char *const command_names[] = {
    "help", "ls", "cat", "head", "mkdir", "touch", "rm", "echo", "jobs", "fg", "bg", "run", "exit",
};

static int complete_line(int len)
{
    int tstart = len;

    while (tstart > 0 && line_buf[tstart - 1] != ' ')
        --tstart;

    char token[LINE_MAX];
    int toklen = len - tstart;
    for (int i = 0; i < toklen; ++i)
        token[i] = line_buf[tstart + i];
    token[toklen] = '\0';

    int slash = -1;
    for (int i = 0; i < toklen; ++i) {
        if (token[i] == '/')
            slash = i;
    }

    int matches = 0;

    if (tstart == 0 && slash < 0) {
        for (u64 i = 0; i < sizeof(command_names) / sizeof(command_names[0]);
             ++i) {
            if (prefix_of(command_names[i], token)) {
                copy_str(candidates[matches], command_names[i]);
                ++matches;
            }
        }
        if (matches == 1) {
            int clen = (int)slen(candidates[0]);
            if (tstart + clen + 1 >= LINE_MAX)
                return len;
            for (int i = 0; i < clen; ++i)
                line_buf[tstart + i] = candidates[0][i];
            line_buf[tstart + clen] = ' ';
            line_buf[tstart + clen + 1] = '\0';
            int newlen = tstart + clen + 1;
            redraw_line(len, newlen);
            return newlen;
        }
    } else {
        char dirpath[LINE_MAX];
        const char *prefix;
        int path_prefix_len;

        if (slash >= 0) {
            if (slash == 0) {
                dirpath[0] = '/';
                dirpath[1] = '\0';
            } else {
                for (int i = 0; i < slash; ++i)
                    dirpath[i] = token[i];
                dirpath[slash] = '\0';
            }
            prefix = token + slash + 1;
            path_prefix_len = slash + 1;
        } else {
            dirpath[0] = '/';
            dirpath[1] = '\0';
            prefix = token;
            path_prefix_len = 0;
        }

        i64 fd = sys_open(dirpath, O_RDONLY);
        if (fd >= 0) {
            struct dirent de;
            while (sys_read(fd, &de, sizeof(de)) == (i64)sizeof(de)) {
                if (!prefix_of(de.d_name, prefix))
                    continue;
                if (matches >= CAND_MAX)
                    break;

                int pos = 0;
                if (path_prefix_len + (int)slen(de.d_name) + 2 >= CAND_LEN)
                    continue;
                for (int i = 0; i < path_prefix_len; ++i)
                    candidates[matches][pos++] = token[i];
                int nlen = (int)slen(de.d_name);
                for (int i = 0; i < nlen; ++i)
                    candidates[matches][pos++] = de.d_name[i];
                candidates[matches][pos++] = (de.d_type == DT_DIR) ? '/' : ' ';
                candidates[matches][pos] = '\0';
                ++matches;
            }
            sys_close(fd);
        }

        if (matches == 1) {
            int clen = (int)slen(candidates[0]);
            if (tstart + clen >= LINE_MAX)
                return len;
            for (int i = 0; i < clen; ++i)
                line_buf[tstart + i] = candidates[0][i];
            line_buf[tstart + clen] = '\0';
            int newlen = tstart + clen;
            redraw_line(len, newlen);
            return newlen;
        }
    }

    if (matches == 0)
        return len;

    print("\n");
    for (int i = 0; i < matches; ++i) {
        print(candidates[i]);
        print(" ");
    }
    print("\n");
    print(PROMPT);
    sys_write(STDOUT_FD, line_buf, (u64)len);
    return len;
}

static int read_line(void)
{
    int len = 0;
    int hp = history_count;

    line_buf[0] = '\0';

    for (;;) {
        char c;
        i64 n = sys_read(STDIN_FD, &c, 1);

        if (n != 1)
            return -1;

        if (c == '\n') {
            print("\n");
            line_buf[len] = '\0';
            return len;
        }
        if (c == '\b' || c == (char)0x7F) {
            if (len > 0) {
                --len;
                sys_write(STDOUT_FD, "\b \b", 3);
            }
            continue;
        }
        if (c == '\t') {
            len = complete_line(len);
            continue;
        }
        if (c == 0x1B) {
            char b1;
            char b2;

            if (sys_read(STDIN_FD, &b1, 1) != 1 || b1 != '[')
                continue;
            if (sys_read(STDIN_FD, &b2, 1) != 1)
                continue;

            if (b2 == 'A') {          /* up */
                if (hp > 0)
                    --hp;
                copy_str(line_buf, history[hp]);
                int nl = (int)slen(line_buf);
                redraw_line(len, nl);
                len = nl;
            } else if (b2 == 'B') {   /* down */
                if (hp < history_count - 1) {
                    ++hp;
                    copy_str(line_buf, history[hp]);
                } else {
                    hp = history_count;
                    line_buf[0] = '\0';
                }
                int nl = (int)slen(line_buf);
                redraw_line(len, nl);
                len = nl;
            }
            continue;
        }
        if (c == 0x03) {              /* Ctrl-C */
            print("^C\n");
            line_buf[0] = '\0';
            return 0;
        }
        if ((unsigned char)c >= 0x20 && len < LINE_MAX - 1) {
            line_buf[len++] = c;
            sys_write(STDOUT_FD, &c, 1);
        }
    }
}

static void run_background(char *line)
{
    i64 pid = sys_fork();

    if (pid == 0) {
        bg_mode = 1;
        sys_setpgid(0, 0);
        run_command(line);
        sys_exit(0);
    }

    if (pid > 0) {
        sys_detach((int)pid);
        print("[bg] started\n");
    } else {
        print("bg: fork failed\n");
    }
}

void _start(void)
{
    print("Lantest shell (M4-13) - type 'help'\n");
    sys_set_fg(0);
    {
        char boot_prog[] = "/bin/hello";
        cmd_run(boot_prog);   /* boot-time fork/exec smoke test */
    }
    {
        char boot_pipe[] = "echo pipe-ok | cat";
        run_command(boot_pipe);   /* boot-time pipe smoke test */
    }

    for (;;) {
        print(PROMPT);
        int n = read_line();

        if (n < 0) {
            print("shell: no input, exiting\n");
            sys_exit(1);
        }
        if (n == 0)
            continue;

        trim(line_buf);

        {
            char *amp = 0;

            for (char *q = line_buf; *q != '\0'; ++q) {
                if (*q == '&')
                    amp = q;
            }
            if (amp != 0) {
                *amp = '\0';
                trim(line_buf);
                if (*line_buf != '\0')
                    run_background(line_buf);
                continue;
            }
        }

        history_add(line_buf);
        run_command(line_buf);
    }
}
