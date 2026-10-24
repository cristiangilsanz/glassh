#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include "glassh.h"

#define RELAY_BUFFER 65536
#define POLL_INTERVAL 20

typedef struct {
    char **argv;
    size_t argc;
    char **assignments;
    size_t assignment_count;
    Redirect *redirects;
    size_t redirect_count;
    char *text;
} Prepared;

typedef struct {
    int source;
    int sink;
    char *buffer;
    size_t length;
    size_t offset;
    size_t bytes;
} Relay;

typedef struct {
    int *fds;
    size_t count;
} Held;

static void release(Prepared *prepared)
{
    free_words(prepared->argv);
    free_words(prepared->assignments);
    for (size_t i = 0; i < prepared->redirect_count; i++) {
        free(prepared->redirects[i].target);
    }
    free(prepared->redirects);
    free(prepared->text);
}

static int prepare(Shell *shell, const Command *command, Prepared *prepared)
{
    *prepared = (Prepared){0};
    prepared->text = command_text(command);
    prepared->assignments = checked_calloc(1, sizeof(char *));
    size_t first = 0;
    char *error = NULL;
    while (first < command->word_count && is_assignment(command->words[first])) {
        const char *raw = command->words[first];
        const char *equals = strchr(raw, '=');
        char *value = expand_single(shell, equals + 1, &error);
        if (!value) {
            free(error);
            error = NULL;
            value = checked_strdup("");
        }
        size_t size = (size_t)(equals - raw) + strlen(value) + 2;
        char *assignment = checked_malloc(size);
        snprintf(assignment, size, "%.*s=%s", (int)(equals - raw), raw, value);
        free(value);
        prepared->assignments = checked_realloc(prepared->assignments,
                                                (prepared->assignment_count + 2) * sizeof(char *));
        prepared->assignments[prepared->assignment_count++] = assignment;
        prepared->assignments[prepared->assignment_count] = NULL;
        first++;
    }
    prepared->argv = expand_words(shell, command->words + first, command->word_count - first,
                                  &prepared->argc, &error);
    if (!prepared->argv) {
        fprintf(stderr, "glassh: %s\n", error);
        free(error);
        return -1;
    }
    prepared->redirects = checked_calloc(command->redirect_count, sizeof(Redirect));
    for (size_t i = 0; i < command->redirect_count; i++) {
        const Redirect *redirect = &command->redirects[i];
        prepared->redirects[i].kind = redirect->kind;
        prepared->redirect_count++;
        if (redirect->target) {
            prepared->redirects[i].target = expand_single(shell, redirect->target, &error);
            if (!prepared->redirects[i].target) {
                fprintf(stderr, "glassh: %s\n", error);
                free(error);
                return -1;
            }
        }
    }
    return 0;
}

static void apply_assignments(const Prepared *prepared)
{
    for (size_t i = 0; i < prepared->assignment_count; i++) {
        char *assignment = prepared->assignments[i];
        char *equals = strchr(assignment, '=');
        *equals = '\0';
        setenv(assignment, equals + 1, 1);
        *equals = '=';
    }
}

static const char *open_flags(RedirectKind kind)
{
    switch (kind) {
    case REDIRECT_IN:
        return "O_RDONLY";
    case REDIRECT_APPEND:
    case REDIRECT_ERR_APPEND:
        return "O_WRONLY|O_CREAT|O_APPEND";
    default:
        return "O_WRONLY|O_CREAT|O_TRUNC";
    }
}

static int apply_redirects(Shell *shell, const Prepared *prepared, bool child)
{
    void (*say)(Shell *, const char *, ...) = child ? narrate_child : narrate;
    for (size_t i = 0; i < prepared->redirect_count; i++) {
        const Redirect *redirect = &prepared->redirects[i];
        if (redirect->kind == REDIRECT_ERR_TO_OUT) {
            say(shell, "dup2(1, 2)");
            dup2(STDOUT_FILENO, STDERR_FILENO);
            continue;
        }
        int flags = O_WRONLY | O_CREAT | O_TRUNC;
        int target = STDOUT_FILENO;
        if (redirect->kind == REDIRECT_IN) {
            flags = O_RDONLY;
            target = STDIN_FILENO;
        } else if (redirect->kind == REDIRECT_APPEND || redirect->kind == REDIRECT_ERR_APPEND) {
            flags = O_WRONLY | O_CREAT | O_APPEND;
        }
        if (redirect->kind == REDIRECT_ERR || redirect->kind == REDIRECT_ERR_APPEND) {
            target = STDERR_FILENO;
        }
        int fd = open(redirect->target, flags, 0644);
        if (fd < 0) {
            fprintf(stderr, "glassh: %s: %s\n", redirect->target, strerror(errno));
            return -1;
        }
        say(shell, "open(\"%s\", %s) = %d", redirect->target, open_flags(redirect->kind), fd);
        say(shell, "dup2(%d, %d)", fd, target);
        dup2(fd, target);
        close(fd);
    }
    return 0;
}

