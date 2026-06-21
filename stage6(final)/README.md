# Stage 6: Final Stage

### 1. OverlayFS (The Storage Engine)

**The Intuition:**
Imagine a teacher handing out a printed worksheet to a classroom. Instead of giving every student their own piece of paper (which wastes a massive amount of paper), the teacher places the worksheet on a desk and hands every student a clear, transparent plastic sheet. The students lay their plastic sheet over the worksheet and write their answers with a marker. When they look down, they see the printed questions *and* their written answers as one single document.

**How it actually works (The Mechanics):**
OverlayFS is a "Union Filesystem." It literally stacks directories on top of each other and merges them into a single viewport.

In your code, you defined three specific directories:

1. **`lowerdir` (The Worksheet):** This is your extracted Ubuntu or Alpine base image. It is strictly **Read-Only**. The kernel forbids any process from altering these files.
2. **`upperdir` (The Plastic Sheet):** This is a completely empty, private folder created specifically for one container. It is **Writable**.
3. **`merged` (The Viewport):** This is the `active_rootfs_path` where your container `chroot`s into. It is a virtual illusion created by the kernel.

Here is exactly what the Linux kernel does when your container interacts with files in the `merged` view:

* **Scenario A: Reading a file (`cat /etc/os-release`)**
The kernel looks in the `upperdir`. It finds nothing. It instantly falls back and reads the file from the `lowerdir`.
* **Scenario B: Modifying a file (Copy-on-Write - COW)**
Your container tries to append text to `/etc/hosts`. Because the `lowerdir` is locked, the kernel intercepts the write request. It silently and instantly **copies** the original `/etc/hosts` from the `lowerdir` up into the `upperdir`. It then lets the container modify the new copy in the `upperdir`. The original file on your host disk remains completely untouched.
* **Scenario C: Deleting a file (`rm /bin/ping`)**
If the container deletes a file that belongs to the base image, the kernel doesn't actually delete it. Instead, it creates a **Whiteout File** in the `upperdir`. A whiteout is a special, empty "character device" file (like a blank piece of opaque tape on the plastic sheet). When the `merged` view looks down, the whiteout completely blocks the underlying file, making the container think the file was successfully deleted.

---

### 2. Linux Bridge Networking (`br0`)

**The Intuition:**
In Stage 5, your `veth` pair was like a single Ethernet cable directly connecting two computers. In Stage 6, you built a **virtual hardware switch** inside your computer's RAM.

**How it actually works (The Mechanics):**
A Linux Bridge (`br0`) operates at **Layer 2** of the OSI model. It does not route based on IP addresses; it switches based on MAC addresses, exactly like a physical Netgear or Cisco switch sitting on a server rack.

Here is the exact journey of network packets when your orchestrator is running multiple containers:

* **The Setup:**
You create `br0` and give it the IP `10.0.0.1`. This acts as the default gateway (the router) for the entire subnet. When you spin up Container Alpha, you create a virtual cable. You plug `veth_c_alpha` into the container namespace (IP `10.0.0.2`) and you plug `veth_h_alpha` directly into the `br0` switch (`master br0`).
* **Container to Container Communication:**
Container Alpha (`10.0.0.2`) pings Container Beta (`10.0.0.3`).
1. Alpha doesn't know Beta's MAC address, so it sends an ARP broadcast out of its `veth_c` interface: *"Who has 10.0.0.3?"*
2. The broadcast travels down the virtual cable into the host's `br0` switch.
3. The `br0` switch floods that broadcast to all other plugged-in cables (Container Beta).
4. Beta replies with its MAC address.
5. The `br0` switch learns which MAC address is on which port. The ping packets now flow directly between the two containers through the switch. The host OS completely ignores this traffic because it never leaves the virtual switch.


* **Container to Internet Communication (NAT):**
Container Alpha pings `google.com`.
1. Alpha realizes `google.com` is not on the `10.0.0.0/24` subnet. It sends the packet to its default gateway: `10.0.0.1` (the `br0` switch).
2. The packet hits the host kernel at the switch. The kernel's routing table says, *"I need to send this out of the physical WiFi/Ethernet card."*
3. **The NAT Masquerade:** The internet does not know how to send data back to `10.0.0.2` (a private IP). As the packet leaves the host's physical network card, your `iptables` rule intercepts it. It strips off the container's `10.0.0.2` IP and slaps the Host's actual IP address on it (Masquerading).
4. When Google replies, the host catches the packet, remembers the translation, strips the host IP, slaps `10.0.0.2` back on, and shoves it down the `br0` switch into the container.


## Let's break into **six logical phases**.

---

