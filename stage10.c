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
#include <signal.h>
#include <yaml.h>

#define STACK_SIZE (1024 * 1024)
#define CONFIG_FILE "/etc/mydocker.conf"
#define MAX_VOLUMES 8
#define MAX_SERVICES 10

int sync_pipe[2];
char active_rootfs_path[512];
char active_container_cmd[512]; // Global to pass the CMD into the namespace

typedef struct {
    char memory_limit[32];
    char pids_limit[32];
    char dns_server[64];
} ContainerConfig;

typedef struct {
    char host_path[256];
    char container_path[256];
} VolumeMapping;

typedef struct {
    char name[64];
    char setup_cmd[512];
    VolumeMapping volumes[MAX_VOLUMES];
    int volume_count;
} ServiceManifest;

ContainerConfig current_cfg = { .memory_limit = "256M", .pids_limit = "64", .dns_server = "8.8.8.8" };

void write_file(const char *path, const char *value) {
    FILE *fp = fopen(path, "w");
    if (fp) { fprintf(fp, "%s", value); fclose(fp); }
}

void get_base_dir(char *buf, size_t len) {
    FILE *fp = fopen(CONFIG_FILE, "r");
    if (!fp) { strncpy(buf, "/var/lib/my_docker", len); return; }
    if (fgets(buf, len, fp)) buf[strcspn(buf, "\n")] = 0;
    fclose(fp);
}

