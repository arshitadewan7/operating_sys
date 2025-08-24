#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <signal.h>

void handle_hup(int sig) {
    printf("Ouch!\n");
    fflush(stdout);
}

void handle_int(int sig) {
    printf("Yeah!\n");
    fflush(stdout);
}

int main(int argc, char *argv[]) {
    if (argc != 2) {
        fprintf(stderr, "Usage: %s <n>\n", argv[0]);
        exit(EXIT_FAILURE);
    }

    int n = atoi(argv[1]);

    signal(SIGHUP, handle_hup);
    signal(SIGINT, handle_int);

    for (int i = 0; i < n; i++) {
        printf("%d\n", i * 2);
        fflush(stdout);
        sleep(5);
    }

    return 0;
}