### Phase 1: Building the Central Switch (`br0`)

Imagine buying a physical network switch and placing it on your desk. This switch will act as the central hub for all your containers.

```c
system("sudo ip link add name br0 type bridge 2>/dev/null || true");
system("sudo ip addr add 10.0.0.1/24 dev br0 2>/dev/null || true");
system("sudo ip link set br0 up 2>/dev/null || true");

```

* **`ip link add`**: Hey Linux, create a new network interface.
* **`name br0 type bridge`**: Name it `br0` (Bridge 0) and make it function exactly like a physical network switch.
* **`2>/dev/null || true`**: If `br0` already exists (from a previous run), Linux will throw an error. This specific text tells the terminal to hide the error and continue running the code anyway so the program doesn't crash.
* **`ip addr add 10.0.0.1/24 dev br0`**: Give this switch an IP address of `10.0.0.1`. The `/24` means the subnet spans from 10.0.0.1 to 10.0.0.254. This switch is now the **Default Gateway** (router) for your containers.
* **`ip link set br0 up`**: Turn the power button on for the switch.

---

### Phase 2: Creating the Virtual Ethernet Cable

Now we need a cable to connect the new container to the switch.

```c
snprintf(cmd, sizeof(cmd), "ip link add veth_h_%s type veth peer name veth_c_%s", name, name);

```

* **`ip link add ... type veth`**: Create a Virtual Ethernet (`veth`) cable. The Linux kernel requires these to be created in pairs (a left end and a right end). What goes in one end instantly comes out the other.
* **`veth_h_%s`**: Name the Host's end of the cable (e.g., `veth_h_alpha`).
* **`peer name veth_c_%s`**: Name the Container's end of the cable (e.g., `veth_c_alpha`).
*(Right now, both ends of the cable are just sitting on the host machine floor).*

---

### Phase 3: Plugging the Cable into the Switch

```c
snprintf(cmd, sizeof(cmd), "sudo ip link set veth_h_%s master br0", name);
snprintf(cmd, sizeof(cmd), "sudo ip link set veth_h_%s up", name);

```

* **`ip link set veth_h_alpha`**: Pick up the Host's end of the cable...
* **`master br0`**: ...and plug it directly into the `br0` switch we created in Phase 1.
* **`up`**: Activate this end of the cable.

---

### Phase 4: Throwing the Cable Over the Wall

This is the magic line. By default, networks are isolated. We must push the other end of the cable through the invisible wall into the container's isolated room.

```c
snprintf(cmd, sizeof(cmd), "ip link set veth_c_%s netns %d", name, container_pid);

```

* **`ip link set veth_c_alpha`**: Pick up the Container's end of the cable...
* **`netns <container_pid>`**: ...and move it out of the host's network and into the isolated **Network Namespace** (`netns`) owned by the container's Process ID. Once this command runs, the host can no longer see `veth_c_alpha`. It strictly exists inside the container's sandbox.

---

### Phase 5: Configuring the Container from the Outside (`nsenter`)

The cable is in the container, but it's turned off and has no IP address. Because we are on the host, we use a tool called `nsenter` to act like a ghost, reaching through the wall to configure the container's network without actually being inside the container.

*Every line here starts with `sudo nsenter -t <container_pid> -n`. This means: "Temporarily enter the network namespace (`-n`) of the target process (`-t`), run the following command, and immediately exit back to the host."*

```c
snprintf(cmd, sizeof(cmd), "sudo nsenter -t %d -n ip addr add 10.0.0.%d/24 dev veth_c_%s", container_pid, ip_suffix, name);

```

* **`ip addr add 10.0.0.2/24 dev veth_c_alpha`**: Inside the container, assign the dynamically generated IP (e.g., 10.0.0.2) to its end of the cable.

```c
snprintf(cmd, sizeof(cmd), "sudo nsenter -t %d -n ip link set veth_c_%s name veth_child", container_pid, name);

```

* **`name veth_child`**: Rename the interface from `veth_c_alpha` to a generic name (`veth_child`). We do this so that no matter what the container is named, scripts inside the container can always look for the same interface name.

```c
snprintf(cmd, sizeof(cmd), "sudo nsenter -t %d -n ip link set veth_child up", container_pid);
snprintf(cmd, sizeof(cmd), "sudo nsenter -t %d -n ip link set lo up", container_pid);

```

* **`up`**: Turn on the container's main cable, and turn on the Loopback (`lo` / `127.0.0.1`) interface so local applications inside the container don't crash.

```c
snprintf(cmd, sizeof(cmd), "sudo nsenter -t %d -n ip route add default via 10.0.0.1", container_pid);

```

