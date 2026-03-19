#include "vfs.h"
#include "dev.h"
#include "fs.h"

#include <string.h>
#include <stddef.h>

/* -------------------------------------------------------------------------
 * Device-path mapping table
 *
 * Populated by vfs_mount_dev().  The VFS checks this table first when
 * opening a path; paths not found here are forwarded to the filesystem.
 * ------------------------------------------------------------------------- */
#define VFS_MAX_DEV_MOUNTS 8u

typedef struct {
    bool     used;
    char     path[VFS_PATH_MAX];
    dev_id_t id;
} dev_mount_t;

static dev_mount_t dev_mounts[VFS_MAX_DEV_MOUNTS];

/* -------------------------------------------------------------------------
 * Open file descriptor table
 * ------------------------------------------------------------------------- */
static vfs_fd_t fd_table[VFS_MAX_OPEN];

/* -------------------------------------------------------------------------
 * vfs_init
 * ------------------------------------------------------------------------- */
void vfs_init(void)
{
    memset(fd_table, 0, sizeof(fd_table));
    for (uint32_t i = 0u; i < VFS_MAX_OPEN; i++) {
        fd_table[i].used = false;
    }

    memset(dev_mounts, 0, sizeof(dev_mounts));
    for (uint32_t i = 0u; i < VFS_MAX_DEV_MOUNTS; i++) {
        dev_mounts[i].used = false;
    }

    /* Register standard device paths. */
    vfs_mount_dev("/dev/console", DEV_CONSOLE);
    vfs_mount_dev("/dev/timer",   DEV_TIMER);
    vfs_mount_dev("/dev/flash",   DEV_FLASH);
    vfs_mount_dev("/dev/gpio",    DEV_GPIO);
#ifdef PICOOS_DISPLAY_ENABLE
    vfs_mount_dev("/dev/display", DEV_DISPLAY);
#endif
#ifdef PICOOS_LED_ENABLE
    vfs_mount_dev("/dev/led", DEV_LED);
#endif
}

/* -------------------------------------------------------------------------
 * vfs_mount_dev
 * ------------------------------------------------------------------------- */
int vfs_mount_dev(const char *path, dev_id_t id)
{
    if (path == NULL) {
        return -1;
    }

    for (uint32_t i = 0u; i < VFS_MAX_DEV_MOUNTS; i++) {
        if (!dev_mounts[i].used) {
            dev_mounts[i].used = true;
            dev_mounts[i].id   = id;
            strncpy(dev_mounts[i].path, path, VFS_PATH_MAX - 1u);
            dev_mounts[i].path[VFS_PATH_MAX - 1u] = '\0';
            return 0;
        }
    }
    return -1;   /* mount table full */
}

/* -------------------------------------------------------------------------
 * Internal: find a free fd slot
 * ------------------------------------------------------------------------- */
/* fd_alloc — find and return the index of the first free slot in fd_table.
 * Returns -1 if all VFS_MAX_OPEN slots are currently in use.  The caller is
 * responsible for marking the returned slot as used before releasing any lock
 * that prevents concurrent allocation. */
static int fd_alloc(void)
{
    for (int i = 0; i < (int)VFS_MAX_OPEN; i++) {
        if (!fd_table[i].used) {
            return i;
        }
    }
    return -1;
}

/* -------------------------------------------------------------------------
 * Internal: look up a device mount by path
 * ------------------------------------------------------------------------- */
/* dev_mount_find — search the device mount table for an entry whose path
 * matches path exactly (up to VFS_PATH_MAX characters).
 * Returns the table index on success, or -1 if no matching mount is found.
 * A return value of -1 means the path should be forwarded to the filesystem
 * layer rather than dispatched to a device driver. */
static int dev_mount_find(const char *path)
{
    for (uint32_t i = 0u; i < VFS_MAX_DEV_MOUNTS; i++) {
        if (dev_mounts[i].used &&
            strncmp(dev_mounts[i].path, path, VFS_PATH_MAX) == 0) {
            return (int)i;
        }
    }
    return -1;
}

/* -------------------------------------------------------------------------
 * vfs_open
 * ------------------------------------------------------------------------- */
int vfs_open(const char *path, int mode)
{
    if (path == NULL) {
        return -1;
    }

    int fd = fd_alloc();
    if (fd < 0) {
        return -1;
    }

    vfs_fd_t *f = &fd_table[fd];

    /* Check if this path maps to a registered device. */
    int mount_idx = dev_mount_find(path);
    if (mount_idx >= 0) {
        dev_id_t did = dev_mounts[mount_idx].id;

        if (dev_open(did) < 0) {
            return -1;
        }

        f->used      = true;
        f->type      = VFS_TYPE_DEV;
        f->pos       = 0u;
        f->dev_id    = did;
        f->fs_file_id = 0u;
        f->mode      = mode;
        strncpy(f->path, path, VFS_PATH_MAX - 1u);
        f->path[VFS_PATH_MAX - 1u] = '\0';

        return fd;
    }

    /* Otherwise delegate to the filesystem layer. */
    int fs_fd = fs_open(path, mode);
    if (fs_fd < 0) {
        return -1;
    }

    f->used       = true;
    f->type       = VFS_TYPE_FILE;
    f->pos        = 0u;
    f->dev_id     = DEV_COUNT;   /* invalid / unused */
    f->fs_file_id = (uint32_t)fs_fd;
    f->mode       = mode;
    strncpy(f->path, path, VFS_PATH_MAX - 1u);
    f->path[VFS_PATH_MAX - 1u] = '\0';

    return fd;
}

/* -------------------------------------------------------------------------
 * vfs_read
 * ------------------------------------------------------------------------- */
int vfs_read(int fd, uint8_t *buf, uint32_t n)
{
    if (fd < 0 || fd >= (int)VFS_MAX_OPEN || !fd_table[fd].used) {
        return -1;
    }

    vfs_fd_t *f = &fd_table[fd];

    if (f->type == VFS_TYPE_DEV) {
        return dev_read(f->dev_id, buf, n);
    }

    /* Filesystem file: delegate with position tracking. */
    int result = fs_read((int)f->fs_file_id, buf, n);
    if (result > 0) {
        f->pos += (uint32_t)result;
    }
    return result;
}

/* -------------------------------------------------------------------------
 * vfs_write
 * ------------------------------------------------------------------------- */
int vfs_write(int fd, const uint8_t *buf, uint32_t n)
{
    if (fd < 0 || fd >= (int)VFS_MAX_OPEN || !fd_table[fd].used) {
        return -1;
    }

    vfs_fd_t *f = &fd_table[fd];

    if (f->type == VFS_TYPE_DEV) {
        return dev_write(f->dev_id, buf, n);
    }

    int result = fs_write((int)f->fs_file_id, buf, n);
    if (result > 0) {
        f->pos += (uint32_t)result;
    }
    return result;
}

/* -------------------------------------------------------------------------
 * vfs_close
 * ------------------------------------------------------------------------- */
int vfs_close(int fd)
{
    if (fd < 0 || fd >= (int)VFS_MAX_OPEN || !fd_table[fd].used) {
        return -1;
    }

    vfs_fd_t *f = &fd_table[fd];

    if (f->type == VFS_TYPE_DEV) {
        dev_close(f->dev_id);
    } else {
        fs_close((int)f->fs_file_id);
    }

    f->used = false;
    return 0;
}
