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
#include <dirent.h>
#include <errno.h>

#define STACK_SIZE (1024 * 1024)
#define CONFIG_FILE "/etc/mydocker.conf"

int sync_pipe[2];

typedef struct {
    char memory_limit[32];
    char pids_limit[32];
    char dns_server[64];
} ContainerConfig;

ContainerConfig current_cfg = { .memory_limit = "50M", .pids_limit = "4", .dns_server = "8.8.8.8" };

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

int allocate_next_ip_index(const char *base_dir, const char *container_name) {
    char state_path[512];
    snprintf(state_path, sizeof(state_path), "%s/ipam", base_dir);
    mkdir(state_path, 0755);

    snprintf(state_path, sizeof(state_path), "%s/ipam/%s.ip", base_dir, container_name);
    FILE *fp = fopen(state_path, "r");
    if (fp) {
        int assigned_index = 2;
        if (fscanf(fp, "%d", &assigned_index) == 1) {
            fclose(fp);
            return assigned_index;
        }
        fclose(fp);
    }

    for (int current_slot = 2; current_slot < 254; current_slot++) {
        int slot_taken = 0;
        char check_path[512];
        
        snprintf(check_path, sizeof(check_path), "%s/ipam/", base_dir);
        DIR *dir = opendir(check_path);
        if (dir) {
            struct dirent *entry;
            while ((entry = readdir(dir)) != NULL) {
                if (strstr(entry->d_name, ".ip")) {
                    char full_entry_path[1024];
                    snprintf(full_entry_path, sizeof(full_entry_path), "%s/ipam/%s", base_dir, entry->d_name);
                    FILE *cf = fopen(full_entry_path, "r");
                    if (cf) {
                        int read_val;
                        if (fscanf(cf, "%d", &read_val) == 1 && read_val == current_slot) {
                            slot_taken = 1;
                        }
                        fclose(cf);
                    }
                }
            }
            closedir(dir);
        }
        
        if (!slot_taken) {
            FILE *wf = fopen(state_path, "w");
            if (wf) {
                fprintf(wf, "%d", current_slot);
                fclose(wf);
            }
            return current_slot;
        }
    }
    return 2; 
}

