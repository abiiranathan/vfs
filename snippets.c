#include <poppler/glib/poppler.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdlib.h>
#include "vfs.h"

/**
 * @brief Reads a PDF file from the virtual filesystem and instantiates a Poppler document.
 * 
 * @param vfs    Mounted VFS handle.
 * @param path   Absolute virtual path to the PDF file (e.g., "/documents/manual.pdf").
 * @param error  GLib error return location, or NULL to ignore.
 * @return A new PopplerDocument instance on success, or NULL on failure.
 */
PopplerDocument* load_pdf_from_vfs(vfs_t* vfs, const char* path, GError** error) {
    vfs_stat_t st;
    vfs_status_t status;
    vfs_fd_t fd;
    gpointer buffer = NULL;
    size_t bytes_read = 0;

    /* 1. Retrieve the file's size to allocate the exact memory footprint */
    status = vfs_stat(vfs, path, &st);
    if (status != VFS_OK) {
        if (error) {
            *error = g_error_new(G_FILE_ERROR, G_FILE_ERROR_NOENT, "VFS path error: %s", vfs_strerror(status));
        }
        return NULL;
    }

    /* 2. Open the file in the virtual filesystem */
    fd = vfs_fopen(vfs, path, VFS_O_RDONLY);
    if (fd < 0) {
        if (error) {
            *error = g_error_new(G_FILE_ERROR, G_FILE_ERROR_FAILED, "Failed to open VFS path: %s",
                                 vfs_strerror((vfs_status_t)fd));
        }
        return NULL;
    }

    /* 3. Allocate heap space using GLib's allocation helper */
    buffer = g_try_malloc(st.size);
    if (buffer == NULL && st.size > 0) {
        vfs_fclose(vfs, fd);
        if (error) { *error = g_error_new(G_FILE_ERROR, G_FILE_ERROR_NOMEM, "Heap allocation failed"); }
        return NULL;
    }

    /* 4. Stream the data from the virtual disk into our memory buffer */
    status = vfs_fread(vfs, fd, buffer, st.size, &bytes_read);
    vfs_fclose(vfs, fd);

    if (status != VFS_OK || bytes_read != st.size) {
        g_free(buffer);
        if (error) { *error = g_error_new(G_FILE_ERROR, G_FILE_ERROR_IO, "VFS read failed: %s", vfs_strerror(status)); }
        return NULL;
    }

    /* 5. Wrap the buffer inside a GBytes container.
     *    g_bytes_new_take transfers buffer memory ownership to the GBytes object.
     *    When the GBytes refcount hits 0, it calls g_free() automatically. */
    GBytes* bytes = g_bytes_new_take(buffer, st.size);

    /* 6. Construct the Poppler document using the wrapped bytes */
    PopplerDocument* doc = poppler_document_new_from_bytes(bytes, error);

    /* 7. Unreference the GBytes object immediately.
     *    The returned PopplerDocument takes its own internal reference to GBytes
     *    if required to keep the memory alive, preventing memory leaks. */
    g_bytes_unref(bytes);

    return doc;
}
