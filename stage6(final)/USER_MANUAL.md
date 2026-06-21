# 🐳 MyDocker Engine: Official User Manual

Welcome to the documentation for your custom, C-based container orchestration engine. This tool requires root privileges (`sudo`) to interact directly with the Linux kernel's networking stack, mount tables, and cgroup interfaces.

Here is the comprehensive breakdown of every command, its syntax, and exactly what it is doing under the hood.

---

### `init`

**Syntax:** `sudo ./engine init`

The initialization wizard that prepares your host machine to run containers. This must be run once before using the engine.

* **Under the Hood:**
1. **Directory Scaffolding:** Creates the foundational storage path (default: `/var/lib/my_docker`), alongside subdirectories for `containers`, `base_rootfs`, `run`, and `ipam` (IP Address Management).
2. **Hardware Detection:** Uses the `uname()` syscall to detect your CPU architecture (x86_64 vs. ARM64).
3. **Image Pulling:** Downloads the corresponding lightweight Alpine Linux Mini RootFS tarball and extracts it into the `base_rootfs` directory. This acts as the immutable standard layer for all future containers.



---

### `spin_up`

**Syntax:** `sudo ./engine spin_up <container_name> [--memory <limit>] [--pids <limit>] [--dns <ip>]`

Instantly creates and launches a brand new, isolated container environment.

* **Under the Hood:**
1. **OverlayFS Assembly:** Creates an `upper` (read-write) and `work` directory for the container. It then mounts an `overlay` filesystem, fusing the read-only `base_rootfs` with the read-write `upper` layer.
2. **Resource Limiting (cgroups):** Creates a dedicated control group (`/sys/fs/cgroup/my_container_<name>`) and applies your memory and process (PID) limits to prevent the container from monopolizing host resources.
3. **Isolation (`clone`):** Spawns the container process using the `clone()` syscall with `CLONE_NEWNS` (mounts), `CLONE_NEWPID` (processes), `CLONE_NEWUTS` (hostname), and `CLONE_NEWNET` (network) flags.
4. **Network Plumbing:** * Allocates a dynamic IP (e.g., `10.0.0.2`).
* Creates a Virtual Ethernet (`veth`) pair, attaching one end to the host's `br0` bridge and pushing the other end into the container's isolated network namespace.
* Configures `iptables` NAT routing to masquerade outgoing traffic.





---

### `restart`

**Syntax:** `sudo ./engine restart <container_name>`

Wakes up a stopped container, restoring its exact filesystem state from when it was last running.

* **Under the Hood:**
1. **State Verification:** Checks if the requested container's directory structure still exists on the host.
2. **Layer Re-attachment:** Re-executes the `mount -t overlay` command. Because the container's `upper` directory was preserved on disk, any files created, packages installed, or code modified during its previous run are instantly remounted and available.
3. **Process Re-ignition:** Skips the initial directory creation phases and jumps straight to cloning the isolated process and wiring up the `veth` network interfaces.



---

### `save`

**Syntax:** `sudo ./engine save <container_name> <backup_directory_path>`

Captures a snapshot of the container's current state by exporting its modified files.

* **Under the Hood:**
1. **Delta Extraction:** Instead of backing up the entire OS, this command specifically targets the container's `upper` directory. Because of how Union Filesystems work, the `upper` directory contains *only* the specific file changes and additions made since the container was created.
2. **Archive:** Copies these delta files safely to a backup directory on your host, acting as a lightweight version of a `docker commit`.



---

### `delete`

**Syntax:** `sudo ./engine delete <container_name>`

Completely destroys a container, freeing up its disk space and network resources. **This is irreversible.**

* **Under the Hood:**
1. **Unmounting:** Uses a lazy unmount (`umount -l`) to detach the OverlayFS rootfs from the host's mount table safely.
2. **Filesystem Scrub:** Recursively deletes (`rm -rf`) the container's directory, permanently wiping its `upper` (read-write) layer.
3. **IP Release:** Deletes the tracking file inside the `/ipam` directory, allowing the container's 10.0.0.x IP address to be recycled and allocated to the next container that spins up.



---

### `uninstall`

**Syntax:** `sudo ./engine uninstall`

The nuclear option. Removes the entire MyDocker ecosystem from your host machine.

* **Under the Hood:**
1. **Mass Detachment:** Unmounts every active container rootfs simultaneously.
2. **Storage Purge:** Deletes the entire `/var/lib/my_docker` directory, wiping all downloaded base images, container states, and IP tracking data.
3. **Kernel Cleanup:** Destroys the `br0` virtual bridge from the Linux networking stack.
4. **Config Removal:** Deletes `/etc/mydocker.conf`, returning your host machine to a perfectly clean state.