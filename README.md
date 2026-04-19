# Multi-Container Runtime with Supervisor and Kernel Memory Monitor

---

## 1. Team Information

**Member 1**

* Name: Vishnu Narayanan Ajay
* SRN: PES2UG24CS594

**Member 2**

* Name: Vishnu S R
* SRN: PES2UG24CS595

---

## 2. Build, Load, and Run Instructions

### Step 1: Build

```bash
cd boilerplate
make
```

### Step 2: Load Kernel Module

```bash
sudo insmod monitor.ko
ls -l /dev/container_monitor
```

### Step 3: Prepare Root Filesystem

```bash
mkdir rootfs-base
tar -xzf alpine-minirootfs-*.tar.gz -C rootfs-base

cp -a ./rootfs-base ./rootfs-alpha
cp -a ./rootfs-base ./rootfs-beta
```

### Step 4: Start Supervisor

```bash
sudo ./engine supervisor ../rootfs-base
```

### Step 5: Start Containers (in another terminal)

```bash
sudo ./engine start alpha ../rootfs-alpha /cpu_hog
sudo ./engine start beta ../rootfs-beta /cpu_hog
```

### Step 6: List Containers

```bash
sudo ./engine ps
```

### Step 7: View Logs

```bash
sudo ./engine logs alpha
```

### Step 8: Stop Containers

```bash
sudo ./engine stop alpha
sudo ./engine stop beta
```

### Step 9: Unload Module

```bash
sudo rmmod monitor
```

---

## 3. Demo with Screenshots

### 1. Multi-Container Supervision
![Multi Container](https://github.com/vishnu201917-n/OS-Jackfruit/blob/bd9ed6db2f2ec3ab000e9f0fbdbb008dde95232c/Git1.png)

Two containers running under a single supervisor process.

### 2. Metadata Tracking

Output of `ps` showing container IDs, PIDs, and states.

### 3. Bounded-Buffer Logging

Logs captured via producer-consumer logging system.

### 4. CLI and IPC

CLI command interacting with supervisor via UNIX socket.

### 5. Soft Limit Warning

Kernel log showing memory soft-limit warning.

### 6. Hard Limit Enforcement

Container terminated after exceeding hard limit.

### 7. Scheduling Experiment

CPU-bound vs I/O-bound behavior comparison.
(cpu_hog ≈ 95%, io_pulse ≈ 0.3%)

### 8. Clean Teardown

No zombie processes after stopping containers.

---

## 4. Engineering Analysis

### 1. Isolation Mechanisms

Operating systems provide isolation to ensure that processes do not interfere with each other. Linux achieves this using namespaces, which create separate views of system resources. PID namespaces isolate process identifiers so that each container has its own process tree. UTS namespaces isolate system identifiers such as hostname, while mount namespaces provide separate filesystem views.

However, all containers still share the same underlying kernel. This means that while processes appear isolated, they ultimately depend on the same kernel scheduler, memory manager, and system calls. The chroot mechanism further restricts filesystem access by changing the root directory visible to a process, but it does not provide complete security like full virtualization.

---

### 2. Supervisor and Process Lifecycle

In Linux, every process has a parent-child relationship. When a process creates another process, it becomes responsible for managing its lifecycle. If a parent does not collect the exit status of its child, the child becomes a zombie process.

A long-running supervisor process is useful because it acts as a consistent parent for all container processes. This allows centralized tracking of process state, proper reaping using waitpid(), and controlled signal handling. The operating system relies on this parent-child structure to manage process cleanup and resource deallocation.

---

### 3. IPC, Threads, and Synchronization

Operating systems support multiple IPC mechanisms to allow processes to communicate safely. UNIX domain sockets provide reliable communication between processes on the same machine, while pipes are commonly used for streaming data between related processes.

When multiple threads or processes access shared data structures, race conditions can occur. To prevent this, synchronization primitives such as mutexes and condition variables are used. The producer-consumer model demonstrates how operating systems manage concurrent data flow efficiently while ensuring correctness and avoiding deadlocks.

---

### 4. Memory Management and Enforcement

The operating system manages memory using both virtual memory and physical memory tracking. RSS (Resident Set Size) represents the portion of a process’s memory that is currently loaded in physical RAM. However, RSS does not include swapped-out memory or shared memory segments fully.

Soft and hard limits represent different policy decisions. A soft limit allows the system to warn about excessive usage without immediate action, enabling flexibility. A hard limit enforces strict control by terminating the process, ensuring system stability.

Memory enforcement is more reliable in kernel space because the kernel has direct control over process memory and scheduling. User-space programs cannot guarantee enforcement since processes can ignore or bypass user-level checks.

---

### 5. Scheduling Behavior

The Linux scheduler is designed to balance fairness, responsiveness, and throughput. CPU-bound processes continuously request CPU time, while I/O-bound processes frequently yield the CPU while waiting for I/O operations.

As a result, I/O-bound processes often receive quick response times, while CPU-bound processes consume more CPU overall. The nice value influences scheduling priority, where lower values increase priority and higher values decrease it. The scheduler dynamically adjusts execution to ensure that no single process monopolizes system resources.

These behaviors reflect the core goals of operating system scheduling: efficient CPU utilization, fair resource allocation, and responsive interaction.

---

## 5. Design Decisions and Tradeoffs

* Used chroot instead of pivot_root
  → Simpler implementation but less secure

* Used UNIX domain sockets for IPC
  → Easy to implement but limited to local communication

* Used mutex + condition variables
  → Simple and efficient but may cause blocking

* Used kernel module for memory monitoring
  → Accurate enforcement but increases complexity

---

## 6. Scheduler Experiment Results

| Workload | Nice Value | CPU Usage       |
| -------- | ---------- | --------------- |
| cpu_hog  | 0          | High (95.4%)    |
| io_pulse | 0          | Low (0.3%)      |
| cpu_hog  | 10         | Reduced (77.4%) |

**Observation:**
CPU-bound processes received more CPU time, while I/O-bound processes yielded CPU frequently. Increasing nice value reduced priority.

---

## 7. Conclusion

The project successfully demonstrated containerization using Linux namespaces, process supervision, IPC mechanisms, kernel-level memory monitoring, and scheduling behavior. It provided practical insight into operating system concepts such as isolation, resource management, and process scheduling.
