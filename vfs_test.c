/**
 * @file vfs_test.c
 * @brief Comprehensive test suite verifying indirect addressing, allocation boundaries,
 *        concurrency, sendfile zero-copy transfers, durability, and I/O throughput.
 */

#include "vfs.h"

#include <assert.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#define TEST_IMAGE "test_system.vfs"

/* =========================================================================
 * Assertion Macros
 * ======================================================================= */

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

static void cleanup_image(void) { unlink(TEST_IMAGE); }

/* =========================================================================
 * Category 1: Lifecycle & Persistence
 * ======================================================================= */

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

static bool test_persistence_durability_across_reopen(void) {
    vfs_t* vfs = NULL;
    cleanup_image();
    VFS_ASSERT_STATUS_OK(vfs_create(TEST_IMAGE, &vfs));

    const char* sample_data = "Persistent Data Content 12345";
    size_t sample_len = strlen(sample_data);
    VFS_ASSERT_STATUS_OK(vfs_write_file(vfs, "/persist.txt", sample_data, sample_len));
    VFS_ASSERT_STATUS_OK(vfs_write_file(vfs, "/to_delete.txt", "delete me", 9));
    VFS_ASSERT_STATUS_OK(vfs_unlink(vfs, "/to_delete.txt"));

    VFS_ASSERT_STATUS_OK(vfs_sync(vfs));
    vfs_close(vfs);
    vfs = NULL;

    /* Reopen read-only to verify durability */
    VFS_ASSERT_STATUS_OK(vfs_open(TEST_IMAGE, true, &vfs));
    VFS_ASSERT_TRUE(vfs_exists(vfs, "/persist.txt"));
    VFS_ASSERT_TRUE(!vfs_exists(vfs, "/to_delete.txt"));

    size_t read_len = 0;
    void* read_data = vfs_read_file(vfs, "/persist.txt", &read_len);
    VFS_ASSERT_TRUE(read_data != NULL);
    VFS_ASSERT_TRUE(read_len == sample_len);
    VFS_ASSERT_TRUE(memcmp(read_data, sample_data, sample_len) == 0);
    free(read_data);

    vfs_close(vfs);
    return true;
}

#define TEMP_DISK_IMAGE "temp_build_image.vfs"
#define TEST_FILE_PATH  "/assets/config.json"
#define TEST_PAYLOAD    "{\"database\": \"embedded_ram_db\", \"status\": \"active\"}"

static bool test_open_from_memory(void) {
    vfs_t* disk_vfs = NULL;
    unlink(TEMP_DISK_IMAGE);

    /* 1. Create a temporary image on disk */
    VFS_ASSERT_STATUS_OK(vfs_create(TEMP_DISK_IMAGE, &disk_vfs));
    vfs_fd_t fd = vfs_fopen(disk_vfs, TEST_FILE_PATH, VFS_O_CREAT | VFS_O_WRONLY);
    VFS_ASSERT_FD_OK(fd);

    size_t payload_len = sizeof(TEST_PAYLOAD) - 1;
    size_t written = 0;
    VFS_ASSERT_STATUS_OK(vfs_fwrite(disk_vfs, fd, TEST_PAYLOAD, payload_len, &written));
    VFS_ASSERT_TRUE(written == payload_len);
    VFS_ASSERT_STATUS_OK(vfs_fclose(disk_vfs, fd));
    vfs_close(disk_vfs);

    /* 2. Read into RAM buffer and remove disk image */
    FILE* f = fopen(TEMP_DISK_IMAGE, "rb");
    VFS_ASSERT_TRUE(f != NULL);
    fseek(f, 0, SEEK_END);
    long file_size = ftell(f);
    fseek(f, 0, SEEK_SET);

    void* memory_buffer = malloc((size_t)file_size);
    VFS_ASSERT_TRUE(memory_buffer != NULL);
    size_t read_bytes = fread(memory_buffer, 1, (size_t)file_size, f);
    VFS_ASSERT_TRUE(read_bytes == (size_t)file_size);
    fclose(f);
    unlink(TEMP_DISK_IMAGE);

    /* 3. Mount RAM image */
    vfs_t* mem_vfs = NULL;
    VFS_ASSERT_STATUS_OK(vfs_open_embedded(memory_buffer, (size_t)file_size, true, &mem_vfs));
    free(memory_buffer);

    /* 4. Validate content */
    VFS_ASSERT_TRUE(vfs_exists(mem_vfs, TEST_FILE_PATH));
    vfs_fd_t mem_fd = vfs_fopen(mem_vfs, TEST_FILE_PATH, VFS_O_RDONLY);
    VFS_ASSERT_FD_OK(mem_fd);

    char read_buffer[128] = {0};
    size_t read_count = 0;
    VFS_ASSERT_STATUS_OK(vfs_fread(mem_vfs, mem_fd, read_buffer, sizeof(read_buffer) - 1, &read_count));
    VFS_ASSERT_TRUE(read_count == payload_len);
    VFS_ASSERT_TRUE(strcmp(read_buffer, TEST_PAYLOAD) == 0);

    VFS_ASSERT_STATUS_OK(vfs_fclose(mem_vfs, mem_fd));
    vfs_close(mem_vfs);
    return true;
}