static int run_in_shell(Shell *shell, Prepared *prepared, Builtin builtin)
{
    int saved[3];
    for (int fd = 0; fd < 3; fd++) {
        saved[fd] = fcntl(fd, F_DUPFD_CLOEXEC, 10);
    }
    int status = 1;
    if (builtin && strcmp(prepared->argv[0], "trace") != 0) {
        narrate(shell, "%s is a built-in, it runs inside the shell (pid %ld)", prepared->argv[0],
                (long)getpid());
    }
    if (apply_redirects(shell, prepared, false) == 0) {
        if (builtin) {
            status = builtin(shell, (int)prepared->argc, prepared->argv);
        } else {
            apply_assignments(prepared);
            status = 0;
        }
    }
    fflush(stdout);
    fflush(stderr);
    for (int fd = 0; fd < 3; fd++) {
        if (saved[fd] >= 0) {
            dup2(saved[fd], fd);
            close(saved[fd]);
        }
    }
    return status;
}

static void default_signals(void)
{
    int signals[] = {SIGINT, SIGQUIT, SIGTSTP, SIGTTIN, SIGTTOU, SIGPIPE, SIGCHLD};
    for (size_t i = 0; i < sizeof signals / sizeof *signals; i++) {
        signal(signals[i], SIG_DFL);
    }
}

static void hold(Held *held, int fd)
{
    held->fds = checked_realloc(held->fds, (held->count + 1) * sizeof(int));
    held->fds[held->count++] = fd;
}

static void let_go(Held *held, int fd)
{
    for (size_t i = 0; i < held->count; i++) {
        if (held->fds[i] == fd) {
            held->fds[i] = held->fds[--held->count];
            break;
        }
    }
    close(fd);
}

static void become_child(Shell *shell, Prepared *prepared, int in, int out, const Held *held,
                         pid_t pgid, bool foreground)
{
    if (shell->own_groups) {
        pid_t group = pgid ? pgid : getpid();
        setpgid(0, group);
        if (shell->interactive && foreground) {
            tcsetpgrp(shell->terminal, group);
        }
    }
    default_signals();
    if (in != STDIN_FILENO) {
        narrate_child(shell, "dup2(%d, 0)", in);
        dup2(in, STDIN_FILENO);
    }
    if (out != STDOUT_FILENO) {
        narrate_child(shell, "dup2(%d, 1)", out);
        dup2(out, STDOUT_FILENO);
    }
    for (size_t i = 0; i < held->count; i++) {
        close(held->fds[i]);
    }
    if (apply_redirects(shell, prepared, true) != 0) {
        _exit(1);
    }
    apply_assignments(prepared);
    if (prepared->argc == 0) {
        _exit(0);
    }
    Builtin builtin = find_builtin(prepared->argv[0]);
    if (builtin) {
        shell->interactive = false;
        int status = builtin(shell, (int)prepared->argc, prepared->argv);
        fflush(NULL);
        _exit(status);
    }
    narrate_child(shell, "execvp(\"%s\")", prepared->argv[0]);
    execvp(prepared->argv[0], prepared->argv);
    int code = errno == ENOENT ? 127 : 126;
    if (errno == ENOENT) {
        fprintf(stderr, "glassh: %s: command not found\n", prepared->argv[0]);
    } else {
        fprintf(stderr, "glassh: %s: %s\n", prepared->argv[0], strerror(errno));
    }
    _exit(code);
}

static Stage *stage_of(Stage *stages, size_t count, pid_t pid)
{
    for (size_t i = 0; i < count; i++) {
        if (stages[i].pid == pid) {
            return &stages[i];
        }
    }
    return NULL;
}

