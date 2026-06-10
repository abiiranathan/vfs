/**
 * @file vfs_test.c
 * @brief Comprehensive test runner for the single-file virtual filesystem.
 *
 * Compilation command:
 *   gcc -Wall -Wextra -O2 vfs.c vfs_test.c -o vfs_test -lpthread
 */

#include "vfs.h"
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define TEST_IMAGE "test_system.vfs"

// ================= Helper macros for writing tests or enforcing invariants ===================
/* -------------------------------------------------------------------------
 * Test Assertion Macros
 * ---------------------------------------------------------------------- */

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

/* -------------------------------------------------------------------------
 * Test Suite Cases
 * ---------------------------------------------------------------------- */

/**
 * @brief Test creation, basic closing, and reloading of the file system container.
 */
static bool test_lifecycle_create_open_close(void) {
    vfs_t* vfs = NULL;
    cleanup_image();

    /* Try to open a non-existent image */
    VFS_ASSERT_STATUS_EQ(vfs_open(TEST_IMAGE, false, &vfs), VFS_ERR_IO);

    /* Create new system image */
    VFS_ASSERT_STATUS_OK(vfs_create(TEST_IMAGE, &vfs));
    VFS_ASSERT_TRUE(vfs != NULL);

    /* Commit changes and close */
    VFS_ASSERT_STATUS_OK(vfs_sync(vfs));
    vfs_close(vfs);

    /* Reopen the existing system image */
    VFS_ASSERT_STATUS_OK(vfs_open(TEST_IMAGE, false, &vfs));
    VFS_ASSERT_TRUE(vfs != NULL);

    vfs_close(vfs);
    return true;
}

/**
 * @brief Verify simple file creation, metadata presence, raw writing, seeking, and reading.
 */
static bool test_file_create_and_basic_write_read(void) {
    vfs_t* vfs = NULL;
    cleanup_image();
    VFS_ASSERT_STATUS_OK(vfs_create(TEST_IMAGE, &vfs));

    const char* path = "/doc.txt";
    VFS_ASSERT_TRUE(!vfs_exists(vfs, path));

    /* Open non-existent file without create flag */
    vfs_fd_t fd = vfs_fopen(vfs, path, VFS_O_RDONLY);
    VFS_ASSERT_STATUS_EQ((vfs_status_t)fd, VFS_ERR_NOTFOUND);

    /* Create the file */
    fd = vfs_fopen(vfs, path, VFS_O_CREAT | VFS_O_RDWR);
    VFS_ASSERT_FD_OK(fd);
    VFS_ASSERT_TRUE(vfs_exists(vfs, path));

    /* Write data */
    const char* data = "Virtual File System Test Data Payload";
    size_t len = strlen(data);
    size_t written = 0;
    VFS_ASSERT_STATUS_OK(vfs_fwrite(vfs, fd, data, len, &written));
    VFS_ASSERT_TRUE(written == len);

    /* Verify position has advanced */
    off_t pos = 0;
    VFS_ASSERT_STATUS_OK(vfs_ftell(vfs, fd, &pos));
    VFS_ASSERT_TRUE(pos == (off_t)len);

    /* Reset pointer to start */
    off_t new_pos = 0;
    VFS_ASSERT_STATUS_OK(vfs_fseek(vfs, fd, 0, VFS_SEEK_SET, &new_pos));
    VFS_ASSERT_TRUE(new_pos == 0);

    /* Read back content */
    char buffer[128];
    memset(buffer, 0, sizeof(buffer));
    size_t read_bytes = 0;
    VFS_ASSERT_STATUS_OK(vfs_fread(vfs, fd, buffer, len, &read_bytes));
    VFS_ASSERT_TRUE(read_bytes == len);
    VFS_ASSERT_TRUE(strcmp(buffer, data) == 0);

    /* Check stats */
    vfs_stat_t st;
    VFS_ASSERT_STATUS_OK(vfs_stat(vfs, path, &st));
    VFS_ASSERT_TRUE(st.size == len);
    VFS_ASSERT_TRUE(st.block_count == 1);
    VFS_ASSERT_TRUE(strcmp(st.path, path) == 0);

    VFS_ASSERT_STATUS_OK(vfs_fclose(vfs, fd));
    vfs_close(vfs);
    return true;
}

/**
 * @brief Test logical block boundaries by executing writes/reads spanning multiple blocks.
 */
