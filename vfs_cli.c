/**
 * @file vfs_cli.c
 * @brief Unified CLI for the high-throughput extent-based VFS.
 *
 * @code
 *   vfs create  -c image.vfs
 *   vfs pack    -c image.vfs -d ./assets [--verbose] [-j N]
 *   vfs unpack  -c image.vfs -d ./out    [--verbose] [-j N]
 *   vfs ls      -c image.vfs [prefix]
 *   vfs add     -c image.vfs <host_src> <vfs_dst>
 *   vfs extract -c image.vfs <vfs_src> <host_dst>
 *   vfs rm      -c image.vfs <vfs_path>
 *   vfs mv      -c image.vfs <vfs_old> <vfs_new>
 *   vfs stat    -c image.vfs <vfs_path>
 *   vfs exists  -c image.vfs <vfs_path>
 *   vfs dump    -c image.vfs
 * @endcode
 *
 * Extract/unpack use vfs_sendfile() (VFS → host). Pack/add use a single
 * vfs_import_fd() call per file, which moves each whole VFS extent
 * kernel-side (copy_file_range, reflinked when the filesystem supports it).
 *
 * pack/unpack are multi-threaded: the file list is collected first, then
 * imports/exports run on a fixed-size pthread pool (-j/--threads, default
 * = online CPU count). Work is handed out via a single atomic fetch-add
 * cursor over the pre-collected job array rather than a work-stealing
 * queue: the job list is static and fully known before dispatch, so
 * there is nothing to steal from and no benefit to per-worker deques.
 * A profiled work-stealing pool spent >95% of its cycles spinning in
 * its steal/backoff path under this exact workload; the flat cursor
 * has no spin-wait and no lock in the common case, so workers stay on
 * real I/O instead of scheduler overhead. The library's per-inode
 * locks and lockless host I/O make concurrent file copies safe.
 */

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include <errno.h>
#include <fcntl.h>
#include <pthread.h>  // for pthread_create, pthread_join
#include <stdarg.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

#include <solidc/defer.h>
#include <solidc/filepath.h>
#include <solidc/flags.h>

#include "vfs.h"

#ifndef UNUSED
#define UNUSED(x) ((void)(x))
#endif

#define CLI_MAX_THREADS 64

/* =========================================================================
 * Parallel-for: fixed pthread pool driven by an atomic work cursor.
 *
 * All jobs are known up front (pack/unpack collect the full list before
 * any thread is spawned), so this intentionally skips the machinery of a
 * general-purpose thread pool (submit queue, condvar wakeups, work
 * stealing). Each worker races a single atomic counter for the next job
 * index; there is no queue to contend on and no idle worker ever spins
 * or blocks waiting for stealable work, since the cursor exhausts
 * monotonically and workers simply exit when it does.
 * ======================================================================= */

/** Shared state for one parallel_for() dispatch. */
typedef struct {
    void (*fn)(void* job_array, size_t index, void* ctx); /**< Per-job callback. */
    void* job_array;      /**< Base pointer passed through to fn.           */
    size_t job_count;     /**< Total number of jobs.                       */
    void* ctx;            /**< Opaque context passed through to fn.        */
    atomic_size_t cursor; /**< Next unclaimed job index.                   */
} ParallelForState;

static void* parallel_for_worker(void* arg) {
    ParallelForState* st = arg;
    for (;;) {
        size_t i = atomic_fetch_add_explicit(&st->cursor, 1, memory_order_relaxed);
        if (i >= st->job_count) {
            return NULL;
        }
        st->fn(st->job_array, i, st->ctx);
    }
}

/**
 * Runs fn(job_array, i, ctx) for i in [0, job_count) across nthreads
 * worker threads, blocking until all jobs complete.
 *
 * @param job_array Opaque base pointer forwarded to every fn() call;
 *                  fn is responsible for indexing into it.
 * @param job_count Number of jobs; if zero, returns immediately.
 * @param nthreads  Number of worker threads to spawn (>= 1). Clamped by
 *                  the caller before this is invoked.
 * @param ctx       Opaque context forwarded to every fn() call.
 * @param fn        Per-job callback; must be safe for concurrent use
 *                  from multiple threads, since jobs run in parallel.
 * @return true on success; false if a worker thread could not be
 *         created, in which case any already-spawned workers are still
 *         joined before returning (partial completion is possible).
 */
