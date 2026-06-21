# Implementation: docker-compose

## The Major Architecture Upgrades

### 1. Declarative Orchestration (YAML Parsing)

* **Stage 6:** You were using imperative CLI commands (`sudo ./engine spin_up my_app --memory 50M`). You had to spin up one container at a time and manually track them.
* **Final Stage:** You implemented a custom YAML parser using `libyaml`. The engine now reads a desired state (`docker-compose.yml`) and uses `fork()` to spawn multiple independent container processes simultaneously. The parent process then uses `waitpid()` to monitor the entire stack.

### 2. The Network Layer: Port Forwarding (DNAT/SNAT)

In Stage 6, your containers had an IP address (e.g., `10.0.0.2`), but they were completely invisible to the outside world. To reach a web server inside, you had to query `10.0.0.2` directly from the host.

In the Final Stage, you introduced **Network Address Translation (NAT)** via `iptables`, creating a true gateway:

* **PREROUTING (DNAT):** Catches traffic hitting your host machine's physical network card (e.g., port 8080) and rewrites the destination packet to the container's isolated IP (e.g., `10.0.0.2:8080`).
* **OUTPUT:** Does the exact same thing, but for traffic generated locally on the host (when you type `localhost:8080` in your browser).
* **POSTROUTING (Masquerade):** Ensures that when the container sends a response back out, the source IP is rewritten to look like the host machine. Without this, the client would drop the packet, seeing a response from an unknown `10.0.0.X` address instead of the host it queried.

### 3. Persistent Bind Mounts

* **Stage 6:** You only used an `OverlayFS`. Everything written inside the container was trapped in the `upper` directory and vanished if you deleted the container.
* **Final Stage:** You implemented `mount --bind`. This directly links a directory on your host (like `/home/prashant/docker_test/victory_stack/backend`) to a directory inside the container's isolated root filesystem (like `/app`). The VFS (Virtual File System) maps them to the exact same inodes on the disk.

---

## How to Run the "Victory Stack" Demo

Your directory structure is perfectly set up for an impressive demo. Here is the exact sequence to execute it:

1. **Initialize the Engine (Once per system):**
```bash
sudo ./engine init

```


This downloads the base Ubuntu layer to your host.
2. **Deploy the Stack:**
Navigate to your `victory_stack` directory where the `docker-compose.yml` lives.
```bash
sudo ./engine compose-up

```


Your engine will parse the YAML, fork two child processes, bind-mount the `backend` and `frontend` directories, and wire up the `iptables` routing for ports 8000 and 8080.
3. **Demonstrate Live Reloading:**
Open your browser to `http://localhost:8000`. Then, open `backend/main.py` on your host machine, change a string of text, and hit save. Because of the bind mount, the container sees the file change instantly.
4. **Demonstrate Persistence:**
Use the UI to write a message to the database. It will save to the `database/` folder on your host.
Run `sudo ./engine compose-down`, then `sudo ./engine compose-up` again. The data survives the container destruction.

---
