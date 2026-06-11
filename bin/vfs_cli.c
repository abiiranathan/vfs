#include <assert.h>
#include <fcntl.h>
#include <getopt.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

#include "vfs.h"

#ifndef UNUSED
#define UNUSED(x) (void)(x)
#endif

static void print_usage(const char* prog_name) {
    fprintf(stderr, "Usage: %s -i <image> <mode> [args]\n", prog_name);
    fprintf(stderr, "\nGlobal Options:\n");
    fprintf(stderr, "  -i <image_path>          Path to the VFS container file (Required)\n");
    fprintf(stderr, "\nOperation Modes (Select Exactly One):\n");
    fprintf(stderr, "  -c                       Create a new VFS container\n");
    fprintf(stderr, "  -l [prefix]              List files inside the VFS (optional prefix matching)\n");
    fprintf(stderr, "  -a <local_src> <vfs_dst> Add a host file to the VFS\n");
    fprintf(stderr, "  -x <vfs_src> <local_dst> Extract a file from the VFS to the host\n");
    fprintf(stderr, "  -r <vfs_path>            Remove a file from the VFS\n");
    fprintf(stderr, "  -n <vfs_old> <vfs_new>   Rename a file inside the VFS\n");
    fprintf(stderr, "  -s <vfs_path>            Display file statistics\n");
    fprintf(stderr, "  -d                       Dump metadata diagnostics (superblock/tables)\n");
}

/* =========================================================================
 * Operation Handlers
 * ======================================================================= */

static int do_create(const char* image_path) {
    vfs_t* vfs = NULL;
    vfs_status_t s = vfs_create(image_path, &vfs);
    if (s != VFS_OK) {
        fprintf(stderr, "Error: Failed to create VFS image '%s': %s\n", image_path, vfs_strerror(s));
        return EXIT_FAILURE;
    }
    vfs_close(vfs);
    printf("Successfully created VFS container: %s\n", image_path);
    return EXIT_SUCCESS;
}

static bool list_cb(const char* path, const vfs_stat_t* st, void* userdata) {
    UNUSED(userdata);
    struct tm* tm_info = localtime(&st->modified_at);
    char time_buf[26];
    if (tm_info) {
        strftime(time_buf, sizeof(time_buf), "%Y-%m-%d %H:%M:%S", tm_info);
    } else {
        strncpy(time_buf, "unknown", sizeof(time_buf));
    }

    printf("%12lu  %s  %s\n", (unsigned long)st->size, time_buf, path);
    return true;
}

static int do_list(const char* image_path, const char* prefix) {
    vfs_t* vfs = NULL;
    vfs_status_t s = vfs_open(image_path, true, &vfs); /* Read-only mount */
    if (s != VFS_OK) {
        fprintf(stderr, "Error: Failed to open VFS container: %s\n", vfs_strerror(s));
        return EXIT_FAILURE;
    }

    printf("%12s  %-19s  %s\n", "SIZE (Bytes)", "MODIFIED AT", "PATH");
    printf("----------------------------------------------------------------------\n");
    vfs_list(vfs, prefix, list_cb, NULL);

    vfs_close(vfs);
    return EXIT_SUCCESS;
}

