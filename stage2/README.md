# Stage2: One More Step

## In this stage we will Upgrade the Previous stage by Providing a File System

1. **A Root File System (RootFS):** A folder on your host that contains a mini Linux distribution layout (`/bin`, `/lib`, `/proc`, etc.). We will use **Alpine Linux** because it is tiny (around 5MB).
2. **The `chroot` or `pivot_root` Call:** A system call in our C code to declare that this new folder is the absolute root (`/`) for our container.

---

## Step 1: Prepare the Root Filesystem on your Host

Before writing code, we need to download the mini Linux filesystem. Run these commands in your terminal **on your host machine** (outside the container):

```bash
# 1. Create a workspace directory for your container's files
mkdir -p ./rootfs

# 2. Download the official Alpine Linux mini-rootfs
wget https://dl-cdn.alpinelinux.org/alpine/v3.20/releases/x86_64/alpine-minirootfs-3.20.0-x86_64.tar.gz

# Download the ARM64 version of Alpine
wget https://dl-cdn.alpinelinux.org/alpine/v3.20/releases/aarch64/alpine-minirootfs-3.20.0-aarch64.tar.gz

# 3. Extract it into your rootfs folder
tar -xvf alpine-minirootfs-3.20.0-x86_64.tar.gz -C ./rootfs

# In case of ARM64 version of Alpine
tar -xvf alpine-minirootfs-3.20.0-aarch64.tar.gz -C ./rootfs

```

If you look inside `./rootfs`, you will see it has its own `bin`, `dev`, `etc`, `proc`, and `sys` folders. This is the isolated playground your container will live in.

---

## Step 2: Update the C Code (`main.c`)

Now, we need to modify our C code to do three things *inside* the container function, right before launching the shell:

1. Use `CLONE_NEWNS` in the `clone` flags to unshare the mount space.
2. Call `chroot("./rootfs")` to jail the process.
3. Call `chdir("/")` to move the process into its new home.
4. Mount a fresh `/proc` filesystem so commands like `ps` work properly.



Here is the technical breakdown of exactly how these four new additions function, the arguments they take, and how they alter the kernel's behavior.

---

### **1. `CLONE_NEWNS` (The Mount Namespace)**

* **What it does:** This tells the kernel to create a brand new **Mount Namespace** for the child process.
* **The Effect:** When the process starts, it inherits an exact copy of the host's mount points. However, because of `CLONE_NEWNS`, any new mounts or unmounts the container performs (like mounting `/proc` later on) are **completely invisible to the host machine**. If you didn't have this flag, your container would be modifying the host's actual partition tables and mount points, which would be disastrous.

### **2. `chroot("./rootfs")` (Changing the Root)**

* **What it takes:** A single string representing a directory path (`"./rootfs"`). This directory must contain all the binaries and libraries your container needs to run (like Alpine's `/bin/sh`, `/lib`, `/etc`, etc.).
* **What it does:** `chroot` stands for "Change Root." It redefines what the system considers the absolute top of the file system tree (`/`) for the current process and its future children.
* **The Effect:** The kernel places a hard visual barrier around this process. If your C program tries to read `/etc/passwd`, the kernel will look inside `./rootfs/etc/passwd`. The process is completely blind to anything on your host machine that exists physically "above" or outside of the `./rootfs` folder. It is securely jailed.

### **3. `chdir("/")` (Moving into the Jail)**

* **What it takes:** A single string representing the destination directory (`"/"`).
* **What it does:** It changes the Current Working Directory (CWD) of the process to the new root.
* **The Effect:** **This is a critical security step.** When you call `chroot`, you change the meaning of `/`, but the process's *current working directory pointer* might still be lingering outside the jail (e.g., in `/home/ubuntu/docker_testing`). If a malicious process doesn't call `chdir("/")`, it can use relative paths like `../../` to escape the `chroot` jail before it executes the shell. Calling `chdir("/")` forces the process physically into the new, restricted root space, neutralizing that escape route.

### **4. `mount("proc", "/proc", "proc", 0, NULL)` (The Virtual Filesystem)**

This is a direct C wrapper around the Linux `mount` command. It takes five precise arguments:

1. **`source` (`"proc"`):** Normally, this is a physical device like `/dev/sda1`. But `proc` is a "pseudo-filesystem" (it lives entirely in RAM, generated dynamically by the kernel). So, we just pass the dummy string `"proc"`.
2. **`target` (`"/proc"`):** The directory where we want to attach this filesystem. Because we are inside our `chroot` jail, this actually points to `./rootfs/proc`.
3. **`filesystemtype` (`"proc"`):** Tells the kernel exactly what type of filesystem driver to use.
4. **`mountflags` (`0`):** We pass `0` to use the default read/write permissions.
5. **`data` (`NULL`):** No extra filesystem-specific options are needed.

* **The Effect:** In your previous tests, `ps aux` crashed because the container was looking at the host's `/proc` folder while being in an isolated PID namespace. By mounting a fresh `proc` filesystem *after* applying `CLONE_NEWPID` and `CLONE_NEWNS`, the Linux kernel populates this new `/proc` directory **only** with processes that belong to the container's isolated PID namespace. When your Alpine shell runs `ps`, it reads this new directory and correctly sees only itself.

---
### The Ultimate Isolation Test 🧪

Once inside your new Alpine shell, run these commands to verify that you are completely cut off from the host:

1. **Check the files:**
```bash
ls /

```


* **Result:** You will only see Alpine files. If you try to do `ls /home/your_username`, it will say "No such file or directory". Your host files are completely invisible.


2. **Check the process isolation:**
```bash
ps aux

```


* **Result:** Instead of seeing hundreds of host processes, you will only see two or three processes: your `/bin/sh` shell running as PID 1, and the `ps` command you just ran!

