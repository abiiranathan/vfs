/**
 * @file vfs_demo.c
 * @brief Demo showcasing structural creation and directory listings.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include "vfs.h"

#define VFS_CONTAINER "system_container.vfs"

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

static bool list_callback(const char* path, const vfs_stat_t* st, void* userdata) {
    (void)userdata;
    char time_buf[26];
    struct tm* tm_info = localtime(&st->modified_at);
    strftime(time_buf, sizeof(time_buf), "%Y-%m-%d %H:%M:%S", tm_info);

    printf("  %-25s  Size: %-6lu bytes  Blocks: %-3u  Mtime: %s\n", path, (unsigned long)st->size, st->block_count,
           time_buf);
    return true;
}

int main(void) {
    vfs_t* vfs = NULL;
    vfs_status_t status;

    unlink(VFS_CONTAINER);

    printf("Creating scaled virtual filesystem container: %s\n\n", VFS_CONTAINER);
    status = vfs_create(VFS_CONTAINER, &vfs);
    if (status != VFS_OK) {
        fprintf(stderr, "Failed to create VFS image: %s\n", vfs_strerror(status));
        return EXIT_FAILURE;
    }

    printf("[1/5] Writing application configuration files...\n");
    const char network_config[] = "# Network Settings\nip_address=192.168.1.50\ngateway=192.168.1.1\ndns=8.8.8.8\n";
    const char database_config
        [] = "# Database Connection Pool\ndb_host=localhost\ndb_port=5432\ndb_user=vfs_admin\nmax_connections=10\n";

    if (write_vfs_file(vfs, "/config/network.cfg", network_config, sizeof(network_config) - 1) != VFS_OK ||
        write_vfs_file(vfs, "/config/database.cfg", database_config, sizeof(database_config) - 1) != VFS_OK) {
        fprintf(stderr, "Error writing configurations.\n");
        vfs_close(vfs);
        return EXIT_FAILURE;
    }

    printf("[2/5] Appending entries to logs...\n");
    append_vfs_file(vfs, "/logs/system.log", "2026-06-09 18:00:01 [INFO] Kernel initialized.\n", 47);
    append_vfs_file(vfs, "/logs/system.log", "2026-06-09 18:00:02 [INFO] Network interface up.\n", 49);

    printf("[3/5] Reading back written config and log files:\n\n");
    display_vfs_file(vfs, "/config/network.cfg");
    display_vfs_file(vfs, "/logs/system.log");

    printf("[4/5] Traversing configurations namespace (/config prefix):\n");
    vfs_list(vfs, "/config", list_callback, NULL);
    printf("\n");

    printf("[5/5] Querying metadata for the database configuration file:\n");
    vfs_stat_t st;
    status = vfs_stat(vfs, "/config/database.cfg", &st);
    if (status == VFS_OK) {
        printf("  Path verified  : %s\n", st.path);
        printf("  Bytes resolved : %lu\n", (unsigned long)st.size);
        printf("  Disk Blocks    : %u (allocated data size: %u bytes)\n", st.block_count,
               st.block_count * VFS_BLOCK_SIZE);
    } else {
        fprintf(stderr, "Could not read stat: %s\n", vfs_strerror(status));
    }

    printf("\nClosing VFS. Structural changes flushed to disk.\n");
    vfs_close(vfs);

    return EXIT_SUCCESS;
}