* **`ip route add default via 10.0.0.1`**: Tell the container's internal routing table: *"If you are trying to reach an IP you don't recognize (like google.com), don't panic. Just send it to `10.0.0.1` (the `br0` switch) and let the host figure it out."*

---

### Phase 6: Granting Internet Access (Routing & NAT)

Right now, the container can reach the host, but if it tries to ping the internet, the internet doesn't know how to reply to a private `10.0.0.x` address.

```c
system("sysctl -w net.ipv4.ip_forward=1 > /dev/null");

```

* **`sysctl -w net.ipv4.ip_forward=1`**: By default, Linux computers ignore packets that aren't meant for them. This command changes a kernel setting, officially turning your host computer into a router that is allowed to forward traffic on behalf of others.

```c
system("iptables -t nat -A POSTROUTING -s 10.0.0.0/24 -j MASQUERADE 2>/dev/null || true");

```

* **`iptables -t nat`**: Open the Linux firewall's Network Address Translation (NAT) table.
* **`-A POSTROUTING`**: Append (`-A`) a rule right before packets leave the host computer for the internet.
* **`-s 10.0.0.0/24`**: Only apply this rule if the packet originated from our container subnet.
* **`-j MASQUERADE`**: The crucial step. It strips off the container's private IP (`10.0.0.2`) and replaces it (masquerades it) with the host computer's actual public WiFi/Ethernet IP. When a website replies, the host catches it, remembers the masquerade, and forwards the data back down the `br0` switch into the container.

## Testing

### Phase 1: The Setup & Initialization (Terminal 1)

First, we compile the engine and download the base operating system template.

```bash
# 1. Compile the code
gcc main.c -o engine

# 2. Run the initialization wizard
sudo ./engine init

```

*Press **Enter** when asked for the directory to accept the default (`/var/lib/my_docker`). You will see it download the Alpine base layer and extract it.*

---

### Phase 2: Testing OverlayFS & Persistence (Terminal 1)

Let's prove that the engine can spin up a container, let you modify files, and remember those changes when you restart it.

**1. Spin up the first container:**

```bash
sudo ./engine spin_up alpha

```

**2. Inside `node_alpha`, make some permanent changes:**

```bash
# Create a new custom file
echo "I am the alpha node" > /root/identity.txt

# Delete a system directory to prove the base image isn't harmed
rm -rf /etc/apk

# Verify the file is there, then exit the container
cat /root/identity.txt
exit

```

**3. Restart the container to prove persistence:**

```bash
sudo ./engine restart node_alpha

```

**4. Inside `node_alpha` (after restart), verify the data survived:**

```bash
cat /root/identity.txt
# Expected output: "I am the alpha node"

```

*Leave `node_alpha` running in this terminal and move to Terminal 2.*

---

### Phase 3: Multi-Tenant Networking & Cgroups (Terminal 2)

While `node_alpha` is running in Terminal 1, let's spin up a second container to test the virtual switch (`br0`) and the Cgroups resource limits.

**1. Spin up a second container with strict limits:**

```bash
sudo ./engine spin_up beta --memory 20M --pids 3 --dns 1.1.1.1

```

**2. Inside `node_beta`, test the internal Bridge network:**

```bash
# Check node_beta's dynamic IP (Should be 10.0.0.3)
ip a

# Ping node_alpha across the virtual switch (node_alpha is usually 10.0.0.2)
ping 10.0.0.2
# Expected: 0% packet loss! Your containers are talking to each other.

```

**3. Inside `node_beta`, test the external Internet (NAT):**

```bash
ping google.com
# Expected: Successful replies, proving the DNS injection and NAT masquerade work.

```

**4. Inside `node_beta`, test the Cgroups Process Limit:**

```bash
# Try to launch 4 background shells (limit is 3)
sh & sh & sh & sh &
# Expected: The kernel blocks the last one with "Resource temporarily unavailable"

```

*Type `exit` inside `beta` to close it, and type `exit` inside `node_alpha` in Terminal 1 to close it.*

---

### Phase 4: Delta Extraction & Teardown (Terminal 1)

Now that both containers are shut down, let's test the backup system and the final uninstaller.

**1. Extract `alpha`'s delta changes (The `save` command):**

```bash
sudo ./engine save alpha /tmp/alpha_backup

```

*Check `/tmp/alpha_backup` on your host. You will see your `identity.txt` file and a "whiteout" file representing the deleted `/etc/apk` folder, taking up almost zero disk space!*

**2. Delete `node_alpha` completely:**

```bash
sudo ./engine delete node_alpha

```

**3. The Nuclear Option (Uninstall):**

```bash
sudo ./engine uninstall

```

