#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <sched.h>
#include <sys/wait.h>
#include <sys/mount.h>
#include <sys/stat.h>   
#include <unistd.h>

#define STACK_SIZE (1024 * 1024)

int sync_pipe[2];

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


    if (chroot("./rootfs") != 0) {
        perror("chroot failed"); return 1; 
    }
    chdir("/");


    mount("proc", "/proc", "proc", 0, NULL);

    /* --- INSIDE NETWORK CONFIGURATION --- */
    // Bring up the loopback interface
    system("ip link set lo up");
    // Configure the injected veth interface end and give it an IP address
    system("ip addr add 10.0.0.2/24 dev veth_child");
    system("ip link set veth_child up");
    // Route all outside traffic through the host gateway interface
    system("ip route add default via 10.0.0.1");
    /* ------------------------------------ */

    char *container_args[] = {"/bin/sh", NULL};
    printf("[Container] Launching Alpine Sh shell with network link active...\n\n");
    execvp(container_args[0], container_args);

    return 1;
}

int main() {
    pipe(sync_pipe);
    char *stack = malloc(STACK_SIZE);

    printf("[Host] Cloning process with Network, Mount, PID, and UTS isolation...\n");

    int container_pid = clone(
        container_main, 
        stack + STACK_SIZE, 
        CLONE_NEWPID | CLONE_NEWUTS | CLONE_NEWNS | CLONE_NEWNET | SIGCHLD, 
        NULL
    );
    usleep(10000);

    close(sync_pipe[0]); 


    const char *cgroup_dir = "/sys/fs/cgroup/my_container";
    mkdir(cgroup_dir, 0755);
    char path[256];
    snprintf(path, sizeof(path), "%s/memory.max", cgroup_dir);  write_file(path, "50M");
    snprintf(path, sizeof(path), "%s/pids.max", cgroup_dir);    write_file(path, "4");
    snprintf(path, sizeof(path), "%s/cgroup.procs", cgroup_dir);
    char pid_str[32]; snprintf(pid_str, sizeof(pid_str), "%d", container_pid);
    write_file(path, pid_str);

    /* --- STAGE 5: HOST SIDE NETWORK PLUMBING --- */
    printf("[Host] Plumbing virtual ethernet wires into container...\n");
    char cmd[512];

    // 1. Create a virtual ethernet cable pair
    system("ip link add veth_host type veth peer name veth_child");

    // 2. Push the child end of the cable directly into the container's network namespace
    snprintf(cmd, sizeof(cmd), "ip link set veth_child netns %d", container_pid);
    system(cmd);

    // 3. Configure the host side end of the cable
    system("ip addr add 10.0.0.1/24 dev veth_host");
    system("ip link set veth_host up");

    // 4. Enable NAT/IP Forwarding on the host so the container can reach outer web hardware
    system("sysctl -w net.ipv4.ip_forward=1 > /dev/null");
    system("iptables -t nat -A POSTROUTING -s 10.0.0.0/24 -j MASQUERADE");
    /* ------------------------------------------- */


    write(sync_pipe[1], "O", 1);
    close(sync_pipe[1]); 

    waitpid(container_pid, NULL, 0);


    system("ip link del veth_host");
    rmdir(cgroup_dir);
    free(stack);
    return 0;
}