/*********************************************************************
   Program  : miniShell                   Version    : 2.0 (Final)
 --------------------------------------------------------------------
   Minimal POSIX-compatible command-line interpreter for Assignment 1
   - Background jobs with '&' and completion reporting
   - Built-in 'cd' with HOME fallback
   - perror() after every relevant system call
   - Child terminates if exec() fails
   - Prompt only when stdin is a TTY (so pipes/tests are clean)
   - Parent ignores SIGINT; child restores default (Ctrl+C kills fg job)

   Notes on changes & why:
   * '&' background: track jobs by (job_id, pid, cmd) so we can print
     "[#] PID" at start and "[#]+ Done  command" when they finish later.
   * 'cd' must be built-in (no fork). Supports "cd" → $HOME and "cd path".
   * perror after fgets/fork/execvp/waitpid/chdir/sigaction to satisfy
     "perror after each system call" requirement.
   * Child exits on exec error to avoid zombie/loops.
   * Parent ignores SIGINT so Ctrl+C kills fg job, not the shell itself.
 ********************************************************************/

#define _POSIX_C_SOURCE 200809L

#include <sys/types.h>
#include <sys/wait.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <stdlib.h>
#include <signal.h>
#include <errno.h>

#define NV       128    /* max number of command tokens */
#define NL       1024   /* input buffer size */
#define MAX_JOBS 128    /* max tracked background jobs */

static char line[NL];   /* command input buffer */

/* ------------------------------
   Job table for background work
   ------------------------------ */
typedef struct Job {
    int   used;
    int   job_id;
    pid_t pid;
    char  cmd[NL];
} Job;

static Job jobs[MAX_JOBS];
static int next_job_id = 1;

static void init_jobs(void) {
    memset(jobs, 0, sizeof(jobs));
    next_job_id = 1;
}

static int add_job(pid_t pid, const char* cmd) {
    for (int k = 0; k < MAX_JOBS; ++k) {
        if (!jobs[k].used) {
            jobs[k].used   = 1;
            jobs[k].job_id = next_job_id++;
            jobs[k].pid    = pid;
            snprintf(jobs[k].cmd, sizeof(jobs[k].cmd), "%s", cmd ? cmd : "");
            return jobs[k].job_id;
        }
    }
    return -1; // table full
}

static Job* find_job_by_pid(pid_t pid) {
    for (int k = 0; k < MAX_JOBS; ++k) {
        if (jobs[k].used && jobs[k].pid == pid) return &jobs[k];
    }
    return NULL;
}

static void remove_job(Job* j) {
    if (j) memset(j, 0, sizeof(*j));
}

/* Render prompt only when interactive */
static void prompt(void) {
    if (isatty(STDIN_FILENO)) {
        fprintf(stdout, "msh> ");
        fflush(stdout);
    }
}

/* Join argv tokens into a single command string (for Done messages) */
static void join_tokens(char *dst, size_t dstsz, char *const v[]) {
    dst[0] = '\0';
    for (int i = 0; v[i]; ++i) {
        if (i) strncat(dst, " ", dstsz - strlen(dst) - 1);
        strncat(dst, v[i], dstsz - strlen(dst) - 1);
    }
}

/* Reap any finished background children and report them */
static void reap_background(void) {
    int status;
    pid_t done;
    while ((done = waitpid(-1, &status, WNOHANG)) > 0) {
        Job* j = find_job_by_pid(done);
        if (j) {
            /* Match expected style (spacing similar to typical shells):
               [#]+ Done                 <cmd> */
            printf("[%d]+ Done                 %s\n", j->job_id,
                   j->cmd[0] ? j->cmd : "");
            fflush(stdout);
            remove_job(j);
        }
    }
    if (done == -1 && errno != ECHILD) {
        perror("waitpid");
    }
}

