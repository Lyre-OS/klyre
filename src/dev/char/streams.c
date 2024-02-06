#include <stddef.h>
#include <stdint.h>
#include <lib/errno.k.h>
#include <lib/libc.k.h>
#include <lib/random.k.h>
#include <lib/resource.k.h>
#include <fs/devtmpfs.k.h>
#include <dev/char/streams.k.h>

static ssize_t null_read(struct resource *this, struct f_description *description, void *buf, off_t offset, size_t count) {
    (void)this;
    (void)description;
    (void)buf;
    (void)offset;
    (void)count;
    return 0;
}

static ssize_t null_write(struct resource *this, struct f_description *description, const void *buf, off_t offset, size_t count) {
    (void)this;
    (void)description;
    (void)buf;
    (void)offset;
    return count;
}

static ssize_t full_read(struct resource *this, struct f_description *description, void *buf, off_t offset, size_t count) {
    (void)this;
    (void)description;
    (void)offset;
    memset(buf, 0, count);
    return count;
}

static ssize_t full_write(struct resource *this, struct f_description *description, const void *buf, off_t offset, size_t count) {
    (void)this;
    (void)description;
    (void)buf;
    (void)offset;
    (void)count;
    errno = ENOSPC;
    return -1;
}

static ssize_t zero_read(struct resource *this, struct f_description *description, void *buf, off_t offset, size_t count) {
    (void)this;
    (void)description;
    (void)offset;
    memset(buf, 0, count);
    return count;
}

static ssize_t zero_write(struct resource *this, struct f_description *description, const void *buf, off_t offset, size_t count) {
    (void)this;
    (void)description;
    (void)buf;
    (void)offset;
    return count;
}

static ssize_t urandom_read(struct resource *this, struct f_description *description, void *buf, off_t offset, size_t count) {
    (void)this;
    (void)description;
    (void)offset;
    random_fill(buf, count);
    return count;
}

static ssize_t urandom_write(struct resource *this, struct f_description *description, const void *buf, off_t offset, size_t count) {
    (void)this;
    (void)description;
    (void)buf;
    (void)offset;
    return count;
}

void streams_init(void) {
    struct resource *null = resource_create(sizeof(struct resource));
    null->read = null_read;
    null->write = null_write;
    null->stat.st_size = 0;
    null->stat.st_blocks = 0;
    null->stat.st_blksize = 4096;
    null->stat.st_rdev = resource_create_dev_id();
    null->stat.st_mode = 0666 | S_IFCHR;
    devtmpfs_add_device(null, "null");

    struct resource *full = resource_create(sizeof(struct resource));
    full->read = full_read;
    full->write = full_write;
    full->stat.st_size = 0;
    full->stat.st_blocks = 0;
    full->stat.st_blksize = 4096;
    full->stat.st_rdev = resource_create_dev_id();
    full->stat.st_mode = 0666 | S_IFCHR;
    devtmpfs_add_device(full, "full");

    struct resource *zero = resource_create(sizeof(struct resource));
    zero->read = zero_read;
    zero->write = zero_write;
    zero->stat.st_size = 0;
    zero->stat.st_blocks = 0;
    zero->stat.st_blksize = 4096;
    zero->stat.st_rdev = resource_create_dev_id();
    zero->stat.st_mode = 0666 | S_IFCHR;
    devtmpfs_add_device(zero, "zero");

    struct resource *urandom = resource_create(sizeof(struct resource));
    urandom->read = urandom_read;
    urandom->write = urandom_write;
    urandom->stat.st_size = 0;
    urandom->stat.st_blocks = 0;
    urandom->stat.st_blksize = 4096;
    urandom->stat.st_rdev = resource_create_dev_id();
    urandom->stat.st_mode = 0666 | S_IFCHR;
    devtmpfs_add_device(urandom, "urandom");
}
