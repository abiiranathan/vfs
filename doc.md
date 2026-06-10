# Single-File Virtual Filesystem (VFS)

This document provides a detailed overview of the architecture, on-disk layout, memory management, and execution flows of the single-file Virtual Filesystem (VFS) implementation.

---

## 1. High-Level Architecture

The VFS is a flat-namespace virtual filesystem contained entirely within a single host image file. It exposes a file-descriptor-based interface (similar to POSIX) to interact with logical files. 

```mermaid
graph TD
    subgraph Client Application
        App[App Code]
    end

    subgraph VFS Library Runtime
        API[Public API: vfs_fopen, vfs_fwrite, vfs_fread...]
        Mutex[(Coarse Global Mutex)]
        OFT[Open-File Table: 64 Slots]
        InMemInodes[In-Memory Inode Table: 1024 Inodes]
        InMemSB[In-Memory Superblock]
    end

    subgraph Host OS
        FD[Host File Descriptor]
        Disk[Host File System Image]
    end

    App -->|Calls API| API
    API -->|Acquires| Mutex
    API -->|Translates Offset/Flags| OFT
    API -->|Manages Metadata| InMemInodes
    API -->|Manages Blocks| InMemSB
    API -->|I/O System Calls: pread/pwrite| FD
    FD -->|Persists Data| Disk
```

---

## 2. On-Disk Layout

The host storage image is divided into three contiguous, non-overlapping physical regions:
1.  **Superblock Region**: Fixed at `65,536` bytes. Contains filesystem metrics and the block allocation bitmap.
2.  **Inode Table**: Holds `1,024` static, fixed-size directory entries (`vfs_inode_t`), each occupying `1,312` bytes.
3.  **Data Region**: Subdivided into `262,144` physical data blocks of `4,096` bytes each.

```mermaid
gantt
    title Virtual Disk Geometry (Offset Mapping)
    dateFormat  X
    axisFormat %s
    section Regions
    Superblock Region (64 KiB) :active, 0, 65536
    Inode Table (1.28 MiB)    :crit, 65536, 1409024
    Data Blocks (1.00 GiB)    :normal, 1409024, 1075150848
```

### 2.1 Offset and Sizing Calculations

*   **Superblock (`VFS_SUPERBLOCK_SIZE`)**: `65,536` bytes ($64\text{ KiB}$).
*   **Inode Table Offset (`VFS_INODE_TABLE_OFFSET`)**: `65,536` bytes.
*   **Inode Size**: Each `vfs_inode_t` is exactly `1,312` bytes:
    $$\text{sizeof(vfs\_inode\_t)} = \underbrace{256}_{\text{path}} + \underbrace{8}_{\text{size}} + \underbrace{16}_{\text{timestamps}} + \underbrace{4}_{\text{block\_count}} + \underbrace{1024}_{\text{block pointer array}} + \underbrace{4}_{\text{padding}} = 1,312\text{ bytes}$$
*   **Total Inodes**: $1,024$ entries $\implies$ Inode table occupies $1,024 \times 1,312 = 1,343,488\text{ bytes}$ ($1.28\text{ MiB}$).
*   **Data Region Offset (`VFS_DATA_OFFSET`)**: Starts at physical offset `1,409,024` ($65,536 + 1,343,488$).
*   **Total Addressable Data Blocks (`VFS_TOTAL_BLOCKS`)**: $1,024 \times 256 = 262,144$ blocks. At $4,096$ bytes per block, the maximum data payload capacity is $1,073,741,824$ bytes ($1\text{ GiB}$).

---

## 3. In-Memory Representation

The in-memory handle (`vfs_t`) coordinates system runtime structures, metadata caches, and hardware interaction channels.

```mermaid
classDiagram
    class vfs_t {
        +int fd
        +bool readonly
        +pthread_mutex_t lock
        +vfs_super_t super
        +vfs_inode_t inodes[1024]
        +open_file_t oft[64]
    }
    class vfs_super_t {
        +uint32_t magic
        +uint32_t version
        +uint32_t block_size
        +uint32_t max_inodes
        +uint32_t total_blocks
        +uint32_t free_block_count
        +uint32_t free_inode_count
        +uint32_t bitmap[8192]
    }
    class vfs_inode_t {
        +char path[256]
        +uint64_t size
        +uint64_t created_at
        +uint64_t modified_at
        +uint32_t block_count
        +uint32_t blocks[256]
    }
    class open_file_t {
        +int inode_idx
        +off_t pos
        +unsigned int flags
    }

    vfs_t *-- vfs_super_t : contains
    vfs_t *-- vfs_inode_t : caches table
    vfs_t *-- open_file_t : tracks
```

