# Stage 0: Pre-Requisite before starting (Base)

### **1. User Space vs. Kernel Space & System Calls**

* **User Space:** This is the restricted area where your normal programs run (like your web browser, or the compiled C program you just ran). Programs here cannot directly touch hardware or critical system memory.
* **Kernel Space:** This is the core of the Linux operating system. It has absolute power over the hardware, memory, and CPU.
* **System Calls (Syscalls):** If a program in User Space wants to do something privileged (like create a file, open a network connection, or *create a new process*), it must politely ask the kernel to do it via a "System Call." Functions like `clone()`, `getpid()`, and `execvp()` are essentially wrappers around these kernel-level system calls.

---

### **2. The `clone()` System Call (The Engine of Containers)**

In Linux, the traditional way to create a new process is with a system call named `fork()`. `fork()` creates an exact, identical copy of the running program.

`clone()` is the advanced, highly customizable cousin of `fork()`.

* Instead of copying everything blindly, `clone()` takes a checklist of **flags** (like `CLONE_NEWPID`).
* This checklist allows you to tell the kernel exactly what the new process should *share* with its parent, and what it should get a *completely fresh, isolated copy* of.
* When you isolate resources using `clone()` flags, you are essentially creating a container. Docker, Podman, and LXC all rely heavily on `clone()` under the hood.

---

### **3. Linux Namespaces (The Walls of the Container)**

Namespaces are a Linux kernel feature that tricks a process into thinking it has its own dedicated, isolated instance of a global system resource. It limits what a process can *see*.

In Next Stage,we used two specific namespaces via the `clone` flags:

#### **A. The UTS Namespace (`CLONE_NEWUTS`)**

* **What it stands for:** Unix Timesharing System. It’s an old, historical name.
* **What it actually does:** It isolates the **Hostname** and **Domain Name** of the system.
* **Why it matters:** Without this, if you type `hostname new-name` inside your container, it would change the name of your actual host computer (your laptop or server). By passing `CLONE_NEWUTS`, the container gets its own copy of the hostname. It can change its name to `c-container-demo` without affecting the outside world.

#### **B. The PID Namespace (`CLONE_NEWPID`)**

* **What it stands for:** Process ID. Every running program in Linux has a unique number (PID). The very first process that boots up on a Linux machine is always PID `1` (usually `systemd` or `init`).
* **What it actually does:** It isolates the process ID number tree.
* **Why it matters:** When you pass `CLONE_NEWPID`, the new process is assigned PID `1` *inside* its new isolated environment. It cannot see any other processes running on your host machine. However, the host machine can still see the container. The kernel maintains a mapping: the host might see the container as PID `8452`, but the container looks in the mirror and sees PID `1`.

#### **C. Other Important Namespaces**

* **Mount (`CLONE_NEWNS`):** Isolates the file system (files and folders). Allows the container to have its own `C:` drive or root `/` directory.
* **Network (`CLONE_NEWNET`):** Isolates network interfaces. Gives the container its own IP address, ports, and routing tables.
* **User (`CLONE_NEWUSER`):** Isolates user and group IDs. A process can be the omnipotent `root` user *inside* the container, but just a harmless, restricted normal user on the host.

---

### **4. The Stack Pointer and Memory Management**



* **What is the Stack?** When a C program runs, it needs memory to keep track of function calls, local variables, and where to return after a function finishes. This is called the "Call Stack."
* **Why we do it:** A standard process gets its stack set up automatically by the OS. But because `clone()` is a raw, low-level system call, you must manually provide a chunk of memory for the new process to use as its stack.
* On standard computer architectures (like x86), the stack **grows downwards** in memory. If you allocate a block of memory from address 0 to 1000, the stack starts at 1000 and grows towards 0. Therefore, you must point `clone` to the *top* (the end) of the memory block you allocated.

---

### **4. Process Replacement (`execvp`)**

* **What it does:** The `exec` family of functions completely destroys the currently running program and replaces it with a new one, *without changing the Process ID*.
* **Why it matters:** When `clone` runs, your container is just running your C code (`container_main`). That's not very useful. By calling `execvp("/bin/bash")`, the process rips out the C program from its memory and replaces its own brain with the Bash shell. Because it happens inside the namespaces we set up, you suddenly drop into an interactive, isolated terminal.


The `execvp` function is part of a larger family of functions in C (the `exec` family) that all do the same thing: replace the current process with a new one.

The name `execvp` actually gives you a hint about exactly what arguments it expects. The **`v`** stands for **Vector** (meaning it takes an array of arguments), and the **`p`** stands for **Path** (meaning it will automatically search your system's `PATH` to find the program).

Here is its exact C signature:

```c
int execvp(const char *file, char *const argv[]);

```

Let's break down those two arguments:

### **1. `const char *file` (The Program to Run)**

This is a string representing the name of the program or command you want to execute.

* Because of the **`p`** in `execvp`, you don't necessarily have to provide the exact, full file path (like `/bin/bash`).
* If you just pass `"bash"`, the OS will automatically search through all the directories listed in your system's `$PATH` environment variable to find the `bash` executable, just like your terminal does when you type a command.

### **2. `char *const argv[]` (The Argument Vector)**

This is an array of strings representing the command-line arguments you want to pass to the program. There are two absolute, unbreakable rules for this array:

* **Rule A: The first element (`argv[0]`) must be the name of the program itself.** This is a Unix convention. When a program starts, it expects its own name to be the first argument it sees.
* **Rule B: The array must be terminated with a `NULL` pointer.** Since C arrays don't know their own length, `execvp` needs a way to know when to stop reading arguments. The `NULL` acts as a stop sign.

---

### **For Example**



```c
char *container_args[] = {"/bin/bash", NULL};

execvp(container_args[0], container_args);

```

Here is exactly what is happening:

1. You created the array `container_args`.
2. The first element (`container_args[0]`) is `"/bin/bash"`.
3. The second element is `NULL` (the stop sign).
4. When you call `execvp`, you pass `container_args[0]` as the first argument (telling it to find `/bin/bash`).
5. You pass the entire `container_args` array as the second argument, satisfying the rules that the first item is the program name and the last item is `NULL`.

If you wanted to launch bash and immediately tell it to run a specific command (like `ls -l`), your array would look like this instead:

```c
char *container_args[] = {"bash", "-c", "ls -l", NULL};
execvp(container_args[0], container_args);

```