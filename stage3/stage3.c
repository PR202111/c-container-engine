#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <sched.h>
#include <sys/wait.h>
#include <sys/mount.h>
#include <sys/stat.h>   // Required for mkdir()
#include <unistd.h>

#define STACK_SIZE (1024 * 1024)

// This function remains unchanged from Stage 2
int container_main(void *arg) {
    printf("[Container] Inside the container namespaces!\n");
    sethostname("c-container-demo", 16);

    if (chroot("./rootfs") != 0) {
        perror("[Container] chroot failed");
        return 1;
    }
    if (chdir("/") != 0) {
        perror("[Container] chdir failed");
        return 1;
    }

    if (mount("proc", "/proc", "proc", 0, NULL) != 0) {
        perror("[Container] Mounting /proc failed");
    }

    char *container_args[] = {"/bin/sh", NULL};
    printf("[Container] Launching Alpine Sh shell...\n\n");
    execvp(container_args[0], container_args);

    perror("[Container] execvp failed");
    return 1;
}

// Helper function to write text strings into Cgroup control files
void write_cgroup_file(const char *path, const char *value) {
    FILE *fp = fopen(path, "w");
    if (!fp) {
        perror("Failed to open cgroup file");
        exit(1);
    }
    if (fprintf(fp, "%s", value) < 0) {
        perror("Failed to write to cgroup file");
        fclose(fp);
        exit(1);
    }
    fclose(fp);
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

    printf("[Host] Container created. Host-side PID: %d\n", container_pid);

    /* --- STAGE 3: APPLY CGROUP RESOURCE LIMITS --- */
    printf("[Host] Configuring Cgroups resource limits...\n");

    // 1. Create a cgroup directory for our container
    const char *cgroup_dir = "/sys/fs/cgroup/my_container";
    mkdir(cgroup_dir, 0755);

    // 2. Limit memory usage to 50 Megabytes
    char memory_max_path[256];
    snprintf(memory_max_path, sizeof(memory_max_path), "%s/memory.max", cgroup_dir);
    write_cgroup_file(memory_max_path, "50M");

    char pids_max_path[256];
    snprintf(pids_max_path, sizeof(pids_max_path), "%s/pids.max", cgroup_dir);
    write_cgroup_file(pids_max_path, "4");

    // 3. Move the container process ID into this target cgroup
    char cgroup_procs_path[256];
    snprintf(cgroup_procs_path, sizeof(cgroup_procs_path), "%s/cgroup.procs", cgroup_dir);
    
    char pid_str[32];
    snprintf(pid_str, sizeof(pid_str), "%d", container_pid);
    write_cgroup_file(cgroup_procs_path, pid_str);

    printf("[Host] Cgroup limits active (Max Memory: 50MB).\n");
    printf("[Host] Cgroup limits active (Max Process: 4).\n");
    

    waitpid(container_pid, NULL, 0);

    // Cleanup: Clean up the directory after the container terminates
    rmdir(cgroup_dir);

    free(stack);
    printf("[Host] Container has closed.\n");
    return 0;
}
// Challenge: Can you limit cpu also and test with a fork bomb and observer in host system ?