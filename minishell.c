/*********************************************************************
   Program  : miniShell                   Version    : 1.3 → 1.4 (Stage 1+2)
 --------------------------------------------------------------------
   Stage 1: Add perror() after system calls
   Stage 2: Ensure child terminates if execvp() fails

   NOTE: This is intentionally minimal to pass Stage 1+2 tests.
         - No 'cd' built-in yet.
         - No background job handling yet.
         - The "done" printf from the skeleton is still here (remove
           before final submission as the comment states).
********************************************************************/

#include <sys/types.h>
#include <sys/wait.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <stdlib.h>
#include <signal.h>
#include <errno.h>

#define NV 20            /* max number of command tokens */
#define NL 100           /* input buffer size */
char line[NL];           /* command input buffer */

/*
  shell prompt
 */
void prompt(void)
{
  // ## REMOVE THIS 'fprintf' STATEMENT BEFORE SUBMISSION
  fprintf(stdout, "\n msh> ");
  fflush(stdout);
}

/* argk - number of arguments */
/* argv - argument vector from command line */
/* envp - environment pointer */
int main(int argk, char *argv[], char *envp[])
{
  int frkRtnVal;                /* value returned by fork sys call */
  char *v[NV];                  /* array of pointers to command line tokens */
  char *sep = (char *)" \t\n";  /* command line token separators    */
  int i;                        /* parse index */

  /* prompt for and process one command line at a time  */
  while (1) {                   /* do Forever */
    prompt();

    /* fgets: add perror on error; retain Gradescope EOF behavior */
    if (fgets(line, NL, stdin) == NULL) {
      if (feof(stdin)) {        /* non-zero on EOF  */
        exit(0);
      }
      /* Error case */
      perror("fgets");
      clearerr(stdin);
      continue;                 /* try next prompt */
    }
    fflush(stdin);              /* kept from skeleton */

    // This if() required for gradescope
    if (feof(stdin)) {          /* non-zero on EOF  */
      exit(0);
    }
    if (line[0] == '#' || line[0] == '\n' || line[0] == '\000'){
      continue;                 /* to prompt */
    }

    v[0] = strtok(line, sep);
    for (i = 1; i < NV; i++) {
      v[i] = strtok(NULL, sep);
      if (v[i] == NULL){
        break;
      }
    }
    /* assert i is number of tokens + 1 */

    /* fork a child process to exec the command in v[0] */
    switch (frkRtnVal = fork()) {
      case -1:                  /* fork returns error to parent process */
      {
        perror("fork");
        break;
      }
      case 0:                   /* code executed only by child process */
      {
        execvp(v[0], v);
        /* If we reach here, execvp failed */
        perror("execvp");
        _exit(EXIT_FAILURE);    /* ensure child terminates */
      }
      default:                  /* code executed only by parent process */
      {
        int status = 0;
        if (waitpid(frkRtnVal, &status, 0) == -1) {
          perror("waitpid");
        }
        // REMOVE PRINTF STATEMENT BEFORE SUBMISSION
        printf("%s done \n", v[0]);
        break;
      }
    }                           /* switch */
  }                             /* while */
}                               /* main */