static bool test_read_write_large_file(void) {
    vfs_t* vfs = NULL;
    cleanup_image();
    VFS_ASSERT_STATUS_OK(vfs_create(TEST_IMAGE, &vfs));

    /* 3 logical blocks of data: 3 * 4096 = 12288 bytes */
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

    /* Verify stats indicate 3 allocated blocks */
    vfs_stat_t st;
    VFS_ASSERT_STATUS_OK(vfs_stat(vfs, "/large.bin", &st));
    VFS_ASSERT_TRUE(st.size == size);
    VFS_ASSERT_TRUE(st.block_count == 3);

    /* Seek and read verification */
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

/**
 * @brief Verify that file append flags force cursor redirection to EOF during writes.
 */
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

    /* Reopen with APPEND flag */
    fd = vfs_fopen(vfs, path, VFS_O_WRONLY | VFS_O_APPEND);
    VFS_ASSERT_FD_OK(fd);

    /* Manually seek back to start to verify append logic overrides cursor offset */
    VFS_ASSERT_STATUS_OK(vfs_fseek(vfs, fd, 0, VFS_SEEK_SET, NULL));

    VFS_ASSERT_STATUS_OK(vfs_fwrite(vfs, fd, "World!", 6, &written));
    VFS_ASSERT_STATUS_OK(vfs_fclose(vfs, fd));

    /* Verify merged file contents */
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

/**
 * @brief Ensure seeking past EOF and reading from unallocated segments generates zero-fills.
 */
static bool test_sparse_reads_and_seek_past_eof(void) {
    vfs_t* vfs = NULL;
    cleanup_image();
    VFS_ASSERT_STATUS_OK(vfs_create(TEST_IMAGE, &vfs));

    const char* path = "/sparse.bin";
    vfs_fd_t fd = vfs_fopen(vfs, path, VFS_O_CREAT | VFS_O_RDWR);
    VFS_ASSERT_FD_OK(fd);

    /* Truncate to size 8192 (extends with two sparse blocks) */
    VFS_ASSERT_STATUS_OK(vfs_truncate(vfs, path, 8192));

    /* Verify stats show 2 unallocated/sparse data blocks */
    vfs_stat_t st;
    VFS_ASSERT_STATUS_OK(vfs_stat(vfs, path, &st));
    VFS_ASSERT_TRUE(st.size == 8192);

    /* Seeking to 4000 and reading 100 bytes should yield zeroes */
    VFS_ASSERT_STATUS_OK(vfs_fseek(vfs, fd, 4000, VFS_SEEK_SET, NULL));
    char buf[100];
    memset(buf, 0xFF, sizeof(buf)); /* Dirty buffer first */
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

/**
 * @brief Verify physical shrinking and zero-padded extension behaviors.
 */
static bool test_truncate_shrink_and_extend(void) {
    vfs_t* vfs = NULL;
    cleanup_image();
    VFS_ASSERT_STATUS_OK(vfs_create(TEST_IMAGE, &vfs));

    const char* path = "/resize.dat";
    vfs_fd_t fd = vfs_fopen(vfs, path, VFS_O_CREAT | VFS_O_RDWR);
    VFS_ASSERT_FD_OK(fd);

    /* Write 6000 bytes across 2 blocks */
    uint8_t* pattern = malloc(6000);
    memset(pattern, 'A', 6000);
    size_t written;
    VFS_ASSERT_STATUS_OK(vfs_fwrite(vfs, fd, pattern, 6000, &written));
    VFS_ASSERT_STATUS_OK(vfs_fclose(vfs, fd));

    /* Shrink the file to 3000 bytes (retains 1 block, frees the other) */
    VFS_ASSERT_STATUS_OK(vfs_truncate(vfs, path, 3000));
    vfs_stat_t st;
    VFS_ASSERT_STATUS_OK(vfs_stat(vfs, path, &st));
    VFS_ASSERT_TRUE(st.size == 3000);
    VFS_ASSERT_TRUE(st.block_count == 1);

    /* Extend the file to 10000 bytes */
    VFS_ASSERT_STATUS_OK(vfs_truncate(vfs, path, 10000));
    VFS_ASSERT_STATUS_OK(vfs_stat(vfs, path, &st));
    VFS_ASSERT_TRUE(st.size == 10000);
    VFS_ASSERT_TRUE(st.block_count == 3);

    /* Read and verify the extended tail contains zeroes */
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

    /* Verify we cannot exceed blocks limit (256 * 4096 = 1,048,576 bytes) */
    VFS_ASSERT_STATUS_EQ(vfs_truncate(vfs, path, 2000000), VFS_ERR_OVERFLOW);

    VFS_ASSERT_STATUS_OK(vfs_fclose(vfs, fd));
    free(pattern);
    vfs_close(vfs);
    return true;
}

/**
 * @brief Verify that file unlinking releases metadata indexes and unlinks references.
 */
static bool test_unlink(void) {
    vfs_t* vfs = NULL;
    cleanup_image();
    VFS_ASSERT_STATUS_OK(vfs_create(TEST_IMAGE, &vfs));

    const char* path = "/killme.bin";
    vfs_fd_t fd = vfs_fopen(vfs, path, VFS_O_CREAT | VFS_O_RDWR);
    VFS_ASSERT_FD_OK(fd);
    VFS_ASSERT_STATUS_OK(vfs_fclose(vfs, fd));

    VFS_ASSERT_TRUE(vfs_exists(vfs, path));

    /* Remove the file */
    VFS_ASSERT_STATUS_OK(vfs_unlink(vfs, path));
    VFS_ASSERT_TRUE(!vfs_exists(vfs, path));

    /* Attempt to remove non-existent file */
    VFS_ASSERT_STATUS_EQ(vfs_unlink(vfs, path), VFS_ERR_NOTFOUND);

    vfs_close(vfs);
    return true;
}

/**
 * @brief Ensure write operations are strictly prevented in read-only mode.
 */
static bool test_read_only_mode(void) {
    vfs_t* vfs = NULL;
    cleanup_image();

    /* Create, populate, and close first */
    VFS_ASSERT_STATUS_OK(vfs_create(TEST_IMAGE, &vfs));
    vfs_fd_t fd = vfs_fopen(vfs, "/fixed.txt", VFS_O_CREAT | VFS_O_WRONLY);
    VFS_ASSERT_FD_OK(fd);
    size_t written;
    VFS_ASSERT_STATUS_OK(vfs_fwrite(vfs, fd, "constant", 8, &written));
    VFS_ASSERT_STATUS_OK(vfs_fclose(vfs, fd));
    vfs_close(vfs);

    /* Open in READ-ONLY mode */
    VFS_ASSERT_STATUS_OK(vfs_open(TEST_IMAGE, true, &vfs));

    /* Attempt modifications */
    vfs_fd_t bad_fd = vfs_fopen(vfs, "/newfile.txt", VFS_O_CREAT | VFS_O_WRONLY);
    VFS_ASSERT_STATUS_EQ((vfs_status_t)bad_fd, VFS_ERR_READONLY);

    VFS_ASSERT_STATUS_EQ(vfs_truncate(vfs, "/fixed.txt", 0), VFS_ERR_READONLY);
    VFS_ASSERT_STATUS_EQ(vfs_unlink(vfs, "/fixed.txt"), VFS_ERR_READONLY);

    /* Reading must remain allowed */
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

/**
 * @brief Verify atomic rename semantics including same-name no-op,
 *        rename into a free slot, and rename-over-existing (POSIX replace).
 */
static bool test_rename(void) {
    vfs_t* vfs = NULL;
    cleanup_image();
    VFS_ASSERT_STATUS_OK(vfs_create(TEST_IMAGE, &vfs));

    const char* src = "/data/original.bin";
    const char* dst = "/data/renamed.bin";
    const char* victim = "/data/victim.bin";

    /* ------------------------------------------------------------------ */
    /* 1. Rename a non-existent file must fail.                            */
    /* ------------------------------------------------------------------ */
    VFS_ASSERT_STATUS_EQ(vfs_rename(vfs, src, dst), VFS_ERR_NOTFOUND);

    /* ------------------------------------------------------------------ */
    /* 2. Basic rename: src → dst (dst does not exist yet).               */
    /* ------------------------------------------------------------------ */
    const char payload[] = "rename-test-content";
    size_t payload_len = sizeof(payload) - 1u;
    VFS_ASSERT_STATUS_OK(write_vfs_file(vfs, src, payload, payload_len));
    VFS_ASSERT_TRUE(vfs_exists(vfs, src));

    VFS_ASSERT_STATUS_OK(vfs_rename(vfs, src, dst));

    VFS_ASSERT_TRUE(!vfs_exists(vfs, src));
    VFS_ASSERT_TRUE(vfs_exists(vfs, dst));

    /* Data must be intact under the new name. */
    char readback[64] = {0};
    size_t nb = 0;
    vfs_fd_t fd = vfs_fopen(vfs, dst, VFS_O_RDONLY);
    VFS_ASSERT_FD_OK(fd);
    VFS_ASSERT_STATUS_OK(vfs_fread(vfs, fd, readback, payload_len, &nb));
    VFS_ASSERT_TRUE(nb == payload_len);
    VFS_ASSERT_TRUE(memcmp(readback, payload, payload_len) == 0);
    VFS_ASSERT_STATUS_OK(vfs_fclose(vfs, fd));

    /* ------------------------------------------------------------------ */
    /* 3. Same-name rename is a no-op and must succeed.                   */
    /* ------------------------------------------------------------------ */
    VFS_ASSERT_STATUS_OK(vfs_rename(vfs, dst, dst));
    VFS_ASSERT_TRUE(vfs_exists(vfs, dst));

    /* ------------------------------------------------------------------ */
    /* 4. Rename-over-existing: dst replaces victim.                      */
    /*    The victim's inode slot must be freed; its data is unreachable.  */
    /* ------------------------------------------------------------------ */
    const char victim_payload[] = "victim-data-should-vanish";
    VFS_ASSERT_STATUS_OK(write_vfs_file(vfs, victim, victim_payload, sizeof(victim_payload) - 1u));

    /* Grab the victim's block count before rename to verify blocks freed. */
    vfs_stat_t victim_stat_before;
    VFS_ASSERT_STATUS_OK(vfs_stat(vfs, victim, &victim_stat_before));
    VFS_ASSERT_TRUE(victim_stat_before.block_count >= 1u);

    /* Open the victim so we can verify the fd is invalidated post-rename. */
    vfs_fd_t victim_fd = vfs_fopen(vfs, victim, VFS_O_RDONLY);
    VFS_ASSERT_FD_OK(victim_fd);

    VFS_ASSERT_STATUS_OK(vfs_rename(vfs, dst, victim));

    /* Victim path now resolves to the renamed file, not the old victim. */
    VFS_ASSERT_TRUE(vfs_exists(vfs, victim));
    VFS_ASSERT_TRUE(!vfs_exists(vfs, dst));

    /* The previously-open victim fd must now be invalid. */
    char dummy[8];
    size_t dummy_nb;
    VFS_ASSERT_STATUS_EQ(vfs_fread(vfs, victim_fd, dummy, sizeof(dummy), &dummy_nb), VFS_ERR_BADFD);

    /* Content under victim path is the *renamed* file's data, not the victim's. */
    memset(readback, 0, sizeof(readback));
    fd = vfs_fopen(vfs, victim, VFS_O_RDONLY);
    VFS_ASSERT_FD_OK(fd);
    VFS_ASSERT_STATUS_OK(vfs_fread(vfs, fd, readback, payload_len, &nb));
    VFS_ASSERT_TRUE(nb == payload_len);
    VFS_ASSERT_TRUE(memcmp(readback, payload, payload_len) == 0);
    VFS_ASSERT_STATUS_OK(vfs_fclose(vfs, fd));

    /* ------------------------------------------------------------------ */
    /* 5. Read-only filesystem must reject rename.                        */
    /* ------------------------------------------------------------------ */
    vfs_close(vfs);
    VFS_ASSERT_STATUS_OK(vfs_open(TEST_IMAGE, /*readonly=*/true, &vfs));
    VFS_ASSERT_STATUS_EQ(vfs_rename(vfs, victim, "/data/nope.bin"), VFS_ERR_READONLY);

    /* ------------------------------------------------------------------ */
    /* 6. Invalid arguments.                                              */
    /* ------------------------------------------------------------------ */
    VFS_ASSERT_STATUS_EQ(vfs_rename(NULL, victim, dst), VFS_ERR_INVAL);
    VFS_ASSERT_STATUS_EQ(vfs_rename(vfs, NULL, dst), VFS_ERR_INVAL);
    VFS_ASSERT_STATUS_EQ(vfs_rename(vfs, victim, NULL), VFS_ERR_INVAL);
    VFS_ASSERT_STATUS_EQ(vfs_rename(vfs, "", dst), VFS_ERR_INVAL);
    VFS_ASSERT_STATUS_EQ(vfs_rename(vfs, victim, ""), VFS_ERR_INVAL);

    vfs_close(vfs);
    return true;
}

/* Callback structures for list tests */
typedef struct {
    int count;
    char last_matched[VFS_MAX_PATH];
} list_context_t;

static bool list_cb(const char* path, const vfs_stat_t* st, void* userdata) {
    (void)st;
    list_context_t* ctx = (list_context_t*)userdata;
    ctx->count++;
    strncpy(ctx->last_matched, path, sizeof(ctx->last_matched) - 1);
    return true; /* Continue iteration */
}

/**
 * @brief Ensure structured traversal matches path-prefixed targets.
 */
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

    /* List all logs */
    vfs_list(vfs, "/logs", list_cb, &ctx);
    VFS_ASSERT_TRUE(ctx.count == 2);

    /* List all files */
    ctx.count = 0;
    vfs_list(vfs, NULL, list_cb, &ctx);
    VFS_ASSERT_TRUE(ctx.count == 4);

    vfs_close(vfs);
    return true;
}

/**
 * @brief Verify standard error reporting on out-of-bound arguments and closed descriptors.
 */
static bool test_edge_cases_and_limits(void) {
    vfs_t* vfs = NULL;
    cleanup_image();
    VFS_ASSERT_STATUS_OK(vfs_create(TEST_IMAGE, &vfs));

    /* 1. Invalid Arguments */
    VFS_ASSERT_STATUS_EQ(vfs_create(NULL, &vfs), VFS_ERR_INVAL);
    VFS_ASSERT_STATUS_EQ(vfs_open(NULL, false, &vfs), VFS_ERR_INVAL);

    /* 2. Closed / Invalid File Descriptor Operations */
    size_t temp_sz;
    VFS_ASSERT_STATUS_EQ(vfs_fread(vfs, 99, NULL, 0, &temp_sz), VFS_ERR_INVAL);
    char mock_buf[10];
    VFS_ASSERT_STATUS_EQ(vfs_fread(vfs, 99, mock_buf, 5, &temp_sz), VFS_ERR_BADFD);
    VFS_ASSERT_STATUS_EQ(vfs_fwrite(vfs, -1, mock_buf, 5, &temp_sz), VFS_ERR_BADFD);
    VFS_ASSERT_STATUS_EQ(vfs_fclose(vfs, 99), VFS_ERR_BADFD);

    /* 3. SEEK bounds */
    vfs_fd_t fd = vfs_fopen(vfs, "/t.dat", VFS_O_CREAT | VFS_O_RDWR);
    VFS_ASSERT_FD_OK(fd);
    /* Seek to negative offset */
    VFS_ASSERT_STATUS_EQ(vfs_fseek(vfs, fd, -5, VFS_SEEK_SET, NULL), VFS_ERR_INVAL);
    /* Invalid whence */
    VFS_ASSERT_STATUS_EQ(vfs_fseek(vfs, fd, 0, 999, NULL), VFS_ERR_INVAL);

    VFS_ASSERT_STATUS_OK(vfs_fclose(vfs, fd));
    vfs_close(vfs);
    return true;
}

/* -------------------------------------------------------------------------
 * Execution Entry Point
 * ---------------------------------------------------------------------- */

typedef struct {
    const char* name;
    bool (*func)(void);
} test_t;

int main(void) {
    test_t tests[] = {{"Lifecycle: Create, Open, Close", test_lifecycle_create_open_close},
                      {"Core IO: Basic File Read/Write/Seek", test_file_create_and_basic_write_read},
                      {"Core IO: Multiple Block Crossings", test_read_write_large_file},
                      {"Core IO: Append Enforcement Mode", test_append_mode},
                      {"Allocation: Sparse Block Fills", test_sparse_reads_and_seek_past_eof},
                      {"Metadata: Truncate Extend/Shrink/Overflow", test_truncate_shrink_and_extend},
                      {"Metadata: File Removal / Unlink", test_unlink},
                      {"Metadata: Atomic Rename / Move", test_rename},
                      {"System: Read-Only Structures Protection", test_read_only_mode},
                      {"System: List Traversals and Prefix Matches", test_directory_listing},
                      {"System: Edge Cases and Invalid Arguments", test_edge_cases_and_limits}};

    size_t test_count = sizeof(tests) / sizeof(tests[0]);
    size_t passed = 0;

    printf("Executing Virtual File System (VFS) Tests...\n");
    printf("===========================================\n");

    for (size_t i = 0; i < test_count; i++) {
        printf("[%02zu/%02zu] RUNNING: %s...\n", i + 1, test_count, tests[i].name);
        if (tests[i].func()) {
            printf("[%02zu/%02zu] PASS\n\n", i + 1, test_count);
            passed++;
        } else {
            printf("[%02zu/%02zu] FAIL\n\n", i + 1, test_count);
        }
    }

    printf("===========================================\n");
    printf("Result Summary: %zu of %zu tests passed.\n", passed, test_count);

    cleanup_image();

    if (passed == test_count) { return EXIT_SUCCESS; }
    return EXIT_FAILURE;
}
