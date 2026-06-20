#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <sched.h>
#include <sys/wait.h>
#include <unistd.h>
#include <string.h>

#define STACK_SIZE (1024 * 1024)


int container_main(void *arg) {
    printf("[Container] Inside the container namespaces!\n");
    printf("[Container] My perspective PID: %d\n", getpid()); // Ques: what will be the output from the getpid() ?

    if (sethostname("c-container-demo", 16) != 0) {   // Ques: can we setup hostname without passing the "CLONE_NEWUTS" ?
        perror("[Container] Failed to set hostname");
    }

    char *container_args[] = {"/bin/bash", NULL};

    printf("[Container] Launching Bash shell...\n\n");
    
    execvp(container_args[0], container_args);

    perror("[Container] execvp failed to launch bash");

    return 1;
}

int main() {
    printf("[Host] Parent process started (PID: %d)\n", getpid());

    char *stack = malloc(STACK_SIZE);
    if (!stack) {
        perror("[Host] Could not allocate memory for container stack");
        exit(1);
    }

    printf("[Host] Cloning process into isolated namespaces...\n");

    int container_pid = clone(container_main, stack + STACK_SIZE, // Ques: Why we didnt pass then stack as normal stack and why we need the stack
        CLONE_NEWPID | CLONE_NEWUTS | SIGCHLD, NULL);

    if (container_pid == -1) {
        perror("[Host] Clone system call failed");
        free(stack);
        exit(1);
    }

    printf("[Host] Container process created with Host-side PID: %d\n", container_pid);

    waitpid(container_pid, NULL, 0);

    free(stack);
    printf("[Host] Container has closed. Exiting smoothly.\n");

    return 0;
}