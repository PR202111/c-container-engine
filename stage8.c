#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sched.h>
#include <sys/wait.h>
#include <sys/mount.h>
#include <sys/stat.h>   
#include <sys/utsname.h>
#include <unistd.h>

#define STACK_SIZE (1024 * 1024)
#define CONFIG_FILE "/etc/mydocker.conf"

int sync_pipe[2];
char active_rootfs_path[512];

typedef struct {
    char memory_limit[32];
    char pids_limit[32];
    char dns_server[64];
} ContainerConfig;

ContainerConfig current_cfg = { .memory_limit = "50M", .pids_limit = "4", .dns_server = "8.8.8.8" };

void write_file(const char *path, const char *value) {
    FILE *fp = fopen(path, "w");
    if (!fp) { perror("Failed to open file"); exit(1); }
    if (fprintf(fp, "%s", value) < 0) { perror("Failed to write"); fclose(fp); exit(1); }
    fclose(fp);
}

void get_base_dir(char *buf, size_t len) {
    FILE *fp = fopen(CONFIG_FILE, "r");
    if (!fp) {
        strncpy(buf, "/var/lib/my_docker", len);
        return;
    }
    if (fgets(buf, len, fp)) {
        buf[strcspn(buf, "\n")] = 0;
    }
    fclose(fp);
}

int container_main(void *arg) {
    char ch;
    close(sync_pipe[1]); 
    if (read(sync_pipe[0], &ch, 1) != 1) return 1;
    close(sync_pipe[0]);

    printf("[Container] Inside the container namespaces!\n");
    sethostname("c-container-demo", 16);

    if (chroot(active_rootfs_path) != 0) { perror("chroot failed"); return 1; }
    if (chdir("/") != 0) { perror("chdir failed"); return 1; }
    
    mount("proc", "/proc", "proc", 0, NULL);

    // CRITICAL FIX: The container script no longer executes internal "ip" commands!
    // The host configures everything from the outside while this child process stays suspended.

    char *container_args[] = {"/bin/bash", NULL};
    execvp(container_args[0], container_args);

    char *fallback_args[] = {"/bin/sh", NULL};
    printf("[Container] Bash not found, falling back to Sh shell...\n\n");
    execvp(fallback_args[0], fallback_args);
    return 1;
}

void run_container_engine(ContainerConfig *cfg) {
    pipe(sync_pipe);
    char *stack = malloc(STACK_SIZE);
    if (!stack) { perror("Stack allocation failed"); exit(1); }

    int container_pid = clone(
        container_main, 
        stack + STACK_SIZE, 
        CLONE_NEWPID | CLONE_NEWUTS | CLONE_NEWNS | CLONE_NEWNET | SIGCHLD, 
        NULL
    );
    if (container_pid == -1) { perror("Clone failed"); free(stack); exit(1); }
    close(sync_pipe[0]); 

    // Cgroups setup
    const char *cgroup_dir = "/sys/fs/cgroup/my_container";
    mkdir(cgroup_dir, 0755);
    char path[256];
    snprintf(path, sizeof(path), "%s/memory.max", cgroup_dir);  write_file(path, cfg->memory_limit);
    snprintf(path, sizeof(path), "%s/pids.max", cgroup_dir);    write_file(path, cfg->pids_limit);
    snprintf(path, sizeof(path), "%s/cgroup.procs", cgroup_dir);
    char pid_str[32]; snprintf(pid_str, sizeof(pid_str), "%d", container_pid);
    write_file(path, pid_str);

    // Host Side Network Plumbing
// Host Side Network Plumbing
    char cmd[512];
    system("ip link add veth_host type veth peer name veth_child");
    snprintf(cmd, sizeof(cmd), "ip link set veth_child netns %d", container_pid);
    system(cmd);

    /* === FIXED NETWORK INJECTION USING NSENTER === */
    // Explicitly enter the container's network namespace using its PID and force settings up
    snprintf(cmd, sizeof(cmd), "sudo nsenter -t %d -n ip addr add 10.0.0.2/24 dev veth_child", container_pid);
    system(cmd);
    snprintf(cmd, sizeof(cmd), "sudo nsenter -t %d -n ip link set veth_child up", container_pid);
    system(cmd);
    snprintf(cmd, sizeof(cmd), "sudo nsenter -t %d -n ip link set lo up", container_pid);
    system(cmd);
    snprintf(cmd, sizeof(cmd), "sudo nsenter -t %d -n ip route add default via 10.0.0.1", container_pid);
    system(cmd);
    /* ============================================= */

    system("ip addr add 10.0.0.1/24 dev veth_host");
    system("ip link set veth_host up");
    system("sysctl -w net.ipv4.ip_forward=1 > /dev/null");
    system("iptables -t nat -A POSTROUTING -s 10.0.0.0/24 -j MASQUERADE");

    write(sync_pipe[1], "O", 1);
    close(sync_pipe[1]); 

    waitpid(container_pid, NULL, 0);
    
    system("ip link del veth_host 2>/dev/null");
    rmdir(cgroup_dir);
    free(stack);
}