// ================= IPAM (IP Address Management) =================
int allocate_next_ip_index(const char *base_dir, const char *container_name) {
    char state_path[512];
    snprintf(state_path, sizeof(state_path), "%s/ipam", base_dir);
    mkdir(state_path, 0755);

    snprintf(state_path, sizeof(state_path), "%s/ipam/%s.ip", base_dir, container_name);
    FILE *fp = fopen(state_path, "r");
    if (fp) {
        int assigned_index = 2;
        if (fscanf(fp, "%d", &assigned_index) == 1) { fclose(fp); return assigned_index; }
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
                    char full_path[1024]; snprintf(full_path, sizeof(full_path), "%s/ipam/%s", base_dir, entry->d_name);
                    FILE *cf = fopen(full_path, "r");
                    if (cf) {
                        int read_val;
                        if (fscanf(cf, "%d", &read_val) == 1 && read_val == current_slot) slot_taken = 1;
                        fclose(cf);
                    }
                }
            }
            closedir(dir);
        }
        if (!slot_taken) {
            FILE *wf = fopen(state_path, "w");
            if (wf) { fprintf(wf, "%d", current_slot); fclose(wf); }
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

// ================= ISOLATED CONTAINER PROCESS (PID 1) =================
int container_main(void *arg) {
    char ch;
    close(sync_pipe[1]); 
    if (read(sync_pipe[0], &ch, 1) != 1) return 1;
    close(sync_pipe[0]);

    sethostname("c-container-demo", 16);
    if (chroot(active_rootfs_path) != 0) { perror("chroot failed"); return 1; }
    if (chdir("/") != 0) { perror("chdir failed"); return 1; }
    mount("proc", "/proc", "proc", 0, NULL);

    // ARCHITECTURE FIX: Execute the provided CMD array as PID 1
    if (strlen(active_container_cmd) > 0) {
        char *args[64];
        int i = 0;
        char *token = strtok(active_container_cmd, " ");
        while (token != NULL && i < 63) {
            args[i++] = token;
            token = strtok(NULL, " ");
        }
        args[i] = NULL;
        execvp(args[0], args);
        perror("Exec failed for specified command");
    }

    // Fallback if no command specified
    char *fallback_args[] = {"/bin/sh", NULL};
    execvp(fallback_args[0], fallback_args);
    return 1;
}

// ================= ENGINE EXECUTION & PLUMBING =================
void run_container_engine(ContainerConfig *cfg, const char *base_dir, const char *name) {
    pipe(sync_pipe);
    char *stack = malloc(STACK_SIZE);

    int container_pid = clone(container_main, stack + STACK_SIZE, CLONE_NEWPID | CLONE_NEWUTS | CLONE_NEWNS | CLONE_NEWNET | SIGCHLD, NULL);
    close(sync_pipe[0]); 

    // STATE TRACKING: Save PID to run directory for ps/rm commands
    char pid_file[512];
    snprintf(pid_file, sizeof(pid_file), "%s/run/%s.pid", base_dir, name);
    char pid_str_val[32]; snprintf(pid_str_val, sizeof(pid_str_val), "%d", container_pid);
    write_file(pid_file, pid_str_val);

    int ip_suffix = allocate_next_ip_index(base_dir, name);
    printf("[Host] Network Interface wired. IP: 10.0.0.%d\n", ip_suffix);

    char cgroup_dir[512]; snprintf(cgroup_dir, sizeof(cgroup_dir), "/sys/fs/cgroup/my_container_%s", name); mkdir(cgroup_dir, 0755);
    char path[512];
    snprintf(path, sizeof(path), "%s/memory.max", cgroup_dir);  write_file(path, cfg->memory_limit);
    snprintf(path, sizeof(path), "%s/pids.max", cgroup_dir);    write_file(path, cfg->pids_limit);
    snprintf(path, sizeof(path), "%s/cgroup.procs", cgroup_dir); write_file(path, pid_str_val);

    char cmd[512]; char short_name[5] = {0}; strncpy(short_name, name, 4);

    system("sudo ip link add name br0 type bridge 2>/dev/null || true");
    system("sudo ip addr add 10.0.0.1/24 dev br0 2>/dev/null || true");
    system("sudo ip link set br0 up 2>/dev/null || true");

    snprintf(cmd, sizeof(cmd), "ip link add veth_h_%s type veth peer name veth_c_%s 2>/dev/null || true", short_name, short_name); system(cmd);
    snprintf(cmd, sizeof(cmd), "sudo ip link set veth_h_%s master br0 2>/dev/null || true", short_name); system(cmd);
    snprintf(cmd, sizeof(cmd), "sudo ip link set veth_h_%s up 2>/dev/null || true", short_name); system(cmd);
    snprintf(cmd, sizeof(cmd), "ip link set veth_c_%s netns %d 2>/dev/null || true", short_name, container_pid); system(cmd);

    snprintf(cmd, sizeof(cmd), "sudo nsenter -t %d -n ip addr add 10.0.0.%d/24 dev veth_c_%s 2>/dev/null", container_pid, ip_suffix, short_name); system(cmd);
    snprintf(cmd, sizeof(cmd), "sudo nsenter -t %d -n ip link set veth_c_%s name veth_child 2>/dev/null", container_pid, short_name); system(cmd);
    snprintf(cmd, sizeof(cmd), "sudo nsenter -t %d -n ip link set veth_child up 2>/dev/null", container_pid); system(cmd);
    snprintf(cmd, sizeof(cmd), "sudo nsenter -t %d -n ip link set lo up 2>/dev/null", container_pid); system(cmd);
    snprintf(cmd, sizeof(cmd), "sudo nsenter -t %d -n ip route add default via 10.0.0.1 2>/dev/null", container_pid); system(cmd);

    system("sysctl -w net.ipv4.ip_forward=1 > /dev/null");
    system("iptables -t nat -A POSTROUTING -s 10.0.0.0/24 -j MASQUERADE 2>/dev/null || true");

    // Wake up child process
    write(sync_pipe[1], "O", 1); close(sync_pipe[1]); 

    // Wait for the container to finish executing its workload
    waitpid(container_pid, NULL, 0);
    
    // Cleanup on exit
    snprintf(cmd, sizeof(cmd), "ip link del veth_h_%s 2>/dev/null", short_name); system(cmd);
    rmdir(cgroup_dir);
    unlink(pid_file); // Remove state file
    free(stack);
}

// ================= STORAGE & MOUNT SETUP =================
void execute_spin_up(const char *base_dir, const char *name, ContainerConfig *cfg, VolumeMapping *vols, int vol_count, const char *exec_cmd) {
    char cmd_buf[2048];
    snprintf(active_rootfs_path, sizeof(active_rootfs_path), "%s/containers/%s/rootfs", base_dir, name);
    
    // Copy the target command into the global buffer for the child process
    memset(active_container_cmd, 0, sizeof(active_container_cmd));
    if (exec_cmd) strcpy(active_container_cmd, exec_cmd);

    snprintf(cmd_buf, sizeof(cmd_buf), "mkdir -p %s/containers/%s/upper/dev %s/containers/%s/work %s", base_dir, name, base_dir, name, active_rootfs_path); system(cmd_buf);
    snprintf(cmd_buf, sizeof(cmd_buf), "sudo mknod -m 666 %s/containers/%s/upper/dev/null c 1 3 2>/dev/null || true", base_dir, name); system(cmd_buf);
    snprintf(cmd_buf, sizeof(cmd_buf), "sudo mount -t overlay overlay -o lowerdir=%s/base_rootfs,upperdir=%s/containers/%s/upper,workdir=%s/containers/%s/work %s 2>/dev/null || true", base_dir, base_dir, name, base_dir, name, active_rootfs_path); system(cmd_buf);

    for (int i = 0; i < vol_count; i++) {
        char mount_target[1024];
        snprintf(mount_target, sizeof(mount_target), "%s/%s", active_rootfs_path, vols[i].container_path);
        snprintf(cmd_buf, sizeof(cmd_buf), "mkdir -p %s && mkdir -p %s && sudo mount --bind %s %s 2>/dev/null || true", vols[i].host_path, mount_target, vols[i].host_path, mount_target);
        system(cmd_buf);
    }

    char dns_path[1024]; snprintf(dns_path, sizeof(dns_path), "%s/containers/%s/upper/etc", base_dir, name);
    snprintf(cmd_buf, sizeof(cmd_buf), "mkdir -p %s", dns_path); system(cmd_buf);
    snprintf(dns_path, sizeof(dns_path), "%s/containers/%s/upper/etc/resolv.conf", base_dir, name);
    char dns_buf[128]; snprintf(dns_buf, sizeof(dns_buf), "nameserver %s\n", cfg->dns_server); write_file(dns_path, dns_buf);

    printf("\n================ 🚀 Launching Service: [%s] ================\n", name);
    run_container_engine(cfg, base_dir, name);

    for (int i = 0; i < vol_count; i++) {
        snprintf(cmd_buf, sizeof(cmd_buf), "sudo umount -l %s/%s 2>/dev/null", active_rootfs_path, vols[i].container_path); system(cmd_buf);
    }
    snprintf(cmd_buf, sizeof(cmd_buf), "sudo umount -l %s 2>/dev/null", active_rootfs_path); system(cmd_buf);
}

// ================= LIFECYCLE MANAGEMENT =================
void delete_container(const char *base_dir, const char *name) {
    char pid_file[512];
    snprintf(pid_file, sizeof(pid_file), "%s/run/%s.pid", base_dir, name);
    FILE *f = fopen(pid_file, "r");
    if (f) {
        int pid;
        if (fscanf(f, "%d", &pid) == 1 && pid > 0) {
            printf("[Host] Stopping running container process (PID: %d)...\n", pid);
            kill(pid, SIGKILL);
        }
        fclose(f);
        unlink(pid_file);
    }

    char cmd_buf[1024];
    snprintf(cmd_buf, sizeof(cmd_buf), "sudo umount -l %s/containers/%s/rootfs 2>/dev/null", base_dir, name); system(cmd_buf);
    snprintf(cmd_buf, sizeof(cmd_buf), "rm -rf %s/containers/%s", base_dir, name); system(cmd_buf);
    release_ip_index(base_dir, name);
    printf("[Host] Container '%s' scrubbed entirely from disk and network.\n", name);
}

// ================= YAML PARSER =================
// ================= YAML PARSER =================
void parse_docker_compose_yml(const char *filepath, ServiceManifest *services, int *service_count) {
    FILE *file = fopen(filepath, "r");
    if (!file) { printf("[Error] Missing 'docker-compose.yml'\n"); exit(1); }

    yaml_parser_t parser; yaml_event_t event;
    yaml_parser_initialize(&parser); yaml_parser_set_input_file(&parser, file);

    int depth = 0, in_services = 0, in_volumes = 0, expecting_command = 0;
    char last_key[256] = {0}; ServiceManifest *current_svc = NULL;

    while (yaml_parser_parse(&parser, &event)) {
        
        /* === THE INFINITE LOOP FIX === */
        if (event.type == YAML_STREAM_END_EVENT) {
            yaml_event_delete(&event);
            break;
        }
        /* ============================= */

        if (event.type == YAML_MAPPING_START_EVENT) depth++;
        else if (event.type == YAML_MAPPING_END_EVENT) { depth--; if (depth < 2) in_services = 0; }
        else if (event.type == YAML_SEQUENCE_START_EVENT) { if (strcmp(last_key, "volumes") == 0) in_volumes = 1; }
        else if (event.type == YAML_SEQUENCE_END_EVENT) in_volumes = 0;
        else if (event.type == YAML_SCALAR_EVENT) {
            char *val = (char *)event.data.scalar.value;
            if (depth == 1 && strcmp(val, "services") == 0) in_services = 1;
            else if (in_services && depth == 2) {
                if (*service_count < MAX_SERVICES) {
                    current_svc = &services[(*service_count)++];
                    strcpy(current_svc->name, val); current_svc->volume_count = 0; memset(current_svc->setup_cmd, 0, sizeof(current_svc->setup_cmd));
                }
            } else if (in_services && depth >= 3 && current_svc) {
                if (in_volumes) {
                    char *colon = strchr(val, ':');
                    if (colon && current_svc->volume_count < MAX_VOLUMES) {
                        *colon = '\0'; VolumeMapping *v = &current_svc->volumes[current_svc->volume_count++];
                        strcpy(v->host_path, val); strcpy(v->container_path, colon + 1);
                    }
                } else if (expecting_command) { strcpy(current_svc->setup_cmd, val); expecting_command = 0; } 
                else { strcpy(last_key, val); if (strcmp(val, "command") == 0) expecting_command = 1; }
            }
        }
        if (event.type != YAML_SCALAR_EVENT) expecting_command = 0; 
        yaml_event_delete(&event);
    }
    yaml_parser_delete(&parser); fclose(file);
}

void print_help() {
    printf("\n============== 🐳 MyDocker Orchestration Engine ==============\n");
    printf("Usage: sudo ./engine <command> [arguments]\n\n");
    printf("  init             Downloads base images.\n");
    printf("  compose-up       Parses docker-compose.yml and deploys stack.\n");
    printf("  compose-down     Reads yaml and destroys the entire stack.\n");
    printf("  ps               Lists all running/tracked containers.\n");
    printf("  rm <name>        Force kills and removes a specific container.\n");
    printf("==============================================================\n\n");
}

int main(int argc, char *argv[]) {
    if (argc < 2) { print_help(); return 0; }
    char *command = argv[1]; char base_dir[256]; char cmd_buf[2048];

    if (strcmp(command, "init") == 0) {
        printf("=== Initialization ===\nPath [/var/lib/my_docker]: ");
        char input[256]; if (fgets(input, sizeof(input), stdin) == NULL) return 1;
        input[strcspn(input, "\n")] = 0; strcpy(base_dir, strlen(input) == 0 ? "/var/lib/my_docker" : input);
        snprintf(cmd_buf, sizeof(cmd_buf), "mkdir -p /etc %s/containers %s/base_rootfs %s/run %s/ipam", base_dir, base_dir, base_dir, base_dir); system(cmd_buf);
        write_file(CONFIG_FILE, base_dir);
        
        struct utsname sysinfo; uname(&sysinfo); char url[1024] = {0};
        if (strcmp(sysinfo.machine, "x86_64") == 0) strcpy(url, "https://cdimage.ubuntu.com/ubuntu-base/releases/noble/release/ubuntu-base-24.04.4-base-amd64.tar.gz");
        else strcpy(url, "https://cdimage.ubuntu.com/ubuntu-base/releases/noble/release/ubuntu-base-24.04.4-base-arm64.tar.gz");
        
        snprintf(cmd_buf, sizeof(cmd_buf), "wget -q --show-progress \"%s\" -O /tmp/base.tar.gz && tar -xf /tmp/base.tar.gz -C %s/base_rootfs/", url, base_dir); system(cmd_buf);
        return 0;
    }

    get_base_dir(base_dir, sizeof(base_dir));

    // ================= NEW COMMAND: PS =================
    if (strcmp(command, "ps") == 0) {
        printf("\n%-20s %-10s %-10s\n", "CONTAINER ID", "PID", "STATUS");
        printf("------------------------------------------------\n");
        char run_dir[512]; snprintf(run_dir, sizeof(run_dir), "%s/run", base_dir);
        DIR *dir = opendir(run_dir);
        if (dir) {
            struct dirent *entry;
            while ((entry = readdir(dir)) != NULL) {
                if (strstr(entry->d_name, ".pid")) {
                    char name[128]; strcpy(name, entry->d_name); name[strlen(name)-4] = '\0';
                    char full_path[1024]; snprintf(full_path, sizeof(full_path), "%s/%s", run_dir, entry->d_name);
                    FILE *f = fopen(full_path, "r");
                    if (f) {
                        int pid; fscanf(f, "%d", &pid); fclose(f);
                        int status = kill(pid, 0); // 0 means process exists
                        printf("%-20s %-10d %-10s\n", name, pid, status == 0 ? "\033[32mUp\033[0m" : "\033[31mExited\033[0m");
                    }
                }
            }
            closedir(dir);
        }
        printf("\n");
    }
    // ================= NEW COMMAND: RM =================
    else if (strcmp(command, "rm") == 0) {
        if (argc < 3) { printf("Error: Provide container name.\n"); return 1; }
        delete_container(base_dir, argv[2]);
    }
    // ================= NEW COMMAND: COMPOSE-DOWN =================
    else if (strcmp(command, "compose-down") == 0) {
        printf("\n================ 🛑 Tearing Down Compose Stack ================\n");
        ServiceManifest services[MAX_SERVICES]; int service_count = 0;
        parse_docker_compose_yml("docker-compose.yml", services, &service_count);
        for (int i = 0; i < service_count; i++) {
            delete_container(base_dir, services[i].name);
        }
    }
    // ================= MODIFIED: COMPOSE-UP =================
    else if (strcmp(command, "compose-up") == 0) {
        printf("\n================ 🏙️ Spinning Up Compose Stack ================\n");
        ServiceManifest services[MAX_SERVICES]; int service_count = 0;
        parse_docker_compose_yml("docker-compose.yml", services, &service_count);

        int child_pids[MAX_SERVICES];
        for (int i = 0; i < service_count; i++) {
            int pid = fork();
            if (pid == 0) {
                // The YAML command is now passed as the execution command, NOT a setup hook!
                execute_spin_up(base_dir, services[i].name, &current_cfg, services[i].volumes, services[i].volume_count, services[i].setup_cmd);
                exit(0);
            }
            child_pids[i] = pid;
            sleep(1); // Stagger network creations slightly
        }

        // Wait for processes to finish (or run indefinitely if it's a web server)
        for (int i = 0; i < service_count; i++) waitpid(child_pids[i], NULL, 0);
    } 
    return 0;
}