int main(int argk, char *argv[], char *envp[]) {
    (void)argk; (void)argv; (void)envp;

    init_jobs();

    /* Ignore SIGINT in the shell so Ctrl+C terminates the foreground child,
       not the shell itself (child resets to default). */
    struct sigaction ign;
    memset(&ign, 0, sizeof(ign));
    ign.sa_handler = SIG_IGN;
    sigemptyset(&ign.sa_mask);
    if (sigaction(SIGINT, &ign, NULL) == -1) {
        perror("sigaction");
        /* continue; shell can still operate */
    }

    const char *sep = " \t\n";
    char *v[NV];

    while (1) {
        prompt();

        /* fgets: add perror on error; retain EOF behavior from skeleton */
        if (!fgets(line, NL, stdin)) {
            if (feof(stdin)) {      /* EOF (e.g., Ctrl+D or end of a pipe) */
                putchar('\n');
                exit(0);
            }
            perror("fgets");
            clearerr(stdin);
            continue;               /* try next prompt */
        }

        /* Skip empty/comment lines (leading '#') */
        if (line[0] == '\n' || line[0] == '\0' || line[0] == '#') {
            reap_background();
            continue;
        }

        /* Tokenize */
        v[0] = strtok(line, sep);
        if (!v[0]) { reap_background(); continue; }
        int i;
        for (i = 1; i < NV; i++) {
            v[i] = strtok(NULL, sep);
            if (v[i] == NULL) break;
        }

        /* Built-in: cd */
        if (strcmp(v[0], "cd") == 0) {
            const char *target = NULL;
            if (v[1] == NULL) {
                target = getenv("HOME");
                if (!target) {
                    fprintf(stderr, "cd: HOME not set\n");
                    reap_background();
                    continue;
                }
            } else {
                target = v[1];
            }
            if (chdir(target) == -1) {
                perror("chdir");
            }
            reap_background();
            continue;
        }

        /* Built-in: exit/quit */
        if (strcmp(v[0], "exit") == 0 || strcmp(v[0], "quit") == 0) {
            /* Optionally wait for background jobs to finish */
            int status;
            while (waitpid(-1, &status, 0) > 0) { /* no-op */ }
            if (errno != ECHILD && errno != 0) perror("waitpid");
            break;
        }

        /* Background? last token == "&" */
        int background = 0;
        if (i > 0 && v[i-1] && strcmp(v[i-1], "&") == 0) {
            background = 1;
            v[i-1] = NULL; /* remove & from argv */
        }

        /* Save command line (without '&') for job table / Done printing */
        char cmdline[NL];
        join_tokens(cmdline, sizeof(cmdline), v);

        /* Fork & exec */
        pid_t pid = fork();
        if (pid == -1) {
            perror("fork");
            reap_background();
            continue;
        }

        if (pid == 0) {
            /* Child process: restore default SIGINT behavior */
            struct sigaction dfl;
            memset(&dfl, 0, sizeof(dfl));
            dfl.sa_handler = SIG_DFL;
            sigemptyset(&dfl.sa_mask);
            if (sigaction(SIGINT, &dfl, NULL) == -1) {
                perror("sigaction");
                _exit(EXIT_FAILURE);
            }

            execvp(v[0], v);
            /* If execvp returns, it's an error */
            perror("execvp");
            _exit(EXIT_FAILURE);
        } else {
            /* Parent process */
            if (background) {
                int job_id = add_job(pid, cmdline);
                if (job_id == -1) {
                    fprintf(stderr, "job table full; not tracking PID %d\n", pid);
                } else {
                    /* Start line: "[#] PID" */
                    printf("[%d] %d\n", job_id, (int)pid);
                    fflush(stdout);
                }
                /* Reap any other finished background jobs */
                reap_background();
            } else {
                /* Foreground: wait for this specific child */
                int status;
                if (waitpid(pid, &status, 0) == -1) {
                    perror("waitpid");
                }
                /* After foreground completes, also reap any bg completions */
                reap_background();
            }
        }
    }

    return 0;
}
