# Stage 3: The Cage (Cgroups Resource Limits)

## Introduction
Right now, your container is isolated visually, but it is dangerous. If someone runs a malicious script or a memory-leaking app inside your container, it can consume all the RAM on your host machine and crash the whole system.

To fix this, we will use **Linux Control Groups (Cgroups v2)** to strictly limit our container to a maximum of **50 Megabytes of RAM**.

### How Cgroups Work in C


If **Namespaces** are the walls of a container that dictate what a process can *see*, **Cgroups** (Control Groups) are the ceiling that dictates what a process can *use*.

When building distributed compute-sharing systems or task orchestrators, cgroups are the critical safety net. They ensure that a single rogue or poorly written worker task cannot monopolize a node's RAM or CPU and crash the entire system.

Here is exactly how cgroups work and how our C code interacts with them.

---

### **The "Everything is a File" Philosophy**

Unlike namespaces, which require a dedicated system call (`clone`), cgroups are managed entirely through the Linux **Virtual File System (VFS)**.

The kernel exposes the cgroup interface as a pseudo-filesystem mounted at `/sys/fs/cgroup`. You don't use special functions to control resources; you simply create directories and write text strings into files. The kernel intercepts these file operations and instantly translates them into internal resource accounting rules.

Here is how your code executes this step-by-step:

### **1. Creating the Cgroup (`mkdir`)**

```c
const char *cgroup_dir = "/sys/fs/cgroup/my_container";
mkdir(cgroup_dir, 0755);

```

* **What it does:** You are creating a standard directory.
* **The Kernel Magic:** Because you are inside `/sys/fs/cgroup`, the kernel detects this `mkdir` command and says, "Ah, the user wants a new resource group." The kernel instantly instantiates a new cgroup in memory and **auto-populates** that empty directory with dozens of control files (like `memory.max`, `cpu.max`, `cgroup.procs`, etc.).

### **2. Defining the Boundaries (`write_cgroup_file`)**

```c
write_cgroup_file(memory_max_path, "50M");
write_cgroup_file(pids_max_path, "4");

```

* **What it does:** You are opening the auto-generated files and writing standard strings into them.
* **The Kernel Magic:** * By writing `"50M"` to `memory.max`, you set a hard memory limit. If your Alpine container tries to allocate 51MB of RAM, the kernel's **OOM (Out Of Memory) Killer** will immediately step in and assassinate the process to protect the host.
* By writing `"4"` to `pids.max`, you implement a fork-bomb protection. Your container is only allowed to spawn 4 processes. If the shell inside tries to run a 5th command simultaneously, the kernel will block the `fork()` syscall, returning a `Resource temporarily unavailable` error.



### **3. Trapping the Process (`cgroup.procs`)**

```c
snprintf(pid_str, sizeof(pid_str), "%d", container_pid);
write_cgroup_file(cgroup_procs_path, pid_str);

```

* **What it does:** You write the container's Host-side Process ID (e.g., `55255`) into the `cgroup.procs` file.
* **The Kernel Magic:** This is the moment the trap springs. Writing the PID into this file tells the kernel, "Take this specific process (and any children it creates in the future) and put them under the accounting umbrella of this cgroup." From this exact microsecond forward, the 50MB and 4-PID limits are actively enforced on your container.

### **4. The Teardown (`rmdir`)**

```c
rmdir(cgroup_dir);

```

* **What it does:** After the container shell exits, the parent process deletes the directory.
* **The Kernel Magic:** The kernel cleans up its internal tracking structs and releases the cgroup completely. *(Note: You can only delete a cgroup directory if there are no running PIDs left inside `cgroup.procs`).*

---
### **1. Testing `memory.max` (The "String Doubler")**

Instead of writing to the disk (which could accidentally write to the host if your mount isn't perfect), we will force the shell to hold an exponentially growing string in its RAM until the kernel assassinates it.

**Run this one-liner inside your container:**

```sh
s="a"; while true; do s="$s$s"; echo "Doubled!"; done

```

**What happens:**

1. The variable `s` starts as 1 byte (`a`).
2. Every loop, it concatenates the string with itself, doubling its size in memory: 1MB → 2MB → 4MB → 8MB → 16MB → 32MB → **64MB**.
3. The moment the shell tries to allocate the memory block that pushes it over your 50MB limit, the process will abruptly freeze.
4. Your terminal will simply output: `Killed` and drop you back to the host terminal.

**The Proof (Host Side):**
To see the kernel actually doing the work, run this command on your **Host** machine immediately after the container crashes:

```bash
dmesg -T | tail -n 15

```

You will see red kernel logs explicitly stating: `Memory cgroup out of memory: Killed process...` and the name of the cgroup that triggered the execution.

---

### **2. Testing `pids.max` (The Controlled Fork Bomb)**

**Run this loop inside your container:**

```sh
for i in 1 2 3 4 5; do sleep 100 & done

```

**What happens:**

1. This loop tries to spawn 5 background processes (`sleep 100`) simultaneously.
2. **PID 1:** Is your main `sh` shell.
3. **PID 2, 3, 4:** The first three `sleep` commands spawn successfully. (Total PIDs = 4).
4. **PID 5:** When the shell tries to run the fourth `sleep` command, it asks the kernel for a 5th PID.
5. The kernel checks `/sys/fs/cgroup/my_container/pids.max`, sees the limit is 4, and blocks the `clone` system call.
6. The shell will instantly throw an error: `sh: can't fork: Resource temporarily unavailable`.

This proves your host is completely immune to fork bombs originating from inside this container!

---