static int do_add(const char* image_path, const char* local_path, const char* vfs_path) {
    vfs_t* vfs = NULL;
    vfs_status_t s = vfs_open(image_path, false, &vfs); /* Read-Write mount */
    if (s != VFS_OK) {
        fprintf(stderr, "Error: Failed to open VFS container: %s\n", vfs_strerror(s));
        return EXIT_FAILURE;
    }

    struct stat st;
    if (stat(local_path, &st) < 0) {
        perror("Error: Failed to access local file");
        vfs_close(vfs);
        return EXIT_FAILURE;
    }

    if (S_ISDIR(st.st_mode)) {
        fprintf(stderr, "Error: Local path '%s' is a directory\n", local_path);
        vfs_close(vfs);
        return EXIT_FAILURE;
    }

    vfs_fd_t v_fd = vfs_fopen(vfs, vfs_path, VFS_O_WRONLY | VFS_O_CREAT | VFS_O_TRUNC);
    if (v_fd < 0) {
        fprintf(stderr, "Error: Failed to open VFS file: %s\n", vfs_strerror((vfs_status_t)v_fd));
        vfs_close(vfs);
        return EXIT_FAILURE;
    }

    if (st.st_size > 0) {
        int l_fd = open(local_path, O_RDONLY);
        if (l_fd < 0) {
            perror("Error: Failed to open local file");
            vfs_fclose(vfs, v_fd);
            vfs_close(vfs);
            return EXIT_FAILURE;
        }

        /* Fast zero-copy memory mapping */
        void* src = mmap(NULL, (size_t)st.st_size, PROT_READ, MAP_SHARED, l_fd, 0);
        if (src == MAP_FAILED) {
            perror("Error: Failed to memory-map local file");
            close(l_fd);
            vfs_fclose(vfs, v_fd);
            vfs_close(vfs);
            return EXIT_FAILURE;
        }

        size_t written = 0;
        s = vfs_fwrite(vfs, v_fd, src, (size_t)st.st_size, &written);
        munmap(src, (size_t)st.st_size);
        close(l_fd);

        if (s != VFS_OK) {
            fprintf(stderr, "Error: Failed to write data: %s\n", vfs_strerror(s));
            vfs_fclose(vfs, v_fd);
            vfs_close(vfs);
            return EXIT_FAILURE;
        }

        if (written != (size_t)st.st_size) {
            fprintf(stderr, "Error: Incomplete write to VFS (%lu of %lu written)\n", (unsigned long)written,
                    (unsigned long)st.st_size);
            vfs_fclose(vfs, v_fd);
            vfs_close(vfs);
            return EXIT_FAILURE;
        }
    }

    vfs_fclose(vfs, v_fd);
    vfs_close(vfs);
    printf("Imported: '%s' -> VFS:'%s' (%lu bytes)\n", local_path, vfs_path, (unsigned long)st.st_size);
    return EXIT_SUCCESS;
}