### 3.1 Metadata Caching Strategy
*   **Superblock and Inodes**: Loaded entirely into memory during `vfs_open()`. Written back collectively upon calling `vfs_sync()` or `vfs_close()`. Standard file creation/truncation updates the individual cached entries in memory and flushes only the affected inode block to disk via `inode_write_locked()`.
*   **Data Blocks**: No memory cache is maintained for file payloads. Reads and writes bypass memory structures and execute directly against the host file using `pread()` and `pwrite()`.

---

## 4. Key Design Mechanics

### 4.1 Locking Strategy & Deadlock Prevention
The entire filesystem operates under coarse-grained locking using a `pthread_mutex_t` located inside `vfs_t`.
*   Public API functions (e.g., `vfs_fopen`, `vfs_fwrite`) acquire the lock immediately upon entry and release it prior to exit.
*   Internal functions (suffixed with `_locked`) rely on the caller to maintain the locked state.
*   **Re-entrancy Protection**: During path traversal in `vfs_list()`, the system releases `vfs->lock` before invoking the user-provided callback function. This prevents a deadlock if the callback attempts to invoke another synchronized VFS function (e.g., `vfs_stat` or `vfs_fread`) from the same thread.

### 4.2 Block Allocation Bitmap
Block status is evaluated using a bit array where `1` represents a free block and `0` indicates an allocated block.
*   **Bit-level indexing**: For physical block ID $N$:
    $$\text{Word index} = \lfloor N / 32 \rfloor, \quad \text{Bit shift} = N \pmod{32}$$
*   On creation, all bitmap words are initialized to `0xFFFFFFFF` (all blocks free).

---

## 5. Execution Flows

### 5.1 File Write Flow (`vfs_fwrite`)

This sequence diagram illustrates the boundary interactions and disk updates performed during a logical write operation.

```mermaid
sequenceDiagram
    autonumber
    participant App as Client Application
    participant VFS as vfs_fwrite (API)
    participant Lock as pthread_mutex
    participant Alloc as block_alloc_locked
    participant Disk as Host Disk File (pread/pwrite)

    App->>VFS: vfs_fwrite(vfs, fd, buf, count, &written)
    VFS->>Lock: lock(&vfs->lock)
    Note over VFS: Validate FD & Flags<br/>Apply Appends
    
    loop Until Count Met or Out of Space
        Note over VFS: Calc Logical Block Index
        alt Block Index >= Current Allocations
            VFS->>Alloc: block_alloc_locked()
            Alloc-->>VFS: return new physical block ID (e.g., blk_id)
            VFS->>Disk: block_zero_locked(blk_id)
            Note over VFS: Record block ID in inode->blocks
        end
        VFS->>Disk: pwrite_all(offset + block_off, segment_bytes)
    end

    Note over VFS: Update Inode Size & Modification Time
    VFS->>Disk: inode_write_locked() (Persists Inode)
    VFS->>Lock: unlock(&vfs->lock)
    VFS-->>App: return Status (e.g., VFS_OK)
```

### 5.2 File Reading and Sparse-Block Handling (`vfs_fread`)

When a read operation targets an address outside the physical bounds of an allocated block range (but within the file size limit, such as inside a sparse file), the implementation routes virtual zeroes back to the application buffer rather than reading uninitialized or random data from disk.

```mermaid
flowchart TD
    A[Start vfs_fread] --> B[Acquire vfs->lock]
    B --> C{Offset >= File Size?}
    C -- Yes --> D[Unlock & Return 0 Bytes Read]
    C -- No --> E[Clamp Read Count to Remaining File Size]
    E --> F[Calculate Logical Block Index]
    F --> G{Logical Index >= Inode's Block Count?}
    G -- Yes [Sparse Region] --> H[Zero out Destination Buffer for Block Segment]
    G -- No [Allocated Region] --> I[Resolve Physical Block Address via Inode Block List]
    I --> J[Execute pread_all on Disk Block Offset]
    H --> K[Advance Pos Cursor and Remaining Count]
    J --> K
    K --> L{More Bytes to Read?}
    L -- Yes --> F
    L -- No --> M[Unlock vfs->lock]
    M --> N[Return Status]
```
