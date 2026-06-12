/**
 * @file vfs_test.c
 * @brief Comprehensive test suite verifying indirect addressing, allocation boundaries, and I/O throughput.
 */

#include "vfs.h"
#include <fcntl.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#define TEST_IMAGE "test_system.vfs"

#define VFS_ASSERT_TRUE(expr)                                                                   \
    do {                                                                                        \
        if (!(expr)) {                                                                          \
            fprintf(stderr, "[FAIL] %s:%d: Assertion failed: %s\n", __FILE__, __LINE__, #expr); \
            return false;                                                                       \
        }                                                                                       \
    } while (0)

#define VFS_ASSERT_STATUS_OK(expr)                                                                                   \
    do {                                                                                                             \
        vfs_status_t __res = (expr);                                                                                 \
        if (__res != VFS_OK) {                                                                                       \
            fprintf(stderr, "[FAIL] %s:%d: Expected VFS_OK, got %s (%d)\n", __FILE__, __LINE__, vfs_strerror(__res), \
                    __res);                                                                                          \
            return false;                                                                                            \
        }                                                                                                            \
    } while (0)

#define VFS_ASSERT_STATUS_EQ(expr, expected)                                                            \
    do {                                                                                                \
        vfs_status_t __res = (expr);                                                                    \
        if (__res != (expected)) {                                                                      \
            fprintf(stderr, "[FAIL] %s:%d: Expected status %s (%d), got %s (%d)\n", __FILE__, __LINE__, \
                    vfs_strerror(expected), expected, vfs_strerror(__res), __res);                      \
            return false;                                                                               \
        }                                                                                               \
    } while (0)

#define VFS_ASSERT_FD_OK(expr)                                                                          \
    do {                                                                                                \
        vfs_fd_t __fd = (expr);                                                                         \
        if (__fd < 0) {                                                                                 \
            fprintf(stderr, "[FAIL] %s:%d: Expected valid FD, got error %s (%d)\n", __FILE__, __LINE__, \
                    vfs_strerror((vfs_status_t)__fd), __fd);                                            \
            return false;                                                                               \
        }                                                                                               \
    } while (0)

static void cleanup_image(void) {
    unlink(TEST_IMAGE);
}

static bool test_lifecycle_create_open_close(void) {
    vfs_t* vfs = NULL;
    cleanup_image();

    VFS_ASSERT_STATUS_EQ(vfs_open(TEST_IMAGE, false, &vfs), VFS_ERR_IO);

    VFS_ASSERT_STATUS_OK(vfs_create(TEST_IMAGE, &vfs));
    VFS_ASSERT_TRUE(vfs != NULL);

    VFS_ASSERT_STATUS_OK(vfs_sync(vfs));
    vfs_close(vfs);

    VFS_ASSERT_STATUS_OK(vfs_open(TEST_IMAGE, false, &vfs));
    VFS_ASSERT_TRUE(vfs != NULL);

    vfs_close(vfs);
    return true;
}

static bool test_file_create_and_basic_write_read(void) {
    vfs_t* vfs = NULL;
    cleanup_image();
    VFS_ASSERT_STATUS_OK(vfs_create(TEST_IMAGE, &vfs));

    const char* path = "/doc.txt";
    VFS_ASSERT_TRUE(!vfs_exists(vfs, path));

    vfs_fd_t fd = vfs_fopen(vfs, path, VFS_O_RDONLY);
    VFS_ASSERT_STATUS_EQ((vfs_status_t)fd, VFS_ERR_NOTFOUND);

    fd = vfs_fopen(vfs, path, VFS_O_CREAT | VFS_O_RDWR);
    VFS_ASSERT_FD_OK(fd);
    VFS_ASSERT_TRUE(vfs_exists(vfs, path));

    const char* data = "Virtual File System Test Data Payload";
    size_t len = strlen(data);
    size_t written = 0;
    VFS_ASSERT_STATUS_OK(vfs_fwrite(vfs, fd, data, len, &written));
    VFS_ASSERT_TRUE(written == len);

    off_t pos = 0;
    VFS_ASSERT_STATUS_OK(vfs_ftell(vfs, fd, &pos));
    VFS_ASSERT_TRUE(pos == (off_t)len);

    off_t new_pos = 0;
    VFS_ASSERT_STATUS_OK(vfs_fseek(vfs, fd, 0, VFS_SEEK_SET, &new_pos));
    VFS_ASSERT_TRUE(new_pos == 0);

    char buffer[128];
    memset(buffer, 0, sizeof(buffer));
    size_t read_bytes = 0;
    VFS_ASSERT_STATUS_OK(vfs_fread(vfs, fd, buffer, len, &read_bytes));
    VFS_ASSERT_TRUE(read_bytes == len);
    VFS_ASSERT_TRUE(strcmp(buffer, data) == 0);

    vfs_stat_t st;
    VFS_ASSERT_STATUS_OK(vfs_stat(vfs, path, &st));
    VFS_ASSERT_TRUE(st.size == len);
    VFS_ASSERT_TRUE(st.block_count == 1);

    VFS_ASSERT_STATUS_OK(vfs_fclose(vfs, fd));
    vfs_close(vfs);
    return true;
}

static bool test_read_write_large_file(void) {
    vfs_t* vfs = NULL;
    cleanup_image();
    VFS_ASSERT_STATUS_OK(vfs_create(TEST_IMAGE, &vfs));

    /* 3 logical blocks */
    size_t size = 12288;
    uint8_t* out_data = malloc(size);
    uint8_t* in_data = malloc(size);
    VFS_ASSERT_TRUE(out_data != NULL && in_data != NULL);

    for (size_t i = 0; i < size; i++) {
        out_data[i] = (uint8_t)(i % 256);
    }

    vfs_fd_t fd = vfs_fopen(vfs, "/large.bin", VFS_O_CREAT | VFS_O_RDWR);
    VFS_ASSERT_FD_OK(fd);

    size_t written = 0;
    VFS_ASSERT_STATUS_OK(vfs_fwrite(vfs, fd, out_data, size, &written));
    VFS_ASSERT_TRUE(written == size);

    vfs_stat_t st;
    VFS_ASSERT_STATUS_OK(vfs_stat(vfs, "/large.bin", &st));
    VFS_ASSERT_TRUE(st.size == size);
    VFS_ASSERT_TRUE(st.block_count == 3);

    VFS_ASSERT_STATUS_OK(vfs_fseek(vfs, fd, 0, VFS_SEEK_SET, NULL));
    size_t read_bytes = 0;
    VFS_ASSERT_STATUS_OK(vfs_fread(vfs, fd, in_data, size, &read_bytes));
    VFS_ASSERT_TRUE(read_bytes == size);
    VFS_ASSERT_TRUE(memcmp(in_data, out_data, size) == 0);

    VFS_ASSERT_STATUS_OK(vfs_fclose(vfs, fd));
    free(out_data);
    free(in_data);
    vfs_close(vfs);
    return true;
}

static bool test_append_mode(void) {
    vfs_t* vfs = NULL;
    cleanup_image();
    VFS_ASSERT_STATUS_OK(vfs_create(TEST_IMAGE, &vfs));

    const char* path = "/log.txt";
    vfs_fd_t fd = vfs_fopen(vfs, path, VFS_O_CREAT | VFS_O_RDWR);
    VFS_ASSERT_FD_OK(fd);

    size_t written;
    VFS_ASSERT_STATUS_OK(vfs_fwrite(vfs, fd, "Hello ", 6, &written));
    VFS_ASSERT_STATUS_OK(vfs_fclose(vfs, fd));

    fd = vfs_fopen(vfs, path, VFS_O_WRONLY | VFS_O_APPEND);
    VFS_ASSERT_FD_OK(fd);

    VFS_ASSERT_STATUS_OK(vfs_fseek(vfs, fd, 0, VFS_SEEK_SET, NULL));
    VFS_ASSERT_STATUS_OK(vfs_fwrite(vfs, fd, "World!", 6, &written));
    VFS_ASSERT_STATUS_OK(vfs_fclose(vfs, fd));

    fd = vfs_fopen(vfs, path, VFS_O_RDONLY);
    VFS_ASSERT_FD_OK(fd);
    char buf[32] = {0};
    size_t read_bytes;
    VFS_ASSERT_STATUS_OK(vfs_fread(vfs, fd, buf, sizeof(buf), &read_bytes));
    VFS_ASSERT_TRUE(read_bytes == 12);
    VFS_ASSERT_TRUE(strcmp(buf, "Hello World!") == 0);

    VFS_ASSERT_STATUS_OK(vfs_fclose(vfs, fd));
    vfs_close(vfs);
    return true;
}

static bool test_sparse_reads_and_seek_past_eof(void) {
    vfs_t* vfs = NULL;
    cleanup_image();
    VFS_ASSERT_STATUS_OK(vfs_create(TEST_IMAGE, &vfs));

    const char* path = "/sparse.bin";
    vfs_fd_t fd = vfs_fopen(vfs, path, VFS_O_CREAT | VFS_O_RDWR);
    VFS_ASSERT_FD_OK(fd);

    VFS_ASSERT_STATUS_OK(vfs_truncate(vfs, path, 8192));

    vfs_stat_t st;
    VFS_ASSERT_STATUS_OK(vfs_stat(vfs, path, &st));
    VFS_ASSERT_TRUE(st.size == 8192);

    VFS_ASSERT_STATUS_OK(vfs_fseek(vfs, fd, 4000, VFS_SEEK_SET, NULL));
    char buf[100];
    memset(buf, 0xFF, sizeof(buf));
    size_t read_bytes = 0;
    VFS_ASSERT_STATUS_OK(vfs_fread(vfs, fd, buf, sizeof(buf), &read_bytes));
    VFS_ASSERT_TRUE(read_bytes == sizeof(buf));

    for (size_t i = 0; i < sizeof(buf); i++) {
        VFS_ASSERT_TRUE(buf[i] == 0);
    }

    VFS_ASSERT_STATUS_OK(vfs_fclose(vfs, fd));
    vfs_close(vfs);
    return true;
}

static bool test_truncate_shrink_and_extend(void) {
    vfs_t* vfs = NULL;
    cleanup_image();
    VFS_ASSERT_STATUS_OK(vfs_create(TEST_IMAGE, &vfs));

    const char* path = "/resize.dat";
    vfs_fd_t fd = vfs_fopen(vfs, path, VFS_O_CREAT | VFS_O_RDWR);
    VFS_ASSERT_FD_OK(fd);

    uint8_t* pattern = malloc(6000);
    memset(pattern, 'A', 6000);
    size_t written;
    VFS_ASSERT_STATUS_OK(vfs_fwrite(vfs, fd, pattern, 6000, &written));
    VFS_ASSERT_STATUS_OK(vfs_fclose(vfs, fd));

    VFS_ASSERT_STATUS_OK(vfs_truncate(vfs, path, 3000));
    vfs_stat_t st;
    VFS_ASSERT_STATUS_OK(vfs_stat(vfs, path, &st));
    VFS_ASSERT_TRUE(st.size == 3000);
    VFS_ASSERT_TRUE(st.block_count == 1);

    VFS_ASSERT_STATUS_OK(vfs_truncate(vfs, path, 10000));
    VFS_ASSERT_STATUS_OK(vfs_stat(vfs, path, &st));
    VFS_ASSERT_TRUE(st.size == 10000);
    VFS_ASSERT_TRUE(st.block_count == 3);

    fd = vfs_fopen(vfs, path, VFS_O_RDONLY);
    VFS_ASSERT_FD_OK(fd);
    VFS_ASSERT_STATUS_OK(vfs_fseek(vfs, fd, 3000, VFS_SEEK_SET, NULL));
    uint8_t check_buf[100];
    size_t read_bytes;
    VFS_ASSERT_STATUS_OK(vfs_fread(vfs, fd, check_buf, sizeof(check_buf), &read_bytes));
    VFS_ASSERT_TRUE(read_bytes == sizeof(check_buf));
    for (size_t i = 0; i < sizeof(check_buf); i++) {
        VFS_ASSERT_TRUE(check_buf[i] == 0);
    }

    /* Verify overflow threshold is adjusted for indirect addressing */
    VFS_ASSERT_STATUS_EQ(vfs_truncate(vfs, path, 5000000000ULL), VFS_ERR_OVERFLOW);

    VFS_ASSERT_STATUS_OK(vfs_fclose(vfs, fd));
    free(pattern);
    vfs_close(vfs);
    return true;
}

static bool test_unlink(void) {
    vfs_t* vfs = NULL;
    cleanup_image();
    VFS_ASSERT_STATUS_OK(vfs_create(TEST_IMAGE, &vfs));

    const char* path = "/killme.bin";
    vfs_fd_t fd = vfs_fopen(vfs, path, VFS_O_CREAT | VFS_O_RDWR);
    VFS_ASSERT_FD_OK(fd);
    VFS_ASSERT_STATUS_OK(vfs_fclose(vfs, fd));

    VFS_ASSERT_TRUE(vfs_exists(vfs, path));

    VFS_ASSERT_STATUS_OK(vfs_unlink(vfs, path));
    VFS_ASSERT_TRUE(!vfs_exists(vfs, path));

    VFS_ASSERT_STATUS_EQ(vfs_unlink(vfs, path), VFS_ERR_NOTFOUND);

    vfs_close(vfs);
    return true;
}

static bool test_read_only_mode(void) {
    vfs_t* vfs = NULL;
    cleanup_image();

    VFS_ASSERT_STATUS_OK(vfs_create(TEST_IMAGE, &vfs));
    vfs_fd_t fd = vfs_fopen(vfs, "/fixed.txt", VFS_O_CREAT | VFS_O_WRONLY);
    VFS_ASSERT_FD_OK(fd);
    size_t written;
    VFS_ASSERT_STATUS_OK(vfs_fwrite(vfs, fd, "constant", 8, &written));
    VFS_ASSERT_STATUS_OK(vfs_fclose(vfs, fd));
    vfs_close(vfs);

    VFS_ASSERT_STATUS_OK(vfs_open(TEST_IMAGE, true, &vfs));

    vfs_fd_t bad_fd = vfs_fopen(vfs, "/newfile.txt", VFS_O_CREAT | VFS_O_WRONLY);
    VFS_ASSERT_STATUS_EQ((vfs_status_t)bad_fd, VFS_ERR_READONLY);

    VFS_ASSERT_STATUS_EQ(vfs_truncate(vfs, "/fixed.txt", 0), VFS_ERR_READONLY);
    VFS_ASSERT_STATUS_EQ(vfs_unlink(vfs, "/fixed.txt"), VFS_ERR_READONLY);

    fd = vfs_fopen(vfs, "/fixed.txt", VFS_O_RDONLY);
    VFS_ASSERT_FD_OK(fd);
    char buf[16] = {0};
    size_t read_bytes;
    VFS_ASSERT_STATUS_OK(vfs_fread(vfs, fd, buf, 8, &read_bytes));
    VFS_ASSERT_TRUE(strcmp(buf, "constant") == 0);

    VFS_ASSERT_STATUS_OK(vfs_fclose(vfs, fd));
    vfs_close(vfs);
    return true;
}

static bool test_rename(void) {
    vfs_t* vfs = NULL;
    cleanup_image();
    VFS_ASSERT_STATUS_OK(vfs_create(TEST_IMAGE, &vfs));

    const char* src = "/data/original.bin";
    const char* dst = "/data/renamed.bin";
    const char* victim = "/data/victim.bin";

    VFS_ASSERT_STATUS_EQ(vfs_rename(vfs, src, dst), VFS_ERR_NOTFOUND);

    const char payload[] = "rename-test-content";
    size_t payload_len = sizeof(payload) - 1u;
    VFS_ASSERT_STATUS_OK(write_vfs_file(vfs, src, payload, payload_len));
    VFS_ASSERT_TRUE(vfs_exists(vfs, src));

    VFS_ASSERT_STATUS_OK(vfs_rename(vfs, src, dst));

    VFS_ASSERT_TRUE(!vfs_exists(vfs, src));
    VFS_ASSERT_TRUE(vfs_exists(vfs, dst));

    char readback[64] = {0};
    size_t nb = 0;
    vfs_fd_t fd = vfs_fopen(vfs, dst, VFS_O_RDONLY);
    VFS_ASSERT_FD_OK(fd);
    VFS_ASSERT_STATUS_OK(vfs_fread(vfs, fd, readback, payload_len, &nb));
    VFS_ASSERT_TRUE(nb == payload_len);
    VFS_ASSERT_TRUE(memcmp(readback, payload, payload_len) == 0);
    VFS_ASSERT_STATUS_OK(vfs_fclose(vfs, fd));

    VFS_ASSERT_STATUS_OK(vfs_rename(vfs, dst, dst));
    VFS_ASSERT_TRUE(vfs_exists(vfs, dst));

    const char victim_payload[] = "victim-data-should-vanish";
    VFS_ASSERT_STATUS_OK(write_vfs_file(vfs, victim, victim_payload, sizeof(victim_payload) - 1u));

    vfs_stat_t victim_stat_before;
    VFS_ASSERT_STATUS_OK(vfs_stat(vfs, victim, &victim_stat_before));
    VFS_ASSERT_TRUE(victim_stat_before.block_count >= 1u);

    vfs_fd_t victim_fd = vfs_fopen(vfs, victim, VFS_O_RDONLY);
    VFS_ASSERT_FD_OK(victim_fd);

    VFS_ASSERT_STATUS_OK(vfs_rename(vfs, dst, victim));

    VFS_ASSERT_TRUE(vfs_exists(vfs, victim));
    VFS_ASSERT_TRUE(!vfs_exists(vfs, dst));

    char dummy[8];
    size_t dummy_nb;
    VFS_ASSERT_STATUS_EQ(vfs_fread(vfs, victim_fd, dummy, sizeof(dummy), &dummy_nb), VFS_ERR_BADFD);

    memset(readback, 0, sizeof(readback));
    fd = vfs_fopen(vfs, victim, VFS_O_RDONLY);
    VFS_ASSERT_FD_OK(fd);
    VFS_ASSERT_STATUS_OK(vfs_fread(vfs, fd, readback, payload_len, &nb));
    VFS_ASSERT_TRUE(nb == payload_len);
    VFS_ASSERT_TRUE(memcmp(readback, payload, payload_len) == 0);
    VFS_ASSERT_STATUS_OK(vfs_fclose(vfs, fd));

    vfs_close(vfs);
    VFS_ASSERT_STATUS_OK(vfs_open(TEST_IMAGE, true, &vfs));
    VFS_ASSERT_STATUS_EQ(vfs_rename(vfs, victim, "/data/nope.bin"), VFS_ERR_READONLY);

    VFS_ASSERT_STATUS_EQ(vfs_rename(NULL, victim, dst), VFS_ERR_INVAL);
    VFS_ASSERT_STATUS_EQ(vfs_rename(vfs, NULL, dst), VFS_ERR_INVAL);
    VFS_ASSERT_STATUS_EQ(vfs_rename(vfs, victim, NULL), VFS_ERR_INVAL);
    VFS_ASSERT_STATUS_EQ(vfs_rename(vfs, "", dst), VFS_ERR_INVAL);
    VFS_ASSERT_STATUS_EQ(vfs_rename(vfs, victim, ""), VFS_ERR_INVAL);

    vfs_close(vfs);
    return true;
}

typedef struct {
    int count;
    char last_matched[VFS_MAX_PATH];
} list_context_t;

static bool list_cb(const char* path, const vfs_stat_t* st, void* userdata) {
    (void)st;
    list_context_t* ctx = (list_context_t*)userdata;
    ctx->count++;
    strncpy(ctx->last_matched, path, sizeof(ctx->last_matched) - 1);
    return true;
}

static bool test_directory_listing(void) {
    vfs_t* vfs = NULL;
    cleanup_image();
    VFS_ASSERT_STATUS_OK(vfs_create(TEST_IMAGE, &vfs));

    const char* files[] = {"/logs/app.log", "/logs/sys.log", "/data/db.bin", "/data/config.xml"};

    for (size_t i = 0; i < 4; i++) {
        vfs_fd_t fd = vfs_fopen(vfs, files[i], VFS_O_CREAT | VFS_O_WRONLY);
        VFS_ASSERT_FD_OK(fd);
        VFS_ASSERT_STATUS_OK(vfs_fclose(vfs, fd));
    }

    list_context_t ctx = {0, ""};

    vfs_list(vfs, "/logs", list_cb, &ctx);
    VFS_ASSERT_TRUE(ctx.count == 2);

    ctx.count = 0;
    vfs_list(vfs, NULL, list_cb, &ctx);
    VFS_ASSERT_TRUE(ctx.count == 4);

    vfs_close(vfs);
    return true;
}

static bool test_edge_cases_and_limits(void) {
    vfs_t* vfs = NULL;
    cleanup_image();
    VFS_ASSERT_STATUS_OK(vfs_create(TEST_IMAGE, &vfs));

    VFS_ASSERT_STATUS_EQ(vfs_create(NULL, &vfs), VFS_ERR_INVAL);
    VFS_ASSERT_STATUS_EQ(vfs_open(NULL, false, &vfs), VFS_ERR_INVAL);

    size_t temp_sz;
    VFS_ASSERT_STATUS_EQ(vfs_fread(vfs, 99, NULL, 0, &temp_sz), VFS_ERR_INVAL);
    char mock_buf[10];
    VFS_ASSERT_STATUS_EQ(vfs_fread(vfs, 99, mock_buf, 5, &temp_sz), VFS_ERR_BADFD);
    VFS_ASSERT_STATUS_EQ(vfs_fwrite(vfs, -1, mock_buf, 5, &temp_sz), VFS_ERR_BADFD);
    VFS_ASSERT_STATUS_EQ(vfs_fclose(vfs, 99), VFS_ERR_BADFD);

    vfs_fd_t fd = vfs_fopen(vfs, "/t.dat", VFS_O_CREAT | VFS_O_RDWR);
    VFS_ASSERT_FD_OK(fd);
    VFS_ASSERT_STATUS_EQ(vfs_fseek(vfs, fd, -5, VFS_SEEK_SET, NULL), VFS_ERR_INVAL);
    VFS_ASSERT_STATUS_EQ(vfs_fseek(vfs, fd, 0, 999, NULL), VFS_ERR_INVAL);

    VFS_ASSERT_STATUS_OK(vfs_fclose(vfs, fd));
    vfs_close(vfs);
    return true;
}

/**
 * @brief Verifies that the open-file table permits up to 1024 concurrent 
 *        handles and rejects any subsequent open requests.
 */
static bool test_max_open_files_limit(void) {
    vfs_t* vfs = NULL;
    cleanup_image();
    VFS_ASSERT_STATUS_OK(vfs_create(TEST_IMAGE, &vfs));

    const char* path = "/limit_test.txt";
    vfs_fd_t fds[VFS_MAX_OPEN_FILES];

    /* 1. Open the file 1024 times concurrently */
    for (uint32_t i = 0; i < VFS_MAX_OPEN_FILES; i++) {
        /* VFS_O_CREAT is used; subsequent opens resolve to the existing inode */
        fds[i] = vfs_fopen(vfs, path, VFS_O_CREAT | VFS_O_RDWR);
        if (fds[i] < 0) {
            fprintf(stderr, "[FAIL] Failed to open file at index %u: %s\n", i, vfs_strerror((vfs_status_t)fds[i]));
            vfs_close(vfs);
            return false;
        }
    }

    /* 2. The 1025th concurrent open must fail with VFS_ERR_NOSPACE */
    vfs_fd_t extra_fd = vfs_fopen(vfs, path, VFS_O_RDWR);
    VFS_ASSERT_STATUS_EQ((vfs_status_t)extra_fd, VFS_ERR_NOSPACE);

    /* 3. Close one file descriptor to free a slot in the table */
    VFS_ASSERT_STATUS_OK(vfs_fclose(vfs, fds[0]));

    /* 4. Verifying that a slot is now free and can be reused */
    fds[0] = vfs_fopen(vfs, path, VFS_O_RDWR);
    VFS_ASSERT_FD_OK(fds[0]);

    /* 5. Clean up all open descriptors */
    for (uint32_t i = 0; i < VFS_MAX_OPEN_FILES; i++) {
        VFS_ASSERT_STATUS_OK(vfs_fclose(vfs, fds[i]));
    }

    vfs_close(vfs);
    return true;
}

/* =========================================================================
 * vfs_sendfile tests
 * ======================================================================= */

/**
 * Drains exactly @p n bytes from read-end of a pipe into @p dst.
 *
 * @return true on success, false if a short read or error occurs.
 */
static bool pipe_read_exact(int rfd, void* dst, size_t n) {
    uint8_t* p = (uint8_t*)dst;
    size_t rem = n;
    while (rem > 0) {
        ssize_t r = read(rfd, p, rem);
        if (r <= 0) { return false; }
        p += (size_t)r;
        rem -= (size_t)r;
    }
    return true;
}

/**
 * Verifies a full file transfer with offset == NULL (cursor-advancing mode).
 *
 * After the call of_pos must equal the file size, and every byte received
 * on the pipe must match the original payload.
 */
static bool test_sendfile_full_transfer(void) {
    vfs_t* vfs = NULL;
    cleanup_image();
    VFS_ASSERT_STATUS_OK(vfs_create(TEST_IMAGE, &vfs));

    /* Write a payload that spans exactly 3 blocks so the run-length path
     * exercises at least one SIB lookup. */
    const size_t payload_len = 3u * 4096u; /* 12 KiB */
    uint8_t* payload = malloc(payload_len);
    VFS_ASSERT_TRUE(payload != NULL);
    for (size_t i = 0; i < payload_len; i++) {
        payload[i] = (uint8_t)(i & 0xFF);
    }

    const char* path = "/sf_full.bin";
    VFS_ASSERT_STATUS_OK(write_vfs_file(vfs, path, payload, payload_len));

    vfs_fd_t fd = vfs_fopen(vfs, path, VFS_O_RDONLY);
    VFS_ASSERT_FD_OK(fd);

    int pipefd[2];
    VFS_ASSERT_TRUE(pipe(pipefd) == 0);

    size_t bytes_sent = 0;
    VFS_ASSERT_STATUS_OK(vfs_sendfile(vfs, pipefd[1], fd,
                                      /*offset=*/NULL, payload_len, &bytes_sent));
    VFS_ASSERT_TRUE(bytes_sent == payload_len);

    /* Cursor must have advanced to EOF. */
    off_t pos = 0;
    VFS_ASSERT_STATUS_OK(vfs_ftell(vfs, fd, &pos));
    VFS_ASSERT_TRUE(pos == (off_t)payload_len);

    /* Verify every byte received on the pipe. */
    uint8_t* recv = malloc(payload_len);
    VFS_ASSERT_TRUE(recv != NULL);
    VFS_ASSERT_TRUE(pipe_read_exact(pipefd[0], recv, payload_len));
    VFS_ASSERT_TRUE(memcmp(recv, payload, payload_len) == 0);

    close(pipefd[0]);
    close(pipefd[1]);
    free(payload);
    free(recv);
    VFS_ASSERT_STATUS_OK(vfs_fclose(vfs, fd));
    vfs_close(vfs);
    return true;
}

/**
 * Verifies a partial transfer using an explicit @p offset.
 *
 * Linux sendfile(2) contract: the VFS file cursor must NOT move; the
 * caller-supplied offset must be updated to reflect the next unread byte.
 */
static bool test_sendfile_offset_semantics(void) {
    vfs_t* vfs = NULL;
    cleanup_image();
    VFS_ASSERT_STATUS_OK(vfs_create(TEST_IMAGE, &vfs));

    /* 64-byte payload; we will transfer a 24-byte window starting at byte 16. */
    const char payload[64] = {0};
    /* Fill with a visible pattern so partial copies are easy to diff. */
    for (int i = 0; i < 64; i++) {
        ((char*)payload)[i] = (char)('A' + (i % 26));
    }

    const char* path = "/sf_offset.bin";
    VFS_ASSERT_STATUS_OK(write_vfs_file(vfs, path, payload, sizeof(payload)));

    /* Open with cursor parked at byte 0. */
    vfs_fd_t fd = vfs_fopen(vfs, path, VFS_O_RDONLY);
    VFS_ASSERT_FD_OK(fd);

    int pipefd[2];
    VFS_ASSERT_TRUE(pipe(pipefd) == 0);

    off_t off = 16;
    size_t count = 24;
    size_t bytes_sent = 0;
    VFS_ASSERT_STATUS_OK(vfs_sendfile(vfs, pipefd[1], fd, &off, count, &bytes_sent));
    VFS_ASSERT_TRUE(bytes_sent == count);

    /* offset must now point one past the last transferred byte. */
    VFS_ASSERT_TRUE(off == 16 + 24);

    /* File cursor must be untouched (still 0). */
    off_t cursor = -1;
    VFS_ASSERT_STATUS_OK(vfs_ftell(vfs, fd, &cursor));
    VFS_ASSERT_TRUE(cursor == 0);

    /* Content check: bytes 16..39 of the payload. */
    char recv[24];
    VFS_ASSERT_TRUE(pipe_read_exact(pipefd[0], recv, sizeof(recv)));
    VFS_ASSERT_TRUE(memcmp(recv, payload + 16, count) == 0);

    close(pipefd[0]);
    close(pipefd[1]);
    VFS_ASSERT_STATUS_OK(vfs_fclose(vfs, fd));
    vfs_close(vfs);
    return true;
}

/**
 * Verifies that sparse holes are materialised as zero bytes on the pipe.
 *
 * A file extended with vfs_truncate has no physical blocks; the sendfile
 * implementation must emit actual zero bytes rather than skipping them,
 * because a pipe reader has no concept of holes.
 */
static bool test_sendfile_sparse_hole(void) {
    vfs_t* vfs = NULL;
    cleanup_image();
    VFS_ASSERT_STATUS_OK(vfs_create(TEST_IMAGE, &vfs));

    /* Extend to 8 KiB without writing any data — two fully sparse blocks. */
    const char* path = "/sf_sparse.bin";
    const size_t fsize = 2u * 4096u;
    vfs_fd_t cfd = vfs_fopen(vfs, path, VFS_O_CREAT | VFS_O_WRONLY);
    VFS_ASSERT_FD_OK(cfd);
    VFS_ASSERT_STATUS_OK(vfs_fclose(vfs, cfd));
    VFS_ASSERT_STATUS_OK(vfs_truncate(vfs, path, fsize));

    vfs_fd_t fd = vfs_fopen(vfs, path, VFS_O_RDONLY);
    VFS_ASSERT_FD_OK(fd);

    int pipefd[2];
    VFS_ASSERT_TRUE(pipe(pipefd) == 0);

    size_t bytes_sent = 0;
    VFS_ASSERT_STATUS_OK(vfs_sendfile(vfs, pipefd[1], fd, NULL, fsize, &bytes_sent));
    VFS_ASSERT_TRUE(bytes_sent == fsize);

    uint8_t* recv = calloc(1, fsize);
    VFS_ASSERT_TRUE(recv != NULL);
    VFS_ASSERT_TRUE(pipe_read_exact(pipefd[0], recv, fsize));

    /* Every byte must be zero. */
    for (size_t i = 0; i < fsize; i++) {
        if (recv[i] != 0) {
            fprintf(stderr, "[FAIL] Non-zero byte at sparse offset %zu\n", i);
            free(recv);
            close(pipefd[0]);
            close(pipefd[1]);
            VFS_ASSERT_STATUS_OK(vfs_fclose(vfs, fd));
            vfs_close(vfs);
            return false;
        }
    }

    free(recv);
    close(pipefd[0]);
    close(pipefd[1]);
    VFS_ASSERT_STATUS_OK(vfs_fclose(vfs, fd));
    vfs_close(vfs);
    return true;
}

/**
 * Verifies that a write-only VFS fd is rejected with VFS_ERR_INVAL.
 */
static bool test_sendfile_rejects_writeonly_fd(void) {
    vfs_t* vfs = NULL;
    cleanup_image();
    VFS_ASSERT_STATUS_OK(vfs_create(TEST_IMAGE, &vfs));

    const char* path = "/sf_wo.bin";
    VFS_ASSERT_STATUS_OK(write_vfs_file(vfs, path, "data", 4));

    /* Open write-only — must not be usable as a sendfile source. */
    vfs_fd_t fd = vfs_fopen(vfs, path, VFS_O_WRONLY);
    VFS_ASSERT_FD_OK(fd);

    int pipefd[2];
    VFS_ASSERT_TRUE(pipe(pipefd) == 0);

    size_t bytes_sent = 1; /* pre-poison to ensure it is zeroed on error */
    vfs_status_t s = vfs_sendfile(vfs, pipefd[1], fd, NULL, 4, &bytes_sent);
    VFS_ASSERT_STATUS_EQ(s, VFS_ERR_INVAL);
    VFS_ASSERT_TRUE(bytes_sent == 0);

    close(pipefd[0]);
    close(pipefd[1]);
    VFS_ASSERT_STATUS_OK(vfs_fclose(vfs, fd));
    vfs_close(vfs);
    return true;
}

/**
 * Verifies boundary and NULL-guard conditions.
 */
static bool test_sendfile_edge_cases(void) {
    vfs_t* vfs = NULL;
    cleanup_image();
    VFS_ASSERT_STATUS_OK(vfs_create(TEST_IMAGE, &vfs));

    const char* path = "/sf_edge.bin";
    VFS_ASSERT_STATUS_OK(write_vfs_file(vfs, path, "hello", 5));

    vfs_fd_t fd = vfs_fopen(vfs, path, VFS_O_RDONLY);
    VFS_ASSERT_FD_OK(fd);

    int pipefd[2];
    VFS_ASSERT_TRUE(pipe(pipefd) == 0);
    size_t bytes_sent = 0;

    /* NULL vfs. */
    VFS_ASSERT_STATUS_EQ(vfs_sendfile(NULL, pipefd[1], fd, NULL, 5, &bytes_sent), VFS_ERR_INVAL);

    /* Invalid host fd. */
    VFS_ASSERT_STATUS_EQ(vfs_sendfile(vfs, -1, fd, NULL, 5, &bytes_sent), VFS_ERR_INVAL);

    /* NULL bytes_sent. */
    VFS_ASSERT_STATUS_EQ(vfs_sendfile(vfs, pipefd[1], fd, NULL, 5, NULL), VFS_ERR_INVAL);

    /* Bad VFS fd. */
    VFS_ASSERT_STATUS_EQ(vfs_sendfile(vfs, pipefd[1], 99, NULL, 5, &bytes_sent), VFS_ERR_BADFD);

    /* count == 0 is always a no-op: bytes_sent stays 0, returns VFS_OK. */
    bytes_sent = 1; /* pre-poison */
    VFS_ASSERT_STATUS_OK(vfs_sendfile(vfs, pipefd[1], fd, NULL, 0, &bytes_sent));
    VFS_ASSERT_TRUE(bytes_sent == 0);

    /* offset past EOF: VFS_OK, bytes_sent == 0. */
    off_t past_eof = 9999;
    bytes_sent = 1; /* pre-poison */
    VFS_ASSERT_STATUS_OK(vfs_sendfile(vfs, pipefd[1], fd, &past_eof, 5, &bytes_sent));
    VFS_ASSERT_TRUE(bytes_sent == 0);

    close(pipefd[0]);
    close(pipefd[1]);
    VFS_ASSERT_STATUS_OK(vfs_fclose(vfs, fd));
    vfs_close(vfs);
    return true;
}

/**
 * Verifies that a multi-block transfer survives a VFS reopen (i.e., block
 * mappings are correctly persisted and recovered before sendfile runs).
 */
static bool test_sendfile_after_reopen(void) {
    vfs_t* vfs = NULL;
    cleanup_image();
    VFS_ASSERT_STATUS_OK(vfs_create(TEST_IMAGE, &vfs));

    /* Write a payload that exercises both direct and single-indirect blocks:
     * 300 blocks = 254 direct + 46 SIB entries. */
    const size_t payload_len = 300u * 4096u;
    uint8_t* payload = malloc(payload_len);
    VFS_ASSERT_TRUE(payload != NULL);
    for (size_t i = 0; i < payload_len; i++) {
        payload[i] = (uint8_t)((i ^ (i >> 8)) & 0xFF);
    }

    const char* path = "/sf_reopen.bin";
    VFS_ASSERT_STATUS_OK(write_vfs_file(vfs, path, payload, payload_len));
    vfs_close(vfs);
    vfs = NULL;

    /* Reopen read-only to prove the block map survives persistence. */
    VFS_ASSERT_STATUS_OK(vfs_open(TEST_IMAGE, /*readonly=*/true, &vfs));

    vfs_fd_t fd = vfs_fopen(vfs, path, VFS_O_RDONLY);
    VFS_ASSERT_FD_OK(fd);

    int pipefd[2];
    VFS_ASSERT_TRUE(pipe(pipefd) == 0);

    /* Read back in pipe-buffer-sized chunks to avoid a write/read deadlock:
 * vfs_sendfile blocks until `want` bytes are written to the pipe, but the
 * pipe buffer is typically only 64 KiB, so `want` must not exceed that. */
    size_t total_sent = 0;
    uint8_t* recv = malloc(payload_len);
    VFS_ASSERT_TRUE(recv != NULL);
    uint8_t* rp = recv;
    const size_t chunk = 64u * 1024u; /* <= typical pipe capacity */

    while (total_sent < payload_len) {
        size_t want = payload_len - total_sent;
        if (want > chunk) { want = chunk; }

        size_t bytes_sent = 0;
        VFS_ASSERT_STATUS_OK(vfs_sendfile(vfs, pipefd[1], fd, NULL, want, &bytes_sent));
        VFS_ASSERT_TRUE(bytes_sent == want);

        VFS_ASSERT_TRUE(pipe_read_exact(pipefd[0], rp, want));
        rp += want;
        total_sent += want;
    }

    VFS_ASSERT_TRUE(memcmp(recv, payload, payload_len) == 0);

    close(pipefd[0]);
    close(pipefd[1]);
    free(payload);
    free(recv);
    VFS_ASSERT_STATUS_OK(vfs_fclose(vfs, fd));
    vfs_close(vfs);
    return true;
}

/**
 * Verifies a full transfer from the VFS to a real host filesystem file
 * (out_fd is a regular file, not a pipe).
 *
 * This exercises the Linux sendfile(2) path with a destination type that
 * supports normal lseek/pread semantics, and confirms the bytes written to
 * the host file exactly match the source payload.
 */
static bool test_sendfile_to_host_file(void) {
    vfs_t* vfs = NULL;
    cleanup_image();
    VFS_ASSERT_STATUS_OK(vfs_create(TEST_IMAGE, &vfs));

    /* Payload spans multiple blocks, including a sparse hole, so the host
     * file must end up byte-identical including the zero-filled region. */
    const size_t data_len = 5u * 4096u;
    uint8_t* payload = malloc(data_len);
    VFS_ASSERT_TRUE(payload != NULL);
    for (size_t i = 0; i < data_len; i++) {
        payload[i] = (uint8_t)((i * 31u) & 0xFF);
    }

    const char* path = "/sf_to_host.bin";
    VFS_ASSERT_STATUS_OK(write_vfs_file(vfs, path, payload, data_len));

    /* Extend by 2 sparse blocks (no physical allocation) past the payload. */
    const size_t hole_len = 2u * 4096u;
    const size_t total_len = data_len + hole_len;
    VFS_ASSERT_STATUS_OK(vfs_truncate(vfs, path, total_len));

    vfs_fd_t fd = vfs_fopen(vfs, path, VFS_O_RDONLY);
    VFS_ASSERT_FD_OK(fd);

    /* Destination is a real regular file on the host filesystem. */
    const char* host_path = "sf_host_dest.bin";
    int out_fd = open(host_path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    VFS_ASSERT_TRUE(out_fd >= 0);

    size_t bytes_sent = 0;
    VFS_ASSERT_STATUS_OK(vfs_sendfile(vfs, out_fd, fd, /*offset=*/NULL, total_len, &bytes_sent));
    VFS_ASSERT_TRUE(bytes_sent == total_len);

    /* Cursor must have advanced to EOF. */
    off_t pos = 0;
    VFS_ASSERT_STATUS_OK(vfs_ftell(vfs, fd, &pos));
    VFS_ASSERT_TRUE(pos == (off_t)total_len);

    close(out_fd);

    /* Reopen the host file and verify its contents byte-for-byte. */
    out_fd = open(host_path, O_RDONLY);
    VFS_ASSERT_TRUE(out_fd >= 0);

    uint8_t* host_data = malloc(total_len);
    VFS_ASSERT_TRUE(host_data != NULL);
    ssize_t n = read(out_fd, host_data, total_len);
    VFS_ASSERT_TRUE(n == (ssize_t)total_len);

    /* First data_len bytes must match the payload exactly. */
    VFS_ASSERT_TRUE(memcmp(host_data, payload, data_len) == 0);

    /* The trailing sparse region must read back as zeros. */
    for (size_t i = data_len; i < total_len; i++) {
        VFS_ASSERT_TRUE(host_data[i] == 0);
    }

    close(out_fd);
    unlink(host_path);
    free(payload);
    free(host_data);
    VFS_ASSERT_STATUS_OK(vfs_fclose(vfs, fd));
    vfs_close(vfs);
    return true;
}

/* =========================================================================
 * Quantitative Benchmarks
 * ======================================================================= */

/**
 * @brief Configurable benchmark parameters
 */
#define BENCH_CHUNK_SIZE     (256 * 1024)           /* 256 KiB chunks */
#define BENCH_TOTAL_SIZE     (512ULL * 1024 * 1024) /* 512 MiB total */
#define BENCH_PROGRESS_EVERY (64ULL * 1024 * 1024)  /* Print progress every 64 MiB */

/**
 * @brief Measures the throughput of sequential write operations.
 */
static bool benchmark_write_throughput(void) {
    vfs_t* vfs = NULL;
    cleanup_image();
    VFS_ASSERT_STATUS_OK(vfs_create(TEST_IMAGE, &vfs));

    const char* path = "/bench.bin";
    vfs_fd_t fd = vfs_fopen(vfs, path, VFS_O_CREAT | VFS_O_WRONLY);
    VFS_ASSERT_FD_OK(fd);

    size_t chunk_size = BENCH_CHUNK_SIZE;
    size_t total_size = BENCH_TOTAL_SIZE;
    uint8_t* buffer = malloc(chunk_size);
    VFS_ASSERT_TRUE(buffer != NULL);
    memset(buffer, 0x5A, chunk_size);

    struct timespec start, end;
    VFS_ASSERT_TRUE(clock_gettime(CLOCK_MONOTONIC, &start) == 0);

    size_t bytes_written_total = 0;
    size_t last_progress = 0;

    while (bytes_written_total < total_size) {
        size_t remaining = total_size - bytes_written_total;
        size_t write_size = (remaining < chunk_size) ? remaining : chunk_size;

        size_t written = 0;
        vfs_status_t s = vfs_fwrite(vfs, fd, buffer, write_size, &written);
        if (s != VFS_OK || written != write_size) {
            printf("\n    Write FAILED at %zu / %zu MiB\n", bytes_written_total / (1024 * 1024),
                   total_size / (1024 * 1024));
            free(buffer);
            vfs_fclose(vfs, fd);
            vfs_close(vfs);
            return false;
        }
        bytes_written_total += written;

        /* Progress indicator */
        if (bytes_written_total - last_progress >= BENCH_PROGRESS_EVERY) {
            last_progress = bytes_written_total;
            double progress = (double)bytes_written_total / (double)total_size * 100.0;
            double elapsed = 0.0;
            struct timespec now;
            if (clock_gettime(CLOCK_MONOTONIC, &now) == 0) {
                elapsed = (double)(now.tv_sec - start.tv_sec) + (double)(now.tv_nsec - start.tv_nsec) / 1e9;
            }
            double current_throughput = (double)(bytes_written_total / (1024.0 * 1024.0)) / elapsed;
            printf("    Write: %.1f%% (%5zu MiB) - %.1f sec, %.0f MiB/s\r", progress,
                   bytes_written_total / (1024 * 1024), elapsed, current_throughput);
            fflush(stdout);
        }
    }

    VFS_ASSERT_TRUE(clock_gettime(CLOCK_MONOTONIC, &end) == 0);
    VFS_ASSERT_STATUS_OK(vfs_fclose(vfs, fd));

    double seconds = (double)(end.tv_sec - start.tv_sec) + (double)(end.tv_nsec - start.tv_nsec) / 1e9;
    double mib = (double)total_size / (1024.0 * 1024.0);
    double throughput = mib / seconds;

    printf("\n\n  Sequential Write Benchmark:\n");
    printf("    Write Size   : %.2f MiB\n", mib);
    printf("    Chunk Size   : %zu KiB\n", chunk_size / 1024);
    printf("    Elapsed Time : %.4f seconds\n", seconds);
    printf("    Throughput   : %.2f MiB/sec\n", throughput);

    free(buffer);
    vfs_close(vfs);
    return true;
}

/**
 * @brief Measures the throughput of sequential read operations.
 */
static bool benchmark_read_throughput(void) {
    vfs_t* vfs = NULL;
    VFS_ASSERT_STATUS_OK(vfs_open(TEST_IMAGE, true, &vfs));

    const char* path = "/bench.bin";
    vfs_fd_t fd = vfs_fopen(vfs, path, VFS_O_RDONLY);
    VFS_ASSERT_FD_OK(fd);

    size_t chunk_size = BENCH_CHUNK_SIZE;
    size_t total_size = BENCH_TOTAL_SIZE;
    uint8_t* buffer = malloc(chunk_size);
    VFS_ASSERT_TRUE(buffer != NULL);

    struct timespec start, end;
    VFS_ASSERT_TRUE(clock_gettime(CLOCK_MONOTONIC, &start) == 0);

    size_t bytes_read_total = 0;
    size_t last_progress = 0;

    while (bytes_read_total < total_size) {
        size_t remaining = total_size - bytes_read_total;
        size_t read_size = (remaining < chunk_size) ? remaining : chunk_size;

        size_t read_bytes = 0;
        vfs_status_t s = vfs_fread(vfs, fd, buffer, read_size, &read_bytes);
        if (s != VFS_OK || read_bytes != read_size) {
            printf("\n    Read FAILED at %zu / %zu MiB\n", bytes_read_total / (1024 * 1024),
                   total_size / (1024 * 1024));
            free(buffer);
            vfs_fclose(vfs, fd);
            vfs_close(vfs);
            return false;
        }
        bytes_read_total += read_bytes;

        /* Progress indicator */
        if (bytes_read_total - last_progress >= BENCH_PROGRESS_EVERY) {
            last_progress = bytes_read_total;
            double progress = (double)bytes_read_total / (double)total_size * 100.0;
            double elapsed = 0.0;
            struct timespec now;
            if (clock_gettime(CLOCK_MONOTONIC, &now) == 0) {
                elapsed = (double)(now.tv_sec - start.tv_sec) + (double)(now.tv_nsec - start.tv_nsec) / 1e9;
            }
            double current_throughput = (double)(bytes_read_total / (1024.0 * 1024.0)) / elapsed;
            printf("    Read:  %.1f%% (%5zu MiB) - %.1f sec, %.0f MiB/s\r", progress, bytes_read_total / (1024 * 1024),
                   elapsed, current_throughput);
            fflush(stdout);
        }
    }

    VFS_ASSERT_TRUE(clock_gettime(CLOCK_MONOTONIC, &end) == 0);
    VFS_ASSERT_STATUS_OK(vfs_fclose(vfs, fd));

    double seconds = (double)(end.tv_sec - start.tv_sec) + (double)(end.tv_nsec - start.tv_nsec) / 1e9;
    double mib = (double)total_size / (1024.0 * 1024.0);
    double throughput = mib / seconds;

    printf("\n\n  Sequential Read Benchmark:\n");
    printf("    Read Size    : %.2f MiB\n", mib);
    printf("    Chunk Size   : %zu KiB\n", chunk_size / 1024);
    printf("    Elapsed Time : %.4f seconds\n", seconds);
    printf("    Throughput   : %.2f MiB/sec\n", throughput);

    free(buffer);
    vfs_close(vfs);
    return true;
}

/* =============== EMBED ================== */
#define TEMP_DISK_IMAGE "temp_build_image.vfs"
#define TEST_FILE_PATH  "/assets/config.json"
#define TEST_PAYLOAD    "{\"database\": \"embedded_ram_db\", \"status\": \"active\"}"

bool test_open_from_memory(void) {
    printf("Starting Embedded VFS Integration Test...\n");

    vfs_t* disk_vfs = NULL;
    vfs_status_t status;

    /* =========================================================================
     * Step 1: Create a physical VFS image on disk to generate the payload
     * ======================================================================= */
    printf("[1/4] Creating temporary VFS image on disk...\n");
    status = vfs_create(TEMP_DISK_IMAGE, &disk_vfs);
    VFS_ASSERT_STATUS_OK(status);

    /* Open a new file in the disk-backed VFS */
    vfs_fd_t fd = vfs_fopen(disk_vfs, TEST_FILE_PATH, VFS_O_CREAT | VFS_O_WRONLY);
    VFS_ASSERT_FD_OK(fd);

    /* Write the test data payload */
    size_t payload_len = sizeof(TEST_PAYLOAD) - 1;
    size_t written = 0;
    status = vfs_fwrite(disk_vfs, fd, TEST_PAYLOAD, payload_len, &written);
    assert(status == VFS_OK);
    assert(written == payload_len);

    /* Close file and close the VFS to flush everything to disk */
    status = vfs_fclose(disk_vfs, fd);
    assert(status == VFS_OK);
    vfs_close(disk_vfs);
    disk_vfs = NULL;

    /* =========================================================================
     * Step 2: Read the disk image into a memory buffer and delete the disk file
     * ======================================================================= */
    printf("[2/4] Reading disk image into memory buffer...\n");
    FILE* f = fopen(TEMP_DISK_IMAGE, "rb");
    if (!f) {
        perror("Failed to open temporary image file");
        return false;
    }

    fseek(f, 0, SEEK_END);
    long file_size = ftell(f);
    fseek(f, 0, SEEK_SET);

    void* memory_buffer = malloc((size_t)file_size);
    assert(memory_buffer != NULL);

    size_t read_bytes = fread(memory_buffer, 1, (size_t)file_size, f);
    assert(read_bytes == (size_t)file_size);
    fclose(f);

    /* Delete the physical disk image to guarantee we cannot read from disk */
    printf("      Deleting physical disk image: '%s'\n", TEMP_DISK_IMAGE);
    int unlink_status = unlink(TEMP_DISK_IMAGE);
    assert(unlink_status == 0);

    /* =========================================================================
     * Step 3: Mount the VFS directly from the memory buffer
     * ======================================================================= */
    printf("[3/4] Mounting in-memory VFS from buffer using helper...\n");
    vfs_t* mem_vfs = NULL;
    status = vfs_open_embedded(memory_buffer, (size_t)file_size, true, &mem_vfs);
    if (status != VFS_OK) {
        fprintf(stderr, "Failed to mount embedded VFS: %s\n", vfs_strerror(status));
        free(memory_buffer);
        return false;
    }

    /* We can safely free the local user buffer now, as memfd holds its own copy */
    free(memory_buffer);
    memory_buffer = NULL;

    /* =========================================================================
     * Step 4: Validate file contents from the memory-mounted VFS
     * ======================================================================= */
    printf("[4/4] Verifying file availability in memory-mounted VFS...\n");
    assert(vfs_exists(mem_vfs, TEST_FILE_PATH) == true);

    vfs_fd_t mem_fd = vfs_fopen(mem_vfs, TEST_FILE_PATH, VFS_O_RDONLY);
    if (mem_fd < 0) {
        fprintf(stderr, "Failed to open embedded file: %s\n", vfs_strerror((vfs_status_t)mem_fd));
        vfs_close(mem_vfs);
        return false;
    }

    char read_buffer[128];
    memset(read_buffer, 0, sizeof(read_buffer));
    size_t read_count = 0;
    status = vfs_fread(mem_vfs, mem_fd, read_buffer, sizeof(read_buffer) - 1, &read_count);
    assert(status == VFS_OK);
    assert(read_count == payload_len);

    printf("      Read payload from RAM: '%s'\n", read_buffer);
    assert(strcmp(read_buffer, TEST_PAYLOAD) == 0);

    /* Cleanup */
    status = vfs_fclose(mem_vfs, mem_fd);
    assert(status == VFS_OK);
    vfs_close(mem_vfs);

    printf("Embedded VFS Integration Test: PASS\n");
    return true;
}

typedef struct {
    const char* name;
    bool (*func)(void);
} test_t;

int main(void) {
    test_t tests[] = {
        {"Lifecycle: Create, Open, Close", test_lifecycle_create_open_close},
        {"Lifecycle: Open from memory", test_open_from_memory},
        {"Core IO: Basic File Read/Write/Seek", test_file_create_and_basic_write_read},
        {"Core IO: Multiple Block Crossings", test_read_write_large_file},
        {"Core IO: Append Enforcement Mode", test_append_mode},
        {"Allocation: Sparse Block Fills", test_sparse_reads_and_seek_past_eof},
        {"Metadata: Truncate Extend/Shrink/Overflow", test_truncate_shrink_and_extend},
        {"Metadata: File Removal / Unlink", test_unlink},
        {"Metadata: Atomic Rename / Move", test_rename},
        {"System: Read-Only Structures Protection", test_read_only_mode},
        {"System: List Traversals and Prefix Matches", test_directory_listing},
        {"System: Edge Cases and Invalid Arguments", test_edge_cases_and_limits},
        {"System: Maximum Open File descriptors", test_max_open_files_limit},
        {"sendfile: Full transfer, cursor mode", test_sendfile_full_transfer},
        {"sendfile: Partial range, offset semantics", test_sendfile_offset_semantics},
        {"sendfile: Sparse hole materialises zeros", test_sendfile_sparse_hole},
        {"sendfile: Rejects write-only source fd", test_sendfile_rejects_writeonly_fd},
        {"sendfile: Edge cases and NULL guards", test_sendfile_edge_cases},
        {"sendfile: Multi-block transfer after reopen", test_sendfile_after_reopen},
        {"sendfile: Full transfer to host filesystem file", test_sendfile_to_host_file},
        {"Benchmark: Sequential Write Speed", benchmark_write_throughput},
        {"Benchmark: Sequential Read Speed", benchmark_read_throughput},
    };

    size_t test_count = sizeof(tests) / sizeof(tests[0]);
    size_t passed = 0;

    printf("Executing Virtual File System (VFS) Tests and Benchmarks...\n");
    printf("========================================================\n");

    for (size_t i = 0; i < test_count; i++) {
        printf("[%02zu/%02zu] RUNNING: %s...\n", i + 1, test_count, tests[i].name);
        if (tests[i].func()) {
            printf("[%02zu/%02zu] PASS\n\n", i + 1, test_count);
            passed++;
        } else {
            printf("[%02zu/%02zu] FAIL\n\n", i + 1, test_count);
        }
    }

    printf("========================================================\n");
    printf("Result Summary: %zu of %zu tests/benchmarks passed.\n", passed, test_count);

    cleanup_image();

    if (passed == test_count) { return EXIT_SUCCESS; }
    return EXIT_FAILURE;
}
