# Stage 4: The Closed Door (User Namespaces)

Now that our container has a localized identity, an isolated filesystem, and rigid resource boundaries, it's time to tackle **Security**.

Right now, if you type `id` inside your container, it says you are `uid=0(root)`. Because we haven't unshared the User Namespace, you are *actual root*. If an attacker exploits a bug in an application running inside your container and finds a way to look past the `chroot` folder, they will have master administrator control over your entire host machine.

We want to implement **User Namespaces (`CLONE_NEWUSER`)** to create a brilliant security illusion:

* **Inside the container:** You look and act like `root` (UID 0). You can change file permissions, create directories, and manage internal configurations.
* **Outside on the host:** The host kernel views your container process as a completely unprivileged, standard user (like your host `ubuntu` user, UID 1000). Even if a malicious process breaks out of the container layout, it has zero power on the host.

---
### **Part 1: The `sync_pipe` (Defeating the Race Condition)**

**Why it is needed:**
When you call `clone()`, the Linux kernel scheduler suddenly has two separate processes running in parallel: the Host (parent) and the Container (child). You cannot control which one gets CPU time first.
If the child gets the CPU first, it will instantly run `chroot`, launch the `/bin/sh` shell, and start executing potentially malicious code *before* the parent has had time to write the Cgroups limits or the User maps. This is a classic Time-of-Check to Time-of-Use (TOCTOU) vulnerability.

**How it works (Step-by-Step):**
To solve this, we use an Inter-Process Communication (IPC) pipe to force strict execution ordering. This functions on the exact same logic as using a kernel waitqueue to put a process to sleep until a specific event wakes it up.

1. **`pipe(sync_pipe)`:** Before cloning, the parent creates a data tube in memory. `sync_pipe[0]` is the read end, and `sync_pipe[1]` is the write end.
2. **The Split (`clone`):** Both the parent and the newly born child now have access to this tube.
3. **The Child Goes to Sleep:** The child immediately calls `read(sync_pipe[0], ...)`. Because the pipe is empty, the `read` system call *blocks*. The child process is removed from the CPU's active queue and put to sleep. It is physically frozen in time and cannot execute the shell.
4. **The Parent Builds the Cage:** While the child is asleep, the parent takes its time safely creating the Cgroup limits (memory, PIDs) and mapping the User Namespaces.
5. **The Release:** Once the cage is completely locked, the parent calls `write(sync_pipe[1], "O", 1)`. A single byte travels down the tube.
6. **The Child Awakens:** The child's `read` call intercepts that byte. It wakes up, proceeds past the synchronization block, enters the `chroot` jail, and finally launches the shell.

---

### **Part 2: User Namespace Mapping (The Security Illusion)**

**Why it is needed:**
Without `CLONE_NEWUSER`, the `root` user inside your container is the actual, omnipotent `root` user of your physical machine. If an attacker finds a way to break out of the `chroot` filesystem jail, they will own your entire node.

User Namespaces create a brilliant illusion. They allow the container to *think* it has root powers (so it can install packages and manage its own internal files), but the kernel secretly translates all of those actions to a standard, unprivileged user on the outside.

**How we map everything (Step-by-Step):**

1. **The Blank Slate:** When `clone` runs with `CLONE_NEWUSER`, the child process is stripped of all identity. It has zero permissions until the parent defines who it is.
2. **Bypassing the `sudo` Trap:** Because you have to run this C program with `sudo`, the `getuid()` function will return `0` (Root). If we mapped `0` to `0`, we defeat the purpose. The code checks `getenv("SUDO_UID")` to find out who the *actual* human is who typed `sudo` (e.g., your standard user, UID 1000).
3. **Writing the Map:** The parent writes the string `0 1000 1` to `/proc/[pid]/uid_map`. This syntax tells the kernel:
* "Take the UID `0` *inside* the container..."
* "...and map it to UID `1000` *outside* on the host..."
* "...for a range of `1` user."


4. **The `setgroups` Quirk:** The Linux kernel refuses to let an unprivileged user map Group IDs (GIDs) unless you explicitly strip their ability to use supplemental groups. Writing `"deny"` to `/proc/[pid]/setgroups` satisfies this kernel security rule.
5. **Writing the Group Map:** We do the exact same translation for the Groups via `gid_map`.

By the time the child process wakes up from the pipe, the kernel has locked in the illusion. The shell prompt says `root`, but the host OS knows it is securely handcuffed.

---
### **The Forensics: The Nested Container Trap**


If you are running this C program *inside* an existing VM, LXC container, or Docker environment. You are trying to build a container inside a container!

When you run Linux inside a containerized environment, the host operating system protects itself by mounting a **restricted, fake `/proc` filesystem** into your workspace.

1. The Linux kernel executed your `clone` system call successfully.
2. However, the security overlay (like `lxcfs` or Docker's AppArmor profiles) actively hides the `uid_map` and `gid_map` files to prevent nested privilege escalation.
3. The files literally do not exist in your virtualized view of `/proc`, causing `fopen` to throw `ENOENT` (No such file or directory).

**Note:** `For Everyone's acees to complete the all the stages we will skip this part in upcoming stage`

---





