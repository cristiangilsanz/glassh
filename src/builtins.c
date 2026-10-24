#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "glassh.h"

#define COPY_BUFFER 65536

extern char **environ;

static bool parse_integer(const char *text, long long *value)
{
    if (!text || !*text) {
        return false;
    }
    char *end;
    errno = 0;
    *value = strtoll(text, &end, 10);
    return errno == 0 && *end == '\0';
}

static bool write_all(int fd, const char *data, size_t length)
{
    while (length) {
        ssize_t written = write(fd, data, length);
        if (written < 0) {
            if (errno == EINTR) {
                continue;
            }
            return false;
        }
        data += written;
        length -= (size_t)written;
    }
    return true;
}

static int copy(Shell *shell, int argc, char **argv)
{
    if (argc != 3) {
        fprintf(stderr, "[ERROR] usage: copy <source> <destination>\n");
        return 1;
    }
    int source = open(argv[1], O_RDONLY);
    if (source < 0) {
        fprintf(stderr, "[ERROR] cannot open %s: %s\n", argv[1], strerror(errno));
        return 1;
    }
    narrate(shell, "open(\"%s\", O_RDONLY) = %d", argv[1], source);
    struct stat from, to;
    fstat(source, &from);
    if (S_ISDIR(from.st_mode)) {
        fprintf(stderr, "[ERROR] %s is a directory\n", argv[1]);
        close(source);
        return 1;
    }
    if (stat(argv[2], &to) == 0 && from.st_dev == to.st_dev && from.st_ino == to.st_ino) {
        fprintf(stderr, "[ERROR] %s and %s are the same file\n", argv[1], argv[2]);
        close(source);
        return 1;
    }
    int destination = open(argv[2], O_WRONLY | O_CREAT | O_TRUNC, from.st_mode & 0777);
    if (destination < 0) {
        fprintf(stderr, "[ERROR] cannot open %s: %s\n", argv[2], strerror(errno));
        close(source);
        return 1;
    }
    narrate(shell, "open(\"%s\", O_WRONLY|O_CREAT|O_TRUNC) = %d", argv[2], destination);
    char *buffer = checked_malloc(COPY_BUFFER);
    size_t total = 0;
    int status = 0;
    for (;;) {
        ssize_t got = read(source, buffer, COPY_BUFFER);
        if (got == 0) {
            break;
        }
        if (got < 0) {
            if (errno == EINTR) {
                continue;
            }
            fprintf(stderr, "[ERROR] cannot read %s: %s\n", argv[1], strerror(errno));
            status = 1;
            break;
        }
        if (!write_all(destination, buffer, (size_t)got)) {
            fprintf(stderr, "[ERROR] cannot write %s: %s\n", argv[2], strerror(errno));
            status = 1;
            break;
        }
        total += (size_t)got;
    }
    free(buffer);
    close(source);
    if (close(destination) != 0 && status == 0) {
        fprintf(stderr, "[ERROR] cannot write %s: %s\n", argv[2], strerror(errno));
        status = 1;
    }
    if (status == 0) {
        char size[32];
        human_bytes((double)total, size, sizeof size);
        narrate(shell, "copied %zu bytes with read() and write()", total);
        printf("[OK] Copied %s to %s (%s)\n", argv[1], argv[2], size);
    }
    return status;
}

static int change_directory(Shell *shell, int argc, char **argv)
{
    (void)shell;
    if (argc > 2) {
        fprintf(stderr, "glassh: cd: too many arguments\n");
        return 1;
    }
    const char *target = argc == 2 ? argv[1] : getenv("HOME");
    bool back = argc == 2 && strcmp(argv[1], "-") == 0;
    if (back) {
        target = getenv("OLDPWD");
    }
    if (!target) {
        fprintf(stderr, "glassh: cd: %s not set\n", back ? "OLDPWD" : "HOME");
        return 1;
    }
    char previous[PATH_MAX];
    bool known = getcwd(previous, sizeof previous) != NULL;
    if (chdir(target) != 0) {
        fprintf(stderr, "glassh: cd: %s: %s\n", target, strerror(errno));
        return 1;
    }
    char current[PATH_MAX];
    if (known) {
        setenv("OLDPWD", previous, 1);
    }
    if (getcwd(current, sizeof current)) {
        setenv("PWD", current, 1);
        if (back) {
            printf("%s\n", current);
        }
    }
    return 0;
}

static int leave(Shell *shell, int argc, char **argv)
{
    long long code = shell->status;
    if (argc > 2 || (argc == 2 && !parse_integer(argv[1], &code))) {
        fprintf(stderr, "glassh: exit: usage: exit [code]\n");
        return 2;
    }
    shell->running = false;
    shell->exit_code = (int)(code & 0xFF);
    return shell->exit_code;
}

