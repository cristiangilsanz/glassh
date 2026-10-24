#include <errno.h>
#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

#include "glassh.h"

Job *add_job(Shell *shell, pid_t pgid, const pid_t *pids, size_t count, const char *text)
{
    int id = 1;
    for (size_t i = 0; i < shell->job_count; i++) {
        if (shell->jobs[i].id >= id) {
            id = shell->jobs[i].id + 1;
        }
    }
    shell->jobs = checked_realloc(shell->jobs, (shell->job_count + 1) * sizeof(Job));
    Job *job = &shell->jobs[shell->job_count++];
    *job = (Job){.id = id, .pgid = pgid, .count = count, .state = JOB_RUNNING};
    job->pids = checked_calloc(count, sizeof(pid_t));
    memcpy(job->pids, pids, count * sizeof(pid_t));
    job->finished = checked_calloc(count, sizeof(bool));
    job->text = checked_strdup(text);
    return job;
}

static void remove_job(Shell *shell, Job *job)
{
    free(job->pids);
    free(job->finished);
    free(job->text);
    size_t index = (size_t)(job - shell->jobs);
    memmove(job, job + 1, (shell->job_count - index - 1) * sizeof(Job));
    shell->job_count--;
}

static void signal_job(Job *job, int number)
{
    if (job->pgid != getpgrp()) {
        kill(-job->pgid, number);
        return;
    }
    for (size_t i = 0; i < job->count; i++) {
        if (!job->finished[i]) {
            kill(job->pids[i], number);
        }
    }
}

static bool settle(Job *job, pid_t pid, int status)
{
    for (size_t i = 0; i < job->count; i++) {
        if (job->pids[i] != pid) {
            continue;
        }
        if (WIFSTOPPED(status)) {
            job->state = JOB_STOPPED;
        } else if (WIFCONTINUED(status)) {
            job->state = JOB_RUNNING;
        } else {
            job->finished[i] = true;
            if (i + 1 == job->count) {
                job->status = status;
            }
        }
        bool done = true;
        for (size_t j = 0; j < job->count; j++) {
            done = done && job->finished[j];
        }
        if (done) {
            job->state = JOB_DONE;
        }
        return true;
    }
    return false;
}

static const char *state_name(JobState state)
{
    return state == JOB_RUNNING ? "Running" : state == JOB_STOPPED ? "Stopped" : "Done";
}

void reap_jobs(Shell *shell, bool report_done)
{
    for (size_t j = 0; j < shell->job_count; j++) {
        Job *job = &shell->jobs[j];
        for (size_t i = 0; i < job->count; i++) {
            int status;
            if (job->finished[i]) {
                continue;
            }
            pid_t pid = waitpid(job->pids[i], &status, WNOHANG | WUNTRACED | WCONTINUED);
            if (pid > 0) {
                settle(job, pid, status);
            } else if (pid < 0 && errno == ECHILD) {
                job->finished[i] = true;
                bool done = true;
                for (size_t k = 0; k < job->count; k++) {
                    done = done && job->finished[k];
                }
                if (done) {
                    job->state = JOB_DONE;
                }
            }
        }
    }
    for (size_t j = 0; j < shell->job_count;) {
        Job *job = &shell->jobs[j];
        if (job->state != JOB_DONE) {
            j++;
            continue;
        }
        if (report_done) {
            fprintf(stderr, "[%d]  Done      %s\n", job->id, job->text);
        }
        remove_job(shell, job);
    }
}

Job *find_job(Shell *shell, const char *spec)
{
    if (shell->job_count == 0) {
        return NULL;
    }
    if (!spec) {
        return &shell->jobs[shell->job_count - 1];
    }
    if (*spec == '%') {
        spec++;
    }
    char *end;
    long id = strtol(spec, &end, 10);
    if (*spec == '\0' || *end != '\0') {
        return NULL;
    }
    for (size_t i = 0; i < shell->job_count; i++) {
        if (shell->jobs[i].id == id) {
            return &shell->jobs[i];
        }
    }
    return NULL;
}

static int wait_job(Shell *shell, Job *job)
{
    for (;;) {
        bool remaining = false;
        for (size_t i = 0; i < job->count; i++) {
            remaining = remaining || !job->finished[i];
        }
        if (!remaining || job->state == JOB_STOPPED) {
            break;
        }
        int status;
        pid_t pid = job->pgid != getpgrp() ? waitpid(-job->pgid, &status, WUNTRACED)
                                           : waitpid(-1, &status, WUNTRACED);
        if (pid < 0) {
            if (errno == EINTR) {
                continue;
            }
            for (size_t i = 0; i < job->count; i++) {
                job->finished[i] = true;
            }
            job->state = JOB_DONE;
            break;
        }
        if (!settle(job, pid, status)) {
            for (size_t j = 0; j < shell->job_count; j++) {
                if (settle(&shell->jobs[j], pid, status)) {
                    break;
                }
            }
        }
    }
    return job->state == JOB_STOPPED ? 128 + SIGTSTP : decode_status(shell, job->status, job->text);
}

int foreground_job(Shell *shell, Job *job)
{
    fprintf(stderr, "%s\n", job->text);
    if (shell->interactive) {
        tcsetpgrp(shell->terminal, job->pgid);
    }
    job->state = JOB_RUNNING;
    signal_job(job, SIGCONT);
    int status = wait_job(shell, job);
    if (shell->interactive) {
        tcsetpgrp(shell->terminal, shell->pgid);
    }
    if (job->state == JOB_STOPPED) {
        fprintf(stderr, "\n[%d]  Stopped   %s\n", job->id, job->text);
    } else {
        remove_job(shell, job);
    }
    return status;
}

void resume_job(Shell *shell, Job *job)
{
    (void)shell;
    job->state = JOB_RUNNING;
    signal_job(job, SIGCONT);
    fprintf(stderr, "[%d]  %s &\n", job->id, job->text);
}

int wait_all_jobs(Shell *shell)
{
    int status = 0;
    while (shell->job_count) {
        Job *job = &shell->jobs[0];
        if (job->state == JOB_STOPPED) {
            fprintf(stderr, "glassh: wait: job %d is stopped\n", job->id);
            return 1;
        }
        status = wait_job(shell, job);
        remove_job(shell, job);
    }
    return status;
}

void list_jobs(Shell *shell)
{
    reap_jobs(shell, false);
    for (size_t i = 0; i < shell->job_count; i++) {
        Job *job = &shell->jobs[i];
        printf("[%d]  %-8s  %s\n", job->id, state_name(job->state), job->text);
    }
}
