/* Helpers required by ICU/Lua with the native-title dynamic libc pipeline. */
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdbool.h>
#include <stdint.h>
#include <sys/stat.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/* The existing ps5_libc_shims.c locale implementation supports the C locale
 * only. Its locale-specific counterpart reports the same byte limit. */
int ___mb_cur_max(void)
{
    return 1;
}

/* Create an actual anonymous temporary file: keep its descriptor open after
 * unlinking the name, so the kernel removes its contents on fclose/exit.
 * The native title's writable directory is /download0. TMPDIR can override it.
 */
FILE *tmpfile(void)
{
    const char *directory = getenv("TMPDIR");
    if (directory == NULL || directory[0] == '\0') {
        directory = "/download0";
    }
    size_t length = strlen(directory);
    static const char suffix[] = "/openmw-XXXXXX";
    if (length > (size_t)-1 - sizeof(suffix)) {
        errno = ENAMETOOLONG;
        return NULL;
    }
    char *path = malloc(length + sizeof(suffix));
    if (path == NULL) {
        errno = ENOMEM;
        return NULL;
    }
    memcpy(path, directory, length);
    memcpy(path + length, suffix, sizeof(suffix));
    int descriptor = mkstemp(path);
    if (descriptor < 0) {
        int error = errno;
        free(path);
        errno = error;
        return NULL;
    }
    if (unlink(path) != 0) {
        int error = errno;
        close(descriptor);
        free(path);
        errno = error;
        return NULL;
    }
    free(path);
    FILE *stream = fdopen(descriptor, "w+b");
    if (stream == NULL) {
        int error = errno;
        close(descriptor);
        errno = error;
    }
    return stream;
}

/* Directory backend adapted from PS5_PayloadSDK/platform/src/directory.c.
 * Copyright (C) 2026 Mihawk. SPDX-License-Identifier: GPL-3.0-or-later.
 * Native-title libc refuses opendir; libkernel's getdents remains available.
 */
struct ps5_directory_stream {
    int fd;
    size_t offset, bytes;
    bool finished;
    struct dirent entry;
    char buffer[64 * 1024];
};

DIR *fdopendir(int descriptor)
{
    struct stat status;
    if (fstat(descriptor, &status) != 0)
        return NULL;
    if (!S_ISDIR(status.st_mode)) {
        errno = ENOTDIR;
        return NULL;
    }
    struct ps5_directory_stream *stream = calloc(1, sizeof(*stream));
    if (stream == NULL) {
        errno = ENOMEM;
        return NULL;
    }
    stream->fd = descriptor;
    return (DIR *)stream;
}

DIR *opendir(const char *path)
{
    int descriptor = open(path, O_RDONLY | O_DIRECTORY);
    if (descriptor < 0)
        return NULL;
    DIR *stream = fdopendir(descriptor);
    if (stream == NULL) {
        int error = errno;
        close(descriptor);
        errno = error;
    }
    return stream;
}

struct dirent *readdir(DIR *opaque)
{
    struct ps5_directory_stream *stream = (struct ps5_directory_stream *)opaque;
    if (stream == NULL) {
        errno = EBADF;
        return NULL;
    }
    while (!stream->finished) {
        if (stream->offset == stream->bytes) {
            int count = getdents(stream->fd, stream->buffer, sizeof(stream->buffer));
            if (count < 0)
                return NULL;
            if (count == 0) {
                stream->finished = true;
                return NULL;
            }
            if ((size_t)count > sizeof(stream->buffer)) {
                stream->finished = true;
                errno = EIO;
                return NULL;
            }
            stream->bytes = (size_t)count;
            stream->offset = 0;
        }
        const char *record = stream->buffer + stream->offset;
        size_t remaining = stream->bytes - stream->offset;
        uint32_t inode = 0;
        uint16_t length = 0;
        if (remaining >= 8) {
            memcpy(&inode, record, 4);
            memcpy(&length, record + 4, 2);
        }
        size_t name_length = remaining >= 8 ? (uint8_t)record[7] : 0;
        if (remaining < 8 || length < 8 + name_length + 1 || length > remaining ||
            name_length >= sizeof(stream->entry.d_name) || record[8 + name_length] != 0) {
            stream->finished = true;
            errno = EIO;
            return NULL;
        }
        stream->offset += length;
        if (inode == 0)
            continue;
        memset(&stream->entry, 0, sizeof(stream->entry));
        stream->entry.d_fileno = inode;
        stream->entry.d_reclen = length;
        stream->entry.d_type = (uint8_t)record[6];
        stream->entry.d_namlen = (uint8_t)name_length;
        memcpy(stream->entry.d_name, record + 8, name_length + 1);
        return &stream->entry;
    }
    return NULL;
}

int closedir(DIR *opaque)
{
    struct ps5_directory_stream *stream = (struct ps5_directory_stream *)opaque;
    if (stream == NULL) {
        errno = EBADF;
        return -1;
    }
    int result = close(stream->fd);
    int error = errno;
    free(stream);
    errno = error;
    return result;
}

int dirfd(DIR *opaque)
{
    struct ps5_directory_stream *stream = (struct ps5_directory_stream *)opaque;
    if (stream == NULL) {
        errno = EBADF;
        return -1;
    }
    return stream->fd;
}

void rewinddir(DIR *opaque)
{
    struct ps5_directory_stream *stream = (struct ps5_directory_stream *)opaque;
    if (stream == NULL) {
        errno = EBADF;
        return;
    }
    if (lseek(stream->fd, 0, SEEK_SET) < 0)
        return;
    stream->offset = stream->bytes = 0;
    stream->finished = false;
}
