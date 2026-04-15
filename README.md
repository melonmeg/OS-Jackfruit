# Mini Container Runtime with Kernel Memory Monitor

---

## 1. Team Information

**Team Members:**

* Meghna Sanjeev — SRN: PES1UG24CS269
* Mrinmayi Raman — SRN: PES1UG24CS278

---

## 2. Project Overview

This project implements a **lightweight multi-container runtime in C** along with a **kernel-space memory monitoring module**.

The system supports:

* Container isolation using namespaces
* Multi-container supervision
* Logging using bounded buffers
* Kernel-level memory monitoring
* Scheduling demonstration

---

## 3. Build, Load, and Run Instructions

### 🔹 1. Build the Project

```bash
make clean
make
```

**Use:**

* Builds engine, workloads, and kernel module

---

### 🔹 2. Compile Workloads (IMPORTANT)

```bash
gcc -static -O2 -o cpu_hog cpu_hog.c
gcc -static -O2 -o memory_hog memory_hog.c
gcc -static -O2 -o io_pulse io_pulse.c
```

**Use:**

* Ensures binaries run inside Alpine containers

---

### 🔹 3. Setup Container Filesystem

```bash
mkdir rootfs-base
wget https://dl-cdn.alpinelinux.org/alpine/v3.20/releases/x86_64/alpine-minirootfs-3.20.3-x86_64.tar.gz
tar -xzf alpine-minirootfs-3.20.3-x86_64.tar.gz -C rootfs-base
```

---

### 🔹 4. Copy Workloads

```bash
cp cpu_hog memory_hog io_pulse rootfs-base/
```

---

### 🔹 5. Create Container Filesystems

```bash
cp -a rootfs-base rootfs-alpha
cp -a rootfs-base rootfs-beta
```

---

### 🔹 6. Start Supervisor

```bash
sudo ./engine supervisor ./rootfs-base
```

---

### 🔹 7. Start Containers

```bash
sudo ./engine start alpha ./rootfs-alpha /bin/sh
sudo ./engine start beta ./rootfs-beta /bin/sh
```

---

### 🔹 8. List Containers

```bash
sudo ./engine ps
```

---

### 🔹 9. View Logs

```bash
sudo ./engine logs alpha
```

---

### 🔹 10. Run Container

```bash
sudo ./engine run test ./rootfs-alpha "echo HELLO && sleep 2"
```

---

### 🔹 11. Stop Container

```bash
sudo ./engine stop alpha
```

---

### 🔹 12. Load Kernel Module

```bash
sudo insmod monitor.ko
ls -l /dev/container_monitor
```

---

### 🔹 13. Memory Test

```bash
sudo ./engine run memtest ./rootfs-alpha "/memory_hog"
sudo dmesg | tail
```

---

### 🔹 14. Scheduling Demonstration

```bash
sudo ./engine start cpu1 ./rootfs-alpha "/cpu_hog"
sudo ./engine start cpu2 ./rootfs-beta "/cpu_hog"
```

```bash
sudo ./engine ps
```

```bash
watch -n 0.2 "ps -o pid,stat,cmd -p <PID1>,<PID2>"
```

---

### 🔹 15. Cleanup

```bash
sudo ./engine stop alpha
sudo ./engine stop beta
sudo ./engine stop cpu1
sudo ./engine stop cpu2
sudo rmmod monitor
ps -el | grep Z
```

---

### 🔹 16. Reset Environment

```bash
rm -rf rootfs-alpha rootfs-beta
cp -a rootfs-base rootfs-alpha
cp -a rootfs-base rootfs-beta
```

---

## 4. Demo with Screenshots

### 1. Multi-container supervision

![Multi Container](screenshots/multi_container.png)
**Figure 1:** Two containers running under the same supervisor.

---

### 2. Metadata tracking

![PS](screenshots/ps_metadata.png)
**Figure 2:** Container metadata showing PID, state, and limits.

---

### 3. Logging system

![Logs](screenshots/logging_output.png)
**Figure 3:** Output captured using bounded-buffer logging.

---

### 4. CLI and IPC

![IPC](screenshots/cli_ipc.png)
**Figure 4:** CLI commands handled via IPC.

---

### 5. Soft-limit warning

![Soft](screenshots/soft_limit.png)
**Figure 5:** Kernel warning for soft memory limit.

---

### 6. Hard-limit enforcement

![Hard](screenshots/hard_limit.png)
**Figure 6:** Container terminated after exceeding hard limit.

---

### 7. Scheduling experiment

![Scheduling](screenshots/scheduling.png)
**Figure 7:** CPU scheduling demonstration.

---

### 8. Clean teardown

![Cleanup](screenshots/clean_teardown.png)
**Figure 8:** Containers stopped with no zombie processes.

---

## 5. Engineering Analysis

### Process Isolation

Namespaces isolate container processes.

**Analysis:**
Provides lightweight virtualization.

---

### Filesystem Isolation

Using `chroot()` restricts file access.

**Analysis:**
Simplifies isolation but is less secure.

---

### Kernel Communication

Uses `ioctl` with `/dev/container_monitor`.

**Analysis:**
Allows structured kernel-user interaction.

---

### Memory Enforcement

Kernel enforces soft and hard limits.

**Analysis:**
Ensures processes cannot bypass limits.

---

### Scheduling

Uses `SIGSTOP` and `SIGCONT`.

**Analysis:**
Demonstrates round-robin scheduling.

---

### Logging

Producer-consumer model used.

**Analysis:**
Ensures safe concurrent logging.

---

### IPC

Uses UNIX sockets.

**Analysis:**
Efficient communication between components.

---

### Lifecycle Management

Supervisor manages processes.

**Analysis:**
Prevents zombie processes.

---

## 6. Design Decisions

* Namespace isolation → lightweight
* Single supervisor → simple
* UNIX sockets → efficient
* Timer monitoring → low overhead
* Signal scheduling → simple

---

## 7. Scheduler Results

CPU workloads share execution time.

**Conclusion:**
Linux scheduling ensures fair CPU distribution.

---

## 8. Conclusion

The project demonstrates:

* Container isolation
* Scheduling
* Memory management
* Kernel interaction

---
