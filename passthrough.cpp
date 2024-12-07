#define FUSE_USE_VERSION 29
#include <fuse.h>
#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <limits.h>
#include <dirent.h>

// Define DEBUG flag
#define DEBUG 0 // Set to 1 to enable logs, 0 to disable

// Macro for debug printing
#if DEBUG
    #define DEBUG_PRINT(fmt, ...) printf(fmt, ##__VA_ARGS__)
#else
    #define DEBUG_PRINT(fmt, ...) // No-op
#endif

static const char *real_root = "/Users/mistermediocre/Projects/sffs/passthrough_target";

static int passthrough_getattr(const char *path, struct stat *stbuf) {
	// If path has ., skip it
	if (strcmp(path, ".") == 0) {
		printf("DEBUG: passthrough_getattr - Path: %s\n", path);
		return -errno;
	}

    char real_path[PATH_MAX];
    snprintf(real_path, sizeof(real_path), "%s%s", real_root, path);

    // Log the getattr call
    DEBUG_PRINT("DEBUG: passthrough_getattr - Path: %s, Real Path: %s\n", path, real_path);

    if (lstat(real_path, stbuf) == -1) {
        DEBUG_PRINT("ERROR: passthrough_getattr - Failed to lstat: %s, Error: %s\n", real_path, strerror(errno));
        return -errno;
    }
    return 0;
}

static int passthrough_open(const char *path, struct fuse_file_info *fi) {
    char real_path[PATH_MAX];
    snprintf(real_path, sizeof(real_path), "%s%s", real_root, path);

    // Log the open attempt
    DEBUG_PRINT("DEBUG: passthrough_open - Opening file: %s with flags: 0x%x\n", real_path, fi->flags);

    // Open the real file with flags
    int fd = open(real_path, fi->flags, 0666); // Ensure permissions are provided for O_CREAT
    if (fd == -1) {
        DEBUG_PRINT("ERROR: passthrough_open - Failed to open file: %s, Error: %s\n", real_path, strerror(errno));
        return -errno;
    }

    // Store the file descriptor in the FUSE file handle
    fi->fh = fd;
    DEBUG_PRINT("DEBUG: passthrough_open - Successfully opened file: %s, FD: %d\n", real_path, fd);

    return 0;
}

static int passthrough_read(const char *path, char *buf, size_t size, off_t offset, struct fuse_file_info *fi) {
    (void) path;
    DEBUG_PRINT("DEBUG: passthrough_read - FD: %ld, Size: %zu, Offset: %lld\n", fi->fh, size, offset);

    ssize_t res = pread(fi->fh, buf, size, offset);
    if (res == -1) {
        DEBUG_PRINT("ERROR: passthrough_read - Failed to read, FD: %ld, Error: %s\n", fi->fh, strerror(errno));
        return -errno;
    }

    DEBUG_PRINT("DEBUG: passthrough_read - Read %zd bytes\n", res);
    return res;
}

static int passthrough_write(const char *path, const char *buf, size_t size, off_t offset, struct fuse_file_info *fi) {
    (void) path;
    DEBUG_PRINT("DEBUG: passthrough_write - FD: %ld, Size: %zu, Offset: %lld\n", fi->fh, size, offset);

    ssize_t res = pwrite(fi->fh, buf, size, offset);
    if (res == -1) {
        DEBUG_PRINT("ERROR: passthrough_write - Failed to write, FD: %ld, Error: %s\n", fi->fh, strerror(errno));
        return -errno;
    }

    DEBUG_PRINT("DEBUG: passthrough_write - Wrote %zd bytes\n", res);
    return res;
}

static int passthrough_release(const char *path, struct fuse_file_info *fi) {
    (void) path;
    DEBUG_PRINT("DEBUG: passthrough_release - FD: %ld\n", fi->fh);

    if (close(fi->fh) == -1) {
        DEBUG_PRINT("ERROR: passthrough_release - Failed to close FD: %ld, Error: %s\n", fi->fh, strerror(errno));
        return -errno;
    }

    DEBUG_PRINT("DEBUG: passthrough_release - File descriptor closed\n");
    return 0;
}

static int passthrough_readdir(const char *path, void *buf, fuse_fill_dir_t filler, off_t offset, struct fuse_file_info *fi) {
    (void) offset;
    (void) fi;

    char real_path[PATH_MAX];
    snprintf(real_path, sizeof(real_path), "%s%s", real_root, path);

    DIR *dp = opendir(real_path);
    if (!dp) {
        return -errno;
    }

    // Ensure "." and ".." are added
    filler(buf, ".", NULL, 0);
    filler(buf, "..", NULL, 0);

    struct dirent *de;
    while ((de = readdir(dp)) != NULL) {
        if (filler(buf, de->d_name, NULL, 0)) {
            closedir(dp);
            return -ENOMEM;
        }
    }

    closedir(dp);
    return 0;
}

static int passthrough_create(const char *path, mode_t mode, struct fuse_file_info *fi) {
    char real_path[PATH_MAX];
    snprintf(real_path, sizeof(real_path), "%s%s", real_root, path);

    // Log the create operation
    DEBUG_PRINT("DEBUG: passthrough_create - Creating file: %s with mode: 0%o\n", real_path, mode);

    // Create and open the real file
    int fd = open(real_path, fi->flags | O_CREAT, mode);
    if (fd == -1) {
        DEBUG_PRINT("ERROR: passthrough_create - Failed to create file: %s, Error: %s\n", real_path, strerror(errno));
        return -errno; // Return the error to FUSE
    }

    // Store the file descriptor in the FUSE file handle
    fi->fh = fd;

    DEBUG_PRINT("DEBUG: passthrough_create - Successfully created file: %s, FD: %d\n", real_path, fd);
    return 0;
}


static struct fuse_operations passthrough_ops = {
    .getattr = passthrough_getattr,
    .open = passthrough_open,
    .read = passthrough_read,
	.readdir = passthrough_readdir,
    .write = passthrough_write,
	.create = passthrough_create,
    .release = passthrough_release,
};

int main(int argc, char *argv[]) {
    DEBUG_PRINT("DEBUG: Starting FUSE passthrough\n");
    return fuse_main(argc, argv, &passthrough_ops, NULL);
}

