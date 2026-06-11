#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#include <solidc/defer.h>
#include <solidc/file.h>
#include <solidc/filepath.h>

#include "vfs.h"

#define ROOT      "/home/nabiizy/Documents"
#define VFS_IMAGE "/home/nabiizy/pdf_container_test.vsf"

int count = 0;

WalkDirOption walk_dir_callback(const FileAttributes* attr, const char* path, const char* name, void* data) {
    UNUSED(name);

    /* Skip directories */
    if (fattr_is_dir(attr)) { return DirContinue; }

    vfs_t* vfs = data;

    /* 1. Derive a clean, relative path for the VFS */
    const char* vfs_path = path;
    size_t root_len = strlen(ROOT);
    if (strncmp(path, ROOT, root_len) == 0) {
        vfs_path = path + root_len;
        while (*vfs_path == '/') {
            vfs_path++;
        }
    }

    /* 2. Open target path in VFS */
    vfs_fd_t fd = vfs_fopen(vfs, vfs_path, VFS_O_WRONLY | VFS_O_CREAT);
    if (fd < 0) {
        fprintf(stderr, "Failed to open file in VFS container: %s (path: %s)\n", vfs_strerror((vfs_status_t)fd),
                vfs_path);
        return DirContinue;
    }
    defer {
        vfs_fclose(vfs, fd);
    };

    /* 3. Skip files instantly */
    if (attr->size == 0) { return DirContinue; }

    /* 4. Open source file using low-level descriptors for system compatibility with mmap */
    int infd = open(path, O_RDONLY);
    if (infd < 0) {
        fprintf(stderr, "Failed to open source file: %s\n", path);
        return DirContinue;
    }
    defer {
        close(infd);
    };

    /* 5. Memory-map the file for zero-copy read performance */
    void* src_data = mmap(NULL, attr->size, PROT_READ, MAP_SHARED, infd, 0);
    if (src_data == MAP_FAILED) {
        fprintf(stderr, "Failed to mmap source file: %s\n", path);
        return DirContinue;
    }
    defer {
        munmap(src_data, attr->size);
    };

    /* 6. Write mapped data directly to VFS */
    size_t written = 0;
    vfs_status_t status = vfs_fwrite(vfs, fd, src_data, attr->size, &written);
    if (status != VFS_OK) {
        fprintf(stderr, "Error writing to VFS: %s (file: %s)\n", vfs_strerror(status), vfs_path);
        return DirContinue;
    }

    if (attr->size != written) {
        fprintf(stderr, "Size mismatch: expected %lu bytes, wrote %lu (file: %s)\n", (unsigned long)attr->size,
                (unsigned long)written, vfs_path);
        return DirContinue;
    }

    ++count;
    printf("(%d) Copied %s -> VFS:%s (%lu bytes)\n", count, path, vfs_path, (unsigned long)written);
    return DirContinue;
}

int main(void) {
    vfs_t* vfs = NULL;
    vfs_status_t status;

    status = vfs_create(VFS_IMAGE, &vfs);
    if (status != VFS_OK) {
        fprintf(stderr, "Failed to create VFS image: %s\n", vfs_strerror(status));
        exit(EXIT_FAILURE);
    }

    dir_walk(ROOT, walk_dir_callback, vfs);

    vfs_close(vfs);
    return 0;
}
