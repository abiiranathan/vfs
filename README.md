# VFS v3 — High-Throughput Extent-Based Virtual Filesystem

```
 ██╗   ██╗███████╗███████╗    ██╗   ██╗██████╗ 
 ██║   ██║██╔════╝██╔════╝    ██║   ██║╚════██╗
 ██║   ██║█████╗  ███████╗    ██║   ██║ █████╔╝
 ╚██╗ ██╔╝██╔══╝  ╚════██║    ╚██╗ ██╔╝ ╚═══██╗
  ╚████╔╝ ██║     ███████║     ╚████╔╝ ██████╔╝
   ╚═══╝  ╚═╝     ╚══════╝      ╚═══╝  ╚═════╝ 
========================================================================
 HIGH-PERFORMANCE MULTITHREADED SINGLE-FILE VIRTUAL FILESYSTEM ENGINE
========================================================================
```

**VFS v3** is a high-performance, multithreaded single-file virtual filesystem engine written in C (C11/POSIX). It packs up to **65,536 files** and **64 GiB of data** into a single container image on the host filesystem while delivering bare-metal I/O throughput.

---

## Table of Contents

- [VFS v3 — High-Throughput Extent-Based Virtual Filesystem](#vfs-v3--high-throughput-extent-based-virtual-filesystem)
  - [Table of Contents](#table-of-contents)
  - [Why VFS? The Problem It Solves](#why-vfs-the-problem-it-solves)
  - [What Changed in Format v3?](#what-changed-in-format-v3)
  - [Key Architectural Features](#key-architectural-features)
  - [On-Disk Layout \& System Geometry](#on-disk-layout--system-geometry)
    - [System Limits \& Specifications](#system-limits--specifications)
  - [Concurrency Model (Lockless Data Plane)](#concurrency-model-lockless-data-plane)
  - [Target Use Cases](#target-use-cases)
  - [API Reference \& Usage Examples](#api-reference--usage-examples)
    - [1. Creating and Writing Files](#1-creating-and-writing-files)
    - [2. Zero-Copy Kernel Streaming (`sendfile`)](#2-zero-copy-kernel-streaming-sendfile)
    - [3. Mounting Directly from RAM / Static Memory](#3-mounting-directly-from-ram--static-memory)
    - [4. Directory Traversal \& Pattern Listing](#4-directory-traversal--pattern-listing)
  - [Compile-Time Configuration](#compile-time-configuration)
  - [Building \& Running the Test Suite](#building--running-the-test-suite)
    - [Sample Benchmark Output](#sample-benchmark-output)
  - [License](#license)

---

## Why VFS? The Problem It Solves

Modern high-performance applications often need to manage thousands of assets, logs, tenant containers, or database partitions. Relying directly on the host OS filesystem or traditional archives presents severe trade-offs:

| Storage Approach                     | Limitations & Bottlenecks                                                                                                                           |
| :----------------------------------- | :-------------------------------------------------------------------------------------------------------------------------------------------------- |
| **Host OS Filesystem (Loose Files)** | Inode exhaustion, host filesystem fragmentation, sluggish directory walks, difficult atomic backups, and slow container deployments.                |
| **ZIP / TAR Archives**               | Read-only or append-only; in-place overwrites, truncations, and random-access writes require rewriting the entire archive.                          |
| **SQLite BLOBs**                     | High overhead from SQL query parsing, write-ahead logging (WAL) amplification, and inability to perform zero-copy kernel transfers (`sendfile(2)`). |
| **VFS v3 (This Engine)**             | **Single host file, fully read-write, extent-indexed, sub-millisecond mounting, per-inode concurrency, and zero-copy kernel streaming.**            |

---

## What Changed in Format v3?

Format v2 was bounded by a single global mutex, a 3-tier indirect block tree (pointer chasing), and synchronous writeback on every close. **Format v3 completely redesigns the engine:**

1. **Extents Instead of Block Trees**: Replaced multi-level direct/single/double indirect pointer tables with a compact, sorted **Extent Array** `(logical_block, physical_block, length)`. Lookups execute in $O(\log N)$ via binary search.
2. **Lockless Host Data I/O**: Extents are resolved and snapshotted under an inode's read lock. **No VFS lock is held across host `pread()`, `pwrite()`, or `sendfile()` calls.**
3. **In-Memory Free-Extent Coalescer**: Free space is tracked as a sorted free-extent list layered on top of the durable bitmap, turning contiguous allocation into an $O(\log N)$ operation.
4. **Smart Overwriting & Truncate**: Newly allocated blocks scheduled for full-page overwrites are **never zero-filled**, eliminating 50% of write amplification.
5. **Batched Metadata Writeback**: Bitmap and inode edits are batched in RAM and flushed only on `vfs_sync()`, `vfs_close()`, or when crossing a dirty threshold.

---

## Key Architectural Features

```
┌─────────────────────────────────────────────────────────────────────────┐
│                           VFS v3 CORE FEATURES                          │
├──────────────────────────┬──────────────────────────────────────────────┤
│ 🚀 Extent-Based Storage  │ 32 inline extents per inode + chained blocks │
│ ⚡ Parallel Concurrency   │ Independent per-inode RWLocks(no global lock)│
│ 🌐 Kernel sendfile(2)    │ Zero-copy transfer directly to host sockets  │
│ 🧠 RAM Mounting (memfd)  │ Mount images directly from static byte arrays│
│ 🎯 O(1) Path Lookups     │ Open-addressing in-memory path hash index    │
│ 🛡️ Crash Durability      │ Superblock & bitmap consistency guarantees   │
└──────────────────────────┴──────────────────────────────────────────────┘
```

---

## On-Disk Layout & System Geometry

A VFS v3 image consists of four contiguous, predictable regions aligned to fixed boundaries:

```
  Offset (Hex)     Offset (Decimal)    Region Description
┌────────────────┬───────────────────┬─────────────────────────────────┐
│ 0x00000000     │ 0 Bytes           │ SUPERBLOCK REGION (64 KiB)      │
│                │                   │ Magic, Version, Geometry, Stats │
├────────────────┼───────────────────┼─────────────────────────────────┤
│ 0x00010000     │ 65,536 Bytes      │ FREE BLOCK BITMAP (2 MiB)       │
│                │                   │ 524,288 x 32-bit Words (1=Free) │
├────────────────┼───────────────────┼─────────────────────────────────┤
│ 0x00210000     │ 2,162,688 Bytes   │ INODE TABLE (~51.5 MiB)         │
│                │                   │ 65,536 Inodes x 824 Bytes       │
├────────────────┼───────────────────┼─────────────────────────────────┤
│ 0x03590000     │ 56,164,352 Bytes  │ DATA PAYLOAD AREA (Up to 64 GiB)│
│                │                   │ Block 0: Permanent Sentinel     │
│                │                   │ Block 1..16,777,215: User Data  │
└────────────────┴───────────────────┴─────────────────────────────────┘
```

### System Limits & Specifications

- **Block Size**: 4,096 bytes (4 KiB)
- **Max Addressable Capacity**: 16,777,216 blocks (**64.000 GiB**)
- **Max Files (Inodes)**: 65,536 (customizable at compile time)
- **Max Inline Extents**: 32 extents per inode (covers up to 64 GiB if contiguous)
- **Overflow Extents**: 256 extents per chained 4 KiB overflow block
- **Max Concurrent Open Handles**: 1,024 active file descriptors
- **Max Path Length**: 256 bytes (NUL-terminated UTF-8)

---

## Concurrency Model (Lockless Data Plane)

VFS v3 decouples the **metadata control plane** from the **data plane**. Threads reading or writing to different files proceed in parallel without lock contention:

```
            THREAD 1 (Writes /fileA)       THREAD 2 (Reads /fileB)
                       │                              │
                       ▼                              ▼
            [ InodeLock[A] (WR) ]          [ InodeLock[B] (RD) ]
                       │                              │
            Resolve/Snapshot Extents       Snapshot Extent Array
                       │                              │
            [ Unlock InodeLock[A] ]        [ Unlock InodeLock[B] ]
                       │                              │
                       ▼                              ▼
            ┌──────────────────────────────────────────────────┐
            │        PARALLEL UNLOCKED DIRECT HOST I/O         │
            │    pwrite(fd, ...)            pread(fd, ...)     │
            │                  sendfile(socket, fd, ...)       │
            └──────────────────────────────────────────────────┘
```

---

## Target Use Cases

* **High-Performance Static Web Asset Servers**: Store thousands of web assets in a single `.vfs` file and serve them over HTTP sockets using `vfs_sendfile()` without userspace memory copies.
* **Game Engine Asset Bundles**: Replace `.pak` or `.vpk` formats with a container that supports runtime in-place patching, dynamic saves, and streaming.
* **Embedded Software & MicroVMs**: Ship a full virtual image inside a firmware binary and mount it instantly in RAM via `vfs_open_embedded()`.
* **Multi-Tenant Edge Isolation**: Isolate tenant data into discrete single-file containers on disk for backup, deletion, or encryption.

---

## API Reference & Usage Examples

### 1. Creating and Writing Files

```c
#include "vfs.h"
#include <stdio.h>
#include <string.h>

int main(void) {
    vfs_t* vfs = NULL;
    
    // 1. Create a fresh VFS image container
    if (vfs_create("storage.vfs", &vfs) != VFS_OK) {
        fprintf(stderr, "Failed to create VFS\n");
        return 1;
    }

    // 2. Open a virtual file with POSIX-style flags
    vfs_fd_t fd = vfs_fopen(vfs, "/data/report.txt", VFS_O_CREAT | VFS_O_RDWR);
    if (fd < 0) {
        fprintf(stderr, "Error: %s\n", vfs_strerror((vfs_status_t)fd));
        vfs_close(vfs);
        return 1;
    }

    // 3. Write data
    const char* message = "High-throughput VFS extent engine.";
    size_t written = 0;
    vfs_fwrite(vfs, fd, message, strlen(message), &written);

    // 4. Seek and Read back
    vfs_fseek(vfs, fd, 0, VFS_SEEK_SET, NULL);
    char buffer[64] = {0};
    size_t bytes_read = 0;
    vfs_fread(vfs, fd, buffer, sizeof(buffer) - 1, &bytes_read);
    printf("Read back: %s\n", buffer);

    // 5. Close handle and sync metadata
    vfs_sync(vfs);
    vfs_close(vfs);
    return 0;
}
```

---

### 2. Zero-Copy Kernel Streaming (`sendfile`)

Stream data directly from a VFS virtual file to a host TCP socket or pipe without copying bytes through userspace:

```c
#include "vfs.h"
#include <unistd.h>

void stream_to_client(vfs_t* vfs, int client_socket_fd, const char* virtual_path) {
    vfs_fd_t vfd = vfs_fopen(vfs, virtual_path, VFS_O_RDONLY);
    if (vfd < 0) return;

    vfs_stat_t st;
    vfs_stat(vfs, virtual_path, &st);

    size_t total_sent = 0;
    // Uses Linux sendfile(2) directly between VFS host image and the client socket
    vfs_status_t status = vfs_sendfile(vfs, client_socket_fd, vfd, NULL, st.size, &total_sent);
    
    if (status == VFS_OK) {
        printf("Streamed %zu bytes zero-copy!\n", total_sent);
    }

    vfs_fclose(vfs, vfd);
}
```

---

### 3. Mounting Directly from RAM / Static Memory

Mount an embedded asset array (e.g., compiled binary assets) without touching disk:

```c
#include "vfs.h"

// Assume embedded_image_bytes was linked via ld -r or xxd
extern const uint8_t embedded_image_bytes[];
extern const size_t embedded_image_size;

void load_embedded_assets(void) {
    vfs_t* mem_vfs = NULL;
    
    // Instantiates an anonymous, purely RAM-backed descriptor (memfd_create)
    vfs_status_t s = vfs_open_embedded(embedded_image_bytes, embedded_image_size, true, &mem_vfs);
    if (s != VFS_OK) {
        return;
    }

    // Access virtual files directly from RAM
    size_t size = 0;
    void* config_data = vfs_read_file(mem_vfs, "/assets/config.json", &size);
    if (config_data) {
        // Process config...
        free(config_data);
    }

    vfs_close(mem_vfs);
}
```

---

### 4. Directory Traversal & Pattern Listing

```c
#include "vfs.h"
#include <stdio.h>

static bool print_file(const char* path, const vfs_stat_t* st, void* userdata) {
    (void)userdata;
    printf("File: %-30s | Size: %8lu Bytes | Blocks: %4u\n", 
           path, (unsigned long)st->size, st->block_count);
    return true; // Return false to halt iteration early
}

void list_directory(vfs_t* vfs, const char* prefix) {
    printf("Listing files under '%s':\n", prefix);
    vfs_list(vfs, prefix, print_file, NULL);
}
```

---

## Compile-Time Configuration

You can tune the maximum inode capacity to balance image overhead against capacity:

```bash
# Build for 65,536 files (Default: ~51.5 MiB table on disk, ~54 MiB RAM)
gcc -O3 -DVFS_MAX_INODES=65536 -c vfs.c

# Build a compact embedded image for 1,024 files (~824 KiB table on disk)
gcc -O3 -DVFS_MAX_INODES=1024 -c vfs.c
```

---

## Building & Running the Test Suite

The comprehensive test suite verifies extent mapping, multi-threaded concurrent I/O, sparse file allocation, zero-copy kernel transfers, and throughput benchmarks.

```bash
# Compile the test suite with optimizations
gcc -O3 -D_GNU_SOURCE -pthread -Wall -Wextra vfs.c vfs_test.c -o vfs_test

# Run the test suite and benchmarks
./vfs_test
```

### Sample Benchmark Output

```
[27/28] RUNNING: Benchmark: Sequential Write Speed (512 MiB)...
    Write: 100.0% (  512 MiB) - 0.3 sec, 2023 MiB/s

  Sequential Write Benchmark:
    Write Size   : 512.00 MiB
    Chunk Size   : 256 KiB
    Elapsed Time : 0.2531 seconds
    Throughput   : 2022.53 MiB/sec
[27/28] PASS

[28/28] RUNNING: Benchmark: Sequential Read Speed (512 MiB)...
    Read:  100.0% (  512 MiB) - 0.0 sec, 11143 MiB/s

  Sequential Read Benchmark:
    Read Size    : 512.00 MiB
    Chunk Size   : 256 KiB
    Elapsed Time : 0.0460 seconds
    Throughput   : 11140.22 MiB/sec
[28/28] PASS

===============================================================
Result Summary: 28 of 28 tests/benchmarks passed.
```

---

## License

MIT License. Free for use in commercial, closed-source, and open-source applications.