static int do_extract(const char* image_path, const char* vfs_path, const char* local_path) {
    vfs_t* vfs = NULL;
    vfs_status_t s = vfs_open(image_path, true, &vfs); /* Read-only mount */
    if (s != VFS_OK) {
        fprintf(stderr, "Error: Failed to open VFS container: %s\n", vfs_strerror(s));
        return EXIT_FAILURE;
    }

    vfs_fd_t v_fd = vfs_fopen(vfs, vfs_path, VFS_O_RDONLY);
    if (v_fd < 0) {
        fprintf(stderr, "Error: Failed to open VFS file: %s\n", vfs_strerror((vfs_status_t)v_fd));
        vfs_close(vfs);
        return EXIT_FAILURE;
    }

    int l_fd = open(local_path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (l_fd < 0) {
        perror("Error: Failed to create host file");
        vfs_fclose(vfs, v_fd);
        vfs_close(vfs);
        return EXIT_FAILURE;
    }

    /* Fast chunk-buffered extraction loop */
    char buf[65536];
    size_t read_bytes = 0;
    size_t total_extracted = 0;
    int exit_code = EXIT_SUCCESS;

    while (vfs_fread(vfs, v_fd, buf, sizeof(buf), &read_bytes) == VFS_OK && read_bytes > 0) {
        const char* ptr = buf;
        size_t rem = read_bytes;
        while (rem > 0) {
            ssize_t w = write(l_fd, ptr, rem);
            if (w < 0) {
                perror("Error: Write to host destination failed");
                exit_code = EXIT_FAILURE;
                break;
            }
            ptr += w;
            rem -= (size_t)w;
        }
        if (exit_code == EXIT_FAILURE) { break; }
        total_extracted += read_bytes;
    }

    close(l_fd);
    vfs_fclose(vfs, v_fd);
    vfs_close(vfs);

    if (exit_code == EXIT_SUCCESS) {
        printf("Extracted: VFS:'%s' -> '%s' (%lu bytes)\n", vfs_path, local_path, (unsigned long)total_extracted);
    } else {
        unlink(local_path); /* Delete partial file on error */
    }
    return exit_code;
}

static int do_remove(const char* image_path, const char* vfs_path) {
    vfs_t* vfs = NULL;
    vfs_status_t s = vfs_open(image_path, false, &vfs);
    if (s != VFS_OK) {
        fprintf(stderr, "Error: Failed to open VFS container: %s\n", vfs_strerror(s));
        return EXIT_FAILURE;
    }

    s = vfs_unlink(vfs, vfs_path);
    vfs_close(vfs);

    if (s != VFS_OK) {
        fprintf(stderr, "Error: Failed to remove VFS file: %s\n", vfs_strerror(s));
        return EXIT_FAILURE;
    }

    printf("Removed: VFS:'%s'\n", vfs_path);
    return EXIT_SUCCESS;
}

static int do_rename(const char* image_path, const char* old_path, const char* new_path) {
    vfs_t* vfs = NULL;
    vfs_status_t s = vfs_open(image_path, false, &vfs);
    if (s != VFS_OK) {
        fprintf(stderr, "Error: Failed to open VFS container: %s\n", vfs_strerror(s));
        return EXIT_FAILURE;
    }

    s = vfs_rename(vfs, old_path, new_path);
    vfs_close(vfs);

    if (s != VFS_OK) {
        fprintf(stderr, "Error: Failed to rename: %s\n", vfs_strerror(s));
        return EXIT_FAILURE;
    }

    printf("Renamed: VFS:'%s' -> VFS:'%s'\n", old_path, new_path);
    return EXIT_SUCCESS;
}

static int do_stat(const char* image_path, const char* vfs_path) {
    vfs_t* vfs = NULL;
    vfs_status_t s = vfs_open(image_path, true, &vfs);
    if (s != VFS_OK) {
        fprintf(stderr, "Error: Failed to open VFS container: %s\n", vfs_strerror(s));
        return EXIT_FAILURE;
    }

    vfs_stat_t st;
    s = vfs_stat(vfs, vfs_path, &st);
    vfs_close(vfs);

    if (s != VFS_OK) {
        fprintf(stderr, "Error: Stat target error: %s\n", vfs_strerror(s));
        return EXIT_FAILURE;
    }

    struct tm* t_c = localtime(&st.created_at);
    char tc_buf[26] = "unknown";
    if (t_c) { strftime(tc_buf, sizeof(tc_buf), "%Y-%m-%d %H:%M:%S", t_c); }

    struct tm* t_m = localtime(&st.modified_at);
    char tm_buf[26] = "unknown";
    if (t_m) { strftime(tm_buf, sizeof(tm_buf), "%Y-%m-%d %H:%M:%S", t_m); }

    printf("File details inside VFS:\n");
    printf("  Path             : %s\n", st.path);
    printf("  Size             : %lu bytes\n", (unsigned long)st.size);
    printf("  Blocks Allocated : %u\n", st.block_count);
    printf("  Created At       : %s\n", tc_buf);
    printf("  Modified At      : %s\n", tm_buf);

    return EXIT_SUCCESS;
}

static int do_dump(const char* image_path) {
    vfs_t* vfs = NULL;
    vfs_status_t s = vfs_open(image_path, true, &vfs);
    if (s != VFS_OK) {
        fprintf(stderr, "Error: Failed to open VFS container: %s\n", vfs_strerror(s));
        return EXIT_FAILURE;
    }

    vfs_dump(vfs, stdout);
    vfs_close(vfs);
    return EXIT_SUCCESS;
}

/* =========================================================================
 * CLI Entry Point
 * ======================================================================= */

int main(int argc, char* argv[]) {
    int opt;
    const char* image_path = NULL;
    char mode = 0;
    const char* mode_arg = NULL;

    /* Added a colon after 's' to correctly indicate it requires an argument: "s:" */
    while ((opt = getopt(argc, argv, "i:cla:x:r:n:s:d")) != -1) {
        switch (opt) {
            case 'i':
                image_path = optarg;
                break;
            case 'c':
            case 'l':
            case 'd':
                if (mode != 0) {
                    fprintf(stderr, "Error: Multiple operation modes specified.\n");
                    return EXIT_FAILURE;
                }
                mode = (char)opt;
                break;
            case 'a':
            case 'x':
            case 'r':
            case 'n':
            case 's': /* Now correctly populates optarg */
                if (mode != 0) {
                    fprintf(stderr, "Error: Multiple operation modes specified.\n");
                    return EXIT_FAILURE;
                }
                mode = (char)opt;
                mode_arg = optarg;
                break;
            default:
                print_usage(argv[0]);
                return EXIT_FAILURE;
        }
    }

    if (image_path == NULL) {
        fprintf(stderr, "Error: Global option '-i <image_path>' is required.\n\n");
        print_usage(argv[0]);
        return EXIT_FAILURE;
    }

    if (mode == 0) {
        fprintf(stderr, "Error: An operation mode must be selected.\n\n");
        print_usage(argv[0]);
        return EXIT_FAILURE;
    }

    int remaining_args = argc - optind;

    switch (mode) {
        case 'c': /* Create */
            if (remaining_args != 0) {
                fprintf(stderr, "Error: Creation mode accepts no extra parameters.\n");
                return EXIT_FAILURE;
            }
            return do_create(image_path);

        case 'l': /* List */
            if (remaining_args > 1) {
                fprintf(stderr, "Error: List mode accepts at most one optional prefix filter.\n");
                return EXIT_FAILURE;
            }
            return do_list(image_path, (remaining_args == 1) ? argv[optind] : NULL);

        case 'a': /* Add: -a <local_src> <vfs_dst> */
            if (remaining_args != 1) {
                fprintf(stderr, "Error: Import requires a target destination inside the VFS.\n");
                fprintf(stderr, "Usage: %s -i %s -a <local_path> <vfs_path>\n", argv[0], image_path);
                return EXIT_FAILURE;
            }
            return do_add(image_path, mode_arg, argv[optind]);

        case 'x': /* Extract: -x <vfs_src> <local_dst> */
            if (remaining_args != 1) {
                fprintf(stderr, "Error: Extraction requires a local output destination.\n");
                fprintf(stderr, "Usage: %s -i %s -x <vfs_path> <local_path>\n", argv[0], image_path);
                return EXIT_FAILURE;
            }
            return do_extract(image_path, mode_arg, argv[optind]);

        case 'r': /* Remove: -r <vfs_path> */
            if (remaining_args != 0) {
                fprintf(stderr, "Error: Removal requires only the target VFS path.\n");
                return EXIT_FAILURE;
            }
            return do_remove(image_path, mode_arg);

        case 'n': /* Rename: -n <vfs_old> <vfs_new> */
            if (remaining_args != 1) {
                fprintf(stderr, "Error: Renaming requires the target VFS path.\n");
                fprintf(stderr, "Usage: %s -i %s -n <vfs_old> <vfs_new>\n", argv[0], image_path);
                return EXIT_FAILURE;
            }
            return do_rename(image_path, mode_arg, argv[optind]);

        case 's': /* Stat: -s <vfs_path> */
            if (remaining_args != 0) {
                fprintf(stderr, "Error: Stats query requires only the target VFS path.\n");
                return EXIT_FAILURE;
            }
            return do_stat(image_path, mode_arg);

        case 'd': /* Dump Diagnostics */
            if (remaining_args != 0) {
                fprintf(stderr, "Error: Diagnostics dumping accepts no extra parameters.\n");
                return EXIT_FAILURE;
            }
            return do_dump(image_path);

        default:
            break;
    }

    return EXIT_FAILURE;
}