```markdown
# custom_docker — minimal container engine learning repo

This repository is a step-by-step implementation of a minimal container engine (a tiny Docker-like project) written in C. It walks through the low-level building blocks of Linux containers and culminates in a small compose-like orchestrator that can launch a simple demo "victory_stack".

## What this project is

- A learning-oriented implementation of container primitives: process isolation (namespaces), resource limits (cgroups), filesystem isolation (chroot + overlayfs), simple networking (veth, bridge, NAT), and a tiny compose-style YAML orchestrator.
- Implemented primarily in C across progressive stages so you can see how each feature is added and why.

## What you'll learn

- How `clone()` and Linux namespaces create process-level isolation (PID, UTS, mount, network, user).
- How to provide a RootFS and use `chroot` / `pivot_root` plus OverlayFS for copy-on-write layers and persistence.
- How to limit resources with cgroups v2 (memory, pids) and enforce safety boundaries.
- How to map user IDs safely with user namespaces and avoid race conditions using pipes.
- How to build container networking using veth pairs, bridges (`br0`) and NAT rules so containers can communicate and access the internet.
- How a small YAML-driven orchestrator can fork multiple containers and wire up bind-mounts, networking and port forwarding.

## Repository layout

- `stage0/` — Prerequisites and foundational concepts (syscalls, namespaces)
- `stage1/` — Basic container using `clone()` with PID and UTS namespaces
- `stage2/` — Add a RootFS and `chroot`/mount namespaces so the container has its own filesystem
- `stage3/` — Add Cgroups (memory, pids) to bound resource usage
- `stage4/` — Add User namespaces and UID/GID mapping with a sync pipe to avoid TOCTOU
- `stage5/` — Add Network namespaces, veth pairs, DNS injection and NAT routing from host
- `stage6(final)/` — OverlayFS storage engine, bridge networking (`br0`), multi-container orchestration and final features
- `docker-implementation/` — A compose-like orchestrator and the `victory_stack` demo wiring backend/frontend/database

Each of the above folders contains a `README.md` with more detailed explanations and step-by-step commands. Open any of these for deeper, stage-specific instructions.

## Quick links

- [Root README for the engine and compose demo](./docker-implementation/README.md)
- [Stage 0 — Pre-Requisite before starting (Base)](./stage0/README.md)
- [Stage 1 — The Beginning](./stage1/README.md)
- [Stage 2 — One More Step (RootFS and chroot)](./stage2/README.md)
- [Stage 3 — The Cage (Cgroups Resource Limits)](./stage3/README.md)
- [Stage 4 — The Closed Door (User Namespaces)](./stage4/README.md)
- [Stage 5 — Network Isolation](./stage5/README.md)
- [Stage 6 (final) — Final Stage (OverlayFS, bridge, compose)](./stage6(final)/README.md)

## Brief stage summaries

- Stage 0: Explains user vs kernel space, `clone()`, stack handling and `execvp`. Good reading before running code.
- Stage 1: Shows a minimal container `clone()` demo that isolates PID and UTS namespaces and launches a bash shell inside the container.
- Stage 2: Adds mount namespace and `chroot` (or `pivot_root`) into a small RootFS (Alpine) and mounts a fresh `/proc` so `ps` works properly inside the container.
- Stage 3: Uses cgroups v2 via the `/sys/fs/cgroup` interface to impose `memory.max` and `pids.max` limits and demonstrates tests that trigger the limits.
- Stage 4: Implements `CLONE_NEWUSER` safely by writing UID/GID maps from the parent and using a synchronization pipe to prevent race conditions.
- Stage 5: Builds container networking with veth pairs, assigns IPs, sets routes, injects DNS into the RootFS, and applies iptables NAT for internet access.
- Stage 6 (final): Introduces OverlayFS for copy-on-write layers, a host-side bridge `br0` to link many containers, YAML parsing for compose-like orchestration and demo scripts.

## How to run the demo (short)

1. Read `docker-implementation/README.md` for the full demo and exact commands.
2. Typical flow:
   - Build the engine (e.g. `gcc engine.c -o engine` or follow the stage-specific compile instructions).
   - Initialize the engine (`sudo ./engine init`) to download the base image.
   - From `docker-implementation/` run `sudo ./engine compose-up` to bring the `victory_stack` up.

## Notes & safety

- These programs exercise low-level kernel APIs and must be run with care (many steps require `sudo`). Run in a controlled environment (a disposable VM) if you are unsure.
- Some setup (like `CLONE_NEWUSER` mapping and cgroup writes) may behave differently inside cloud VMs, nested containers or restricted environments.

## Platform & requirements

- This project targets the Linux kernel and depends on Linux-only features (namespaces, cgroups v2, OverlayFS, iptables, `clone()` flags). It will not run correctly on macOS or Windows hosts directly.
- Recommended environment: a dedicated Linux machine or a disposable virtual machine (Ubuntu 22.04 or similar) with full privileges. This is the safest way to experiment without risking your main OS configuration.
- Caveats: nested or restricted environments (some cloud VMs, containerized CI runners, Docker-in-Docker) may hide or restrict files such as `/proc/[pid]/uid_map` or cgroup mounts, and may prevent creating network devices. If you encounter permission or ENOENT errors when writing maps or creating veth interfaces, try using a plain Linux VM with full capabilities.
- Most operations require root privileges (`sudo`) — be careful and run only on a system where you can safely modify firewall/network settings.

## License

This repository is published under the MIT License. See the bundled `LICENSE` file for the full text.

---
```
