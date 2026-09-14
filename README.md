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

**VFS v3** is a high-performance, multithreaded single-file virtual filesystem engine written in C (C11/POSIX). It packs up to **65,536 files** and **64 GiB of data** into a single container image on the host filesystem. Bulk import/export moves whole extents kernel-side (`copy_file_range`, reflinked metadata-only where supported), random-access I/O goes through `pread`/`pwrite`, and nothing is ever `mmap`'d — so a truncated image is a clean error, never a `SIGBUS`.

---

## Table of Contents

- [Why VFS? The Problem It Solves](#why-vfs-the-problem-it-solves)
- [What Changed in Format v3?](#what-changed-in-format-v3)
- [Key Architectural Features](#key-architectural-features)
- [On-Disk Layout \& System Geometry](#on-disk-layout--system-geometry)
  - [System Limits \& Specifications](#system-limits--specifications)
- [Concurrency Model (Lockless Data Plane)](#concurrency-model-lockless-data-plane)
- [Durability \& Error Model](#durability--error-model)
- [Target Use Cases](#target-use-cases)
- [CLI Reference (`vfs-cli`)](#cli-reference-vfs-cli)
  - [Commands](#commands)
  - [Exit Codes \& Output Conventions](#exit-codes--output-conventions)
  - [CLI Examples](#cli-examples)
  - [Performance Notes for the CLI](#performance-notes-for-the-cli)
- [Library API Reference](#library-api-reference)
  - [Lifecycle](#lifecycle)
  - [Random-Access File I/O](#random-access-file-io)
  - [Bulk Transfer (Fast Path)](#bulk-transfer-fast-path)
  - [Which Transfer API Should I Use?](#which-transfer-api-should-i-use)
  - [Namespace \& Metadata](#namespace--metadata)
  - [Utility](#utility)
  - [Open Flags, Seek Origins \& Error Codes](#open-flags-seek-origins--error-codes)
- [Library Usage Examples](#library-usage-examples)
  - [1. Creating and Writing Files](#1-creating-and-writing-files)
  - [2. Bulk-Importing a Host File (`vfs_import_fd`)](#2-bulk-importing-a-host-file-vfs_import_fd)
  - [3. Bulk-Exporting to a Host File (`vfs_export_fd`)](#3-bulk-exporting-to-a-host-file-vfs_export_fd)
  - [4. Zero-Copy Kernel Streaming to Sockets (`vfs_sendfile`)](#4-zero-copy-kernel-streaming-to-sockets-vfs_sendfile)
  - [5. Sparse Files, Truncate \& Random Access](#5-sparse-files-truncate--random-access)
  - [6. Mounting Directly from RAM / Static Memory](#6-mounting-directly-from-ram--static-memory)
  - [7. Directory Traversal \& Pattern Listing](#7-directory-traversal--pattern-listing)
  - [8. Rename, Remove \& Stat](#8-rename-remove--stat)
  - [9. Multithreaded Server Pattern](#9-multithreaded-server-pattern)
- [Compile-Time Configuration](#compile-time-configuration)
- [Building \& Running the Test Suite](#building--running-the-test-suite)
  - [Sample Benchmark Output](#sample-benchmark-output)
- [License](#license)

---

## Why VFS? The Problem It Solves

Modern high-performance applications often need to manage thousands of assets, logs, tenant containers, or database partitions. Relying directly on the host OS filesystem or traditional archives presents severe trade-offs:

| Storage Approach                     | Limitations & Bottlenecks                                                                                                                                |
| :----------------------------------- | :------------------------------------------------------------------------------------------------------------------------------------------------------- |
| **Host OS Filesystem (Loose Files)** | Inode exhaustion, host filesystem fragmentation, sluggish directory walks, difficult atomic backups, and slow container deployments.                     |
| **ZIP / TAR Archives**               | Read-only or append-only; in-place overwrites, truncations, and random-access writes require rewriting the entire archive.                               |
| **SQLite BLOBs**                     | High overhead from SQL query parsing, write-ahead logging (WAL) amplification, and inability to perform zero-copy kernel transfers (`sendfile(2)`).      |
| **VFS v3 (This Engine)**             | **Single host file, fully read-write, extent-indexed, O(1) path lookups, per-inode concurrency, kernel-side bulk transfer, zero-copy socket streaming.** |

---

## What Changed in Format v3?

Format v2 was bounded by a single global mutex, a 3-tier indirect block tree (pointer chasing), and synchronous writeback on every close. **Format v3 completely redesigns the engine:**

1. **Extents Instead of Block Trees**: Replaced multi-level direct/single/double indirect pointer tables with a compact, sorted **Extent Array** `(logical_block, physical_block, length)`. Lookups execute in $O(\log N)$ via binary search.
2. **Lockless Host Data I/O**: Extents are resolved and snapshotted under an inode's read lock. **No VFS lock is held across host `pread()`, `pwrite()`, `sendfile()`, or `copy_file_range()` calls.**
3. **In-Memory Free-Extent Coalescer**: Free space is tracked as a sorted free-extent list layered on top of the durable bitmap, turning contiguous allocation into an $O(\log N)$ operation.
4. **Smart Overwriting & Truncate**: Newly allocated blocks scheduled for full-page overwrites are **never zero-filled**, eliminating wasted write amplification. Extending a file with `vfs_truncate()` allocates nothing — the gap stays sparse and reads back as zeros.
5. **Batched Metadata Writeback**: Superblock, bitmap, and inode edits are batched in RAM and flushed only on `vfs_sync()` / `vfs_close()`. No per-write threshold scans.
6. **O(1) Path Index**: An open-addressing in-memory hash index maps paths to inode slots, so open/stat/unlink/rename cost $O(1)$ per file and packing $N$ files costs $O(N)$ total instead of $O(N^2)$.
7. **No `mmap` Anywhere on the I/O Path**: The inode table is an explicit `pread`/`pwrite` heap buffer and bulk/host transfers stream through kernel copies or buffered I/O. A short, truncated, or concurrently-shrinking file surfaces as `VFS_ERR_CORRUPT` / `VFS_ERR_IO` — never `SIGBUS`.
8. **Kernel-Side Bulk Transfer**: `vfs_import_fd()` / `vfs_export_fd()` move whole extents with `copy_file_range()`, which the kernel may satisfy as a metadata-only extent share (reflink) on btrfs/xfs.

---

## Key Architectural Features

```
┌─────────────────────────────────────────────────────────────────────────┐
│                           VFS v3 CORE FEATURES                          │
├──────────────────────────┬──────────────────────────────────────────────┤
│ Extent-Based Storage     │ 32 inline extents per inode + chained blocks │
│ Parallel Concurrency     │ Per-inode RWLocks, lockless host data I/O   │
│ Bulk Import/Export       │ copy_file_range per extent (reflinkable)    │
│ Kernel sendfile(2)       │ Zero-copy transfer directly to host sockets  │
│ RAM Mounting (memfd)     │ Mount images directly from static byte arrays│
│ O(1) Path Lookups        │ Open-addressing in-memory path hash index    │
│ No-mmap Durability       │ Short images are errors, never SIGBUS        │
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
- **Max Files (Inodes)**: 65,536 (customizable at compile time, see below)
- **Max Inline Extents**: 32 extents per inode (covers up to 64 GiB if contiguous)
- **Overflow Extents**: 256 extents per chained 4 KiB overflow block (288 extents max per file)
- **Max Concurrent Open Handles**: 1,024 active file descriptors
- **Max Path Length**: 256 bytes (NUL-terminated UTF-8)
- **On-Disk Format**: magic `0x56465303` (`"VFS\x03"`), version 3

---

## Concurrency Model (Lockless Data Plane)

VFS v3 decouples the **metadata control plane** from the **data plane**. Threads operating on different files proceed in parallel without lock contention:

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
            │         copy_file_range(...)  sendfile(...)      │
            └──────────────────────────────────────────────────┘
```

Rules of the road:

- Calls on **different files** run fully in parallel, including bulk import/export.
- Calls on the **same file descriptor** from multiple threads are serialised by that file's per-inode lock (same as POSIX threads sharing one `fd`).
- The filesystem-wide `meta_lock` is held only for short metadata mutations (allocation, extent-map persists, cursor/size updates) — never across host data I/O.
- `vfs_list()` snapshots entries so a single callback may safely call back into read-only APIs; do not mutate the VFS from inside a listing callback.

---

## Durability & Error Model

- **Batched metadata**: superblock, bitmap, and inode-table edits accumulate in RAM and are persisted by `vfs_sync()` or `vfs_close()`. File *data* (`pwrite` / `copy_file_range`) is handed to the host on return. `vfs_sync()` flushes library buffers to the host file; it is not an `fsync()` to stable storage.
- **Close semantics**: `vfs_fclose()` only releases the descriptor — it deliberately does **not** force a metadata flush. Call `vfs_sync()` explicitly when durability across a crash is required immediately.
- **No `mmap`, no `SIGBUS`**: neither the inode table nor host transfers are memory-mapped. Opening an image smaller than the fixed header (`~56 MiB`) fails with `VFS_ERR_CORRUPT`; short reads/writes fail with `VFS_ERR_IO`. Out-of-space conditions return errors instead of signals.
- **Sparse by design**: holes (from `vfs_truncate()` growth or partially-written extents) read back as zeros and cost no storage until written.
- **Read-only mounts**: `vfs_open(path, true, …)` rejects any write-flag open, truncate, unlink, rename, and write with `VFS_ERR_READONLY`; `vfs_sync()` on such a handle is a no-op success.

---

## Target Use Cases

* **High-Performance Static Web Asset Servers**: Store thousands of web assets in a single `.vfs` file and serve them over HTTP sockets using `vfs_sendfile()` without userspace memory copies.
* **Game Engine Asset Bundles**: Replace `.pak` or `.vpk` formats with a container that supports runtime in-place patching, dynamic saves, and streaming.
* **Dataset Distribution & Archival**: Pack gigabyte-scale corpora (documents, media, ML datasets) into one file. On reflink-capable filesystems both pack and unpack are metadata-only operations — effectively instant regardless of size.
* **Backup / Snapshot Staging**: `vfs_import_fd()` snapshots host files into a container; shared extents stay copy-on-write consistent even if sources change afterwards.
* **Embedded Software & MicroVMs**: Ship a full virtual image inside a firmware binary and mount it instantly in RAM via `vfs_open_embedded()`.
* **Multi-Tenant Edge Isolation**: Isolate tenant data into discrete single-file containers on disk for backup, deletion, or encryption.
* **Log Aggregation**: Append-only workloads stream through `vfs_append_file()` / `O_APPEND` with per-file locking.

---

## CLI Reference (`vfs-cli`)

Build with `make` (requires `libsolidc` via `pkg-config`; without it the CLI target is skipped and the library/tests still build). Binary lands at `bin/vfs-cli`.

Global convention: `-c/--container` selects the image, `-d/--dir` a host directory, `-v/--verbose` prints per-file progress, `-j/--threads N` sets worker threads (`0` = all online CPUs, clamped to 64).

### Commands

| Command   | Syntax                                          | Description                                                       |
| :-------- | :---------------------------------------------- | :---------------------------------------------------------------- |
| `create`  | `vfs create -c image.vfs`                       | Create an empty image (overwrites an existing file).              |
| `pack`    | `vfs pack -c image.vfs -d ./src [-v] [-j N]`    | Create image and import a host directory tree (parallel).         |
| `unpack`  | `vfs unpack -c image.vfs -d ./out [-v] [-j N]`  | Extract every file to a host directory (parallel).                |
| `ls`      | `vfs ls -c image.vfs [prefix]`                  | List files whose virtual path starts with `prefix` (default `/`). |
| `add`     | `vfs add -c image.vfs <host_src> <vfs_dst>`     | Import one host file (creates or truncates the destination).      |
| `extract` | `vfs extract -c image.vfs <vfs_src> <host_dst>` | Export one file to the host.                                      |
| `rm`      | `vfs rm -c image.vfs <vfs_path>`                | Delete a file, freeing its blocks.                                |
| `mv`      | `vfs mv -c image.vfs <vfs_old> <vfs_new>`       | Rename/move; replaces the destination like POSIX `rename(2)`.     |
| `stat`    | `vfs stat -c image.vfs <vfs_path>`              | Show size, blocks, creation/modification times.                   |
| `exists`  | `vfs exists -c image.vfs <vfs_path>`            | Print `exists`/`missing` (exit code usable in scripts).           |
| `dump`    | `vfs dump -c image.vfs`                         | Superblock, free-extent and inode-table diagnostics.              |

### Exit Codes & Output Conventions

- `pack`/`unpack` print a one-line summary to **stderr** (`packed N file(s), B byte(s) into …`) and exit non-zero if any file errored. Per-file lines go to **stdout** only with `--verbose`.
- `exists` prints `exists` (exit 0) or `missing` (exit 1) — convenient for shell scripts.
- All other failures print `error: …` to stderr with a non-zero exit.

### CLI Examples

```bash
# Create an image and pack a directory (uses all CPUs)
vfs pack -c app.vfs -d ./assets

# Same, but verbose and limited to 4 workers
vfs pack -c app.vfs -d ./assets --verbose -j 4

# Extract everything
vfs unpack -c app.vfs -d ./out

# Inspect
vfs ls -c app.vfs /img
vfs stat -c app.vfs /img/logo.png
vfs exists -c app.vfs /img/logo.png && echo present

# Single-file operations
vfs add -c app.vfs ./logo.png /img/logo.png
vfs extract -c app.vfs /img/logo.png ./logo.png
vfs mv -c app.vfs /img/logo.png /img/banner.png
vfs rm -c app.vfs /img/banner.png
vfs dump -c app.vfs
```

### Performance Notes for the CLI

- `pack`/`add` move each whole extent with one kernel-side copy. On reflink-capable filesystems (btrfs, xfs, same filesystem for source and image) both pack and unpack of multi-gigabyte trees complete in well under a second; otherwise throughput tracks sequential device speed.
- For bulk runs, keep the image and the source/destination on the **same filesystem** to allow reflink sharing.
- On rotational HDDs, one large file per worker is ideal; tiny-file trees are metadata-bound, so `-j` near CPU count is usually best. Measure with your own tree if in doubt.

---

## Library API Reference

Link against `lib/libvfs.a` plus `-lpthread` (defines: `_GNU_SOURCE` for `copy_file_range`/`sendfile`).

### Lifecycle

| Function                                        | Description                                                                                                           |
| :---------------------------------------------- | :-------------------------------------------------------------------------------------------------------------------- |
| `vfs_create(path, &vfs)`                        | Create (overwrite) an image and mount it read-write. Caller must `vfs_close()`.                                       |
| `vfs_open(path, readonly, &vfs)`                | Mount an existing image. Short/corrupt images fail with `VFS_ERR_CORRUPT`, never `SIGBUS`. Caller must `vfs_close()`. |
| `vfs_close(vfs)`                                | Flush (unless read-only) and free the handle. `NULL` is a no-op.                                                      |
| `vfs_sync(vfs)`                                 | Flush superblock, bitmap, and dirty inodes to the host file. No-op success when read-only.                            |
| `vfs_open_embedded(data, size, readonly, &vfs)` | Mount from a static memory array via an anonymous `memfd` (Linux) — ideal for linked-in assets.                       |

### Random-Access File I/O

The general-purpose primitives for small writes, overwrites, appends, and memory-buffer I/O. Thread-safe; parallel across files.

| Function                                          | Description                                                                                         |
| :------------------------------------------------ | :-------------------------------------------------------------------------------------------------- |
| `vfs_fopen(vfs, path, flags)`                     | Open/create a file. Returns a non-negative `vfs_fd_t`, or a negative `vfs_status_t` on error.       |
| `vfs_fclose(vfs, fd)`                             | Release a descriptor. Does not force a metadata flush — call `vfs_sync()` if needed.                |
| `vfs_fread(vfs, fd, buf, count, &bytes_read)`     | Read up to `count` bytes; advances the cursor. `0` bytes at EOF. Holes read as zeros.               |
| `vfs_fwrite(vfs, fd, buf, count, &bytes_written)` | Write bytes, allocating extents as needed; full-block overwrites skip zero-fill. Honors `O_APPEND`. |
| `vfs_fseek(vfs, fd, offset, whence, &new_offset)` | Reposition the cursor (`VFS_SEEK_SET/CUR/END`). May seek past EOF (sparse gap).                     |
| `vfs_ftell(vfs, fd, &pos)`                        | Query the cursor position.                                                                          |
| `vfs_write_file(vfs, path, content, len)`         | Create-or-truncate + write a whole memory buffer in one call.                                       |
| `vfs_append_file(vfs, path, data, len)`           | Create-if-missing + append a whole memory buffer in one call.                                       |
| `vfs_read_file(vfs, path, &out_size)`             | Read a whole file into a heap buffer (caller frees; `NULL` on any failure).                         |

### Bulk Transfer (Fast Path)

For moving whole files between host descriptors and the VFS. Each contiguous extent travels in a single kernel-side copy — use these instead of `fread`/`fwrite` loops for bulk work.

| Function                                                        | Description                                                                                                                                                                                                                                               |
| :-------------------------------------------------------------- | :-------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| `vfs_import_fd(vfs, fd, host_fd, size)`                         | Import `size` bytes from host offset 0 into `fd` (pass a freshly created/truncated file). Reflinked metadata-only where supported; transparent `pread`/`pwrite` fallback otherwise. Short sources report `VFS_ERR_IO`.                                    |
| `vfs_export_fd(vfs, out_fd, in_fd, offset, count, &bytes_sent)` | Same interface/semantics as `vfs_sendfile()`. Fast path for regular-file destinations starting at offset 0; sockets, pipes, and non-zero offsets delegate to `vfs_sendfile()` byte-identically. Sparse holes materialise as zeros via extend-only sizing. |
| `vfs_sendfile(vfs, out_fd, in_fd, offset, count, &bytes_sent)`  | Kernel `sendfile(2)` transfer to any destination (sockets, pipes, files). `offset != NULL` leaves the cursor untouched (Linux `sendfile` semantics); `NULL` uses/advances it. Short transfer + `VFS_OK` means EOF.                                        |

### Which Transfer API Should I Use?

| Situation                             | Use                 | Why                                                        |
| :------------------------------------ | :------------------ | :--------------------------------------------------------- |
| Importing a host file into the VFS    | `vfs_import_fd`     | One extent-sized kernel copy per run; reflinkable.         |
| Exporting to a host file              | `vfs_export_fd`     | Same, in reverse; holes become sparse zeros for free.      |
| Serving to a socket/pipe (HTTP, IPC)  | `vfs_sendfile`      | Only API that targets non-regular destinations.            |
| Small / random / memory-buffer writes | `vfs_fwrite` family | Fast path APIs only handle whole-file fd-to-fd bulk moves. |
| Partial export at a non-zero offset   | `vfs_sendfile`      | `vfs_export_fd` delegates there automatically anyway.      |

### Namespace & Metadata

| Function                                    | Description                                                                                                      |
| :------------------------------------------ | :--------------------------------------------------------------------------------------------------------------- |
| `vfs_stat(vfs, path, &st)`                  | Fill `vfs_stat_t` (`path`, `size`, `block_count`, `created_at`, `modified_at`).                                  |
| `vfs_exists(vfs, path)`                     | Boolean existence test.                                                                                          |
| `vfs_unlink(vfs, path)`                     | Delete a file, freeing data, overflow, and inode. Closes any open descriptors on it.                             |
| `vfs_rename(vfs, oldpath, newpath)`         | Rename; an existing destination is replaced (POSIX `rename(2)` semantics). No data moves.                        |
| `vfs_truncate(vfs, path, length)`           | Shrink (frees tail extents, zeroes the partial tail block) or grow (sparse, allocates nothing).                  |
| `vfs_list(vfs, prefix, callback, userdata)` | Call `callback(path, st, userdata)` per file under `prefix` (`"/"` or `""` = all). Return `false` to stop early. |

### Utility

| Function               | Description                                                                 |
| :--------------------- | :-------------------------------------------------------------------------- |
| `vfs_strerror(status)` | Human-readable string for any `vfs_status_t`. Never `NULL`.                 |
| `vfs_dump(vfs, out)`   | Superblock, free-extent, inode-table, and open-file diagnostics to `FILE*`. |

### Open Flags, Seek Origins & Error Codes

```c
/* Open flags (OR-able) */
VFS_O_RDONLY  /* read only              */  VFS_O_WRONLY  /* write only           */
VFS_O_RDWR    /* read + write           */  VFS_O_CREAT   /* create if missing    */
VFS_O_TRUNC   /* truncate on open (needs write) */
VFS_O_APPEND  /* writes go to EOF       */  VFS_O_EXCL    /* with CREAT: fail if exists */

/* Seek origins */
VFS_SEEK_SET  /* from start */  VFS_SEEK_CUR  /* from cursor */  VFS_SEEK_END  /* from end */

/* Error codes (all negative except VFS_OK; vfs_fopen returns them as vfs_fd_t) */
VFS_OK (0)  VFS_ERR_IO  VFS_ERR_CORRUPT  VFS_ERR_NOTFOUND  VFS_ERR_EXISTS
VFS_ERR_NOSPACE  VFS_ERR_NOMEM  VFS_ERR_BADFD  VFS_ERR_OVERFLOW
VFS_ERR_INVAL  VFS_ERR_ISDIR  VFS_ERR_READONLY
```

---

## Library Usage Examples

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

    // 4. Seek and read back
    vfs_fseek(vfs, fd, 0, VFS_SEEK_SET, NULL);
    char buffer[64] = {0};
    size_t bytes_read = 0;
    vfs_fread(vfs, fd, buffer, sizeof(buffer) - 1, &bytes_read);
    printf("Read back: %s\n", buffer);

    // 5. Flush metadata and close
    vfs_fclose(vfs, fd);
    vfs_sync(vfs);
    vfs_close(vfs);
    return 0;
}
```

---

### 2. Bulk-Importing a Host File (`vfs_import_fd`)

For whole files, never loop `fread`/`fwrite` — one call moves each extent kernel-side (reflinked where supported):

```c
#include "vfs.h"
#include <fcntl.h>
#include <stdio.h>
#include <sys/stat.h>
#include <unistd.h>

vfs_status_t import_media(vfs_t* vfs, const char* host_src, const char* vfs_dst) {
    int hfd = open(host_src, O_RDONLY | O_CLOEXEC);
    if (hfd < 0) return VFS_ERR_IO;

    struct stat st;
    if (fstat(hfd, &st) < 0 || !S_ISREG(st.st_mode)) {
        close(hfd);
        return VFS_ERR_IO;
    }

    vfs_fd_t vfd = vfs_fopen(vfs, vfs_dst, VFS_O_WRONLY | VFS_O_CREAT | VFS_O_TRUNC);
    if (vfd < 0) {
        close(hfd);
        return (vfs_status_t)vfd;
    }

    // A 300 MiB contiguous file typically costs one allocator pass,
    // one extent record, and one kernel copy call.
    vfs_status_t s = vfs_import_fd(vfs, vfd, hfd, (uint64_t)st.st_size);

    close(hfd);
    vfs_fclose(vfs, vfd);
    return s;
}
```

---

### 3. Bulk-Exporting to a Host File (`vfs_export_fd`)

The reverse direction — same one-call-per-file shape, holes become sparse zeros:

```c
#include "vfs.h"
#include <fcntl.h>
#include <stdio.h>
#include <unistd.h>

vfs_status_t export_media(vfs_t* vfs, const char* vfs_src, const char* host_dst) {
    vfs_stat_t st;
    vfs_status_t s = vfs_stat(vfs, vfs_src, &st);
    if (s != VFS_OK) return s;

    int hfd = open(host_dst, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
    if (hfd < 0) return VFS_ERR_IO;

    vfs_fd_t vfd = vfs_fopen(vfs, vfs_src, VFS_O_RDONLY);
    if (vfd < 0) {
        close(hfd);
        return (vfs_status_t)vfd;
    }

    size_t sent = 0;
    off_t off = 0;
    s = vfs_export_fd(vfs, hfd, vfd, &off, (size_t)st.size, &sent);

    close(hfd);
    vfs_fclose(vfs, vfd);
    if (s != VFS_OK || sent != (size_t)st.size) {
        unlink(host_dst);
        return (s != VFS_OK) ? s : VFS_ERR_IO;
    }
    return VFS_OK;
}
```

---

### 4. Zero-Copy Kernel Streaming to Sockets (`vfs_sendfile`)

Stream data directly from a VFS virtual file to a host TCP socket or pipe without copying bytes through userspace. This is the API to use whenever the destination is **not** a regular file:

```c
#include "vfs.h"
#include <unistd.h>

void stream_to_client(vfs_t* vfs, int client_socket_fd, const char* virtual_path) {
    vfs_fd_t vfd = vfs_fopen(vfs, virtual_path, VFS_O_RDONLY);
    if (vfd < 0) return;

    vfs_stat_t st;
    vfs_stat(vfs, virtual_path, &st);

    size_t total_sent = 0;
    // Uses Linux sendfile(2) directly between the VFS image and the client socket.
    // offset == NULL: the file cursor is used and advanced.
    vfs_status_t status = vfs_sendfile(vfs, client_socket_fd, vfd, NULL, st.size, &total_sent);

    if (status == VFS_OK) {
        printf("Streamed %zu bytes zero-copy!\n", total_sent);
    }

    vfs_fclose(vfs, vfd);
}
```

Partial-range variant (e.g. HTTP `Range` responses) — pass an explicit offset, which is advanced past the bytes sent while the cursor stays put:

```c
off_t range_off = 1048576;          // start 1 MiB in
size_t sent = 0;
vfs_sendfile(vfs, client_socket_fd, vfd, &range_off, 65536, &sent);
```

---

### 5. Sparse Files, Truncate & Random Access

Growing with `vfs_truncate()` allocates nothing; seeking past EOF and writing leaves a zero-filled hole:

```c
#include "vfs.h"
#include <stdio.h>

// Shrink a log file to its last 1 MiB in place.
vfs_status_t rotate_log(vfs_t* vfs, const char* path) {
    vfs_stat_t st;
    if (vfs_stat(vfs, path, &st) != VFS_OK) return VFS_ERR_NOTFOUND;
    if (st.size <= 1048576) return VFS_OK;

    // Read the tail, truncate to zero, write it back at offset 0.
    vfs_fd_t fd = vfs_fopen(vfs, path, VFS_O_RDWR);
    if (fd < 0) return (vfs_status_t)fd;

    char tail[1048576];
    size_t n = 0;
    vfs_fseek(vfs, fd, -1048576, VFS_SEEK_END, NULL);
    vfs_fread(vfs, fd, tail, sizeof(tail), &n);
    vfs_fclose(vfs, fd);

    if (vfs_truncate(vfs, path, 0) != VFS_OK) return VFS_ERR_IO;
    return vfs_write_file(vfs, path, tail, n);
}

// Pre-grow a 5 MiB sparse placeholder; the gap costs no storage
// and reads back as zeros until overwritten.
vfs_truncate(vfs, "/vm/swap.img", 5u * 1024u * 1024u);
```

---

### 6. Mounting Directly from RAM / Static Memory

Mount an embedded asset array (e.g., compiled binary assets) without touching disk:

```c
#include "vfs.h"
#include <stdlib.h>

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

### 7. Directory Traversal & Pattern Listing

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
    vfs_list(vfs, prefix, print_file, NULL); // "/" or "" lists everything
}
```

---

### 8. Rename, Remove & Stat

```c
#include "vfs.h"
#include <stdio.h>

void manage_namespace(vfs_t* vfs) {
    vfs_stat_t st;
    if (vfs_stat(vfs, "/img/logo.png", &st) == VFS_OK) {
        printf("%s: %lu bytes in %u blocks\n",
               st.path, (unsigned long)st.size, st.block_count);
    }

    // Atomic-style rename; an existing destination is replaced.
    vfs_status_t s = vfs_rename(vfs, "/img/logo.png", "/img/banner.png");
    if (s != VFS_OK) {
        fprintf(stderr, "rename failed: %s\n", vfs_strerror(s));
    }

    if (vfs_exists(vfs, "/tmp/scratch.bin")) {
        vfs_unlink(vfs, "/tmp/scratch.bin"); // frees data + inode immediately
    }
}
```

---

### 9. Multithreaded Server Pattern

Different files need no coordination — hand each connection a file and let the per-inode locks do the rest:

```c
#include "vfs.h"
#include <pthread.h>
#include <stdlib.h>
#include <unistd.h>

typedef struct { vfs_t* vfs; int client_fd; char path[VFS_MAX_PATH]; } job_t;

static void* serve_one(void* arg) {
    job_t* job = arg;
    vfs_fd_t vfd = vfs_fopen(job->vfs, job->path, VFS_O_RDONLY);
    if (vfd >= 0) {
        vfs_stat_t st;
        vfs_stat(job->vfs, job->path, &st);
        size_t sent = 0;
        // Lockless host I/O: concurrent clients on different files
        // proceed in parallel inside the kernel.
        vfs_sendfile(job->vfs, job->client_fd, vfd, NULL, st.size, &sent);
        vfs_fclose(job->vfs, vfd);
    }
    close(job->client_fd);
    free(job);
    return NULL;
}
// Per connection: fill a job_t, pthread_create(&tid, NULL, serve_one, job),
// pthread_detach(tid). A single read-only vfs_t handle is shared safely.
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

`VFS_MAX_INODES` must be within `[1, 65536]`; out-of-range values fail at compile time. The on-disk header (`56,164,352` bytes at default settings) scales with this value, and the heap inode table scales with it in RAM.

---

## Building & Running the Test Suite

The Makefile builds the static library, the test suite, and the CLI (the CLI needs `libsolidc` via `pkg-config`; without it the library and tests still build):

```bash
make            # lib/libvfs.a + bin/vfs_test + bin/vfs-cli
make test       # build (if needed) and run the full suite + benchmarks
make clean      # remove build artifacts
sudo make install   # install libvfs.a + vfs.h + vfs-cli (when available; honours PREFIX/DESTDIR)
```

The comprehensive test suite verifies extent mapping, multi-threaded concurrent I/O, sparse file allocation, `sendfile` transfers, error paths, and throughput benchmarks.

### Sample Benchmark Output

```
[27/28] RUNNING: Benchmark: Sequential Write Speed (512 MiB)...
    Write: 100.0% (  512 MiB) - 0.3 sec, 2028 MiB/s

  Sequential Write Benchmark:
    Write Size   : 512.00 MiB
    Chunk Size   : 256 KiB
    Elapsed Time : 0.2525 seconds
    Throughput   : 2027.57 MiB/sec
[27/28] PASS

[28/28] RUNNING: Benchmark: Sequential Read Speed (512 MiB)...
    Read:  100.0% (  512 MiB) - 0.1 sec, 8238 MiB/s

  Sequential Read Benchmark:
    Read Size    : 512.00 MiB
    Chunk Size   : 256 KiB
    Elapsed Time : 0.0622 seconds
    Throughput   : 8236.48 MiB/sec
[28/28] PASS

===============================================================
Result Summary: 28 of 28 tests/benchmarks passed.
```

Real-world bulk throughput is dominated by the host filesystem: on reflink-capable filesystems (btrfs/xfs, image and source on the same filesystem) multi-gigabyte pack/unpack completes in well under a second; otherwise expect sequential device speed.

---

## License

MIT License. Free for use in commercial, closed-source, and open-source applications.