static bool parallel_for(void* job_array, size_t job_count, int nthreads, void* ctx,
                         void (*fn)(void* job_array, size_t index, void* ctx)) {
    if (job_count == 0) {
        return true;
    }
    if (nthreads < 1) {
        nthreads = 1;
    }
    /* Never spawn more workers than jobs; extra threads would just race
     * the cursor to immediate exit. */
    size_t max_useful = job_count < (size_t)nthreads ? job_count : (size_t)nthreads;
    int n = (int)max_useful;

    ParallelForState st = {
        .fn = fn,
        .job_array = job_array,
        .job_count = job_count,
        .ctx = ctx,
        .cursor = 0,
    };

    if (n <= 1) {
        parallel_for_worker(&st);
        return true;
    }

    pthread_t* threads = malloc((size_t)n * sizeof(pthread_t));
    if (!threads) {
        return false;
    }

    int spawned = 0;
    bool ok = true;
    for (int i = 0; i < n; i++) {
        if (pthread_create(&threads[i], NULL, parallel_for_worker, &st) != 0) {
            ok = false;
            break;
        }
        spawned++;
    }
    for (int i = 0; i < spawned; i++) {
        pthread_join(threads[i], NULL);
    }
    free(threads);

    if (!ok) {
        parallel_for_worker(&st);
    }
    return ok;
}

/* =========================================================================
 * App context
 * ======================================================================= */

typedef struct {
    FlagParser* root; /**< Root parser; used to resolve active subcommand. */
    int exit_code;
} AppCtx;

static void app_fail(AppCtx* ctx, const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    fputs("error: ", stderr);
    vfprintf(stderr, fmt, ap);
    fputc('\n', stderr);
    va_end(ap);
    ctx->exit_code = EXIT_FAILURE;
}

static FlagParser* active_sub(AppCtx* ctx) { return flag_active_subcommand(ctx->root); }

static const char* pos_at(AppCtx* ctx, int index) {
    FlagParser* sub = active_sub(ctx);
    if (!sub || index < 0 || index >= flag_positional_count(sub)) {
        return NULL;
    }
    return flag_positional_at(sub, index);
}

static int pos_count(AppCtx* ctx) {
    FlagParser* sub = active_sub(ctx);
    return sub ? flag_positional_count(sub) : 0;
}

/* =========================================================================
 * Path / time helpers
 * ======================================================================= */

static bool normalize_vfs_path(const char* path, char* out, size_t out_sz) {
    if (!path || !path[0] || !out || out_sz < 2) {
        return false;
    }
    if (path[0] == '/') {
        size_t n = strlen(path);
        if (n + 1 > out_sz || n >= VFS_MAX_PATH) {
            return false;
        }
        memcpy(out, path, n + 1);
        return true;
    }
    size_t n = strlen(path);
    if (n + 2 > out_sz || n + 1 >= VFS_MAX_PATH) {
        return false;
    }
    out[0] = '/';
    memcpy(out + 1, path, n + 1);
    return true;
}

static bool host_to_vfs_path(const char* host_path, const char* root, size_t root_len, char* out,
                             size_t out_sz) {
    const char* rel = host_path;
    if (root_len > 0 && strncmp(host_path, root, root_len) == 0) {
        rel = host_path + root_len;
    }
    while (*rel == '/') {
        rel++;
    }
    if (*rel == '\0') {
        if (out_sz < 2) {
            return false;
        }
        out[0] = '/';
        out[1] = '\0';
        return true;
    }
    return normalize_vfs_path(rel, out, out_sz);
}

static int mkdir_parents(const char* host_path) {
    char tmp[4096];
    size_t len = strlen(host_path);
    if (len == 0 || len >= sizeof(tmp)) {
        return -1;
    }
    memcpy(tmp, host_path, len + 1);
    for (char* p = tmp + 1; *p; p++) {
        if (*p != '/') {
            continue;
        }
        *p = '\0';
        if (mkdir(tmp, 0755) < 0 && errno != EEXIST) {
            return -1;
        }
        *p = '/';
    }
    return 0;
}

static void format_time(time_t t, char* buf, size_t n) {
    struct tm* tm_info = localtime(&t);
    if (tm_info) {
        strftime(buf, n, "%Y-%m-%d %H:%M:%S", tm_info);
    } else {
        snprintf(buf, n, "unknown");
    }
}

/**
 * Thin wrapper over vfs_open(). The library no longer uses mmap for the
 * inode table (explicit pread/pwrite instead), so no pre-sizing for mmap
 * is needed here: a short/truncated image correctly reports CORRUPT/IO
 * instead of raising SIGBUS. Host imports already stream via buffered
 * read() (never mmap), so shrinking host files also fail as IO errors.
 */
static vfs_status_t safe_vfs_open(const char* image_path, bool readonly, vfs_t** out_vfs) {
    return vfs_open(image_path, readonly, out_vfs);
}

/* =========================================================================
 * Copy helpers (host → VFS / VFS → host)
 * ======================================================================= */

/**
 * Imports a host file into the VFS. Uses fstat() directly on the open file
 * descriptor to eliminate symlink/size race conditions, then moves the
 * bytes with a single vfs_import_fd() call: each whole VFS extent travels
 * via one kernel-side copy_file_range() (metadata-only reflink when the
 * filesystem supports it), never via mmap -- so a host file that shrinks
 * concurrently fails as an IO error instead of raising SIGBUS.
 */