static void record(Shell *shell, Stage *stage, int status, const struct rusage *usage,
                   size_t *remaining)
{
    stage->status = status;
    if (WIFSTOPPED(status)) {
        return;
    }
    stage->finished = true;
    stage->ended = now();
    stage->usage = *usage;
    (*remaining)--;
    narrate(shell, "wait4() = %ld", (long)stage->pid);
}

static bool collect_each(Shell *shell, Stage *stages, size_t count, size_t *remaining, int flags)
{
    int status;
    struct rusage usage;
    for (size_t i = 0; i < count; i++) {
        if (!stages[i].finished &&
            wait4(stages[i].pid, &status, WNOHANG | (flags & WUNTRACED), &usage) > 0) {
            record(shell, &stages[i], status, &usage, remaining);
            return true;
        }
    }
    if (flags & WNOHANG) {
        return false;
    }
    if (shell->trace) {
        struct timespec pause = {0, 2000000};
        nanosleep(&pause, NULL);
        return true;
    }
    for (size_t i = 0; i < count; i++) {
        if (!stages[i].finished) {
            if (wait4(stages[i].pid, &status, flags, &usage) <= 0) {
                return false;
            }
            record(shell, &stages[i], status, &usage, remaining);
            return true;
        }
    }
    return false;
}

static bool collect(Shell *shell, Stage *stages, size_t count, pid_t pgid, size_t *remaining,
                    int flags)
{
    if (!shell->own_groups) {
        return collect_each(shell, stages, count, remaining, flags);
    }
    int status;
    struct rusage usage;
    pid_t pid = wait4(-pgid, &status, flags, &usage);
    if (pid <= 0) {
        return false;
    }
    Stage *stage = stage_of(stages, count, pid);
    if (stage && !stage->finished) {
        record(shell, stage, status, &usage, remaining);
    }
    return true;
}

static void pump(Shell *shell, Relay *relays, size_t links, Stage *stages, size_t count, pid_t pgid,
                 size_t *remaining)
{
    for (size_t i = 0; i < links; i++) {
        relays[i].buffer = checked_malloc(RELAY_BUFFER);
    }
    struct pollfd *fds = checked_calloc(links * 2, sizeof *fds);
    size_t *owners = checked_calloc(links * 2, sizeof *owners);
    for (;;) {
        size_t watched = 0;
        bool open = false;
        for (size_t i = 0; i < links; i++) {
            Relay *relay = &relays[i];
            open = open || relay->source >= 0 || relay->sink >= 0;
            if (relay->source >= 0 && relay->length == 0) {
                fds[watched] = (struct pollfd){relay->source, POLLIN, 0};
                owners[watched++] = i;
            }
            if (relay->sink >= 0 && relay->length > relay->offset) {
                fds[watched] = (struct pollfd){relay->sink, POLLOUT, 0};
                owners[watched++] = i;
            }
        }
        if (!open) {
            break;
        }
        if (poll(fds, watched, POLL_INTERVAL) < 0 && errno != EINTR) {
            break;
        }
        for (size_t w = 0; w < watched; w++) {
            Relay *relay = &relays[owners[w]];
            if (!fds[w].revents) {
                continue;
            }
            if (fds[w].fd == relay->source) {
                ssize_t got = read(relay->source, relay->buffer, RELAY_BUFFER);
                if (got > 0) {
                    relay->length = (size_t)got;
                    relay->offset = 0;
                    relay->bytes += (size_t)got;
                } else if (got == 0 || (errno != EAGAIN && errno != EINTR)) {
                    close(relay->source);
                    relay->source = -1;
                    if (relay->sink >= 0) {
                        close(relay->sink);
                        relay->sink = -1;
                    }
                }
            } else {
                ssize_t put = write(relay->sink, relay->buffer + relay->offset,
                                    relay->length - relay->offset);
                if (put > 0) {
                    relay->offset += (size_t)put;
                    if (relay->offset == relay->length) {
                        relay->length = relay->offset = 0;
                    }
                } else if (put < 0 && errno != EAGAIN && errno != EINTR) {
                    close(relay->sink);
                    relay->sink = -1;
                    relay->length = relay->offset = 0;
                    if (relay->source >= 0) {
                        close(relay->source);
                        relay->source = -1;
                    }
                }
            }
        }
        while (*remaining && collect(shell, stages, count, pgid, remaining, WNOHANG)) {
        }
    }
    free(fds);
    free(owners);
    for (size_t i = 0; i < links; i++) {
        free(relays[i].buffer);
    }
}

