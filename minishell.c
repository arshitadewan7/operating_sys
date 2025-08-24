/*********************************************************************
   Program  : miniShell                   Version    : 2.4 (EOF + CRLF fixes)
 --------------------------------------------------------------------
   - Background jobs with '&' and completion reporting (cmd & and cmd&)
   - Built-in 'cd' (cd -> HOME fallback to pw_dir; cd <path>)
     Errors printed via perror("chdir")
   - perror() after fgets/fork/execvp/waitpid/chdir/sigaction/getcwd
   - Child terminates if exec fails
   - Prompt only when stdin is a TTY
   - Parent ignores SIGINT; child restores default
   - IMPORTANT: On EOF, block until ALL background jobs are reaped,
     printing "[#]+ Done  <cmd>" for each (fixes multi-bg test)
   - IMPORTANT: Token separators include '\r' to handle CRLF inputs
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
#include <pwd.h>

#define NV       128
#define NL       1024
#define MAX_JOBS 128

static char line[NL];

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
    return -1;
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

static void prompt(void) {
    if (isatty(STDIN_FILENO)) {
        fprintf(stdout, "msh> ");
        fflush(stdout);
    }
}

static void join_tokens(char *dst, size_t dstsz, char *const v[]) {
    dst[0] = '\0';
    for (int i = 0; v[i]; ++i) {
        if (i) strncat(dst, " ", dstsz - strlen(dst) - 1);
        strncat(dst, v[i], dstsz - strlen(dst) - 1);
    }
}

/* Non-blocking reap (used between commands) */
static void reap_background_now(void) {
    int status;
    pid_t done;
    while ((done = waitpid(-1, &status, WNOHANG)) > 0) {
        Job* j = find_job_by_pid(done);
        if (j) {
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

/* Blocking reap for ALL children (used on EOF/exit) */
static void reap_all_blocking(void) {
    int status;
    pid_t done;
    while ((done = waitpid(-1, &status, 0)) > 0) {
        Job* j = find_job_by_pid(done);
        if (j) {
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

/* HOME fallback helper for cd */
static const char* get_home_dir(char *buf, size_t bufsz) {
    const char *home = getenv("HOME");
    if (home && home[0]) return home;
    struct passwd *pw = getpwuid(getuid());
    if (pw && pw->pw_dir && pw->pw_dir[0]) {
        snprintf(buf, bufsz, "%s", pw->pw_dir);
        return buf;
    }
    return NULL;
}

int main(int argk, char *argv[], char *envp[]) {
    (void)argk; (void)argv; (void)envp;

    init_jobs();

    struct sigaction ign;
    memset(&ign, 0, sizeof(ign));
    ign.sa_handler = SIG_IGN;
    sigemptyset(&ign.sa_mask);
    if (sigaction(SIGINT, &ign, NULL) == -1) {
        perror("sigaction");
    }

    const char *sep = " \t\r\n";  /* include '\r' for CRLF inputs */
    char *v[NV];

    while (1) {
        prompt();

        if (!fgets(line, NL, stdin)) {
            if (feof(stdin)) {
                /* On EOF: block until all children reaped, then exit */
                reap_all_blocking();
                putchar('\n');
                exit(0);
            }
            perror("fgets");
            clearerr(stdin);
            continue;
        }

        if (line[0] == '\n' || line[0] == '\0' || line[0] == '#') {
            reap_background_now();
            continue;
        }

        v[0] = strtok(line, sep);
        if (!v[0]) { reap_background_now(); continue; }
        int i;
        for (i = 1; i < NV; i++) {
            v[i] = strtok(NULL, sep);
            if (v[i] == NULL) break;
        }

        /* ---- Built-in: minimal cd ---- */
        if (strcmp(v[0], "cd") == 0) {
            const char *target = NULL;
            char homebuf[NL];

            if (v[1] == NULL) {
                target = get_home_dir(homebuf, sizeof(homebuf));
                if (!target) {
                    fprintf(stderr, "cd: HOME not set\n");
                    reap_background_now();
                    continue;
                }
            } else {
                target = v[1];
            }

            if (chdir(target) == -1) {
                perror("chdir");  /* required error label */
            }
            reap_background_now();
            continue;
        }

        /* Background? handle "&" token and trailing '&' */
        int background = 0;
        if (i > 0 && v[i-1]) {
            size_t len = strlen(v[i-1]);
            if (len == 1 && strcmp(v[i-1], "&") == 0) {
                background = 1;
                v[i-1] = NULL; /* remove & */
            } else if (len > 0 && v[i-1][len-1] == '&') {
                background = 1;
                v[i-1][len-1] = '\0';  /* strip trailing & */
                if (v[i-1][0] == '\0') v[i-1] = NULL; /* empty token -> trim argv */
            }
        }

        char cmdline[NL];
        join_tokens(cmdline, sizeof(cmdline), v);

        pid_t pid = fork();
        if (pid == -1) {
            perror("fork");
            reap_background_now();
            continue;
        }

        if (pid == 0) {
            struct sigaction dfl;
            memset(&dfl, 0, sizeof(dfl));
            dfl.sa_handler = SIG_DFL;
            sigemptyset(&dfl.sa_mask);
            if (sigaction(SIGINT, &dfl, NULL) == -1) {
                perror("sigaction");
                _exit(EXIT_FAILURE);
            }

            execvp(v[0], v);
            perror("execvp");
            _exit(EXIT_FAILURE);
        } else {
            if (background) {
                int job_id = add_job(pid, cmdline);
                if (job_id == -1) {
                    fprintf(stderr, "job table full; not tracking PID %d\n", pid);
                } else {
                    printf("[%d] %d\n", job_id, (int)pid);
                    fflush(stdout);
                }
                reap_background_now();
            } else {
                int status;
                if (waitpid(pid, &status, 0) == -1) {
                    perror("waitpid");
                }
                reap_background_now();
            }
        }
    }

    return 0;
}
