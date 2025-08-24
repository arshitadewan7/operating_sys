/*********************************************************************
   Program  : miniShell                   Version    : 2.1 (Final+fixes)
 --------------------------------------------------------------------
   Minimal POSIX-compatible command-line interpreter for Assignment 1
   - Background jobs with '&' and completion reporting
     * Handles both "cmd &" and "cmd&" (no space before &)
   - Built-in 'cd' with:
     * HOME fallback; if HOME is unset, use getpwuid(getuid())->pw_dir
     * '~' expansion (cd ~, cd ~/path)
     * 'cd -' to jump to previous directory
   - perror() after every relevant system call
   - Child terminates if exec() fails
   - Prompt only when stdin is a TTY (so pipes/tests are clean)
   - Parent ignores SIGINT; child restores default (Ctrl+C kills fg job)
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

/* ---- cd helpers ---- */

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

/* Expand leading ~ (tilde) using HOME / pw_dir */
static void expand_tilde(char *dst, size_t dstsz, const char *src) {
    if (src && src[0] == '~') {
        char homebuf[NL];
        const char *home = get_home_dir(homebuf, sizeof(homebuf));
        if (home) {
            if (src[1] == '\0') {
                snprintf(dst, dstsz, "%s", home);
            } else if (src[1] == '/') {
                snprintf(dst, dstsz, "%s/%s", home, src + 2);
            } else {
                /* Not handling ~user form; copy as-is */
                snprintf(dst, dstsz, "%s", src);
            }
            return;
        }
    }
    snprintf(dst, dstsz, "%s", src ? src : "");
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

    /* Track previous directory for 'cd -' */
    char prev_dir[NL] = "";

    while (1) {
        prompt();

        if (!fgets(line, NL, stdin)) {
            if (feof(stdin)) {
                putchar('\n');
                exit(0);
            }
            perror("fgets");
            clearerr(stdin);
            continue;
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

        /* ---- Built-in: cd ---- */
        if (strcmp(v[0], "cd") == 0) {
            char target_buf[NL];
            const char *target = NULL;

            if (v[1] == NULL) {
                target = get_home_dir(target_buf, sizeof(target_buf));
                if (!target) {
                    fprintf(stderr, "cd: HOME not set\n");
                    reap_background();
                    continue;
                }
            } else if (strcmp(v[1], "-") == 0) {
                if (prev_dir[0] == '\0') {
                    fprintf(stderr, "cd: OLDPWD not set\n");
                    reap_background();
                    continue;
                }
                target = prev_dir;
            } else {
                expand_tilde(target_buf, sizeof(target_buf), v[1]);
                target = target_buf;
            }

            /* Save current dir to prev_dir (for 'cd -') */
            char cwd[NL];
            if (!getcwd(cwd, sizeof(cwd))) {
                perror("getcwd");
                cwd[0] = '\0';
            }

            if (chdir(target) == -1) {
                perror("chdir");
            } else {
                if (cwd[0]) {
                    snprintf(prev_dir, sizeof(prev_dir), "%s", cwd);
                }
            }
            reap_background();
            continue;
        }

        /* Built-in: exit/quit */
        if (strcmp(v[0], "exit") == 0 || strcmp(v[0], "quit") == 0) {
            int status;
            while (waitpid(-1, &status, 0) > 0) { /* wait all background */ }
            if (errno != ECHILD && errno != 0) perror("waitpid");
            break;
        }

        /* ---- Background? handle both "&" token and trailing '&' ---- */
        int background = 0;
        if (i > 0 && v[i-1]) {
            size_t len = strlen(v[i-1]);
            if (len == 1 && strcmp(v[i-1], "&") == 0) {
                background = 1;
                v[i-1] = NULL; /* remove & */
            } else if (len > 1 && v[i-1][len-1] == '&') {
                background = 1;
                v[i-1][len-1] = '\0'; /* strip trailing & from token */
            }
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
            perror("execvp");
            _exit(EXIT_FAILURE);
        } else {
            /* Parent process */
            if (background) {
                int job_id = add_job(pid, cmdline);
                if (job_id == -1) {
                    fprintf(stderr, "job table full; not tracking PID %d\n", pid);
                } else {
                    printf("[%d] %d\n", job_id, (int)pid);
                    fflush(stdout);
                }
                reap_background();
            } else {
                int status;
                if (waitpid(pid, &status, 0) == -1) {
                    perror("waitpid");
                }
                reap_background();
            }
        }
    }

    return 0;
}
