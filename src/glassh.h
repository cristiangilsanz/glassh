#ifndef GLASSH_H
#define GLASSH_H

#include <stdarg.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>
#include <sys/resource.h>
#include <sys/types.h>

#define GLASSH_VERSION "1.0.0"

typedef enum {
    REDIRECT_IN,
    REDIRECT_OUT,
    REDIRECT_APPEND,
    REDIRECT_ERR,
    REDIRECT_ERR_APPEND,
    REDIRECT_ERR_TO_OUT
} RedirectKind;

typedef struct {
    RedirectKind kind;
    char *target;
} Redirect;

typedef struct {
    char **words;
    size_t word_count;
    Redirect *redirects;
    size_t redirect_count;
} Command;

typedef struct {
    Command *commands;
    size_t count;
} Pipeline;

typedef enum { LINK_ALWAYS, LINK_AND, LINK_OR } Link;

typedef struct {
    Pipeline *pipelines;
    Link *links;
    size_t count;
    bool background;
} Chain;

typedef struct {
    Chain *chains;
    size_t count;
} Script;

typedef enum { JOB_RUNNING, JOB_STOPPED, JOB_DONE } JobState;

typedef struct {
    int id;
    pid_t pgid;
    pid_t *pids;
    bool *finished;
    size_t count;
    int status;
    JobState state;
    char *text;
} Job;

typedef struct {
    pid_t pid;
    char *text;
    int status;
    bool finished;
    double started;
    double ended;
    struct rusage usage;
} Stage;

typedef struct {
    int status;
    int exit_code;
    bool running;
    bool interactive;
    bool own_groups;
    bool trace;
    bool color;
    int terminal;
    int trace_fd;
    pid_t pgid;
    Job *jobs;
    size_t job_count;
} Shell;

Script *parse(const char *line, char **error);
void free_script(Script *script);
char *command_text(const Command *command);
char *pipeline_text(const Pipeline *pipeline);
char *chain_text(const Chain *chain);
void explain(FILE *out, const Script *script);

char **expand_words(Shell *shell, char *const *raw, size_t count, size_t *expanded, char **error);
char *expand_single(Shell *shell, const char *raw, char **error);
bool is_assignment(const char *raw);
void free_words(char **words);

int run_line(Shell *shell, const char *line);
int run_script(Shell *shell, const Script *script);
int decode_status(Shell *shell, int status, const char *text);

Job *add_job(Shell *shell, pid_t pgid, const pid_t *pids, size_t count, const char *text);
Job *find_job(Shell *shell, const char *spec);
void reap_jobs(Shell *shell, bool report);
int foreground_job(Shell *shell, Job *job);
void resume_job(Shell *shell, Job *job);
int wait_all_jobs(Shell *shell);
void list_jobs(Shell *shell);

typedef int (*Builtin)(Shell *shell, int argc, char **argv);
Builtin find_builtin(const char *name);

double now(void);
void narrate(Shell *shell, const char *format, ...) __attribute__((format(printf, 2, 3)));
void narrate_child(Shell *shell, const char *format, ...) __attribute__((format(printf, 2, 3)));
void report(Shell *shell, const Stage *stages, size_t count, const size_t *bytes, double wall,
            int status);
void human_bytes(double bytes, char *out, size_t size);
const char *paint(const Shell *shell, const char *code);

void *checked_malloc(size_t size);
void *checked_calloc(size_t count, size_t size);
void *checked_realloc(void *pointer, size_t size);
char *checked_strdup(const char *text);

#endif