int decode_status(Shell *shell, int status, const char *text)
{
    (void)shell;
    if (WIFEXITED(status)) {
        return WEXITSTATUS(status);
    }
    if (WIFSIGNALED(status)) {
        int number = WTERMSIG(status);
        if (number == SIGINT) {
            fputc('\n', stderr);
        } else if (number != SIGPIPE) {
            fprintf(stderr, "glassh: %s: %s\n", text, strsignal(number));
        }
        return 128 + number;
    }
    if (WIFSTOPPED(status)) {
        return 128 + WSTOPSIG(status);
    }
    return 1;
}

static void make_nonblocking(int fd)
{
    fcntl(fd, F_SETFL, fcntl(fd, F_GETFL) | O_NONBLOCK);
}

static int run_pipeline(Shell *shell, const Pipeline *pipeline, bool background)
{
    size_t count = pipeline->count;
    Prepared *prepared = checked_calloc(count, sizeof *prepared);
    int status = 0;
    for (size_t i = 0; i < count; i++) {
        if (prepare(shell, &pipeline->commands[i], &prepared[i]) != 0) {
            for (size_t j = 0; j <= i; j++) {
                release(&prepared[j]);
            }
            free(prepared);
            return 1;
        }
    }
    if (count == 1 && !background) {
        Builtin builtin = prepared[0].argc ? find_builtin(prepared[0].argv[0]) : NULL;
        if (builtin || prepared[0].argc == 0) {
            status = run_in_shell(shell, &prepared[0], builtin);
            release(&prepared[0]);
            free(prepared);
            return status;
        }
    }
    bool relaying = shell->trace && !background && count > 1;
    Stage *stages = checked_calloc(count, sizeof *stages);
    Relay *relays = checked_calloc(count, sizeof *relays);
    Held held = {0};
    pid_t pgid = 0;
    int previous = STDIN_FILENO;
    size_t started = 0;
    double began = now();
    for (size_t i = 0; i < count; i++) {
        int out = STDOUT_FILENO;
        int next = STDIN_FILENO;
        if (i + 1 < count) {
            int fds[2];
            if (pipe(fds) != 0) {
                perror("glassh: pipe");
                break;
            }
            narrate(shell, "pipe() = [%d, %d]", fds[0], fds[1]);
            hold(&held, fds[0]);
            hold(&held, fds[1]);
            out = fds[1];
            next = fds[0];
            if (relaying) {
                int tap[2];
                if (pipe(tap) != 0) {
                    perror("glassh: pipe");
                    break;
                }
                narrate(shell, "pipe() = [%d, %d]  relay through the shell to count bytes", tap[0],
                        tap[1]);
                hold(&held, tap[0]);
                hold(&held, tap[1]);
                make_nonblocking(fds[0]);
                make_nonblocking(tap[1]);
                relays[i] = (Relay){.source = fds[0], .sink = tap[1]};
                next = tap[0];
            }
        }
        fflush(stdout);
        fflush(stderr);
        pid_t pid = fork();
        if (pid < 0) {
            perror("glassh: fork");
            break;
        }
        if (pid == 0) {
            become_child(shell, &prepared[i], previous, out, &held, pgid, !background);
        }
        if (shell->own_groups) {
            if (!pgid) {
                pgid = pid;
            }
            setpgid(pid, pgid);
            if (shell->interactive && !background && i == 0) {
                tcsetpgrp(shell->terminal, pgid);
            }
        } else if (!pgid) {
            pgid = getpgrp();
        }
        stages[i] = (Stage){.pid = pid, .text = prepared[i].text, .started = now()};
        started++;
        narrate(shell, "fork() = %ld  →  %s", (long)pid, prepared[i].text);
        if (previous != STDIN_FILENO) {
            let_go(&held, previous);
        }
        if (out != STDOUT_FILENO) {
            let_go(&held, out);
        }
        previous = next;
    }
    if (previous != STDIN_FILENO) {
        let_go(&held, previous);
    }
    if (background) {
        pid_t *pids = checked_calloc(started, sizeof *pids);
        for (size_t i = 0; i < started; i++) {
            pids[i] = stages[i].pid;
        }
        char *text = pipeline_text(pipeline);
        Job *job = add_job(shell, pgid, pids, started, text);
        fprintf(stderr, "[%d] %ld\n", job->id, started ? (long)pids[started - 1] : 0L);
        free(text);
        free(pids);
    } else {
        size_t remaining = started;
        if (relaying) {
            pump(shell, relays, started ? started - 1 : 0, stages, started, pgid, &remaining);
        }
        bool stopped = false;
        while (remaining) {
            if (!collect(shell, stages, started, pgid, &remaining,
                         shell->own_groups ? WUNTRACED : 0)) {
                if (errno == EINTR) {
                    continue;
                }
                break;
            }
            for (size_t i = 0; i < started; i++) {
                if (!stages[i].finished && stages[i].pid && WIFSTOPPED(stages[i].status)) {
                    stopped = true;
                }
            }
            if (stopped) {
                break;
            }
        }
        if (shell->interactive) {
            tcsetpgrp(shell->terminal, shell->pgid);
        }
        if (stopped) {
            pid_t *pids = checked_calloc(started, sizeof *pids);
            for (size_t i = 0; i < started; i++) {
                pids[i] = stages[i].pid;
            }
            char *text = pipeline_text(pipeline);
            Job *job = add_job(shell, pgid, pids, started, text);
            for (size_t i = 0; i < started; i++) {
                job->finished[i] = stages[i].finished;
            }
            job->state = JOB_STOPPED;
            fprintf(stderr, "\n[%d]  Stopped   %s\n", job->id, text);
            status = 128 + SIGTSTP;
            free(text);
            free(pids);
        } else if (started) {
            status = decode_status(shell, stages[started - 1].status, stages[started - 1].text);
            size_t *bytes = NULL;
            if (relaying) {
                bytes = checked_calloc(count, sizeof *bytes);
                for (size_t i = 0; i + 1 < started; i++) {
                    bytes[i] = relays[i].bytes;
                }
            }
            report(shell, stages, started, bytes, now() - began, status);
            free(bytes);
        } else {
            status = 1;
        }
    }
    for (size_t i = 0; i < held.count; i++) {
        close(held.fds[i]);
    }
    free(held.fds);
    for (size_t i = 0; i < count; i++) {
        release(&prepared[i]);
    }
    free(prepared);
    free(stages);
    free(relays);
    return status;
}

