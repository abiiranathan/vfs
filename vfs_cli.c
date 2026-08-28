/**
 * @file vfs_cli.c
 * @brief Unified CLI for the high-throughput extent-based VFS.
 *
 * @code
 *   vfs create  -c image.vfs
 *   vfs pack    -c image.vfs -d ./assets [--verbose]
 *   vfs unpack  -c image.vfs -d ./out    [--verbose]
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
 * Extract/unpack use vfs_sendfile() (VFS → host). Pack/add use mmap or a
 * buffered read into vfs_fwrite() (host → VFS).
 */

#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
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

#define CLI_MMAP_THRESHOLD (256u * 1024u)
#define CLI_IO_BUF_SIZE    (1024u * 1024u)

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

/** Active subcommand parser (after flag_parse_and_invoke). */
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

static bool host_to_vfs_path(const char* host_path, const char* root, size_t root_len, char* out, size_t out_sz) {
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

/* =========================================================================
 * Copy helpers (host → VFS / VFS → host)
 * ======================================================================= */

static vfs_status_t copy_host_fd_to_vfs(vfs_t* vfs, vfs_fd_t vfd, int host_fd, size_t size, uint8_t* io_buf) {
    if (size == 0) {
        return VFS_OK;
    }

    if (size >= CLI_MMAP_THRESHOLD) {
        void* src = mmap(NULL, size, PROT_READ, MAP_PRIVATE, host_fd, 0);
        if (src == MAP_FAILED) {
            return VFS_ERR_IO;
        }
#ifdef POSIX_MADV_SEQUENTIAL
        (void)posix_madvise(src, size, POSIX_MADV_SEQUENTIAL);
#elif defined(MADV_SEQUENTIAL)
        (void)madvise(src, size, MADV_SEQUENTIAL);
#endif
        size_t written = 0;
        vfs_status_t st = vfs_fwrite(vfs, vfd, src, size, &written);
        munmap(src, size);
        if (st != VFS_OK) {
            return st;
        }
        return (written == size) ? VFS_OK : VFS_ERR_IO;
    }

    size_t remaining = size;
    while (remaining > 0) {
        size_t chunk = remaining < CLI_IO_BUF_SIZE ? remaining : CLI_IO_BUF_SIZE;
        size_t got = 0;
        while (got < chunk) {
            ssize_t n = read(host_fd, io_buf + got, chunk - got);
            if (n < 0) {
                if (errno == EINTR) {
                    continue;
                }
                return VFS_ERR_IO;
            }
            if (n == 0) {
                return VFS_ERR_IO;
            }
            got += (size_t)n;
        }
        size_t written = 0;
        vfs_status_t st = vfs_fwrite(vfs, vfd, io_buf, got, &written);
        if (st != VFS_OK || written != got) {
            return (st != VFS_OK) ? st : VFS_ERR_IO;
        }
        remaining -= got;
    }
    return VFS_OK;
}

static vfs_status_t import_host_file(vfs_t* vfs, const char* host_path, const char* vfs_path, uint8_t* io_buf) {
    struct stat st;
    if (stat(host_path, &st) < 0) {
        return VFS_ERR_IO;
    }
    if (S_ISDIR(st.st_mode)) {
        return VFS_ERR_ISDIR;
    }

    vfs_fd_t vfd = vfs_fopen(vfs, vfs_path, VFS_O_WRONLY | VFS_O_CREAT | VFS_O_TRUNC);
    if (vfd < 0) {
        return (vfs_status_t)vfd;
    }

    vfs_status_t status = VFS_OK;
    if (st.st_size > 0) {
        int hfd = open(host_path, O_RDONLY);
        if (hfd < 0) {
            vfs_fclose(vfs, vfd);
            return VFS_ERR_IO;
        }
#ifdef POSIX_FADV_SEQUENTIAL
        (void)posix_fadvise(hfd, 0, st.st_size, POSIX_FADV_SEQUENTIAL);
#endif
        status = copy_host_fd_to_vfs(vfs, vfd, hfd, (size_t)st.st_size, io_buf);
        close(hfd);
    }
    vfs_fclose(vfs, vfd);
    return status;
}

/** Export one VFS file to the host using vfs_sendfile(). */
static vfs_status_t export_vfs_file(vfs_t* vfs, const char* vfs_path, const char* host_path) {
    vfs_stat_t st;
    vfs_status_t s = vfs_stat(vfs, vfs_path, &st);
    if (s != VFS_OK) {
        return s;
    }
    if (mkdir_parents(host_path) < 0) {
        return VFS_ERR_IO;
    }

    vfs_fd_t vfd = vfs_fopen(vfs, vfs_path, VFS_O_RDONLY);
    if (vfd < 0) {
        return (vfs_status_t)vfd;
    }

    int hfd = open(host_path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (hfd < 0) {
        vfs_fclose(vfs, vfd);
        return VFS_ERR_IO;
    }

    size_t sent = 0;
    off_t off = 0;
    s = vfs_sendfile(vfs, hfd, vfd, &off, (size_t)st.size, &sent);
    close(hfd);
    vfs_fclose(vfs, vfd);

    if (s != VFS_OK || sent != (size_t)st.size) {
        unlink(host_path);
        return (s != VFS_OK) ? s : VFS_ERR_IO;
    }
    return VFS_OK;
}

/* =========================================================================
 * Flag value storage (bound at registration; valid for process lifetime)
 * ======================================================================= */

static char* f_create_c = NULL;

static char* f_pack_c = NULL;
static char* f_pack_d = NULL;
static bool f_pack_v = false;

static char* f_unpack_c = NULL;
static char* f_unpack_d = NULL;
static bool f_unpack_v = false;

static char* f_ls_c = NULL;

static char* f_add_c = NULL;
static char* f_extract_c = NULL;
static char* f_rm_c = NULL;
static char* f_mv_c = NULL;
static char* f_stat_c = NULL;
static char* f_exists_c = NULL;
static char* f_dump_c = NULL;

/* =========================================================================
 * create
 * ======================================================================= */

static void cmd_create(void* ud) {
    AppCtx* ctx = ud;
    if (!f_create_c || !f_create_c[0]) {
        app_fail(ctx, "create requires -c/--container");
        return;
    }
    vfs_t* vfs = NULL;
    vfs_status_t s = vfs_create(f_create_c, &vfs);
    if (s != VFS_OK) {
        app_fail(ctx, "vfs_create(%s): %s", f_create_c, vfs_strerror(s));
        return;
    }
    vfs_close(vfs);
    printf("created %s\n", f_create_c);
}

/* =========================================================================
 * pack
 * ======================================================================= */

typedef struct {
    vfs_t* vfs;
    const char* root;
    size_t root_len;
    size_t num_files;
    size_t num_errors;
    uint64_t total_bytes;
    bool verbose;
    uint8_t* io_buf;
} PackCtx;

static WalkDirOption pack_cb(const FileAttributes* attr, const char* path, const char* name, void* userdata) {
    UNUSED(name);
    if (fattr_is_dir(attr)) {
        return DirContinue;
    }
    PackCtx* d = userdata;
    char vpath[VFS_MAX_PATH];
    if (!host_to_vfs_path(path, d->root, d->root_len, vpath, sizeof(vpath))) {
        fprintf(stderr, "path too long: %s\n", path);
        d->num_errors++;
        return DirContinue;
    }
    vfs_status_t s = import_host_file(d->vfs, path, vpath, d->io_buf);
    if (s != VFS_OK) {
        fprintf(stderr, "pack %s -> %s: %s\n", path, vpath, vfs_strerror(s));
        d->num_errors++;
        return DirContinue;
    }
    d->num_files++;
    d->total_bytes += (uint64_t)attr->size;
    if (d->verbose) {
        printf("(%zu) %s -> %s (%llu bytes)\n", d->num_files, path, vpath, (unsigned long long)attr->size);
    }
    return DirContinue;
}

static void cmd_pack(void* ud) {
    AppCtx* ctx = ud;
    if (!f_pack_c || !f_pack_d) {
        app_fail(ctx, "pack requires -c/--container and -d/--dir");
        return;
    }

    uint8_t* io_buf = malloc(CLI_IO_BUF_SIZE);
    if (!io_buf) {
        app_fail(ctx, "out of memory");
        return;
    }
    defer { free(io_buf); };

    vfs_t* vfs = NULL;
    vfs_status_t s = vfs_create(f_pack_c, &vfs);
    if (s != VFS_OK) {
        app_fail(ctx, "vfs_create(%s): %s", f_pack_c, vfs_strerror(s));
        return;
    }
    defer { vfs_close(vfs); };

    size_t root_len = strlen(f_pack_d);
    while (root_len > 1 && f_pack_d[root_len - 1] == '/') {
        root_len--;
    }

    PackCtx pc = {
        .vfs = vfs,
        .root = f_pack_d,
        .root_len = root_len,
        .verbose = f_pack_v,
        .io_buf = io_buf,
    };
    dir_walk(f_pack_d, pack_cb, &pc);

    fprintf(stderr, "packed %zu file(s), %llu byte(s) into %s", pc.num_files, (unsigned long long)pc.total_bytes,
            f_pack_c);
    if (pc.num_errors) {
        fprintf(stderr, " (%zu error(s))\n", pc.num_errors);
        ctx->exit_code = EXIT_FAILURE;
        return;
    }
    fputc('\n', stderr);
}

/* =========================================================================
 * unpack  (vfs_sendfile per file)
 * ======================================================================= */

typedef struct {
    vfs_t* vfs;
    const char* out_root;
    size_t num_files;
    size_t num_errors;
    uint64_t total_bytes;
    bool verbose;
} UnpackCtx;

static bool unpack_cb(const char* path, const vfs_stat_t* st, void* userdata) {
    UnpackCtx* d = userdata;
    const char* rel = path;
    while (*rel == '/') {
        rel++;
    }

    char host_path[4096];
    int n = snprintf(host_path, sizeof(host_path), "%s/%s", d->out_root, rel);
    if (n < 0 || (size_t)n >= sizeof(host_path)) {
        fprintf(stderr, "host path too long for %s\n", path);
        d->num_errors++;
        return true;
    }

    vfs_status_t s = export_vfs_file(d->vfs, path, host_path);
    if (s != VFS_OK) {
        fprintf(stderr, "unpack %s -> %s: %s\n", path, host_path, vfs_strerror(s));
        d->num_errors++;
        return true;
    }
    d->num_files++;
    d->total_bytes += st->size;
    if (d->verbose) {
        printf("(%zu) %s -> %s (%llu bytes)\n", d->num_files, path, host_path, (unsigned long long)st->size);
    }
    return true;
}

static void cmd_unpack(void* ud) {
    AppCtx* ctx = ud;
    if (!f_unpack_c || !f_unpack_d) {
        app_fail(ctx, "unpack requires -c/--container and -d/--dir");
        return;
    }
    if (mkdir(f_unpack_d, 0755) < 0 && errno != EEXIST) {
        app_fail(ctx, "mkdir(%s): %s", f_unpack_d, strerror(errno));
        return;
    }

    vfs_t* vfs = NULL;
    vfs_status_t s = vfs_open(f_unpack_c, true, &vfs);
    if (s != VFS_OK) {
        app_fail(ctx, "vfs_open(%s): %s", f_unpack_c, vfs_strerror(s));
        return;
    }
    defer { vfs_close(vfs); };

    UnpackCtx uc = {.vfs = vfs, .out_root = f_unpack_d, .verbose = f_unpack_v};
    vfs_list(vfs, "/", unpack_cb, &uc);

    fprintf(stderr, "unpacked %zu file(s), %llu byte(s) to %s", uc.num_files, (unsigned long long)uc.total_bytes,
            f_unpack_d);
    if (uc.num_errors) {
        fprintf(stderr, " (%zu error(s))\n", uc.num_errors);
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
    if (!f_ls_c) {
        app_fail(ctx, "ls requires -c/--container");
        return;
    }
    const char* prefix = "/";
    if (pos_count(ctx) >= 1) {
        prefix = pos_at(ctx, 0);
    }

    vfs_t* vfs = NULL;
    vfs_status_t s = vfs_open(f_ls_c, true, &vfs);
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
    if (!f_add_c) {
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

    uint8_t* io_buf = malloc(CLI_IO_BUF_SIZE);
    if (!io_buf) {
        app_fail(ctx, "out of memory");
        return;
    }
    defer { free(io_buf); };

    vfs_t* vfs = NULL;
    vfs_status_t s = vfs_open(f_add_c, false, &vfs);
    if (s != VFS_OK) {
        app_fail(ctx, "vfs_open: %s", vfs_strerror(s));
        return;
    }
    defer { vfs_close(vfs); };

    s = import_host_file(vfs, host_src, vpath, io_buf);
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
    if (!f_extract_c) {
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
    vfs_status_t s = vfs_open(f_extract_c, true, &vfs);
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
    if (!f_rm_c) {
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
    vfs_status_t s = vfs_open(f_rm_c, false, &vfs);
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
    if (!f_mv_c) {
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
    vfs_status_t s = vfs_open(f_mv_c, false, &vfs);
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
    if (!f_stat_c) {
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
    vfs_status_t s = vfs_open(f_stat_c, true, &vfs);
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
    if (!f_exists_c) {
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
    vfs_status_t s = vfs_open(f_exists_c, true, &vfs);
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
    if (!f_dump_c) {
        app_fail(ctx, "dump requires -c/--container");
        return;
    }
    vfs_t* vfs = NULL;
    vfs_status_t s = vfs_open(f_dump_c, true, &vfs);
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
    FlagParser* root = flag_parser_new("vfs", "Command-line tool for the extent-based virtual filesystem");
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

    /* --- create --- */
    {
        FlagParser* sub = flag_add_subcommand(root, "create", "Create an empty VFS image", cmd_create);
        flag_req_string(sub, "container", 'c', "Path of the new image file", &f_create_c);
    }

    /* --- pack --- */
    {
        FlagParser* sub = flag_add_subcommand(root, "pack", "Create image and pack a host directory into it", cmd_pack);
        flag_req_string(sub, "container", 'c', "Output VFS image path", &f_pack_c);
        flag_req_string(sub, "dir", 'd', "Host directory to pack", &f_pack_d);
        flag_bool(sub, "verbose", 'v', "Print each packed file", &f_pack_v);
    }

    /* --- unpack --- */
    {
        FlagParser* sub =
            flag_add_subcommand(root, "unpack", "Extract all files from an image to a host directory", cmd_unpack);
        flag_req_string(sub, "container", 'c', "VFS image path", &f_unpack_c);
        flag_req_string(sub, "dir", 'd', "Host output directory", &f_unpack_d);
        flag_bool(sub, "verbose", 'v', "Print each extracted file", &f_unpack_v);
    }

    /* --- ls --- */
    {
        FlagParser* sub = flag_add_subcommand(root, "ls", "List files (optional path prefix)", cmd_ls);
        flag_req_string(sub, "container", 'c', "VFS image path", &f_ls_c);
    }

    /* --- add --- */
    {
        FlagParser* sub = flag_add_subcommand(root, "add", "Import one host file into the image", cmd_add);
        flag_req_string(sub, "container", 'c', "VFS image path", &f_add_c);
    }

    /* --- extract --- */
    {
        FlagParser* sub =
            flag_add_subcommand(root, "extract", "Export one VFS file to the host (vfs_sendfile)", cmd_extract);
        flag_req_string(sub, "container", 'c', "VFS image path", &f_extract_c);
    }

    /* --- rm --- */
    {
        FlagParser* sub = flag_add_subcommand(root, "rm", "Remove a file from the image", cmd_rm);
        flag_req_string(sub, "container", 'c', "VFS image path", &f_rm_c);
    }

    /* --- mv --- */
    {
        FlagParser* sub = flag_add_subcommand(root, "mv", "Rename/move a path inside the image", cmd_mv);
        flag_req_string(sub, "container", 'c', "VFS image path", &f_mv_c);
    }

    /* --- stat --- */
    {
        FlagParser* sub = flag_add_subcommand(root, "stat", "Show metadata for a VFS path", cmd_stat);
        flag_req_string(sub, "container", 'c', "VFS image path", &f_stat_c);
    }

    /* --- exists --- */
    {
        FlagParser* sub = flag_add_subcommand(root, "exists", "Test whether a VFS path exists (exit 0/1)", cmd_exists);
        flag_req_string(sub, "container", 'c', "VFS image path", &f_exists_c);
    }

    /* --- dump --- */
    {
        FlagParser* sub = flag_add_subcommand(root, "dump", "Print superblock and inode table diagnostics", cmd_dump);
        flag_req_string(sub, "container", 'c', "VFS image path", &f_dump_c);
    }

    // Register completions on root.
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
