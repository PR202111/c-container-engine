#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sched.h>
#include <sys/wait.h>
#include <sys/mount.h>
#include <sys/stat.h>   
#include <unistd.h>

#define STACK_SIZE (1024 * 1024)

int sync_pipe[2];
char active_rootfs_path[512]; // Dynamically holds the path to the current container's rootfs

void write_file(const char *path, const char *value) {
    FILE *fp = fopen(path, "w");
    if (!fp) { perror("Failed to open file"); exit(1); }
    if (fprintf(fp, "%s", value) < 0) { perror("Failed to write"); fclose(fp); exit(1); }
    fclose(fp);
}

int container_main(void *arg) {
    char ch;
    close(sync_pipe[1]); 

    if (read(sync_pipe[0], &ch, 1) != 1) return 1;
    close(sync_pipe[0]);

    printf("[Container] Inside the container namespaces!\n");
    sethostname("c-container-demo", 16);

    // JAIL THE FILESYSTEM: Point to the dynamically allocated rootfs path
    if (chroot(active_rootfs_path) != 0) { 
        perror("chroot failed"); 
        return 1; 
    }
    chdir("/");

    mount("proc", "/proc", "proc", 0, NULL);

    // Inside Network Config
    system("ip link set lo up");
    system("ip addr add 10.0.0.2/24 dev veth_child");
    system("ip link set veth_child up");
    system("ip route add default via 10.0.0.1");

    char *container_args[] = {"/bin/sh", NULL};
    printf("[Container] Launching Alpine Sh shell...\n\n");
    execvp(container_args[0], container_args);

    return 1;
}

// Spawns the cloned kernel engine once the directory layout is set up on host
void run_container_engine() {
    pipe(sync_pipe);
    char *stack = malloc(STACK_SIZE);

    int container_pid = clone(
        container_main, 
        stack + STACK_SIZE, 
        CLONE_NEWPID | CLONE_NEWUTS | CLONE_NEWNS | CLONE_NEWNET | SIGCHLD, 
        NULL
    );

    close(sync_pipe[0]); 

    // Cgroups setup
    const char *cgroup_dir = "/sys/fs/cgroup/my_container";
    mkdir(cgroup_dir, 0755);
    char path[256];
    snprintf(path, sizeof(path), "%s/memory.max", cgroup_dir);  write_file(path, "50M");
    snprintf(path, sizeof(path), "%s/pids.max", cgroup_dir);    write_file(path, "4");
    snprintf(path, sizeof(path), "%s/cgroup.procs", cgroup_dir);
    char pid_str[32]; snprintf(pid_str, sizeof(pid_str), "%d", container_pid);
    write_file(path, pid_str);

    // Host Side Network Plumbing
    char cmd[512];
    system("ip link add veth_host type veth peer name veth_child");
    snprintf(cmd, sizeof(cmd), "ip link set veth_child netns %d", container_pid);
    system(cmd);
    system("ip addr add 10.0.0.1/24 dev veth_host");
    system("ip link set veth_host up");
    system("sysctl -w net.ipv4.ip_forward=1 > /dev/null");
    system("iptables -t nat -A POSTROUTING -s 10.0.0.0/24 -j MASQUERADE");

    // Wake up child
    write(sync_pipe[1], "O", 1);
    close(sync_pipe[1]); 

    waitpid(container_pid, NULL, 0);

    // Clean network & cgroup state on exit
    // system("ip link del veth_host");
    system("ip link del veth_host 2>/dev/null");
    rmdir(cgroup_dir);
    free(stack);
}

int main(int argc, char *argv[]) {
    if (argc < 3) {
        printf("Usage:\n");
        printf("  sudo ./docker spin_up <name>\n");
        printf("  sudo ./docker restart <name>\n");
        printf("  sudo ./docker save <name> <backup_dir>\n");
        printf("  sudo ./docker delete <name>\n");
        return 1;
    }

    char *command = argv[1];
    char *name = argv[2];
    char cmd_buf[2048];

    // Ensure state base folder directories exist on host disk
    system("mkdir -p /tmp/my_docker/containers");

    if (strcmp(command, "spin_up") == 0) {
        printf("[Host] Spinning up a brand new container instance: %s\n", name);
        
        snprintf(active_rootfs_path, sizeof(active_rootfs_path), "/tmp/my_docker/containers/%s/rootfs", name);
        
        snprintf(cmd_buf, sizeof(cmd_buf), "mkdir -p %s", active_rootfs_path);
        system(cmd_buf);
        
        printf("[Host] Copying clean template image filesystem layers (Safe Copy)...\n");
        snprintf(cmd_buf, sizeof(cmd_buf), "rsync -a --exclude='/proc' --exclude='/sys' ./rootfs/ %s/", active_rootfs_path);
        system(cmd_buf);

        snprintf(cmd_buf, sizeof(cmd_buf), "mkdir -p %s/proc %s/sys", active_rootfs_path, active_rootfs_path);
        system(cmd_buf);

        /* --- ADDED: INJECT DNS RESOLVER --- */
        printf("[Host] Injecting network DNS resolver settings...\n");
        char dns_path[1024];
        snprintf(dns_path, sizeof(dns_path), "%s/etc/resolv.conf", active_rootfs_path);
        // Write standard public Google DNS configuration directly into the container's /etc space
        write_file(dns_path, "nameserver 8.8.8.8\n");
        /* ---------------------------------- */

        run_container_engine();
    }
    else if (strcmp(command, "restart") == 0) {
        printf("[Host] Restarting persistent container: %s\n", name);
        snprintf(active_rootfs_path, sizeof(active_rootfs_path), "/tmp/my_docker/containers/%s/rootfs", name);

        // Verify if it exists first
        struct stat st;
        if (stat(active_rootfs_path, &st) != 0) {
            printf("[Error] Container '%s' does not exist. Run spin_up first.\n", name);
            return 1;
        }

        run_container_engine();
    } 
    else if (strcmp(command, "save") == 0) {
        if (argc < 4) {
            printf("Usage: sudo ./docker save <name> <backup_destination_dir>\n");
            return 1;
        }
        char *dest = argv[3];
        printf("[Host] Freezing states. Committing container '%s' to archive target '%s'...\n", name, dest);
        
        system("mkdir -p /tmp/my_docker/backups");
        snprintf(cmd_buf, sizeof(cmd_buf), "cp -r /tmp/my_docker/containers/%s/rootfs %s", name, dest);
        system(cmd_buf);
        printf("[Host] Done! Saved changes successfully.\n");
    } 
    else if (strcmp(command, "delete") == 0) {
        printf("[Host] Erasing container sandbox layers for: %s\n", name);
        
        // Unmount stuck interior virtual proc channels gracefully if any remained attached
        snprintf(cmd_buf, sizeof(cmd_buf), "sudo umount -l /tmp/my_docker/containers/%s/rootfs/proc 2>/dev/null", name);
        system(cmd_buf);
        
        // Delete container storage space from disk completely
        snprintf(cmd_buf, sizeof(cmd_buf), "rm -rf /tmp/my_docker/containers/%s", name);
        system(cmd_buf);
        printf("[Host] Deleted successfully.\n");
    } 
    else {
        printf("Unknown command: %s\n", command);
    }

    return 0;
}