static vfs_status_t import_host_file(vfs_t* vfs, const char* host_path, const char* vfs_path,
                                      uint8_t* io_buf, size_t io_buf_sz) {
    UNUSED(io_buf);
    UNUSED(io_buf_sz);
    int oflags = O_RDONLY | O_CLOEXEC;
#ifdef O_NOATIME
    oflags |= O_NOATIME;
#endif
    int hfd = open(host_path, oflags);
    if (hfd < 0 && errno == EPERM) {
        hfd = open(host_path, O_RDONLY | O_CLOEXEC);
    }
    if (hfd < 0) {
        return VFS_ERR_IO;
    }

    struct stat st;
    if (fstat(hfd, &st) < 0) {
        close(hfd);
        return VFS_ERR_IO;
    }
    if (!S_ISREG(st.st_mode)) {
        close(hfd);
        return S_ISDIR(st.st_mode) ? VFS_ERR_ISDIR : VFS_OK;
    }

    uint64_t size = (uint64_t)st.st_size;

    vfs_fd_t vfd = vfs_fopen(vfs, vfs_path, VFS_O_WRONLY | VFS_O_CREAT | VFS_O_TRUNC);
    if (vfd < 0) {
        close(hfd);
        return (vfs_status_t)vfd;
    }

    vfs_status_t status = VFS_OK;
    if (size > 0) {
#ifdef POSIX_FADV_SEQUENTIAL
        (void)posix_fadvise(hfd, 0, (off_t)size, POSIX_FADV_SEQUENTIAL);
#endif
#ifdef POSIX_FADV_NOREUSE
        (void)posix_fadvise(hfd, 0, (off_t)size, POSIX_FADV_NOREUSE);
#endif
        status = vfs_import_fd(vfs, vfd, hfd, size);
    }

    close(hfd);
    vfs_fclose(vfs, vfd);
    return status;
}