/* =========================================================================
 * Category 2: Core File I/O & Addressing
 * ======================================================================= */

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

    VFS_ASSERT_STATUS_OK(vfs_fclose(vfs, fd));
    vfs_close(vfs);
    return true;
}

static bool test_helpers_write_read_file(void) {
    vfs_t* vfs = NULL;
    cleanup_image();
    VFS_ASSERT_STATUS_OK(vfs_create(TEST_IMAGE, &vfs));

    const char* payload = "Hello, VFS Helper APIs! Testing vfs_write_file and vfs_read_file.";
    size_t len = strlen(payload);

    VFS_ASSERT_STATUS_OK(vfs_write_file(vfs, "/basic.txt", payload, len));
    VFS_ASSERT_TRUE(vfs_exists(vfs, "/basic.txt"));

    size_t read_len = 0;
    void* data = vfs_read_file(vfs, "/basic.txt", &read_len);
    VFS_ASSERT_TRUE(data != NULL);
    VFS_ASSERT_TRUE(read_len == len);
    VFS_ASSERT_TRUE(memcmp(data, payload, len) == 0);
    free(data);

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

static bool test_sequential_large_extent_write(void) {
    vfs_t* vfs = NULL;
    cleanup_image();
    VFS_ASSERT_STATUS_OK(vfs_create(TEST_IMAGE, &vfs));

    /* 4 MiB: Spans direct, single-indirect, and double-indirect blocks */
    size_t size = 4u * 1024u * 1024u;
    uint8_t* buf = malloc(size);
    VFS_ASSERT_TRUE(buf != NULL);
    for (size_t i = 0; i < size; i++) {
        buf[i] = (uint8_t)(i * 2654435761u >> 24);
    }

    vfs_fd_t fd = vfs_fopen(vfs, "/big.bin", VFS_O_CREAT | VFS_O_WRONLY | VFS_O_TRUNC);
    VFS_ASSERT_FD_OK(fd);

    size_t written = 0;
    VFS_ASSERT_STATUS_OK(vfs_fwrite(vfs, fd, buf, size, &written));
    VFS_ASSERT_TRUE(written == size);
    VFS_ASSERT_STATUS_OK(vfs_fclose(vfs, fd));

    vfs_stat_t st;
    VFS_ASSERT_STATUS_OK(vfs_stat(vfs, "/big.bin", &st));
    VFS_ASSERT_TRUE(st.size == size);
    VFS_ASSERT_TRUE(st.block_count == (size / VFS_BLOCK_SIZE));

    fd = vfs_fopen(vfs, "/big.bin", VFS_O_RDONLY);
    VFS_ASSERT_FD_OK(fd);
    uint8_t* readback = malloc(size);
    VFS_ASSERT_TRUE(readback != NULL);
    size_t bytes_read = 0;
    VFS_ASSERT_STATUS_OK(vfs_fread(vfs, fd, readback, size, &bytes_read));
    VFS_ASSERT_TRUE(bytes_read == size);
    VFS_ASSERT_TRUE(memcmp(buf, readback, size) == 0);
    VFS_ASSERT_STATUS_OK(vfs_fclose(vfs, fd));

    free(buf);
    free(readback);
    vfs_close(vfs);
    return true;
}

static bool test_block_overwrite_in_place(void) {
    vfs_t* vfs = NULL;
    cleanup_image();
    VFS_ASSERT_STATUS_OK(vfs_create(TEST_IMAGE, &vfs));

    uint8_t block_a[VFS_BLOCK_SIZE];
    uint8_t block_b[VFS_BLOCK_SIZE];
    memset(block_a, 0xAA, sizeof(block_a));
    memset(block_b, 0xBB, sizeof(block_b));

    VFS_ASSERT_STATUS_OK(vfs_write_file(vfs, "/overwrite.bin", block_a, sizeof(block_a)));

    vfs_fd_t fd = vfs_fopen(vfs, "/overwrite.bin", VFS_O_WRONLY);
    VFS_ASSERT_FD_OK(fd);
    size_t written = 0;
    VFS_ASSERT_STATUS_OK(vfs_fwrite(vfs, fd, block_b, sizeof(block_b), &written));
    VFS_ASSERT_TRUE(written == sizeof(block_b));
    VFS_ASSERT_STATUS_OK(vfs_fclose(vfs, fd));

    size_t read_len = 0;
    void* data = vfs_read_file(vfs, "/overwrite.bin", &read_len);
    VFS_ASSERT_TRUE(data != NULL);
    VFS_ASSERT_TRUE(read_len == sizeof(block_b));
    VFS_ASSERT_TRUE(memcmp(data, block_b, sizeof(block_b)) == 0);
    free(data);

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

/* =========================================================================
 * Category 3: Sparse Files & Truncate
 * ======================================================================= */

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

static bool test_sparse_hole_seek_write_read(void) {
    vfs_t* vfs = NULL;
    cleanup_image();
    VFS_ASSERT_STATUS_OK(vfs_create(TEST_IMAGE, &vfs));

    vfs_fd_t fd = vfs_fopen(vfs, "/sparse_seek.bin", VFS_O_CREAT | VFS_O_WRONLY | VFS_O_TRUNC);
    VFS_ASSERT_FD_OK(fd);

    off_t new_off = 0;
    VFS_ASSERT_STATUS_OK(vfs_fseek(vfs, fd, 1024 * 1024, VFS_SEEK_SET, &new_off));
    VFS_ASSERT_TRUE(new_off == 1024 * 1024);

    const char* tail = "end-of-sparse-file";
    size_t tail_len = strlen(tail);
    size_t written = 0;
    VFS_ASSERT_STATUS_OK(vfs_fwrite(vfs, fd, tail, tail_len, &written));
    VFS_ASSERT_TRUE(written == tail_len);
    VFS_ASSERT_STATUS_OK(vfs_fclose(vfs, fd));

    fd = vfs_fopen(vfs, "/sparse_seek.bin", VFS_O_RDONLY);
    VFS_ASSERT_FD_OK(fd);

    uint8_t hole_buf[4096];
    size_t bytes_read = 0;
    VFS_ASSERT_STATUS_OK(vfs_fread(vfs, fd, hole_buf, sizeof(hole_buf), &bytes_read));
    VFS_ASSERT_TRUE(bytes_read == sizeof(hole_buf));
    for (size_t i = 0; i < sizeof(hole_buf); i++) {
        VFS_ASSERT_TRUE(hole_buf[i] == 0);
    }

    VFS_ASSERT_STATUS_OK(vfs_fseek(vfs, fd, 1024 * 1024, VFS_SEEK_SET, NULL));
    char tail_read[32] = {0};
    VFS_ASSERT_STATUS_OK(vfs_fread(vfs, fd, tail_read, tail_len, &bytes_read));
    VFS_ASSERT_TRUE(bytes_read == tail_len);
    VFS_ASSERT_TRUE(memcmp(tail_read, tail, tail_len) == 0);

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

    /* 1. Write 6000 bytes (spans 2 blocks: 4096 + 1904) */
    uint8_t* pattern = malloc(6000);
    VFS_ASSERT_TRUE(pattern != NULL);
    memset(pattern, 'A', 6000);
    size_t written = 0;
    VFS_ASSERT_STATUS_OK(vfs_fwrite(vfs, fd, pattern, 6000, &written));
    VFS_ASSERT_TRUE(written == 6000);
    VFS_ASSERT_STATUS_OK(vfs_fclose(vfs, fd));

    vfs_stat_t st;
    VFS_ASSERT_STATUS_OK(vfs_stat(vfs, path, &st));
    VFS_ASSERT_TRUE(st.size == 6000);
    VFS_ASSERT_TRUE(st.block_count == 2);

    /* 2. Truncate DOWN to 3000 bytes: frees the 2nd block */
    VFS_ASSERT_STATUS_OK(vfs_truncate(vfs, path, 3000));
    VFS_ASSERT_STATUS_OK(vfs_stat(vfs, path, &st));
    VFS_ASSERT_TRUE(st.size == 3000);
    VFS_ASSERT_TRUE(st.block_count == 1);

    /* 3. Truncate UP to 10000 bytes:
     * Format v3 creates a sparse hole (POSIX behavior); physical block_count stays 1. */
    VFS_ASSERT_STATUS_OK(vfs_truncate(vfs, path, 10000));
    VFS_ASSERT_STATUS_OK(vfs_stat(vfs, path, &st));
    VFS_ASSERT_TRUE(st.size == 10000);
    VFS_ASSERT_TRUE(st.block_count == 1);

    /* 4. Verify data integrity: original 3000 bytes are intact, extended range reads as 0 */
    fd = vfs_fopen(vfs, path, VFS_O_RDONLY);
    VFS_ASSERT_FD_OK(fd);

    /* Check preserved data */
    uint8_t head_buf[100];
    size_t read_bytes = 0;
    VFS_ASSERT_STATUS_OK(vfs_fread(vfs, fd, head_buf, sizeof(head_buf), &read_bytes));
    VFS_ASSERT_TRUE(read_bytes == sizeof(head_buf));
    for (size_t i = 0; i < sizeof(head_buf); i++) {
        VFS_ASSERT_TRUE(head_buf[i] == 'A');
    }

    /* Check sparse extended region (past byte 3000) reads as zeroes */
    VFS_ASSERT_STATUS_OK(vfs_fseek(vfs, fd, 3000, VFS_SEEK_SET, NULL));
    uint8_t zero_check[100];
    VFS_ASSERT_STATUS_OK(vfs_fread(vfs, fd, zero_check, sizeof(zero_check), &read_bytes));
    VFS_ASSERT_TRUE(read_bytes == sizeof(zero_check));
    for (size_t i = 0; i < sizeof(zero_check); i++) {
        VFS_ASSERT_TRUE(zero_check[i] == 0);
    }

    /* 5. Overflow check: Exceeding total filesystem addressable capacity (> 64 GiB) */
    uint64_t beyond_max_capacity = ((uint64_t)VFS_TOTAL_BLOCKS * (uint64_t)VFS_BLOCK_SIZE) + 4096ULL;
    VFS_ASSERT_STATUS_EQ(vfs_truncate(vfs, path, beyond_max_capacity), VFS_ERR_OVERFLOW);

    VFS_ASSERT_STATUS_OK(vfs_fclose(vfs, fd));
    free(pattern);
    vfs_close(vfs);
    return true;
}

/* =========================================================================
 * Category 4: Metadata, Directory, & Filesystem Limits
 * ======================================================================= */

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
    VFS_ASSERT_STATUS_OK(vfs_write_file(vfs, src, payload, payload_len));
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
    VFS_ASSERT_STATUS_OK(vfs_write_file(vfs, victim, victim_payload, sizeof(victim_payload) - 1u));

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

static bool test_max_open_files_limit(void) {
    vfs_t* vfs = NULL;
    cleanup_image();
    VFS_ASSERT_STATUS_OK(vfs_create(TEST_IMAGE, &vfs));

    const char* path = "/limit_test.txt";
    vfs_fd_t fds[VFS_MAX_OPEN_FILES];

    for (uint32_t i = 0; i < VFS_MAX_OPEN_FILES; i++) {
        fds[i] = vfs_fopen(vfs, path, VFS_O_CREAT | VFS_O_RDWR);
        VFS_ASSERT_FD_OK(fds[i]);
    }

    vfs_fd_t extra_fd = vfs_fopen(vfs, path, VFS_O_RDWR);
    VFS_ASSERT_STATUS_EQ((vfs_status_t)extra_fd, VFS_ERR_NOSPACE);

    VFS_ASSERT_STATUS_OK(vfs_fclose(vfs, fds[0]));
    fds[0] = vfs_fopen(vfs, path, VFS_O_RDWR);
    VFS_ASSERT_FD_OK(fds[0]);

    for (uint32_t i = 0; i < VFS_MAX_OPEN_FILES; i++) {
        VFS_ASSERT_STATUS_OK(vfs_fclose(vfs, fds[i]));
    }

    vfs_close(vfs);
    return true;
}

/* =========================================================================
 * Category 5: Concurrency & Multi-threading
 * ======================================================================= */

typedef struct {
    vfs_t* vfs;
    int thread_id;
    bool ok;
} thread_arg_t;

static void* writer_thread(void* arg) {
    thread_arg_t* ta = (thread_arg_t*)arg;
    ta->ok = false;

    char path[64];
    snprintf(path, sizeof(path), "/concurrent/file_%d.bin", ta->thread_id);

    size_t size = 256u * 1024u; /* 256 KiB per thread */
    uint8_t* buf = malloc(size);
    if (!buf) {
        return NULL;
    }
    memset(buf, (uint8_t)(ta->thread_id & 0xFF), size);

    vfs_fd_t fd = vfs_fopen(ta->vfs, path, VFS_O_CREAT | VFS_O_WRONLY | VFS_O_TRUNC);
    if (fd < 0) {
        free(buf);
        return NULL;
    }

    size_t written = 0;
    vfs_status_t s = vfs_fwrite(ta->vfs, fd, buf, size, &written);
    vfs_fclose(ta->vfs, fd);
    if (s != VFS_OK || written != size) {
        free(buf);
        return NULL;
    }

    fd = vfs_fopen(ta->vfs, path, VFS_O_RDONLY);
    if (fd < 0) {
        free(buf);
        return NULL;
    }

    uint8_t* readback = malloc(size);
    if (!readback) {
        free(buf);
        vfs_fclose(ta->vfs, fd);
        return NULL;
    }

    size_t bytes_read = 0;
    s = vfs_fread(ta->vfs, fd, readback, size, &bytes_read);
    vfs_fclose(ta->vfs, fd);

    if (s == VFS_OK && bytes_read == size && memcmp(buf, readback, size) == 0) {
        ta->ok = true;
    }

    free(buf);
    free(readback);
    return NULL;
}

static bool test_multithreaded_concurrent_io(void) {
    vfs_t* vfs = NULL;
    cleanup_image();
    VFS_ASSERT_STATUS_OK(vfs_create(TEST_IMAGE, &vfs));

    enum { N_THREADS = 8 };
    pthread_t threads[N_THREADS];
    thread_arg_t args[N_THREADS];

    for (int i = 0; i < N_THREADS; i++) {
        args[i].vfs = vfs;
        args[i].thread_id = i;
        args[i].ok = false;
        VFS_ASSERT_TRUE(pthread_create(&threads[i], NULL, writer_thread, &args[i]) == 0);
    }

    for (int i = 0; i < N_THREADS; i++) {
        pthread_join(threads[i], NULL);
        VFS_ASSERT_TRUE(args[i].ok);
    }

    vfs_close(vfs);
    return true;
}

/* =========================================================================
 * Category 6: Zero-Copy Kernel Transfers (sendfile)
 * ======================================================================= */

static bool pipe_read_exact(int rfd, void* dst, size_t n) {
    uint8_t* p = (uint8_t*)dst;
    size_t rem = n;
    while (rem > 0) {
        ssize_t r = read(rfd, p, rem);
        if (r <= 0) {
            return false;
        }
        p += (size_t)r;
        rem -= (size_t)r;
    }
    return true;
}

static bool test_sendfile_full_transfer(void) {
    vfs_t* vfs = NULL;
    cleanup_image();
    VFS_ASSERT_STATUS_OK(vfs_create(TEST_IMAGE, &vfs));

    const size_t payload_len = 3u * 4096u;
    uint8_t* payload = malloc(payload_len);
    VFS_ASSERT_TRUE(payload != NULL);
    for (size_t i = 0; i < payload_len; i++) {
        payload[i] = (uint8_t)(i & 0xFF);
    }

    const char* path = "/sf_full.bin";
    VFS_ASSERT_STATUS_OK(vfs_write_file(vfs, path, payload, payload_len));

    vfs_fd_t fd = vfs_fopen(vfs, path, VFS_O_RDONLY);
    VFS_ASSERT_FD_OK(fd);

    int pipefd[2];
    VFS_ASSERT_TRUE(pipe(pipefd) == 0);

    size_t bytes_sent = 0;
    VFS_ASSERT_STATUS_OK(vfs_sendfile(vfs, pipefd[1], fd, NULL, payload_len, &bytes_sent));
    VFS_ASSERT_TRUE(bytes_sent == payload_len);

    off_t pos = 0;
    VFS_ASSERT_STATUS_OK(vfs_ftell(vfs, fd, &pos));
    VFS_ASSERT_TRUE(pos == (off_t)payload_len);

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

static bool test_sendfile_offset_semantics(void) {
    vfs_t* vfs = NULL;
    cleanup_image();
    VFS_ASSERT_STATUS_OK(vfs_create(TEST_IMAGE, &vfs));

    const char payload[64] = {0};
    for (int i = 0; i < 64; i++) {
        ((char*)payload)[i] = (char)('A' + (i % 26));
    }

    const char* path = "/sf_offset.bin";
    VFS_ASSERT_STATUS_OK(vfs_write_file(vfs, path, payload, sizeof(payload)));

    vfs_fd_t fd = vfs_fopen(vfs, path, VFS_O_RDONLY);
    VFS_ASSERT_FD_OK(fd);

    int pipefd[2];
    VFS_ASSERT_TRUE(pipe(pipefd) == 0);

    off_t off = 16;
    size_t count = 24;
    size_t bytes_sent = 0;
    VFS_ASSERT_STATUS_OK(vfs_sendfile(vfs, pipefd[1], fd, &off, count, &bytes_sent));
    VFS_ASSERT_TRUE(bytes_sent == count);
    VFS_ASSERT_TRUE(off == 16 + 24);

    off_t cursor = -1;
    VFS_ASSERT_STATUS_OK(vfs_ftell(vfs, fd, &cursor));
    VFS_ASSERT_TRUE(cursor == 0);

    char recv[24];
    VFS_ASSERT_TRUE(pipe_read_exact(pipefd[0], recv, sizeof(recv)));
    VFS_ASSERT_TRUE(memcmp(recv, payload + 16, count) == 0);

    close(pipefd[0]);
    close(pipefd[1]);
    VFS_ASSERT_STATUS_OK(vfs_fclose(vfs, fd));
    vfs_close(vfs);
    return true;
}

static bool test_sendfile_sparse_hole(void) {
    vfs_t* vfs = NULL;
    cleanup_image();
    VFS_ASSERT_STATUS_OK(vfs_create(TEST_IMAGE, &vfs));

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

    for (size_t i = 0; i < fsize; i++) {
        VFS_ASSERT_TRUE(recv[i] == 0);
    }

    free(recv);
    close(pipefd[0]);
    close(pipefd[1]);
    VFS_ASSERT_STATUS_OK(vfs_fclose(vfs, fd));
    vfs_close(vfs);
    return true;
}

static bool test_sendfile_rejects_writeonly_fd(void) {
    vfs_t* vfs = NULL;
    cleanup_image();
    VFS_ASSERT_STATUS_OK(vfs_create(TEST_IMAGE, &vfs));

    const char* path = "/sf_wo.bin";
    VFS_ASSERT_STATUS_OK(vfs_write_file(vfs, path, "data", 4));

    vfs_fd_t fd = vfs_fopen(vfs, path, VFS_O_WRONLY);
    VFS_ASSERT_FD_OK(fd);

    int pipefd[2];
    VFS_ASSERT_TRUE(pipe(pipefd) == 0);

    size_t bytes_sent = 1;
    vfs_status_t s = vfs_sendfile(vfs, pipefd[1], fd, NULL, 4, &bytes_sent);
    VFS_ASSERT_STATUS_EQ(s, VFS_ERR_INVAL);
    VFS_ASSERT_TRUE(bytes_sent == 0);

    close(pipefd[0]);
    close(pipefd[1]);
    VFS_ASSERT_STATUS_OK(vfs_fclose(vfs, fd));
    vfs_close(vfs);
    return true;
}

static bool test_sendfile_edge_cases(void) {
    vfs_t* vfs = NULL;
    cleanup_image();
    VFS_ASSERT_STATUS_OK(vfs_create(TEST_IMAGE, &vfs));

    const char* path = "/sf_edge.bin";
    VFS_ASSERT_STATUS_OK(vfs_write_file(vfs, path, "hello", 5));

    vfs_fd_t fd = vfs_fopen(vfs, path, VFS_O_RDONLY);
    VFS_ASSERT_FD_OK(fd);

    int pipefd[2];
    VFS_ASSERT_TRUE(pipe(pipefd) == 0);
    size_t bytes_sent = 0;

    VFS_ASSERT_STATUS_EQ(vfs_sendfile(NULL, pipefd[1], fd, NULL, 5, &bytes_sent), VFS_ERR_INVAL);
    VFS_ASSERT_STATUS_EQ(vfs_sendfile(vfs, -1, fd, NULL, 5, &bytes_sent), VFS_ERR_INVAL);
    VFS_ASSERT_STATUS_EQ(vfs_sendfile(vfs, pipefd[1], fd, NULL, 5, NULL), VFS_ERR_INVAL);
    VFS_ASSERT_STATUS_EQ(vfs_sendfile(vfs, pipefd[1], 99, NULL, 5, &bytes_sent), VFS_ERR_BADFD);

    bytes_sent = 1;
    VFS_ASSERT_STATUS_OK(vfs_sendfile(vfs, pipefd[1], fd, NULL, 0, &bytes_sent));
    VFS_ASSERT_TRUE(bytes_sent == 0);

    off_t past_eof = 9999;
    bytes_sent = 1;
    VFS_ASSERT_STATUS_OK(vfs_sendfile(vfs, pipefd[1], fd, &past_eof, 5, &bytes_sent));
    VFS_ASSERT_TRUE(bytes_sent == 0);

    close(pipefd[0]);
    close(pipefd[1]);
    VFS_ASSERT_STATUS_OK(vfs_fclose(vfs, fd));
    vfs_close(vfs);
    return true;
}

static bool test_sendfile_after_reopen(void) {
    vfs_t* vfs = NULL;
    cleanup_image();
    VFS_ASSERT_STATUS_OK(vfs_create(TEST_IMAGE, &vfs));

    const size_t payload_len = 300u * 4096u;
    uint8_t* payload = malloc(payload_len);
    VFS_ASSERT_TRUE(payload != NULL);
    for (size_t i = 0; i < payload_len; i++) {
        payload[i] = (uint8_t)((i ^ (i >> 8)) & 0xFF);
    }

    const char* path = "/sf_reopen.bin";
    VFS_ASSERT_STATUS_OK(vfs_write_file(vfs, path, payload, payload_len));
    vfs_close(vfs);
    vfs = NULL;

    VFS_ASSERT_STATUS_OK(vfs_open(TEST_IMAGE, true, &vfs));

    vfs_fd_t fd = vfs_fopen(vfs, path, VFS_O_RDONLY);
    VFS_ASSERT_FD_OK(fd);

    int pipefd[2];
    VFS_ASSERT_TRUE(pipe(pipefd) == 0);

    size_t total_sent = 0;
    uint8_t* recv = malloc(payload_len);
    VFS_ASSERT_TRUE(recv != NULL);
    uint8_t* rp = recv;
    const size_t chunk = 64u * 1024u;

    while (total_sent < payload_len) {
        size_t want = payload_len - total_sent;
        if (want > chunk) {
            want = chunk;
        }

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

static bool test_sendfile_to_host_file(void) {
    vfs_t* vfs = NULL;
    cleanup_image();
    VFS_ASSERT_STATUS_OK(vfs_create(TEST_IMAGE, &vfs));

    const size_t data_len = 5u * 4096u;
    uint8_t* payload = malloc(data_len);
    VFS_ASSERT_TRUE(payload != NULL);
    for (size_t i = 0; i < data_len; i++) {
        payload[i] = (uint8_t)((i * 31u) & 0xFF);
    }

    const char* path = "/sf_to_host.bin";
    VFS_ASSERT_STATUS_OK(vfs_write_file(vfs, path, payload, data_len));

    const size_t hole_len = 2u * 4096u;
    const size_t total_len = data_len + hole_len;
    VFS_ASSERT_STATUS_OK(vfs_truncate(vfs, path, total_len));

    vfs_fd_t fd = vfs_fopen(vfs, path, VFS_O_RDONLY);
    VFS_ASSERT_FD_OK(fd);

    const char* host_path = "sf_host_dest.bin";
    int out_fd = open(host_path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    VFS_ASSERT_TRUE(out_fd >= 0);

    size_t bytes_sent = 0;
    VFS_ASSERT_STATUS_OK(vfs_sendfile(vfs, out_fd, fd, NULL, total_len, &bytes_sent));
    VFS_ASSERT_TRUE(bytes_sent == total_len);

    off_t pos = 0;
    VFS_ASSERT_STATUS_OK(vfs_ftell(vfs, fd, &pos));
    VFS_ASSERT_TRUE(pos == (off_t)total_len);

    close(out_fd);

    out_fd = open(host_path, O_RDONLY);
    VFS_ASSERT_TRUE(out_fd >= 0);

    uint8_t* host_data = malloc(total_len);
    VFS_ASSERT_TRUE(host_data != NULL);
    ssize_t n = read(out_fd, host_data, total_len);
    VFS_ASSERT_TRUE(n == (ssize_t)total_len);

    VFS_ASSERT_TRUE(memcmp(host_data, payload, data_len) == 0);
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
 * Category 7: Quantitative Throughput Benchmarks
 * ======================================================================= */

#define BENCH_CHUNK_SIZE     (256 * 1024)           /* 256 KiB chunks */
#define BENCH_TOTAL_SIZE     (512ULL * 1024 * 1024) /* 512 MiB total  */
#define BENCH_PROGRESS_EVERY (64ULL * 1024 * 1024)  /* Print every 64 MiB */

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

/* =========================================================================
 * Main Runner Table
 * ======================================================================= */

typedef struct {
    const char* name;
    bool (*func)(void);
} test_t;

int main(void) {
    test_t tests[] = {
        /* Lifecycle & Storage */
        {"Lifecycle: Create, Open, Close", test_lifecycle_create_open_close},
        {"Lifecycle: Durability & Reopen State", test_persistence_durability_across_reopen},
        {"Lifecycle: Open from RAM / Embedded Memory", test_open_from_memory},

        /* Core File I/O */
        {"Core IO: Basic File Read/Write/Seek", test_file_create_and_basic_write_read},
        {"Core IO: Encapsulated Helper Read/Write", test_helpers_write_read_file},
        {"Core IO: Multiple Block Crossings (12 KiB)", test_read_write_large_file},
        {"Core IO: Sequential Extent Coalescing (4 MiB)", test_sequential_large_extent_write},
        {"Core IO: Overwrite In-Place without Realloc", test_block_overwrite_in_place},
        {"Core IO: Append Mode", test_append_mode},

        /* Sparse & Truncate */
        {"Allocation: Sparse Block Fills", test_sparse_reads_and_seek_past_eof},
        {"Allocation: Sparse Hole Seek & Partial Tail Read", test_sparse_hole_seek_write_read},
        {"Metadata: Truncate Extend/Shrink/Overflow", test_truncate_shrink_and_extend},

        /* Metadata & Filesystem Operations */
        {"Metadata: File Removal / Unlink", test_unlink},
        {"Metadata: Atomic Rename / Move & Collision", test_rename},
        {"System: Read-Only Structures Protection", test_read_only_mode},
        {"System: List Traversals and Prefix Matches", test_directory_listing},
        {"System: Edge Cases and Invalid Arguments", test_edge_cases_and_limits},
        {"System: Maximum Open File Descriptors (1024 Limit)", test_max_open_files_limit},

        /* Concurrency */
        {"Concurrency: 8 Multi-threaded Concurrent Writers", test_multithreaded_concurrent_io},

        /* Zero-Copy Transfers */
        {"sendfile: Full Transfer (Cursor Mode)", test_sendfile_full_transfer},
        {"sendfile: Partial Range (Offset Semantics)", test_sendfile_offset_semantics},
        {"sendfile: Sparse Hole Materialises Zeros", test_sendfile_sparse_hole},
        {"sendfile: Rejects Write-Only Source FD", test_sendfile_rejects_writeonly_fd},
        {"sendfile: Edge Cases and NULL Guards", test_sendfile_edge_cases},
        {"sendfile: Multi-Block Transfer after Reopen", test_sendfile_after_reopen},
        {"sendfile: Full Transfer to Host Filesystem File", test_sendfile_to_host_file},

        /* Benchmarks */
        {"Benchmark: Sequential Write Speed (512 MiB)", benchmark_write_throughput},
        {"Benchmark: Sequential Read Speed (512 MiB)", benchmark_read_throughput},
    };

    size_t test_count = sizeof(tests) / sizeof(tests[0]);
    size_t passed = 0;

    printf("Executing Virtual File System (VFS) Comprehensive Test Suite...\n");
    printf("===============================================================\n");

    for (size_t i = 0; i < test_count; i++) {
        printf("[%02zu/%02zu] RUNNING: %s...\n", i + 1, test_count, tests[i].name);
        if (tests[i].func()) {
            printf("[%02zu/%02zu] PASS\n\n", i + 1, test_count);
            passed++;
        } else {
            printf("[%02zu/%02zu] FAIL\n\n", i + 1, test_count);
        }
    }

    printf("===============================================================\n");
    printf("Result Summary: %zu of %zu tests/benchmarks passed.\n", passed, test_count);

    cleanup_image();

    if (passed == test_count) {
        return EXIT_SUCCESS;
    }
    return EXIT_FAILURE;
}