void print_help() {
    printf("\n============== 🐳 MyDocker CLI Engine Help ==============\n");
    printf("Usage:\n  sudo ./engine <command> [arguments]\n\n");
    printf("Core Commands:\n");
    printf("  %-15s Downloads the base rootfs template image.\n", "init");
    printf("  %-15s Spawns a container instantly using an OverlayFS Mount.\n", "spin_up <name>");
    printf("  %-15s Restores structural persistence mount frameworks.\n", "restart <name>");
    printf("  %-15s Saves only the container change delta (upper layer).\n", "save <name> <dir>");
    printf("  %-15s Unmounts the overlay layer and completely removes storage.\n", "delete <name>");
    printf("  %-15s Purges all configurations and data roots from host storage.\n", "uninstall");
    printf("=========================================================\n\n");
}

int main(int argc, char *argv[]) {
    if (argc < 2 || strcmp(argv[1], "-h") == 0 || strcmp(argv[1], "--help") == 0 || strcmp(argv[1], "help") == 0) {
        print_help();
        return 0;
    }

    char *command = argv[1];
    char base_dir[256];
    char cmd_buf[2048];

    if (strcmp(command, "init") == 0) {
        printf("=== MyDocker Initialization Wizard ===\n");
        printf("Enter the absolute path for base storage directory [default: /var/lib/my_docker]: ");
        char input[256];
        if (fgets(input, sizeof(input), stdin) == NULL) return 1;
        input[strcspn(input, "\n")] = 0;
        strcpy(base_dir, strlen(input) == 0 ? "/var/lib/my_docker" : input);

        printf("\nAvailable Base Distro Images:\n");
        printf("  1) Alpine Linux (Ultra-lightweight ~5MB)\n");
        printf("  2) Ubuntu Base  (Full glibc compatibility ~30MB)\n");
        printf("Select image target option [1-2, default: 1]: ");
        char choice[16];
        if (fgets(choice, sizeof(choice), stdin) == NULL) return 1;
        choice[strcspn(choice, "\n")] = 0;
        int distro_selection = (strlen(choice) == 0) ? 1 : atoi(choice);

        snprintf(cmd_buf, sizeof(cmd_buf), "mkdir -p /etc"); system(cmd_buf);
        write_file(CONFIG_FILE, base_dir);
        
        snprintf(cmd_buf, sizeof(cmd_buf), "mkdir -p %s/containers %s/base_rootfs", base_dir, base_dir);
        system(cmd_buf);

        struct utsname sysinfo;
        uname(&sysinfo);
        printf("\n[Host] Detected CPU Architecture: %s\n", sysinfo.machine);

        char url[1024];
        memset(url, 0, sizeof(url));

        if (distro_selection == 1) { 
            printf("[Host] Image Selected: Alpine Linux\n");
            if (strcmp(sysinfo.machine, "x86_64") == 0) {
                strcpy(url, "https://dl-cdn.alpinelinux.org/alpine/v3.20/releases/x86_64/alpine-minirootfs-3.20.0-x86_64.tar.gz");
            } else if (strcmp(sysinfo.machine, "aarch64") == 0) {
                strcpy(url, "https://dl-cdn.alpinelinux.org/alpine/v3.20/releases/aarch64/alpine-minirootfs-3.20.0-aarch64.tar.gz");
            }
        } else if (distro_selection == 2) { 
            printf("[Host] Image Selected: Ubuntu Base\n");
            if (strcmp(sysinfo.machine, "x86_64") == 0) {
                strcpy(url, "https://cdimage.ubuntu.com/ubuntu-base/releases/noble/release/ubuntu-base-24.04.4-base-amd64.tar.gz");
            } else if (strcmp(sysinfo.machine, "aarch64") == 0) {
                strcpy(url, "https://cdimage.ubuntu.com/ubuntu-base/releases/noble/release/ubuntu-base-24.04.4-base-arm64.tar.gz");
            }
        }

        if (strlen(url) == 0) {
            printf("[Error] Unsupported architecture matrix layout.\n");
            return 1;
        }

        snprintf(cmd_buf, sizeof(cmd_buf), "rm -rf %s/base_rootfs/*", base_dir); system(cmd_buf);

        printf("[Host] Fetching layer tarball from remote mirror registry...\n");
        snprintf(cmd_buf, sizeof(cmd_buf), "wget -q --show-progress \"%s\" -O /tmp/container_base.tar.gz", url); 
        if (system(cmd_buf) != 0) {
            printf("[Error] Download failed.\n");
            return 1;
        }

        printf("[Host] Extracting rootfs image layer blueprints onto storage paths...\n");
        snprintf(cmd_buf, sizeof(cmd_buf), "tar -xf /tmp/container_base.tar.gz -C %s/base_rootfs/", base_dir); 
        system(cmd_buf);
        
        printf("[Host] Initialization complete! Selected operating system environment is ready.\n");
        return 0;
    }

    get_base_dir(base_dir, sizeof(base_dir));

    if (strcmp(command, "spin_up") == 0) {
        if (argc < 3) { printf("[Error] Name parameter required.\n"); return 1; }
        char *name = argv[2];

        char container_path[512];
        snprintf(container_path, sizeof(container_path), "%s/containers/%s", base_dir, name);
        struct stat st;
        if (stat(container_path, &st) == 0) {
            printf("[Error] The container '%s' already exists! Use 'restart' to run it or 'delete' to clear it.\n", name);
            return 1;
        }

        for (int i = 3; i < argc; i++) {
            if (strcmp(argv[i], "--memory") == 0 && i + 1 < argc) strncpy(current_cfg.memory_limit, argv[++i], 32);
            else if (strcmp(argv[i], "--pids") == 0 && i + 1 < argc) strncpy(current_cfg.pids_limit, argv[++i], 32);
            else if (strcmp(argv[i], "--dns") == 0 && i + 1 < argc) strncpy(current_cfg.dns_server, argv[++i], 64);
        }

        printf("\n================ 🚀 Container Launchpad (OverlayFS Active) ================\n");
        snprintf(active_rootfs_path, sizeof(active_rootfs_path), "%s/containers/%s/rootfs", base_dir, name);
        
        // Ensure upper directory has a /dev track ready
        snprintf(cmd_buf, sizeof(cmd_buf), "mkdir -p %s/containers/%s/upper/dev %s/containers/%s/work %s", 
                 base_dir, name, base_dir, name, active_rootfs_path);
        system(cmd_buf);
        
        /* === FIX 1: AUTOMATE MKNOD IN UPPER LAYER BEFORE MOUNT === */
        snprintf(cmd_buf, sizeof(cmd_buf), "sudo mknod -m 666 %s/containers/%s/upper/dev/null c 1 3 2>/dev/null || true", base_dir, name);
        system(cmd_buf);
        /* ========================================================= */

        printf("[Host] Stacking filesystems via core kernel Union-Mounting...\n");
        snprintf(cmd_buf, sizeof(cmd_buf), 
                 "sudo mount -t overlay overlay -o lowerdir=%s/base_rootfs,upperdir=%s/containers/%s/upper,workdir=%s/containers/%s/work %s",
                 base_dir, base_dir, name, base_dir, name, active_rootfs_path);
        system(cmd_buf);

        /* --- ADDED: BIND MOUNT HOST DEV NODES --- */
        snprintf(cmd_buf, sizeof(cmd_buf), "mkdir -p %s/dev && sudo mount --bind /dev %s/dev", active_rootfs_path, active_rootfs_path);
        system(cmd_buf);

        char dns_path[1024]; snprintf(dns_path, sizeof(dns_path), "%s/containers/%s/upper/etc", base_dir, name);
        snprintf(cmd_buf, sizeof(cmd_buf), "mkdir -p %s", dns_path); system(cmd_buf);
        snprintf(dns_path, sizeof(dns_path), "%s/containers/%s/upper/etc/resolv.conf", base_dir, name);
        char dns_buf[128]; snprintf(dns_buf, sizeof(dns_buf), "nameserver %s\n", current_cfg.dns_server);
        write_file(dns_path, dns_buf);

        run_container_engine(&current_cfg);

        /* --- ADDED: UNMOUNT DEV ON SHUTDOWN --- */
        snprintf(cmd_buf, sizeof(cmd_buf), "sudo umount -l %s/dev 2>/dev/null", active_rootfs_path);
        system(cmd_buf);
        /* -------------------------------------- */

        snprintf(cmd_buf, sizeof(cmd_buf), "sudo umount -l %s 2>/dev/null", active_rootfs_path);
        system(cmd_buf);
    } 
    else if (strcmp(command, "restart") == 0) {
        if (argc < 3) { printf("Error: Name required.\n"); return 1; }
        char *name = argv[2];
        snprintf(active_rootfs_path, sizeof(active_rootfs_path), "%s/containers/%s/rootfs", base_dir, name);

        struct stat st;
        if (stat(active_rootfs_path, &st) != 0) {
            printf("[Error] Container '%s' does not exist.\n", name);
            return 1;
        }

        snprintf(cmd_buf, sizeof(cmd_buf), 
                 "sudo mount -t overlay overlay -o lowerdir=%s/base_rootfs,upperdir=%s/containers/%s/upper,workdir=%s/containers/%s/work %s 2>/dev/null",
                 base_dir, base_dir, name, base_dir, name, active_rootfs_path);
        system(cmd_buf);

        /* --- ADDED: BIND MOUNT HOST DEV NODES --- */
        snprintf(cmd_buf, sizeof(cmd_buf), "mkdir -p %s/dev && sudo mount --bind /dev %s/dev", active_rootfs_path, active_rootfs_path);
        system(cmd_buf);
        /* ---------------------------------------- */

        run_container_engine(&current_cfg);

        /* --- ADDED: UNMOUNT DEV ON SHUTDOWN --- */
        snprintf(cmd_buf, sizeof(cmd_buf), "sudo umount -l %s/dev 2>/dev/null", active_rootfs_path);
        system(cmd_buf);
        /* -------------------------------------- */

        snprintf(cmd_buf, sizeof(cmd_buf), "sudo umount -l %s 2>/dev/null", active_rootfs_path);
        system(cmd_buf);
    } 
    else if (strcmp(command, "save") == 0) {
        if (argc < 4) { printf("Usage: sudo ./docker save <name> <backup_dir>\n"); return 1; }
        char *name = argv[2]; char *dest = argv[3];
        
        printf("[Host] Archiving container change layers (Deltas only!)...\n");
        snprintf(cmd_buf, sizeof(cmd_buf), "mkdir -p %s && cp -r %s/containers/%s/upper/* %s/", dest, base_dir, name, dest);
        system(cmd_buf);
        printf("[Host] Delta changes captured successfully at %s\n", dest);
    } 
    else if (strcmp(command, "delete") == 0) {
        if (argc < 3) { printf("Error: Name required.\n"); return 1; }
        char *name = argv[2];
        
        snprintf(cmd_buf, sizeof(cmd_buf), "sudo umount -l %s/containers/%s/rootfs 2>/dev/null", base_dir, name); system(cmd_buf);
        snprintf(cmd_buf, sizeof(cmd_buf), "rm -rf %s/containers/%s", base_dir, name); system(cmd_buf);
        printf("[Host] Container allocation scrubbed from disk.\n");
    } 
    else if (strcmp(command, "uninstall") == 0) {
        printf("[Host] Beginning complete uninstallation layout scrub...\n");
        snprintf(cmd_buf, sizeof(cmd_buf), "sudo umount -l %s/containers/*/rootfs 2>/dev/null", base_dir); system(cmd_buf);
        printf("[Host] Removing container ecosystem base root directory: %s\n", base_dir);
        snprintf(cmd_buf, sizeof(cmd_buf), "rm -rf %s", base_dir); system(cmd_buf);
        printf("[Host] Removing engine configuration registries...\n");
        unlink(CONFIG_FILE);
        printf("[Host] Uninstallation successful! System is completely clean.\n");
    }
    else {
        printf("Unknown command: %s\n", command);
    }
    return 0;
}