void release_ip_index(const char *base_dir, const char *container_name) {
    char state_path[512];
    snprintf(state_path, sizeof(state_path), "%s/ipam/%s.ip", base_dir, container_name);
    unlink(state_path);
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
    mount("none", "/", NULL, MS_REC | MS_PRIVATE, NULL);
    sethostname("c-container-demo", 16);

    if (chroot(".") != 0) {
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
    execvp(container_args[0], container_args);

    char *fallback_args[] = {"/bin/bash", NULL};
    printf("[Container] Bash not found, falling back to Sh shell...\n\n");
    execvp(fallback_args[0], fallback_args);
    return 1;
}

void run_container_engine(ContainerConfig *cfg, const char *base_dir, const char *name) {
    if (pipe(sync_pipe) < 0) {
        perror("Pipe creation failed");
        exit(1);
    }
    char *stack = malloc(STACK_SIZE);
    if (!stack) { 
        perror("Stack allocation failed");
        exit(1); 
    }

    int container_pid = clone(
        container_main, 
        stack + STACK_SIZE, 
        CLONE_NEWUSER | CLONE_NEWPID | CLONE_NEWUTS | CLONE_NEWNS | CLONE_NEWNET | SIGCHLD, 
        NULL
    );
    if (container_pid == -1) { perror("Clone failed"); free(stack); exit(1); }
    close(sync_pipe[0]); 

    int ip_suffix = allocate_next_ip_index(base_dir, name);
    printf("[Host] Dynamic IP Assignment allocated: 10.0.0.%d\n", ip_suffix);

    
    char cgroup_dir[512];
    snprintf(cgroup_dir, sizeof(cgroup_dir), "/sys/fs/cgroup/my_container_%s", name);
    if (mkdir(cgroup_dir, 0755) != 0 && errno != EEXIST) {
        perror("[Host] Failed to build isolated cgroup sandbox");
        exit(1);
    }

    char path_buf[256];
    snprintf(path_buf, sizeof(path_buf), "%s/memory.max", cgroup_dir);
    write_file(path_buf, cfg->memory_limit);

    snprintf(path_buf, sizeof(path_buf), "%s/pids.max", cgroup_dir);
    write_file(path_buf, cfg->pids_limit);

    snprintf(path_buf, sizeof(path_buf), "%s/cgroup.procs", cgroup_dir);
    char pid_str[32];
    snprintf(pid_str, sizeof(pid_str), "%d", container_pid);
    write_file(path_buf, pid_str);

    int target_uid = 0; 
    int target_gid = 0;

    printf("[Host] Mapping User Namespaces (Container UID 0 -> Host UID %d)...\n", target_uid);

    char map_path[256];
    char map_str[256];
    snprintf(map_path, sizeof(map_path), "/proc/%d/uid_map", container_pid);
    snprintf(map_str, sizeof(map_str), "0 %d 1\n", target_uid);
    write_file(map_path, map_str);

    snprintf(map_path, sizeof(map_path), "/proc/%d/setgroups", container_pid);
    if (access(map_path, F_OK) == 0) {
        write_file(map_path, "deny\n");
    }

    snprintf(map_path, sizeof(map_path), "/proc/%d/gid_map", container_pid);
    snprintf(map_str, sizeof(map_str), "0 %d 1\n", target_gid);
    write_file(map_path, map_str);

    char cmd[512];
    system("sudo ip link add name br0 type bridge 2>/dev/null || true");
    system("sudo ip addr add 10.0.0.1/24 dev br0 2>/dev/null || true");
    system("sudo ip link set br0 up 2>/dev/null || true");

    snprintf(cmd, sizeof(cmd), "ip link add veth_h_%s type veth peer name veth_c_%s", name, name);system(cmd);
    snprintf(cmd, sizeof(cmd), "sudo ip link set veth_h_%s master br0", name);system(cmd);
    snprintf(cmd, sizeof(cmd), "sudo ip link set veth_h_%s up", name);system(cmd);

    snprintf(cmd, sizeof(cmd), "ip link set veth_c_%s netns %d", name, container_pid);system(cmd);
    
    snprintf(cmd, sizeof(cmd), "sudo nsenter -t %d -n ip addr add 10.0.0.%d/24 dev veth_c_%s", container_pid, ip_suffix, name);system(cmd);
    snprintf(cmd, sizeof(cmd), "sudo nsenter -t %d -n ip link set veth_c_%s name veth_child", container_pid, name);system(cmd);
    snprintf(cmd, sizeof(cmd), "sudo nsenter -t %d -n ip link set veth_child up", container_pid);system(cmd);
    snprintf(cmd, sizeof(cmd), "sudo nsenter -t %d -n ip link set lo up", container_pid);system(cmd);
    snprintf(cmd, sizeof(cmd), "sudo nsenter -t %d -n ip route add default via 10.0.0.1", container_pid);system(cmd);

    system("sysctl -w net.ipv4.ip_forward=1 > /dev/null");
    system("iptables -t nat -A POSTROUTING -s 10.0.0.0/24 -j MASQUERADE 2>/dev/null || true");

    write(sync_pipe[1], "O", 1);
    close(sync_pipe[1]); 

    waitpid(container_pid, NULL, 0);
    
    printf("[Host] Cleaning up network infrastructures for container '%s'...\n", name);
    snprintf(cmd, sizeof(cmd), "ip link set veth_h_%s down 2>/dev/null", name);
    system(cmd);
    snprintf(cmd, sizeof(cmd), "ip link del veth_h_%s 2>/dev/null", name);
    system(cmd);
    
    system("iptables -t nat -D POSTROUTING -s 10.0.0.0/24 -j MASQUERADE 2>/dev/null || true");

    release_ip_index(base_dir, name);

    if (rmdir(cgroup_dir) != 0) {
        perror("[Host] Warning: Failed to clean up cgroup directory");
    }
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

        snprintf(cmd_buf, sizeof(cmd_buf), "mkdir -p /etc"); system(cmd_buf);
        write_file(CONFIG_FILE, base_dir);
        
        snprintf(cmd_buf, sizeof(cmd_buf), "mkdir -p %s/containers %s/base_rootfs", base_dir, base_dir);
        system(cmd_buf);

        struct utsname sysinfo;
        uname(&sysinfo);
        printf("\n[Host] Detected CPU Architecture: %s\n", sysinfo.machine);

        char url[1024];
        memset(url, 0, sizeof(url));

        printf("[Host] Image : Alpine Base Layer\n");
        if (strcmp(sysinfo.machine, "x86_64") == 0) {
            // Official Alpine 3.20 x86_64 Mini RootFS
            strcpy(url, "https://dl-cdn.alpinelinux.org/alpine/v3.20/releases/x86_64/alpine-minirootfs-3.20.0-x86_64.tar.gz");
        } else if (strcmp(sysinfo.machine, "aarch64") == 0) {
            // Official Alpine 3.20 ARM64 Mini RootFS (For Raspberry Pi / Apple Silicon VMs)
            strcpy(url, "https://dl-cdn.alpinelinux.org/alpine/v3.20/releases/aarch64/alpine-minirootfs-3.20.0-aarch64.tar.gz");
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

        printf("\n================ Container Launchpad ================\n");
        

        char active_rootfs_path[512];
        snprintf(active_rootfs_path, sizeof(active_rootfs_path), "%s/containers/%s/rootfs", base_dir, name);
        
        snprintf(cmd_buf, sizeof(cmd_buf), "mkdir -p %s/containers/%s/upper/dev %s/containers/%s/work %s", 
                 base_dir, name, base_dir, name, active_rootfs_path);
        system(cmd_buf);
        
        snprintf(cmd_buf, sizeof(cmd_buf), "sudo mknod -m 666 %s/containers/%s/upper/dev/null c 1 3 2>/dev/null || true", base_dir, name);
        system(cmd_buf);

        printf("[Host] Stacking filesystems via core kernel Union-Mounting...\n");
        snprintf(cmd_buf, sizeof(cmd_buf), 
                 "sudo mount -t overlay overlay -o lowerdir=%s/base_rootfs,upperdir=%s/containers/%s/upper,workdir=%s/containers/%s/work %s",
                 base_dir, base_dir, name, base_dir, name, active_rootfs_path);
        system(cmd_buf);

        char dns_path[1024]; snprintf(dns_path, sizeof(dns_path), "%s/containers/%s/upper/etc", base_dir, name);
        snprintf(cmd_buf, sizeof(cmd_buf), "mkdir -p %s", dns_path); system(cmd_buf);
        snprintf(dns_path, sizeof(dns_path), "%s/containers/%s/upper/etc/resolv.conf", base_dir, name);
        char dns_buf[128]; snprintf(dns_buf, sizeof(dns_buf), "nameserver %s\n", current_cfg.dns_server);
        write_file(dns_path, dns_buf);

        // Run execution loop context inside the rootfs
        if (chdir(active_rootfs_path) == 0) {
            run_container_engine(&current_cfg, base_dir, name);
        } else {
            perror("[Host] Failed to transition to container storage context path");
        }

        snprintf(cmd_buf, sizeof(cmd_buf), "sudo umount -l %s 2>/dev/null", active_rootfs_path);
        system(cmd_buf);
    } 
    else if (strcmp(command, "restart") == 0) {
        if (argc < 3) { printf("Error: Name required.\n"); return 1; }
        char *name = argv[2];
        
        char active_rootfs_path[512];
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

        if (chdir(active_rootfs_path) == 0) {
            run_container_engine(&current_cfg, base_dir, name);
        } else {
            perror("[Host] Failed to transition context");
        }

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
        release_ip_index(base_dir, name);
        printf("[Host] Container allocation scrubbed from disk.\n");
    } 
    else if (strcmp(command, "uninstall") == 0) {
        printf("[Host] Beginning complete uninstallation layout scrub...\n");
        snprintf(cmd_buf, sizeof(cmd_buf), "sudo umount -l %s/containers/*/rootfs 2>/dev/null", base_dir); system(cmd_buf);
        printf("[Host] Removing container ecosystem base root directory: %s\n", base_dir);
        snprintf(cmd_buf, sizeof(cmd_buf), "rm -rf %s", base_dir); system(cmd_buf);
        printf("[Host] Dropping virtual host bridge switches...\n");
        system("sudo ip link del br0 2>/dev/null || true");
        printf("[Host] Removing engine configuration registries...\n");
        unlink(CONFIG_FILE);
        printf("[Host] Uninstallation successful! System is completely clean.\n");
    }
    else {
        printf("Unknown command: %s\n", command);
    }
    return 0;
}