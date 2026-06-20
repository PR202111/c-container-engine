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

// Runtime settings passed dynamically from CLI flags
typedef struct {
    char memory_limit[32];
    char pids_limit[32];
    char dns_server[64];
} ContainerConfig;

ContainerConfig current_cfg = { .memory_limit = "50M", .pids_limit = "4", .dns_server = "8.8.8.8" };

// Helper function to write text strings into system files
void write_file(const char *path, const char *value) {
    FILE *fp = fopen(path, "w");
    if (!fp) { perror("Failed to open file"); exit(1); }
    if (fprintf(fp, "%s", value) < 0) { perror("Failed to write"); fclose(fp); exit(1); }
    fclose(fp);
}

// Reads global storage path from /etc/mydocker.conf
void get_base_dir(char *buf, size_t len) {
    FILE *fp = fopen(CONFIG_FILE, "r");
    if (!fp) {
        // Fallback default if user skipped init
        strncpy(buf, "/var/lib/my_docker", len);
        return;
    }
    if (fgets(buf, len, fp)) {
        buf[strcspn(buf, "\n")] = 0; // Strip trailing newline
    }
    fclose(fp);
}

// Target entry point for the sandboxed execution domain
int container_main(void *arg) {
    char ch;
    close(sync_pipe[1]); // Close write end in child
    if (read(sync_pipe[0], &ch, 1) != 1) return 1;
    close(sync_pipe[0]);

    printf("[Container] Inside the container namespaces!\n");
    sethostname("c-container-demo", 16);

    // Jail the filesystem layout
    if (chroot(active_rootfs_path) != 0) { perror("chroot failed"); return 1; }
    if (chdir("/") != 0) { perror("chdir failed"); return 1; }
    
    // Mount isolated process space
    mount("proc", "/proc", "proc", 0, NULL);

    // Inside Network Config (BusyBox/Alpine Compliant)
    system("ip link set lo up");
    system("ip addr add 10.0.0.2/24 dev veth_child");
    system("ip link set veth_child up");
    system("ip route add default dev veth_child"); // Pure raw routing mapping targeting host gateway

    char *container_args[] = {"/bin/sh", NULL};
    printf("[Container] Launching Alpine Sh shell...\n\n");
    execvp(container_args[0], container_args);
    return 1;
}

// Spawns the cloned kernel engine once the directory layout is set up on host
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
    close(sync_pipe[0]); // Close read end in parent

    // Cgroups setup using dynamic runtime configurations
    const char *cgroup_dir = "/sys/fs/cgroup/my_container";
    mkdir(cgroup_dir, 0755);
    char path[256];
    snprintf(path, sizeof(path), "%s/memory.max", cgroup_dir);  write_file(path, cfg->memory_limit);
    snprintf(path, sizeof(path), "%s/pids.max", cgroup_dir);    write_file(path, cfg->pids_limit);
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

    // Release the child execution trap
    write(sync_pipe[1], "O", 1);
    close(sync_pipe[1]); 

    waitpid(container_pid, NULL, 0);
    
    // Clean up infrastructure states gracefully on exit
    system("ip link del veth_host 2>/dev/null");
    rmdir(cgroup_dir);
    free(stack);
}

// Renders the self-documenting CLI instruction board
void print_help() {
    printf("\n============== 🐳 MyDocker CLI Engine Help ==============\n");
    printf("A lightweight container virtualization runtime built from scratch.\n\n");
    printf("Usage:\n");
    printf("  sudo ./engine <command> [arguments]\n\n");
    printf("Core Commands:\n");
    printf("  %-15s Downloads and extracts the template rootfs layout.\n", "init");
    printf("  %-15s Spawns a brand new isolated sandbox shell instance.\n", "spin_up <name>");
    printf("  %-15s Boots an existing persistent sandbox right where you left off.\n", "restart <name>");
    printf("  %-15s Packages the entire container filesystem layer into a directory.\n", "save <name> <dir>");
    printf("  %-15s Unmounts active channels and completely wipes the container.\n", "delete <name>");
    
    printf("\nOptional Customization Flags (Passed to spin_up):\n");
    printf("  %-20s Enforces a hard physical RAM barrier (e.g., 30M, 1G). [Default: 50M]\n", "--memory <limit>");
    printf("  %-20s Enforces a max concurrent process limit to block forks. [Default: 4]\n", "--pids <limit>");
    printf("  %-20s Injects a custom network DNS resolver entry. [Default: 8.8.8.8]\n", "--dns <ip>");
    
    printf("\nExamples:\n");
    printf("  sudo ./engine init\n");
    printf("  sudo ./engine spin_up sandbox_shell --memory 15M --pids 2 --dns 1.1.1.1\n");
    printf("=========================================================\n\n");
}

