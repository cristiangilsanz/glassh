#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include "glassh.h"

#define TEXT_WIDTH 28

static const char *RESET = "\033[0m";
static const char *DIM = "\033[2m";
static const char *CYAN = "\033[36m";
static const char *GREEN = "\033[32m";
static const char *RED = "\033[31m";
static const char *YELLOW = "\033[33m";

void *checked_malloc(size_t size)
{
    void *pointer = malloc(size ? size : 1);
    if (!pointer) {
        perror("glassh");
        exit(EXIT_FAILURE);
    }
    return pointer;
}

void *checked_calloc(size_t count, size_t size)
{
    void *pointer = calloc(count ? count : 1, size ? size : 1);
    if (!pointer) {
        perror("glassh");
        exit(EXIT_FAILURE);
    }
    return pointer;
}

void *checked_realloc(void *pointer, size_t size)
{
    void *resized = realloc(pointer, size ? size : 1);
    if (!resized) {
        perror("glassh");
        exit(EXIT_FAILURE);
    }
    return resized;
}

char *checked_strdup(const char *text)
{
    char *copy = strdup(text);
    if (!copy) {
        perror("glassh");
        exit(EXIT_FAILURE);
    }
    return copy;
}

double now(void)
{
    struct timespec moment;
    clock_gettime(CLOCK_MONOTONIC, &moment);
    return (double)moment.tv_sec + (double)moment.tv_nsec / 1e9;
}

const char *paint(const Shell *shell, const char *code)
{
    return shell->color ? code : "";
}

static void vnarrate(Shell *shell, const char *prefix, const char *format, va_list arguments)
{
    if (!shell->trace) {
        return;
    }
    char line[1024];
    vsnprintf(line, sizeof line, format, arguments);
    dprintf(shell->trace_fd, "%s%s%s%s\n", paint(shell, DIM), prefix, line, paint(shell, RESET));
}

void narrate(Shell *shell, const char *format, ...)
{
    va_list arguments;
    va_start(arguments, format);
    vnarrate(shell, "  · ", format, arguments);
    va_end(arguments);
}

void narrate_child(Shell *shell, const char *format, ...)
{
    char prefix[32];
    snprintf(prefix, sizeof prefix, "      ╰─ %ld ", (long)getpid());
    va_list arguments;
    va_start(arguments, format);
    vnarrate(shell, prefix, format, arguments);
    va_end(arguments);
}

void human_bytes(double bytes, char *out, size_t size)
{
    static const char *units[] = {"B", "KiB", "MiB", "GiB", "TiB"};
    size_t unit = 0;
    while (bytes >= 1024 && unit + 1 < sizeof units / sizeof *units) {
        bytes /= 1024;
        unit++;
    }
    snprintf(out, size, unit ? "%.1f %s" : "%.0f %s", bytes, units[unit]);
}

static void milliseconds(double seconds, char *out, size_t size)
{
    double ms = seconds * 1000;
    snprintf(out, size, ms < 10 ? "%.2f ms" : ms < 1000 ? "%.1f ms" : "%.0f ms", ms);
}

static double cpu_seconds(const struct rusage *usage)
{
    return (double)usage->ru_utime.tv_sec + (double)usage->ru_utime.tv_usec / 1e6 +
           (double)usage->ru_stime.tv_sec + (double)usage->ru_stime.tv_usec / 1e6;
}

static double peak_memory(const struct rusage *usage)
{
#ifdef __APPLE__
    return (double)usage->ru_maxrss;
#else
    return (double)usage->ru_maxrss * 1024;
#endif
}

