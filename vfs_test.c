/**
 * @file vfs_test.c
 * @brief Comprehensive test suite verifying indirect addressing and allocation boundaries.
 */

#include "vfs.h"
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
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

typedef struct {
    const char* name;
    bool (*func)(void);
} test_t;

int main(void) {
    test_t tests[] = {
        {"Lifecycle: Create, Open, Close", test_lifecycle_create_open_close},
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
    };

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