int main(int argc, char *argv[]) {
    // Intercept help assertions smoothly
    if (argc < 2 || strcmp(argv[1], "-h") == 0 || strcmp(argv[1], "--help") == 0 || strcmp(argv[1], "help") == 0) {
        print_help();
        return 0;
    }

    char *command = argv[1];
    char base_dir[256];
    char cmd_buf[2048];

    // INITIALIZATION STAGE
    if (strcmp(command, "init") == 0) {
        printf("=== MyDocker Initialization Wizard ===\n");
        printf("Enter the absolute path for base storage directory [default: /var/lib/my_docker]: ");
        char input[256];
        if (fgets(input, sizeof(input), stdin) == NULL) return 1;
        input[strcspn(input, "\n")] = 0;

        if (strlen(input) == 0) {
            strcpy(base_dir, "/var/lib/my_docker");
        } else {
            strcpy(base_dir, input);
        }

        // Save selection to configuration configuration layout
        snprintf(cmd_buf, sizeof(cmd_buf), "mkdir -p /etc");
        system(cmd_buf);
        write_file(CONFIG_FILE, base_dir);
        
        // Setup host tracking ecosystem file path branches
        snprintf(cmd_buf, sizeof(cmd_buf), "mkdir -p %s/containers %s/base_rootfs", base_dir, base_dir);
        system(cmd_buf);

        // Auto-detect instruction set layout targets
        struct utsname sysinfo;
        uname(&sysinfo);
        printf("[Host] Detected CPU Architecture: %s\n", sysinfo.machine);

        char url[512];
        if (strcmp(sysinfo.machine, "x86_64") == 0) {
            strcpy(url, "https://dl-cdn.alpinelinux.org/alpine/v3.20/releases/x86_64/alpine-minirootfs-3.20.0-x86_64.tar.gz");
        } else if (strcmp(sysinfo.machine, "aarch64") == 0) {
            strcpy(url, "https://dl-cdn.alpinelinux.org/alpine/v3.20/releases/aarch64/alpine-minirootfs-3.20.0-aarch64.tar.gz");
        } else {
            printf("[Error] Unsupported environment target architecture.\n");
            return 1;
        }

        printf("[Host] Downloading official matching Alpine core rootfs layers...\n");
        snprintf(cmd_buf, sizeof(cmd_buf), "wget -q --show-progress %s -O /tmp/alpine.tar.gz", url);
        system(cmd_buf);

        printf("[Host] Extracting into clean template storage zone...\n");
        snprintf(cmd_buf, sizeof(cmd_buf), "tar -xf /tmp/alpine.tar.gz -C %s/base_rootfs/", base_dir);
        system(cmd_buf);
        
        printf("[Host] Initialization successful! Core environments ready.\n");
        return 0;
    }

    // Pull configuration details from global store
    get_base_dir(base_dir, sizeof(base_dir));

    // SPIN UP STAGE
    if (strcmp(command, "spin_up") == 0) {
        if (argc < 3) { 
            printf("[Error] Missing container name parameter. Type './engine help' for usage.\n"); 
            return 1; 
        }
        char *name = argv[2];

        // Parse runtime custom override flags dynamically
        for (int i = 3; i < argc; i++) {
            if (strcmp(argv[i], "--memory") == 0 && i + 1 < argc) {
                strncpy(current_cfg.memory_limit, argv[++i], sizeof(current_cfg.memory_limit));
            } else if (strcmp(argv[i], "--pids") == 0 && i + 1 < argc) {
                strncpy(current_cfg.pids_limit, argv[++i], sizeof(current_cfg.pids_limit));
            } else if (strcmp(argv[i], "--dns") == 0 && i + 1 < argc) {
                strncpy(current_cfg.dns_server, argv[++i], sizeof(current_cfg.dns_server));
            }
        }

        printf("\n================ 🚀 Container Launchpad ================\n");
        printf("  Target Identity : %s\n", name);
        printf("  Memory Ceiling  : %s\n", current_cfg.memory_limit);
        printf("  Process Slot Limit: %s processes\n", current_cfg.pids_limit);
        printf("  DNS Nameserver  : %s\n", current_cfg.dns_server);
        printf("========================================================\n\n");
        
        snprintf(active_rootfs_path, sizeof(active_rootfs_path), "%s/containers/%s/rootfs", base_dir, name);
        snprintf(cmd_buf, sizeof(cmd_buf), "mkdir -p %s", active_rootfs_path);
        system(cmd_buf);
        
        printf("[Host] Instantiating isolated rootfs via rsync layers...\n");
        snprintf(cmd_buf, sizeof(cmd_buf), "rsync -a --exclude='/proc' --exclude='/sys' %s/base_rootfs/ %s/", base_dir, active_rootfs_path);
        system(cmd_buf);

        snprintf(cmd_buf, sizeof(cmd_buf), "mkdir -p %s/proc %s/sys", active_rootfs_path, active_rootfs_path);
        system(cmd_buf);

        char dns_path[1024];
        snprintf(dns_path, sizeof(dns_path), "%s/etc/resolv.conf", active_rootfs_path);
        char dns_buf[128];
        snprintf(dns_buf, sizeof(dns_buf), "nameserver %s\n", current_cfg.dns_server);
        write_file(dns_path, dns_buf);

        run_container_engine(&current_cfg);
    } 
    // RESTART STAGE
    else if (strcmp(command, "restart") == 0) {
        if (argc < 3) { printf("Error: Name required.\n"); return 1; }
        char *name = argv[2];
        snprintf(active_rootfs_path, sizeof(active_rootfs_path), "%s/containers/%s/rootfs", base_dir, name);

        struct stat st;
        if (stat(active_rootfs_path, &st) != 0) {
            printf("[Error] Container '%s' does not exist.\n", name);
            return 1;
        }
        
        printf("[Host] Booting up persistent container context: %s...\n", name);
        run_container_engine(&current_cfg);
    } 
    // SAVE STAGE
    else if (strcmp(command, "save") == 0) {
        if (argc < 4) { printf("Usage: sudo ./docker save <name> <backup_dir>\n"); return 1; }
        char *name = argv[2];
        char *dest = argv[3];
        
        snprintf(cmd_buf, sizeof(cmd_buf), "mkdir -p %s", dest);
        system(cmd_buf);
        snprintf(cmd_buf, sizeof(cmd_buf), "cp -r %s/containers/%s/rootfs/* %s/", base_dir, name, dest);
        system(cmd_buf);
        printf("[Host] Saved successfully to %s\n", dest);
    } 
    // DELETE STAGE
    else if (strcmp(command, "delete") == 0) {
        if (argc < 3) { printf("Error: Name required.\n"); return 1; }
        char *name = argv[2];
        printf("[Host] Deleting container sandbox environment layers for: %s\n", name);
        
        snprintf(cmd_buf, sizeof(cmd_buf), "sudo umount -l %s/containers/%s/rootfs/proc 2>/dev/null", base_dir, name);
        system(cmd_buf);
        snprintf(cmd_buf, sizeof(cmd_buf), "rm -rf %s/containers/%s", base_dir, name);
        system(cmd_buf);
        printf("[Host] Deleted successfully.\n");
    } 
    else {
        printf("[Error] Unknown command: '%s'. Type './engine help' for a list of valid controls.\n", command);
    }
    return 0;
}