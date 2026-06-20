#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <sched.h>
#include <sys/wait.h>
#include <sys/mount.h>
#include <unistd.h>

#define STACK_SIZE (1024 * 1024)

int container_main(void *arg) {
    printf("[Container] Inside the container namespaces!\n");

    sethostname("c-container-demo", 16);

    if (chroot("./rootfs") != 0) {
        perror("[Container] chroot failed");
        return 1;
    }
    
    if (chdir("/") != 0) {  // Ques: Can we change the argument of chdir to somehting else like "/home" or something that doesnt exist ?
        perror("[Container] chdir failed");
        return 1;
    }

    if (mount("proc", "/proc", "proc", 0, NULL) != 0) { // Ques: Why we only Mount the proc not all ?
        perror("[Container] Mounting /proc failed");
    }

    // Alpine uses /bin/sh by default instead of /bin/bash
    char *container_args[] = {"/bin/sh", NULL};

    printf("[Container] Launching Alpine Sh shell...\n\n");
    execvp(container_args[0], container_args);

    perror("[Container] execvp failed");
    return 1;
}

int main() {
    char *stack = malloc(STACK_SIZE);
    if (!stack) {
        perror("[Host] Stack allocation failed");
        exit(1);
    }

    printf("[Host] Cloning process with Mount, PID, and UTS isolation...\n");

    int container_pid = clone(
        container_main, 
        stack + STACK_SIZE, 
        CLONE_NEWPID | CLONE_NEWUTS | CLONE_NEWNS | SIGCHLD, 
        NULL
    );

    if (container_pid == -1) {
        perror("[Host] Clone failed");
        free(stack);
        exit(1);
    }

    waitpid(container_pid, NULL, 0);

    free(stack);
    printf("[Host] Container has closed.\n");
    return 0;
}