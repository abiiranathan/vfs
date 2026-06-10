/**
 * @file vfs_demo.c
 * @brief Practical demo simulating a system configuration and logging manager.
 *
 * Compile instruction (linking against the static library built by the Makefile):
 *   make
 *   gcc -Wall -Wextra -static vfs_demo.c -L. -lvfs -lpthread -o vfs_demo
 *   ./vfs_demo
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include "vfs.h"

#define VFS_CONTAINER "system_container.vfs"

/* Helper to read and display file content on stdout */
static vfs_status_t display_vfs_file(vfs_t* vfs, const char* path) {
    vfs_fd_t fd = vfs_fopen(vfs, path, VFS_O_RDONLY);
    if (fd < 0) { return (vfs_status_t)fd; }

    char buffer[256];
    size_t read_bytes = 0;
    vfs_status_t status;

    printf("--- Content of [%s] ---\n", path);
    do {
        status = vfs_fread(vfs, fd, buffer, sizeof(buffer) - 1, &read_bytes);
        if (status == VFS_OK && read_bytes > 0) {
            buffer[read_bytes] = '\0';
            printf("%s", buffer);
        }
    } while (status == VFS_OK && read_bytes > 0);
    printf("\n-----------------------\n\n");

    vfs_fclose(vfs, fd);
    return status;
}

/* Callback function used by vfs_list to display container files */
static bool list_callback(const char* path, const vfs_stat_t* st, void* userdata) {
    (void)userdata;
    char time_buf[26];
    struct tm* tm_info = localtime(&st->modified_at);
    strftime(time_buf, sizeof(time_buf), "%Y-%m-%d %H:%M:%S", tm_info);

    printf("  %-25s  Size: %-6lu bytes  Blocks: %-3u  Mtime: %s\n", path, (unsigned long)st->size, st->block_count,
           time_buf);
    return true; /* Return true to continue iterating */
}

int main(void) {
    vfs_t* vfs = NULL;
    vfs_status_t status;

    // Remove any leftover container image
    unlink(VFS_CONTAINER);

    printf("Creating new virtual filesystem container: %s\n\n", VFS_CONTAINER);
    status = vfs_create(VFS_CONTAINER, &vfs);
    if (status != VFS_OK) {
        fprintf(stderr, "Failed to create VFS image: %s\n", vfs_strerror(status));
        return EXIT_FAILURE;
    }

    /* -------------------------------------------------------------------------
     * 1. Writing configuration files
     * ---------------------------------------------------------------------- */
    printf("[1/5] Writing application configuration files...\n");

    const char network_config
        [] = "# Network Settings\n"
             "ip_address=192.168.1.50\n"
             "gateway=192.168.1.1\n"
             "dns=8.8.8.8\n";

    const char database_config
        [] = "# Database Connection Pool\n"
             "db_host=localhost\n"
             "db_port=5432\n"
             "db_user=vfs_admin\n"
             "max_connections=10\n";

    if (write_vfs_file(vfs, "/config/network.cfg", network_config, sizeof(network_config) - 1) != VFS_OK ||
        write_vfs_file(vfs, "/config/database.cfg", database_config, sizeof(database_config) - 1) != VFS_OK) {
        fprintf(stderr, "Error writing configurations.\n");
        vfs_close(vfs);
        return EXIT_FAILURE;
    }

    /* -------------------------------------------------------------------------
     * 2. Appending to a system log file
     * ---------------------------------------------------------------------- */
    printf("[2/5] Appending diagnostic entries to system logs...\n");

    append_vfs_file(vfs, "/logs/system.log", "2026-06-09 18:00:01 [INFO] Kernel initialized.\n", 47);
    append_vfs_file(vfs, "/logs/system.log", "2026-06-09 18:00:02 [INFO] Network interface up.\n", 49);
    append_vfs_file(vfs, "/logs/system.log", "2026-06-09 18:00:05 [WARN] Database host port check slow.\n", 58);

    /* -------------------------------------------------------------------------
     * 3. Reading back and processing the virtual files
     * ---------------------------------------------------------------------- */
    printf("[3/5] Reading back written config and log files:\n\n");
    display_vfs_file(vfs, "/config/network.cfg");
    display_vfs_file(vfs, "/logs/system.log");

    /* -------------------------------------------------------------------------
     * 4. Listing specific directory-like partitions
     * ---------------------------------------------------------------------- */
    printf("[4/5] Traversing configurations namespace (/config prefix):\n");
    vfs_list(vfs, "/config", list_callback, NULL);
    printf("\n");

    printf("Traversing entire VFS contents (all prefixes):\n");
    vfs_list(vfs, "/", list_callback, NULL);
    printf("\n");

    /* -------------------------------------------------------------------------
     * 5. Performing a specific metadata lookup (vfs_stat)
     * ---------------------------------------------------------------------- */
    printf("[5/5] Querying metadata for the database configuration file:\n");
    vfs_stat_t st;
    status = vfs_stat(vfs, "/config/database.cfg", &st);
    if (status == VFS_OK) {
        printf("  Path verified  : %s\n", st.path);
        printf("  Bytes resolved : %lu\n", (unsigned long)st.size);
        printf("  Disk Blocks    : %u (allocated size: %u bytes)\n", st.block_count, st.block_count * VFS_BLOCK_SIZE);
    } else {
        fprintf(stderr, "Could not read stat for database config: %s\n", vfs_strerror(status));
    }

    /* Clean exit */
    printf("\nClosing VFS. Structural changes flushed to disk.\n");
    vfs_close(vfs);

    return EXIT_SUCCESS;
}

// Run: `make` to build the library `libvsf.a` first. Then run:
// gcc -Wall -Wextra -static vfs_demo.c -L. -lvfs -lpthread -o vfs_demo
