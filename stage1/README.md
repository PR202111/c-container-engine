# Stage 1: The Beginning

### **1. The Preprocessor Directives (Setting the Stage)**

```c
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <sched.h>
#include <sys/wait.h>
#include <unistd.h>

```

* `#define _GNU_SOURCE`: This is absolutely critical. It tells the C compiler to expose specific, non-standard GNU/Linux extensions. Without this, the compiler won't recognize the `clone()` function or the namespace flags (`CLONE_NEWPID`, etc.), because they are specific to Linux, not standard C.
* `#include <stdio.h>`: The Standard Input/Output library. Included so you can use `printf()` to print messages to the screen and `perror()` to print error messages.
* `#include <stdlib.h>`: The Standard Library. Included for memory allocation (`malloc`, `free`) and the `exit()` function.
* `#include <sched.h>`: The Scheduler library. Included specifically so you can use the `clone()` system call, which is the heart of Linux containers.
* `#include <sys/wait.h>`: Included so the parent process can use `waitpid()` to pause and wait for the child process to finish.
* `#include <unistd.h>`: The Unix Standard library. Gives you access to system-level functions like `getpid()` (getting process IDs), `sethostname()` (changing the computer's name), and `execvp()` (running programs).

### **2. The Stack Size Definition**

```c
#define STACK_SIZE (1024 * 1024)

```

* Unlike a normal process (which gets its stack set up automatically by the OS), a process created with `clone()` needs you to manually carve out memory for its "stack".
* This line defines a constant of 1 Megabyte ($1024 \times 1024$ bytes) which is plenty of space for our container to run basic operations.

---

### **3. The Container's Brain (`container_main`)**

This function is what the new isolated process will run the moment it is born.

```c
int container_main(void *arg) {
    printf("[Container] Inside the container namespaces!\n");
    printf("[Container] My perspective PID: %d\n", getpid());

```

* `int container_main(void *arg)`: The signature required by `clone()`. It takes a void pointer argument (which we ignore here) and returns an integer.
* `getpid()`: This asks the OS, "What is my Process ID?" Because of the isolation we set up later, this will actually print `1`. Inside this container, it thinks it is the very first process on the whole system!

```c
    if (sethostname("c-container-demo", 16) != 0) {
        perror("[Container] Failed to set hostname");
    }

```

* `sethostname(...)`: Changes the hostname (the computer's name) of this environment to "c-container-demo". Because of the UTS namespace, this *only* changes the hostname inside the container, not on your actual host computer.
* `perror(...)`: If setting the hostname fails (usually because you didn't run the program with `sudo`), this prints a readable error message.

```c
    char *container_args[] = {"/bin/bash", NULL};
    printf("[Container] Launching Bash shell...\n\n");
    execvp(container_args[0], container_args);

```

* `char *container_args[]`: Creates an array defining the program to run (`/bin/bash`) and terminates it with `NULL`, which is required by the `execvp` function.
* `execvp(...)`: **This is a magic trick.** It completely replaces the current running C program inside the container with the Bash shell. From this point on, `container_main` stops executing, and the process simply becomes an interactive terminal for you to type commands into.

```c
    perror("[Container] execvp failed to launch bash");
    return 1;
}

```

* If `execvp` succeeds, it never returns. If the code reaches `perror`, it means `execvp` failed (e.g., if `/bin/bash` doesn't exist on your system). It returns `1` to signal an error.

---

### **4. The Host Process (`main`)**

This is the actual C program you execute from your terminal. It acts as the "Host" that builds the container.

```c
int main() {
    printf("[Host] Parent process started (PID: %d)\n", getpid());

```

* Prints the actual Process ID of the program running on your normal machine.

```c
    char *stack = malloc(STACK_SIZE);
    if (!stack) { ... exit(1); }

```

* `malloc(STACK_SIZE)`: Grabs 1MB of memory from the system to serve as the temporary memory space (stack) for our container. If it fails (returns `NULL`), it prints an error and quits.

```c
    int container_pid = clone(
        container_main, 
        stack + STACK_SIZE, // Pass the TOP of the stack because it grows downwards
        CLONE_NEWPID | CLONE_NEWUTS | SIGCHLD, 
        NULL
    );

```

* `clone(...)`: This is the Linux system call that creates the container. It's like `fork()`, but it allows you to specify exactly what the child process shares with the parent, and what is isolated.
* `container_main`: Tells `clone` what function the new process should start executing.
* `stack + STACK_SIZE`: In C, the stack grows downwards in memory (from high addresses to low addresses). So, you must give `clone` a pointer to the **top** (the very end) of the memory block you allocated.
* `CLONE_NEWPID`: **The magic of Namespaces.** This tells Linux to put the child in a brand new Process ID namespace. It makes the child think it is PID 1.
* `CLONE_NEWUTS`: Puts the child in a new UTS namespace, which isolates the hostname. This is why our `sethostname` earlier doesn't rename your actual computer.
* `SIGCHLD`: Tells the system to send a signal to the parent when the child dies. This is required for `waitpid` to work properly.
* `NULL`: The argument passed to `container_main` (which we aren't using).

```c
    if (container_pid == -1) { ... free(stack); exit(1); }
    printf("[Host] Container process created with Host-side PID: %d\n", container_pid);

```

* If `clone` fails, it returns `-1`. Otherwise, it returns the *Host's* perspective of the child's PID. So, the host might see the container as PID `4589`, but inside, the container thinks it is PID `1`.

```c
    waitpid(container_pid, NULL, 0);

```

* The parent process halts right here and waits for the container (the bash shell) to be closed by the user. If we didn't have this, the parent program would instantly finish, and the terminal would break or crash.

```c
    free(stack);
    printf("[Host] Container has closed. Exiting smoothly.\n");
    return 0;
}

```

* `free(stack)`: Cleans up the 1MB of memory we borrowed.
* `return 0`: Exits the program successfully.

---
# Experimental Commands at Stage 1

### **Category 1: Proving Isolation (Working Namespaces)**

These commands prove that your `clone()` flags (`CLONE_NEWPID` and `CLONE_NEWUTS`) are actively lying to the Bash shell and isolating it from the host.

* **Command:** `hostname`
* **Expected Output:** `c-container-demo`
* **What it proves:** Your UTS namespace (`CLONE_NEWUTS`) is active. The container has its own distinct hostname, independent of the host machine.


* **Command:** `echo $$`
* **Expected Output:** `1`
* **What it proves:** Your PID namespace (`CLONE_NEWPID`) is active. The Bash shell truly believes it is the very first process to boot up in this environment.



---

### **Category 2: Proving Shared Resources (Missing Namespaces)**

Because you haven't implemented the Mount (`CLONE_NEWNS`), Network (`CLONE_NEWNET`), or User (`CLONE_NEWUSER`) namespaces yet, these commands will prove that your container is still deeply entangled with your host machine.

* **Command:** `id` (or `whoami`)
* **Expected Output:** `uid=0(root) gid=0(root)...`
* **What it proves:** Missing User namespace. You are running as the actual root user of the host machine, not a restricted user inside the container.


* **Command:** `ls`
* **Expected Output:** You will see the files from your host machine
* **What it proves:** Missing Mount namespace. The container shares the exact same hard drive and directory structure as your host.


* **Command:** `ip a` (or `ifconfig`)
* **Expected Output:** A list of all your host's network interfaces (e.g., `eth0`, `lo`, `docker0`).
* **What it proves:** Missing Network namespace. The container has access to your host's network stack. If you listen on port 80 here, you take up port 80 on the host.



---

### **Category 3: The "Breakage and Danger" Tests**

These tests show what happens when isolated namespaces collide with shared host resources. **Be careful with the third test, as it creates a file on your actual host.**

* **Command:** `ps aux`
* **Expected Output:** `fatal library error, lookup self`
* **What it proves:** The clash between the new PID namespace and the shared Mount namespace. The `ps` command gets confused because it thinks it is PID 1, but the `/proc` directory it reads belongs to the host machine (which has thousands of different PIDs).


* **Command:** `mount -t proc proc /proc` followed immediately by `ps aux`
* **Expected Output:** A clean list showing only a couple of processes (like `bash` and `ps`).
* **What it proves:** By manually overriding the `/proc` directory with a fresh one tied to your container's PID namespace, `ps aux` can suddenly understand its environment and function correctly. *(Note: Because the mount namespace is shared, this breaks the host's `/proc` until you exit).*


* **Command:** `touch /tmp/container_was_here.txt`
* **Expected Output:** No output in the terminal, but the file is created.
* **What it proves:** The extreme danger of not isolating the file system and running as root. Exit the container, run `ls /tmp` on your host machine, and you will see the file exists there. Your container can alter the host OS.