/** Exports one VFS file of known size to the host using vfs_sendfile(). */
static vfs_status_t export_vfs_file_sized(vfs_t* vfs, const char* vfs_path, const char* host_path,
                                          uint64_t size) {
    /* Fast path: try opening directly first. Only create parent directories
     * if open fails with ENOENT (eliminates redundant mkdir syscalls). */
    int hfd = open(host_path, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
    if (hfd < 0 && errno == ENOENT) {
        if (mkdir_parents(host_path) == 0) {
            hfd = open(host_path, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
        }
    }
    if (hfd < 0) {
        return VFS_ERR_IO;
    }

    if (size == 0) {
        close(hfd);
        return VFS_OK;
    }

    vfs_fd_t vfd = vfs_fopen(vfs, vfs_path, VFS_O_RDONLY);
    if (vfd < 0) {
        close(hfd);
        return (vfs_status_t)vfd;
    }

    size_t sent = 0;
    off_t off = 0;
    vfs_status_t s = vfs_export_fd(vfs, hfd, vfd, &off, (size_t)size, &sent);
    close(hfd);
    vfs_fclose(vfs, vfd);

    if (s != VFS_OK || sent != (size_t)size) {
        unlink(host_path);
        return (s != VFS_OK) ? s : VFS_ERR_IO;
    }
    return VFS_OK;
}

/** Export one VFS file to the host using vfs_sendfile(). */
static vfs_status_t export_vfs_file(vfs_t* vfs, const char* vfs_path, const char* host_path) {
    vfs_stat_t st;
    vfs_status_t s = vfs_stat(vfs, vfs_path, &st);
    if (s != VFS_OK) {
        return s;
    }
    return export_vfs_file_sized(vfs, vfs_path, host_path, st.size);
}

/* =========================================================================
 * Flag value storage (bound at registration; valid for process lifetime)
 *
 * --container/-c is a single persistent (global) flag registered once on
 * the root parser and inherited by every subcommand, accepted either
 * before or after the subcommand name. Per-command handlers validate its
 * presence themselves so commands that need no image (e.g. completion)
 * keep working.
 * ======================================================================= */

static char* f_container = NULL;

static char* f_pack_d = NULL;
static bool f_pack_v = false;
static int f_pack_j = 0; /**< Worker threads; 0 = auto (online CPU count). */

static char* f_unpack_d = NULL;
static bool f_unpack_v = false;
static int f_unpack_j = 0; /**< Worker threads; 0 = auto (online CPU count). */

/* =========================================================================
 * create
 * ======================================================================= */

static void cmd_create(void* ud) {
    AppCtx* ctx = ud;
    if (!f_container || !f_container[0]) {
        app_fail(ctx, "create requires -c/--container");
        return;
    }
    vfs_t* vfs = NULL;
    vfs_status_t s = vfs_create(f_container, &vfs);
    if (s != VFS_OK) {
        app_fail(ctx, "vfs_create(%s): %s", f_container, vfs_strerror(s));
        return;
    }
    vfs_close(vfs);
    printf("created %s\n", f_container);
}

/* =========================================================================
 * pack  (parallel: collect file list, then import via parallel_for)
 * ======================================================================= */

/** One queued host→VFS import. */
typedef struct {
    char* host_path;             /**< strdup'd host source path.          */
    char vfs_path[VFS_MAX_PATH]; /**< Destination path inside the image.  */
    uint64_t size;               /**< Size at walk time (buffer sizing).  */
} PackJob;

typedef struct {
    PackJob* jobs;
    size_t count;
    size_t cap;
    const char* root;
    size_t root_len;
    bool verbose;
} PackCollector;

static bool pack_jobs_reserve(PackCollector* c, size_t need) {
    if (c->count + need <= c->cap) {
        return true;
    }
    size_t new_cap = c->cap == 0 ? 1024 : c->cap;
    while (new_cap < c->count + need) {
        new_cap *= 2;
    }
    PackJob* grown = realloc(c->jobs, new_cap * sizeof(PackJob));
    if (!grown) {
        return false;
    }
    c->jobs = grown;
    c->cap = new_cap;
    return true;
}

static WalkDirOption pack_collect_cb(const FileAttributes* attr, const char* path, const char* name,
                                     void* userdata) {
    UNUSED(name);
    if (fattr_is_dir(attr)) {
        return DirContinue;
    }
    PackCollector* c = userdata;
    if (!pack_jobs_reserve(c, 1)) {
        fprintf(stderr, "out of memory collecting file list\n");
        return DirStop;
    }
    PackJob* job = &c->jobs[c->count];
    job->host_path = strdup(path);
    if (!job->host_path) {
        fprintf(stderr, "out of memory\n");
        return DirStop;
    }
    if (!host_to_vfs_path(path, c->root, c->root_len, job->vfs_path, sizeof(job->vfs_path))) {
        fprintf(stderr, "path too long: %s\n", path);
        free(job->host_path);
        job->host_path = NULL;
        return DirContinue;
    }
    job->size = (uint64_t)attr->size;
    c->count++;
    return DirContinue;
}

typedef struct {
    _Atomic size_t num_files;
    _Atomic size_t num_errors;
    _Atomic uint64_t total_bytes;
} PackProgress;

/** Shared, read-only-after-setup context for every pack_task_fn() call. */
typedef struct {
    vfs_t* vfs;
    bool verbose;
    PackProgress* progress;
} PackRunCtx;

static void pack_task_fn(void* job_array, size_t index, void* ctx_v) {
    PackJob* job = &((PackJob*)job_array)[index];
    PackRunCtx* ctx = ctx_v;
    PackProgress* p = ctx->progress;

    /* No per-file userspace buffer: vfs_import_fd() moves whole extents
     * kernel-side (copy_file_range), allocating its 1 MiB fallback buffer
     * lazily and only on filesystems without copy offload. */
    vfs_status_t s = import_host_file(ctx->vfs, job->host_path, job->vfs_path, NULL, 0);
    if (s != VFS_OK) {
        fprintf(stderr, "pack %s -> %s: %s\n", job->host_path, job->vfs_path, vfs_strerror(s));
        atomic_fetch_add_explicit(&p->num_errors, 1, memory_order_relaxed);
        return;
    }
    uint64_t bytes = job->size;
    atomic_fetch_add_explicit(&p->total_bytes, bytes, memory_order_relaxed);
    size_t n = atomic_fetch_add_explicit(&p->num_files, 1, memory_order_relaxed) + 1;
    if (ctx->verbose) {
        printf("(%zu) %s -> %s (%llu bytes)\n", n, job->host_path, job->vfs_path,
               (unsigned long long)bytes);
    }
}

static int cli_thread_count(int requested) {
    if (requested > 0) {
        return (requested > CLI_MAX_THREADS) ? CLI_MAX_THREADS : requested;
    }
    long n = sysconf(_SC_NPROCESSORS_ONLN);
    if (n < 1) {
        n = 1;
    }
    return (n > CLI_MAX_THREADS) ? CLI_MAX_THREADS : (int)n;
}

static void cmd_pack(void* ud) {
    AppCtx* ctx = ud;
    if (!f_container || !f_pack_d) {
        app_fail(ctx, "pack requires -c/--container and -d/--dir");
        return;
    }

    vfs_t* vfs = NULL;
    vfs_status_t s = vfs_create(f_container, &vfs);
    if (s != VFS_OK) {
        app_fail(ctx, "vfs_create(%s): %s", f_container, vfs_strerror(s));
        return;
    }
    defer { vfs_close(vfs); };

    size_t root_len = strlen(f_pack_d);
    while (root_len > 1 && f_pack_d[root_len - 1] == '/') {
        root_len--;
    }

    PackCollector collector = {.root = f_pack_d, .root_len = root_len, .verbose = f_pack_v};
    defer {
        for (size_t i = 0; i < collector.count; i++) {
            free(collector.jobs[i].host_path);
        }
        free(collector.jobs);
    };
    dir_walk(f_pack_d, pack_collect_cb, &collector);

    int nthreads = cli_thread_count(f_pack_j);
    PackProgress progress = {
        .num_files = 0,
        .num_errors = 0,
        .total_bytes = 0,
    };
    PackRunCtx run_ctx = {.vfs = vfs, .verbose = f_pack_v, .progress = &progress};

    if (!parallel_for(collector.jobs, collector.count, nthreads, &run_ctx, pack_task_fn)) {
        app_fail(ctx, "failed to spawn worker threads");
        return;
    }

    size_t num_files = atomic_load(&progress.num_files);
    size_t num_errors = atomic_load(&progress.num_errors);
    uint64_t total_bytes = atomic_load(&progress.total_bytes);
    fprintf(stderr, "packed %zu file(s), %llu byte(s) into %s", num_files,
            (unsigned long long)total_bytes, f_container);
    if (num_errors) {
        fprintf(stderr, " (%zu error(s))\n", num_errors);
        ctx->exit_code = EXIT_FAILURE;
        return;
    }
    fputc('\n', stderr);
}

/* =========================================================================
 * unpack  (parallel: collect entries via vfs_list, then export via
 * parallel_for using the kernel sendfile path per file)
 * ======================================================================= */

/** One queued VFS→host export. */
typedef struct {
    char vfs_path[VFS_MAX_PATH]; /**< Source path inside the image. */
    char* host_path;             /**< strdup'd host destination path. */
    uint64_t size;               /**< Logical size from the listing.  */
} UnpackJob;

typedef struct {
    UnpackJob* jobs;
    size_t count;
    size_t cap;
    const char* out_root;
    size_t num_walk_errors;
} UnpackCollector;

static bool unpack_jobs_reserve(UnpackCollector* c, size_t need) {
    if (c->count + need <= c->cap) {
        return true;
    }
    size_t new_cap = c->cap == 0 ? 1024 : c->cap;
    while (new_cap < c->count + need) {
        new_cap *= 2;
    }
    UnpackJob* grown = realloc(c->jobs, new_cap * sizeof(UnpackJob));
    if (!grown) {
        return false;
    }
    c->jobs = grown;
    c->cap = new_cap;
    return true;
}

static bool unpack_collect_cb(const char* path, const vfs_stat_t* st, void* userdata) {
    UnpackCollector* c = userdata;
    const char* rel = path;
    while (*rel == '/') {
        rel++;
    }

    if (!unpack_jobs_reserve(c, 1)) {
        fprintf(stderr, "out of memory collecting file list\n");
        return false;
    }
    UnpackJob* job = &c->jobs[c->count];
    snprintf(job->vfs_path, sizeof(job->vfs_path), "%s", path);

    char host_path[4096];
    int n = snprintf(host_path, sizeof(host_path), "%s/%s", c->out_root, rel);
    if (n < 0 || (size_t)n >= sizeof(host_path)) {
        fprintf(stderr, "host path too long for %s\n", path);
        c->num_walk_errors++;
        return true;
    }
    job->host_path = strdup(host_path);
    if (!job->host_path) {
        fprintf(stderr, "out of memory\n");
        return false;
    }
    job->size = st->size;
    c->count++;
    return true;
}

typedef struct {
    _Atomic size_t num_files;
    _Atomic size_t num_errors;
    _Atomic uint64_t total_bytes;
} UnpackProgress;

/** Shared, read-only-after-setup context for every unpack_task_fn() call. */
typedef struct {
    vfs_t* vfs;
    bool verbose;
    UnpackProgress* progress;
} UnpackRunCtx;

static void unpack_task_fn(void* job_array, size_t index, void* ctx_v) {
    UnpackJob* job = &((UnpackJob*)job_array)[index];
    UnpackRunCtx* ctx = ctx_v;
    UnpackProgress* p = ctx->progress;

    vfs_status_t s = export_vfs_file_sized(ctx->vfs, job->vfs_path, job->host_path, job->size);
    if (s != VFS_OK) {
        fprintf(stderr, "unpack %s -> %s: %s\n", job->vfs_path, job->host_path, vfs_strerror(s));
        atomic_fetch_add_explicit(&p->num_errors, 1, memory_order_relaxed);
        return;
    }
    atomic_fetch_add_explicit(&p->total_bytes, job->size, memory_order_relaxed);
    size_t n = atomic_fetch_add_explicit(&p->num_files, 1, memory_order_relaxed) + 1;
    if (ctx->verbose) {
        printf("(%zu) %s -> %s (%llu bytes)\n", n, job->vfs_path, job->host_path,
               (unsigned long long)job->size);
    }
}

static void cmd_unpack(void* ud) {
    AppCtx* ctx = ud;
    if (!f_container || !f_unpack_d) {
        app_fail(ctx, "unpack requires -c/--container and -d/--dir");
        return;
    }
    if (mkdir(f_unpack_d, 0755) < 0 && errno != EEXIST) {
        app_fail(ctx, "mkdir(%s): %s", f_unpack_d, strerror(errno));
        return;
    }

    vfs_t* vfs = NULL;
    vfs_status_t s = safe_vfs_open(f_container, true, &vfs);
    if (s != VFS_OK) {
        app_fail(ctx, "vfs_open(%s): %s", f_container, vfs_strerror(s));
        return;
    }
    defer { vfs_close(vfs); };

    UnpackCollector collector = {.out_root = f_unpack_d};
    defer {
        for (size_t i = 0; i < collector.count; i++) {
            free(collector.jobs[i].host_path);
        }
        free(collector.jobs);
    };
    vfs_list(vfs, "/", unpack_collect_cb, &collector);

    UnpackProgress progress = {
        .num_files = 0,
        .num_errors = 0,
        .total_bytes = 0,
    };

    int nthreads = cli_thread_count(f_unpack_j);
    UnpackRunCtx run_ctx = {.vfs = vfs, .verbose = f_unpack_v, .progress = &progress};

    if (!parallel_for(collector.jobs, collector.count, nthreads, &run_ctx, unpack_task_fn)) {
        app_fail(ctx, "failed to spawn worker threads");
        return;
    }

    fprintf(stderr, "unpacked %zu file(s), %llu byte(s) to %s", atomic_load(&progress.num_files),
            (unsigned long long)atomic_load(&progress.total_bytes), f_unpack_d);
    size_t num_errors = atomic_load(&progress.num_errors) + collector.num_walk_errors;
    if (num_errors) {
        fprintf(stderr, " (%zu error(s))\n", num_errors);
        ctx->exit_code = EXIT_FAILURE;
        return;
    }
    fputc('\n', stderr);
}

/* =========================================================================
 * ls
 * ======================================================================= */

static bool ls_cb(const char* path, const vfs_stat_t* st, void* userdata) {
    UNUSED(userdata);
    char tbuf[32];
    format_time(st->modified_at, tbuf, sizeof(tbuf));
    printf("%12llu  %s  %s\n", (unsigned long long)st->size, tbuf, path);
    return true;
}

static void cmd_ls(void* ud) {
    AppCtx* ctx = ud;
    if (!f_container) {
        app_fail(ctx, "ls requires -c/--container");
        return;
    }
    const char* prefix = "/";
    if (pos_count(ctx) >= 1) {
        prefix = pos_at(ctx, 0);
    }

    vfs_t* vfs = NULL;
    vfs_status_t s = safe_vfs_open(f_container, true, &vfs);
    if (s != VFS_OK) {
        app_fail(ctx, "vfs_open: %s", vfs_strerror(s));
        return;
    }
    defer { vfs_close(vfs); };

    printf("%12s  %-19s  %s\n", "SIZE", "MODIFIED", "PATH");
    printf("--------------------------------------------------------------------\n");
    vfs_list(vfs, prefix, ls_cb, NULL);
}

/* =========================================================================
 * add
 * ======================================================================= */

static void cmd_add(void* ud) {
    AppCtx* ctx = ud;
    if (!f_container) {
        app_fail(ctx, "add requires -c/--container");
        return;
    }
    if (pos_count(ctx) != 2) {
        app_fail(ctx, "usage: vfs add -c <image> <host_src> <vfs_dst>");
        return;
    }
    const char* host_src = pos_at(ctx, 0);
    char vpath[VFS_MAX_PATH];
    if (!normalize_vfs_path(pos_at(ctx, 1), vpath, sizeof(vpath))) {
        app_fail(ctx, "invalid or too-long vfs path");
        return;
    }

    vfs_t* vfs = NULL;
    vfs_status_t s = safe_vfs_open(f_container, false, &vfs);
    if (s != VFS_OK) {
        app_fail(ctx, "vfs_open: %s", vfs_strerror(s));
        return;
    }
    defer { vfs_close(vfs); };

    s = import_host_file(vfs, host_src, vpath, NULL, 0);
    if (s != VFS_OK) {
        app_fail(ctx, "import %s -> %s: %s", host_src, vpath, vfs_strerror(s));
        return;
    }
    printf("imported %s -> %s\n", host_src, vpath);
}

/* =========================================================================
 * extract  (vfs_sendfile)
 * ======================================================================= */

static void cmd_extract(void* ud) {
    AppCtx* ctx = ud;
    if (!f_container) {
        app_fail(ctx, "extract requires -c/--container");
        return;
    }
    if (pos_count(ctx) != 2) {
        app_fail(ctx, "usage: vfs extract -c <image> <vfs_src> <host_dst>");
        return;
    }
    char vpath[VFS_MAX_PATH];
    if (!normalize_vfs_path(pos_at(ctx, 0), vpath, sizeof(vpath))) {
        app_fail(ctx, "invalid or too-long vfs path");
        return;
    }
    const char* host_dst = pos_at(ctx, 1);

    vfs_t* vfs = NULL;
    vfs_status_t s = safe_vfs_open(f_container, true, &vfs);
    if (s != VFS_OK) {
        app_fail(ctx, "vfs_open: %s", vfs_strerror(s));
        return;
    }
    defer { vfs_close(vfs); };

    s = export_vfs_file(vfs, vpath, host_dst);
    if (s != VFS_OK) {
        app_fail(ctx, "extract %s -> %s: %s", vpath, host_dst, vfs_strerror(s));
        return;
    }
    printf("extracted %s -> %s\n", vpath, host_dst);
}

/* =========================================================================
 * rm / mv / stat / exists / dump
 * ======================================================================= */

static void cmd_rm(void* ud) {
    AppCtx* ctx = ud;
    if (!f_container) {
        app_fail(ctx, "rm requires -c/--container");
        return;
    }
    if (pos_count(ctx) != 1) {
        app_fail(ctx, "usage: vfs rm -c <image> <vfs_path>");
        return;
    }
    char vpath[VFS_MAX_PATH];
    if (!normalize_vfs_path(pos_at(ctx, 0), vpath, sizeof(vpath))) {
        app_fail(ctx, "invalid vfs path");
        return;
    }

    vfs_t* vfs = NULL;
    vfs_status_t s = safe_vfs_open(f_container, false, &vfs);
    if (s != VFS_OK) {
        app_fail(ctx, "vfs_open: %s", vfs_strerror(s));
        return;
    }
    s = vfs_unlink(vfs, vpath);
    vfs_close(vfs);
    if (s != VFS_OK) {
        app_fail(ctx, "unlink(%s): %s", vpath, vfs_strerror(s));
        return;
    }
    printf("removed %s\n", vpath);
}

static void cmd_mv(void* ud) {
    AppCtx* ctx = ud;
    if (!f_container) {
        app_fail(ctx, "mv requires -c/--container");
        return;
    }
    if (pos_count(ctx) != 2) {
        app_fail(ctx, "usage: vfs mv -c <image> <vfs_old> <vfs_new>");
        return;
    }

    char oldp[VFS_MAX_PATH], newp[VFS_MAX_PATH];
    if (!normalize_vfs_path(pos_at(ctx, 0), oldp, sizeof(oldp)) ||
        !normalize_vfs_path(pos_at(ctx, 1), newp, sizeof(newp))) {
        app_fail(ctx, "invalid vfs path");
        return;
    }

    vfs_t* vfs = NULL;
    vfs_status_t s = safe_vfs_open(f_container, false, &vfs);
    if (s != VFS_OK) {
        app_fail(ctx, "vfs_open: %s", vfs_strerror(s));
        return;
    }
    s = vfs_rename(vfs, oldp, newp);
    vfs_close(vfs);
    if (s != VFS_OK) {
        app_fail(ctx, "rename %s -> %s: %s", oldp, newp, vfs_strerror(s));
        return;
    }
    printf("renamed %s -> %s\n", oldp, newp);
}

static void cmd_stat(void* ud) {
    AppCtx* ctx = ud;
    if (!f_container) {
        app_fail(ctx, "stat requires -c/--container");
        return;
    }
    if (pos_count(ctx) != 1) {
        app_fail(ctx, "usage: vfs stat -c <image> <vfs_path>");
        return;
    }
    char vpath[VFS_MAX_PATH];
    if (!normalize_vfs_path(pos_at(ctx, 0), vpath, sizeof(vpath))) {
        app_fail(ctx, "invalid vfs path");
        return;
    }

    vfs_t* vfs = NULL;
    vfs_status_t s = safe_vfs_open(f_container, true, &vfs);
    if (s != VFS_OK) {
        app_fail(ctx, "vfs_open: %s", vfs_strerror(s));
        return;
    }
    vfs_stat_t st;
    s = vfs_stat(vfs, vpath, &st);
    vfs_close(vfs);
    if (s != VFS_OK) {
        app_fail(ctx, "stat(%s): %s", vpath, vfs_strerror(s));
        return;
    }

    char tc[32], tm[32];
    format_time(st.created_at, tc, sizeof(tc));
    format_time(st.modified_at, tm, sizeof(tm));
    printf("path        : %s\n", st.path);
    printf("size        : %llu bytes\n", (unsigned long long)st.size);
    printf("blocks      : %u\n", st.block_count);
    printf("created     : %s\n", tc);
    printf("modified    : %s\n", tm);
}

static void cmd_exists(void* ud) {
    AppCtx* ctx = ud;
    if (!f_container) {
        app_fail(ctx, "exists requires -c/--container");
        return;
    }
    if (pos_count(ctx) != 1) {
        app_fail(ctx, "usage: vfs exists -c <image> <vfs_path>");
        return;
    }
    char vpath[VFS_MAX_PATH];
    if (!normalize_vfs_path(pos_at(ctx, 0), vpath, sizeof(vpath))) {
        app_fail(ctx, "invalid vfs path");
        return;
    }

    vfs_t* vfs = NULL;
    vfs_status_t s = safe_vfs_open(f_container, true, &vfs);
    if (s != VFS_OK) {
        app_fail(ctx, "vfs_open: %s", vfs_strerror(s));
        return;
    }
    bool ok = vfs_exists(vfs, vpath);
    vfs_close(vfs);
    if (ok) {
        printf("exists\n");
    } else {
        printf("missing\n");
        ctx->exit_code = EXIT_FAILURE; /* convenient for scripts */
    }
}

static void cmd_dump(void* ud) {
    AppCtx* ctx = ud;
    if (!f_container) {
        app_fail(ctx, "dump requires -c/--container");
        return;
    }
    vfs_t* vfs = NULL;
    vfs_status_t s = safe_vfs_open(f_container, true, &vfs);
    if (s != VFS_OK) {
        app_fail(ctx, "vfs_open: %s", vfs_strerror(s));
        return;
    }
    vfs_dump(vfs, stdout);
    vfs_close(vfs);
}

/* =========================================================================
 * main — register subcommands
 * ======================================================================= */

int main(int argc, char* argv[]) {
    FlagParser* root =
        flag_parser_new("vfs", "Command-line tool for the extent-based virtual filesystem");
    if (!root) {
        return EXIT_FAILURE;
    }
    defer { flag_parser_free(root); };

    flag_parser_set_footer(root,
                           "Examples:\n"
                           "  vfs create  -c app.vfs\n"
                           "  vfs pack    -c app.vfs -d ./assets\n"
                           "  vfs unpack  -c app.vfs -d ./out\n"
                           "  vfs ls      -c app.vfs /\n"
                           "  vfs add     -c app.vfs ./logo.png /img/logo.png\n"
                           "  vfs extract -c app.vfs /img/logo.png ./logo.png\n"
                           "  vfs mv      -c app.vfs /a /b\n"
                           "  vfs rm      -c app.vfs /b\n");

    /* Global image flag, inherited by every subcommand (accepted before
     * or after the subcommand name). Optional at parse time so commands
     * like completion keep working; each handler validates it. */
    flag_persistent_string(root, "container", 'c', "Path of the VFS image file", &f_container);

    /* --- create --- */
    flag_add_subcommand(root, "create", "Create an empty VFS image", cmd_create);

    /* --- pack --- */
    {
        FlagParser* sub = flag_add_subcommand(
            root, "pack", "Create image and pack a host directory into it", cmd_pack);
        flag_req_string(sub, "dir", 'd', "Host directory to pack", &f_pack_d);
        flag_bool(sub, "verbose", 'v', "Print each packed file", &f_pack_v);
        flag_int(sub, "threads", 'j', "Worker threads (0 = all online CPUs, default 0)", &f_pack_j);
    }

    /* --- unpack --- */
    {
        FlagParser* sub = flag_add_subcommand(
            root, "unpack", "Extract all files from an image to a host directory", cmd_unpack);
        flag_req_string(sub, "dir", 'd', "Host output directory", &f_unpack_d);
        flag_bool(sub, "verbose", 'v', "Print each extracted file", &f_unpack_v);
        flag_int(sub, "threads", 'j', "Worker threads (0 = all online CPUs, default 0)",
                 &f_unpack_j);
    }

    /* --- ls --- */
    flag_add_subcommand(root, "ls", "List files (optional path prefix)", cmd_ls);

    /* --- add --- */
    flag_add_subcommand(root, "add", "Import one host file into the image", cmd_add);

    /* --- extract --- */
    flag_add_subcommand(root, "extract", "Export one VFS file to the host (vfs_sendfile)",
                        cmd_extract);

    /* --- rm --- */
    flag_add_subcommand(root, "rm", "Remove a file from the image", cmd_rm);

    /* --- mv --- */
    flag_add_subcommand(root, "mv", "Rename/move a path inside the image", cmd_mv);

    /* --- stat --- */
    flag_add_subcommand(root, "stat", "Show metadata for a VFS path", cmd_stat);

    /* --- exists --- */
    flag_add_subcommand(root, "exists", "Test whether a VFS path exists (exit 0/1)", cmd_exists);

    /* --- dump --- */
    flag_add_subcommand(root, "dump", "Print superblock and inode table diagnostics", cmd_dump);

    flag_add_completion_cmd(root);

    AppCtx ctx = {.root = root, .exit_code = EXIT_SUCCESS};

    FlagStatus st = flag_parse_and_invoke(root, argc, argv, &ctx);
    if (st != FLAG_OK) {
        fprintf(stderr, "error: %s\n", flag_get_error(root));
        flag_print_usage(root);
        return EXIT_FAILURE;
    }

    /* No subcommand selected (e.g. bare `vfs` or only global flags). */
    if (!flag_active_subcommand(root)) {
        flag_print_usage(root);
        return EXIT_FAILURE;
    }

    return ctx.exit_code;
}
