#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "glassh.h"

static const char *USAGE = "usage: glassh [--trace] [-c command | script]\n";

static void interrupted(int number)
{
    (void)number;
}

static void take_terminal(Shell *shell)
{
    shell->terminal = STDIN_FILENO;
    pid_t group;
    while (tcgetpgrp(shell->terminal) != (group = getpgrp())) {
        kill(-group, SIGTTIN);
    }
    struct sigaction action = {0};
    action.sa_handler = interrupted;
    sigemptyset(&action.sa_mask);
    sigaction(SIGINT, &action, NULL);
    int ignored[] = {SIGQUIT, SIGTSTP, SIGTTIN, SIGTTOU};
    for (size_t i = 0; i < sizeof ignored / sizeof *ignored; i++) {
        signal(ignored[i], SIG_IGN);
    }
    setpgid(0, 0);
    shell->pgid = getpid();
    tcsetpgrp(shell->terminal, shell->pgid);
}

static void prompt(Shell *shell)
{
    char directory[PATH_MAX];
    const char *shown = getcwd(directory, sizeof directory) ? directory : "?";
    const char *home = getenv("HOME");
    size_t home_length = home ? strlen(home) : 0;
    bool inside_home = home_length > 1 && strncmp(shown, home, home_length) == 0 &&
                       (shown[home_length] == '/' || shown[home_length] == '\0');
    fprintf(stderr, "%sglassh%s %s%s%s%s", paint(shell, "[1;36m"), paint(shell, "[0m"),
            paint(shell, "[34m"), inside_home ? "~" : "",
            inside_home ? shown + home_length : shown, paint(shell, "[0m"));
    if (shell->trace) {
        fprintf(stderr, " %s◉ trace%s", paint(shell, "[33m"), paint(shell, "[0m"));
    }
    fprintf(stderr, " %s❯%s ", paint(shell, shell->status ? "[31m" : "[32m"),
            paint(shell, "[0m"));
    fflush(stderr);
}

static int run_stream(Shell *shell, FILE *input)
{
    char *line = NULL;
    size_t capacity = 0;
    while (shell->running) {
        if (shell->interactive) {
            reap_jobs(shell, true);
            prompt(shell);
        }
        errno = 0;
        ssize_t length = getline(&line, &capacity, input);
        if (length < 0) {
            if (errno == EINTR) {
                clearerr(input);
                fputc('\n', stderr);
                continue;
            }
            if (shell->interactive) {
                fputc('\n', stderr);
            }
            break;
        }
        if (length && line[length - 1] == '\n') {
            line[length - 1] = '\0';
        }
        run_line(shell, line);
        if (!shell->interactive) {
            reap_jobs(shell, false);
        }
    }
    free(line);
    return shell->running ? shell->status : shell->exit_code;
}

int main(int argc, char **argv)
{
    Shell shell = {.running = true};
    shell.trace_fd = fcntl(STDERR_FILENO, F_DUPFD_CLOEXEC, 10);
    shell.color = isatty(STDERR_FILENO) && !getenv("NO_COLOR");
    shell.pgid = getpgrp();
    signal(SIGPIPE, SIG_IGN);
    const char *command = NULL;
    const char *script = NULL;
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--trace") == 0) {
            shell.trace = true;
        } else if (strcmp(argv[i], "-c") == 0 && i + 1 < argc) {
            command = argv[++i];
        } else if (strcmp(argv[i], "--version") == 0) {
            printf("glassh %s\n", GLASSH_VERSION);
            return 0;
        } else if (strcmp(argv[i], "--help") == 0) {
            printf("%s", USAGE);
            return 0;
        } else if (argv[i][0] != '-' && !script) {
            script = argv[i];
        } else {
            fprintf(stderr, "%s", USAGE);
            return 2;
        }
    }
    if (command) {
        run_line(&shell, command);
        reap_jobs(&shell, false);
        return shell.running ? shell.status : shell.exit_code;
    }
    FILE *input = stdin;
    if (script) {
        input = fopen(script, "r");
        if (!input) {
            fprintf(stderr, "glassh: %s: %s\n", script, strerror(errno));
            return 127;
        }
    } else if (isatty(STDIN_FILENO) && isatty(STDERR_FILENO)) {
        shell.interactive = true;
        shell.own_groups = true;
        take_terminal(&shell);
        fprintf(stderr, "%sglassh %s · type help for built-ins, trace on to see through it%s\n",
                paint(&shell, "\033[2m"), GLASSH_VERSION, paint(&shell, "\033[0m"));
    }
    int status = run_stream(&shell, input);
    if (input != stdin) {
        fclose(input);
    }
    return status;
}
