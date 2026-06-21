# 🐳 MyDocker Orchestration Engine: Official User Manual

Welcome to the documentation for your custom, declarative container orchestration engine. Because this engine interacts directly with the Linux kernel's networking stack, mount tables, and cgroup interfaces, it requires root privileges (`sudo`) to run.

---

## Part 1: The CLI Commands (How to use it)

### `init`

**Syntax:** `sudo ./engine init`
Run this once to prepare your host machine.

* **What it does:** Creates the foundational directory structure (default: `/var/lib/my_docker`) including `containers/`, `base_rootfs/`, `run/`, and `ipam/`. It checks your CPU architecture and downloads the official Ubuntu 24.04 Noble root filesystem tarball, extracting it to act as the immutable base layer for all your containers.

### `compose-up`

**Syntax:** `sudo ./engine compose-up`
Spins up your entire infrastructure stack concurrently.

* **What it does:** Reads the local `docker-compose.yml` file. For every service defined, it forks a new background process, sets up isolated network namespaces, mounts the storage layers, binds your specified host ports, and launches the command.

### `compose-down`

**Syntax:** `sudo ./engine compose-down [-v]`
Tears down the active infrastructure stack.

* **Without `-v`:** Sends a graceful `SIGTERM` (then `SIGKILL`) to stop the running container processes, clears the port forwarding rules, and detaches the network. **It preserves the filesystem.** If you `compose-up` again, all installed packages and database states remain intact.
* **With `-v`:** Performs the above, but also permanently deletes the container's `upper` storage layer from the disk, wiping it clean.

### `ps`

**Syntax:** `sudo ./engine ps`
Displays the status of tracked containers.

* **What it does:** Scans the `/run` directory for `.pid` files. It pings the process using a `kill(pid, 0)` syscall to check if the container is currently `Up` (running) or `Exited` (stopped/crashed).

### `rm`

**Syntax:** `sudo ./engine rm <container_name>`
Forcefully kills and wipes a single, specific container.

* **What it does:** Bypasses the Compose file to target a single container. It always acts like `compose-down -v` for that specific container, permanently deleting its filesystem and releasing its IP.

---

## Part 2: Internal Architecture (What the C Functions do)

Here is a breakdown of the core functions powering your engine under the hood, grouped by their conceptual domain.

### 1. Orchestration & YAML Parsing

* **`parse_docker_compose_yml()`**: Uses `libyaml` to traverse your `docker-compose.yml` file. It tracks mapping depth to extract the service names, bind volumes (`host:container`), port forwarding rules, and startup commands, packing them into an array of `ServiceManifest` structs.

### 2. Storage & Filesystems

* **`execute_spin_up()`**: The storage orchestrator.
1. Creates the `upper` and `work` directories for the container.
2. Executes `mount -t overlay` to fuse the read-only Ubuntu base with the read-write `upper` layer.
3. Iterates through the YAML volumes and executes `mount --bind` to link host directories directly into the container.
4. Dynamically injects the DNS resolver into the container's `/etc/resolv.conf`.


* **`delete_container()`**: The cleanup crew. It lazily unmounts the OverlayFS (`umount -l`) and, if the `-v` flag is triggered (`remove_fs = 1`), recursively deletes the container's isolated storage directory.

### 3. Isolation & Execution

* **`run_container_engine()`**: The bridge between the host and the container. It allocates the container's memory/PID limits inside `/sys/fs/cgroup/`. It then calls the `clone()` syscall with the crucial Linux Namespace flags (`CLONE_NEWNS`, `CLONE_NEWNET`, etc.) to create the isolated child process. It sits using `waitpid()` to hold the port-forwarding rules open until the container exits.
* **`container_main()`**: **(PID 1 inside the container).** This is the first code executed inside the newly cloned namespace. It synchronizes with the parent using a pipe, sets the custom hostname, traps the process inside the rootfs using `chroot()`, mounts the virtual `/proc` filesystem, and finally executes your Python/Bash command via `execvp()`.

### 4. Networking & Routing

* **`allocate_next_ip_index()` / `release_ip_index()**`: A custom IPAM (IP Address Management) system. It checks the `/ipam` directory to find the next available integer (e.g., `2` for `10.0.0.2`), locking it by creating a file, and deleting the file when the container is destroyed.
* **`system_setup()`**: Globally enables IP forwarding on the Linux host kernel and ensures `iptables` allows traffic across virtual bridges.
* **IPTables Routing Loop (Inside `run_container_engine`)**:
1. Creates a `veth` cable, pushing one end into the container's namespace and plugging the other into the host's `br0` bridge.
2. Uses `nsenter` to inject the allocated IP address directly into the container's network interface.
3. Loops through the ports defined in the YAML, writing custom **DNAT** (Destination NAT) rules in `iptables` to forward traffic from `localhost:8080` into the container's isolated IP, and **Masquerading** the return traffic so the connection works flawlessly.