static bool valid_name(const char *name, size_t length)
{
    if (length == 0 || !(name[0] == '_' || (name[0] >= 'A' && name[0] <= 'Z') ||
                         (name[0] >= 'a' && name[0] <= 'z'))) {
        return false;
    }
    for (size_t i = 1; i < length; i++) {
        char c = name[i];
        if (!(c == '_' || (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
              (c >= '0' && c <= '9'))) {
            return false;
        }
    }
    return true;
}

static int compare_text(const void *left, const void *right)
{
    return strcmp(*(char *const *)left, *(char *const *)right);
}

static int export_variables(Shell *shell, int argc, char **argv)
{
    (void)shell;
    if (argc == 1) {
        size_t count = 0;
        while (environ[count]) {
            count++;
        }
        char **sorted = checked_calloc(count, sizeof(char *));
        memcpy(sorted, environ, count * sizeof(char *));
        qsort(sorted, count, sizeof(char *), compare_text);
        for (size_t i = 0; i < count; i++) {
            printf("export %s\n", sorted[i]);
        }
        free(sorted);
        return 0;
    }
    int status = 0;
    for (int i = 1; i < argc; i++) {
        char *equals = strchr(argv[i], '=');
        size_t length = equals ? (size_t)(equals - argv[i]) : strlen(argv[i]);
        if (!valid_name(argv[i], length)) {
            fprintf(stderr, "glassh: export: %s: not a valid name\n", argv[i]);
            status = 1;
            continue;
        }
        if (equals) {
            *equals = '\0';
            setenv(argv[i], equals + 1, 1);
            *equals = '=';
        }
    }
    return status;
}

static int unset_variables(Shell *shell, int argc, char **argv)
{
    (void)shell;
    for (int i = 1; i < argc; i++) {
        unsetenv(argv[i]);
    }
    return 0;
}

static int jobs(Shell *shell, int argc, char **argv)
{
    (void)argc;
    (void)argv;
    list_jobs(shell);
    return 0;
}

static Job *job_argument(Shell *shell, int argc, char **argv, const char *name)
{
    reap_jobs(shell, false);
    Job *job = find_job(shell, argc > 1 ? argv[1] : NULL);
    if (!job) {
        fprintf(stderr, "glassh: %s: %s\n", name, argc > 1 ? "no such job" : "no current job");
    }
    return job;
}

static int foreground(Shell *shell, int argc, char **argv)
{
    Job *job = job_argument(shell, argc, argv, "fg");
    return job ? foreground_job(shell, job) : 1;
}

static int background(Shell *shell, int argc, char **argv)
{
    Job *job = job_argument(shell, argc, argv, "bg");
    if (!job) {
        return 1;
    }
    resume_job(shell, job);
    return 0;
}

static int wait_jobs(Shell *shell, int argc, char **argv)
{
    (void)argc;
    (void)argv;
    return wait_all_jobs(shell);
}

static int trace(Shell *shell, int argc, char **argv)
{
    if (argc == 1) {
        printf("trace is %s\n", shell->trace ? "on" : "off");
        return 0;
    }
    if (argc == 2 && (strcmp(argv[1], "on") == 0 || strcmp(argv[1], "off") == 0)) {
        shell->trace = strcmp(argv[1], "on") == 0;
        return 0;
    }
    fprintf(stderr, "glassh: trace: usage: trace [on|off]\n");
    return 2;
}

static int explain_line(Shell *shell, int argc, char **argv)
{
    (void)shell;
    size_t size = 1;
    for (int i = 1; i < argc; i++) {
        size += strlen(argv[i]) + 1;
    }
    char *line = checked_calloc(size, 1);
    for (int i = 1; i < argc; i++) {
        strcat(line, argv[i]);
        if (i + 1 < argc) {
            strcat(line, " ");
        }
    }
    char *error = NULL;
    Script *script = parse(line, &error);
    free(line);
    if (!script) {
        fprintf(stderr, "glassh: %s\n", error);
        free(error);
        return 2;
    }
    explain(stdout, script);
    free_script(script);
    return 0;
}

static int help(Shell *shell, int argc, char **argv);

static const struct {
    const char *name;
    Builtin run;
    const char *usage;
} BUILTINS[] = {
    {"cd", change_directory, "cd [dir|-]          change the working directory"},
    {"exit", leave, "exit [code]         leave the shell"},
    {"export", export_variables, "export NAME=value   set an environment variable"},
    {"unset", unset_variables, "unset NAME...       remove environment variables"},
    {"jobs", jobs, "jobs                list background and stopped jobs"},
    {"fg", foreground, "fg [%n]             bring a job to the foreground"},
    {"bg", background, "bg [%n]             resume a stopped job in the background"},
    {"wait", wait_jobs, "wait                wait for every background job"},
    {"trace", trace, "trace [on|off]      narrate system calls and report pipelines"},
    {"explain", explain_line, "explain LINE        show how a line is parsed, without running it"},
    {"copy", copy, "copy SRC DST        copy a file with read() and write()"},
    {"help", help, "help                list the built-in commands"},
};

static int help(Shell *shell, int argc, char **argv)
{
    (void)shell;
    (void)argc;
    (void)argv;
    printf("glassh %s built-ins:\n", GLASSH_VERSION);
    for (size_t i = 0; i < sizeof BUILTINS / sizeof *BUILTINS; i++) {
        printf("  %s\n", BUILTINS[i].usage);
    }
    return 0;
}

Builtin find_builtin(const char *name)
{
    for (size_t i = 0; i < sizeof BUILTINS / sizeof *BUILTINS; i++) {
        if (strcmp(BUILTINS[i].name, name) == 0) {
            return BUILTINS[i].run;
        }
    }
    return NULL;
}