static int run_links(Shell *shell, const Chain *chain, bool background)
{
    int status = shell->status;
    for (size_t i = 0; i < chain->count && shell->running; i++) {
        if (i && chain->links[i] == LINK_AND && status != 0) {
            continue;
        }
        if (i && chain->links[i] == LINK_OR && status == 0) {
            continue;
        }
        status = run_pipeline(shell, &chain->pipelines[i], background);
        shell->status = status;
    }
    return status;
}

static int run_chain(Shell *shell, const Chain *chain)
{
    if (!chain->background || chain->count == 1) {
        return run_links(shell, chain, chain->background);
    }
    fflush(stdout);
    fflush(stderr);
    pid_t pid = fork();
    if (pid < 0) {
        perror("glassh: fork");
        return 1;
    }
    if (pid == 0) {
        setpgid(0, 0);
        default_signals();
        signal(SIGPIPE, SIG_IGN);
        shell->interactive = false;
        shell->own_groups = false;
        shell->trace = false;
        int status = run_links(shell, chain, false);
        fflush(NULL);
        _exit(status);
    }
    setpgid(pid, pid);
    char *text = chain_text(chain);
    Job *job = add_job(shell, pid, &pid, 1, text);
    fprintf(stderr, "[%d] %ld\n", job->id, (long)pid);
    free(text);
    return 0;
}

int run_script(Shell *shell, const Script *script)
{
    for (size_t i = 0; i < script->count && shell->running; i++) {
        shell->status = run_chain(shell, &script->chains[i]);
    }
    return shell->status;
}

int run_line(Shell *shell, const char *line)
{
    char *error = NULL;
    Script *script = parse(line, &error);
    if (!script) {
        fprintf(stderr, "glassh: %s\n", error);
        free(error);
        shell->status = 2;
        return 2;
    }
    int status = run_script(shell, script);
    free_script(script);
    return status;
}
