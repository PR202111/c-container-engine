#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <sched.h>
#include <sys/wait.h>
#include <sys/mount.h>
#include <sys/stat.h>   
#include <unistd.h>
#include <string.h>

#define STACK_SIZE (1024 * 1024)

int sync_pipe[2];

void write_file(const char *path, const char *value) {
    FILE *fp = fopen(path, "w");
    if (!fp) {
        fprintf(stderr, "[Host] FATAL: Failed to open file: %s\n", path);
        perror("[Host] Reason");
        exit(1);
    }
    if (fprintf(fp, "%s", value) < 0) {
        fprintf(stderr, "[Host] FATAL: Failed to write to file: %s\n", path);
        perror("[Host] Reason");
        fclose(fp);
        exit(1);
    }
    fclose(fp);
}

int container_main(void *arg) {
    char ch;
    close(sync_pipe[1]); 

    if (read(sync_pipe[0], &ch, 1) != 1) {
        perror("[Container] Failed to sync with parent");
        return 1;
    }
    close(sync_pipe[0]);

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

int main() {
    if (pipe(sync_pipe) < 0) {
        perror("Pipe creation failed");
        exit(1);
    }

    char *stack = malloc(STACK_SIZE);
    if (!stack) { exit(1); }

    printf("[Host] Cloning process with Mount, PID, User, and UTS isolation...\n");

    int container_pid = clone(
        container_main, 
        stack + STACK_SIZE, 
        CLONE_NEWPID | CLONE_NEWUTS | CLONE_NEWNS | CLONE_NEWUSER | SIGCHLD, 
        NULL
    );

    if (container_pid == -1) {
        perror("[Host] Clone failed");
        exit(1);
    }

    close(sync_pipe[0]); 


    const char *cgroup_dir = "/sys/fs/cgroup/my_container";
    mkdir(cgroup_dir, 0755); 

    char path_buf[256];
    snprintf(path_buf, sizeof(path_buf), "%s/memory.max", cgroup_dir);
    write_file(path_buf, "50M");

    snprintf(path_buf, sizeof(path_buf), "%s/pids.max", cgroup_dir);
    write_file(path_buf, "4");

    snprintf(path_buf, sizeof(path_buf), "%s/cgroup.procs", cgroup_dir);
    char pid_str[32];
    snprintf(pid_str, sizeof(pid_str), "%d", container_pid);
    write_file(path_buf, pid_str);

    /* --- USER NAMESPACE MAPPING --- */
    // UPGRADE: Grab the REAL user ID, bypassing the `sudo` trap
    int host_uid = getuid();
    int host_gid = getgid();
    
    if (getenv("SUDO_UID")) { host_uid = atoi(getenv("SUDO_UID")); }
    if (getenv("SUDO_GID")) { host_gid = atoi(getenv("SUDO_GID")); }

    printf("[Host] Mapping User Namespaces (Container UID 0 -> Host UID %d)...\n", host_uid);

    char map_path[256];
    char map_str[256];

    snprintf(map_path, sizeof(map_path), "/proc/%d/uid_map", container_pid);
    snprintf(map_str, sizeof(map_str), "0 %d 1\n", host_uid);
    write_file(map_path, map_str);

    // UPGRADE: Only attempt to write setgroups if the file physically exists
    snprintf(map_path, sizeof(map_path), "/proc/%d/setgroups", container_pid);
    if (access(map_path, F_OK) == 0) {
        write_file(map_path, "deny\n");
    } else {
        printf("[Host] Notice: /proc/[pid]/setgroups not found, skipping.\n");
    }

    snprintf(map_path, sizeof(map_path), "/proc/%d/gid_map", container_pid);
    snprintf(map_str, sizeof(map_str), "0 %d 1\n", host_gid);
    write_file(map_path, map_str);


    write(sync_pipe[1], "O", 1);
    close(sync_pipe[1]); 

    waitpid(container_pid, NULL, 0);

    rmdir(cgroup_dir);
    free(stack);
    printf("[Host] Container has closed.\n");
    return 0;
}