static const char *signal_name(int number)
{
    static const struct {
        int number;
        const char *name;
    } names[] = {
        {SIGHUP, "SIGHUP"},   {SIGINT, "SIGINT"},   {SIGQUIT, "SIGQUIT"}, {SIGILL, "SIGILL"},
        {SIGABRT, "SIGABRT"}, {SIGFPE, "SIGFPE"},   {SIGKILL, "SIGKILL"}, {SIGBUS, "SIGBUS"},
        {SIGSEGV, "SIGSEGV"}, {SIGPIPE, "SIGPIPE"}, {SIGALRM, "SIGALRM"}, {SIGTERM, "SIGTERM"},
        {SIGUSR1, "SIGUSR1"}, {SIGUSR2, "SIGUSR2"},
    };
    for (size_t i = 0; i < sizeof names / sizeof *names; i++) {
        if (names[i].number == number) {
            return names[i].name;
        }
    }
    return "signal";
}

static void outcome(const Stage *stage, char *out, size_t size)
{
    if (!stage->finished) {
        snprintf(out, size, "running");
    } else if (WIFEXITED(stage->status)) {
        snprintf(out, size, "exit %d", WEXITSTATUS(stage->status));
    } else if (WIFSIGNALED(stage->status)) {
        snprintf(out, size, "%s", signal_name(WTERMSIG(stage->status)));
    } else {
        snprintf(out, size, "stopped");
    }
}

static int text_columns(const char *text)
{
    int columns = 0;
    for (; *text; text++) {
        if (((unsigned char)*text & 0xC0) != 0x80) {
            columns++;
        }
    }
    return columns;
}

static void clip(const char *text, char *out, size_t size)
{
    if (text_columns(text) <= TEXT_WIDTH) {
        snprintf(out, size, "%s", text);
        return;
    }
    size_t used = 0;
    int columns = 0;
    for (; *text && used + 4 < size; text++) {
        if (((unsigned char)*text & 0xC0) != 0x80 && ++columns == TEXT_WIDTH) {
            break;
        }
        out[used++] = *text;
    }
    snprintf(out + used, size - used, "…");
}

void report(Shell *shell, const Stage *stages, size_t count, const size_t *bytes, double wall,
            int status)
{
    if (!shell->trace) {
        return;
    }
    int fd = shell->trace_fd;
    char took[32];
    milliseconds(wall, took, sizeof took);
    int width = 0;
    for (size_t i = 0; i < count; i++) {
        char clipped[256];
        clip(stages[i].text, clipped, sizeof clipped);
        int columns = text_columns(clipped);
        width = columns > width ? columns : width;
    }
    dprintf(fd, "%s┌─%s %zu process%s in %s\n", paint(shell, DIM), paint(shell, RESET), count,
            count == 1 ? "" : "es", took);
    for (size_t i = 0; i < count; i++) {
        const Stage *stage = &stages[i];
        char clipped[256], result[32], wall_time[32], cpu[32], memory[32];
        clip(stage->text, clipped, sizeof clipped);
        outcome(stage, result, sizeof result);
        milliseconds(stage->ended - stage->started, wall_time, sizeof wall_time);
        milliseconds(cpu_seconds(&stage->usage), cpu, sizeof cpu);
        human_bytes(peak_memory(&stage->usage), memory, sizeof memory);
        bool success = stage->finished && WIFEXITED(stage->status) && !WEXITSTATUS(stage->status);
        int padding = width - text_columns(clipped);
        dprintf(fd, "%s│%s  %s%6ld%s  %s%*s  %s%-8s%s %10s   cpu %9s   %s%10s%s\n",
                paint(shell, DIM), paint(shell, RESET), paint(shell, CYAN), (long)stage->pid,
                paint(shell, RESET), clipped, padding, "", paint(shell, success ? GREEN : RED),
                result, paint(shell, RESET), wall_time, cpu, paint(shell, YELLOW), memory,
                paint(shell, RESET));
        if (bytes && i + 1 < count) {
            char flow[32];
            human_bytes((double)bytes[i], flow, sizeof flow);
            dprintf(fd, "%s│          └─▶%s %s%s%s\n", paint(shell, DIM), paint(shell, RESET),
                    paint(shell, YELLOW), flow, paint(shell, RESET));
        }
    }
    dprintf(fd, "%s└─%s status %s%d%s\n", paint(shell, DIM), paint(shell, RESET),
            paint(shell, status ? RED : GREEN), status, paint(shell, RESET));
}
