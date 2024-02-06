#include <fs/vfs/vfs.k.h>
#include <fs/fat32fs.k.h>
#include <stddef.h>
#include <stdint.h>
#include <lib/debug.k.h>
#include <time/time.k.h>
#include <sys/stat.h>
#include <printf/printf.h>
#include <mm/vmm.k.h>
#include <mm/pmm.k.h>

typedef uint32_t cluster_t;

struct fat_bootrecord {
    uint8_t jmp[3];
    uint8_t oem[8];
    uint16_t bytespersector;
    uint8_t sectorspercluster;
    uint16_t reservedsectorcount;
    uint8_t fatcount;
    uint16_t rootdirentries;
    uint16_t sectorcount;
    uint16_t sectorsperfatunused;
    uint8_t mediadesc;
    uint32_t geometry;
    uint32_t hiddensectors;
    uint32_t largesectorcount;
    uint32_t sectorsperfat;
    uint16_t flags;
    uint16_t version;
    uint32_t rootdircluster;
    uint16_t fsinfosector;
    uint16_t backupbootsector;
    uint32_t reserved[3];
    uint8_t drive;
    uint8_t flagsnt;
    uint8_t signature;
    uint32_t volumeid;
    uint8_t volumelabel[11];
    uint8_t identifierstr[8];
} __attribute__((packed));

struct fat_direntry {
    uint8_t name[11];
    uint8_t attributes;
    uint8_t reserved;
    uint8_t createtimetenth;
    uint16_t creationtime;
    uint16_t creationdate;
    uint16_t accessdate;
    uint16_t clusterhigh;
    uint16_t lastmodtime;
    uint16_t lastmoddate;
    uint16_t clusterlow;
    uint32_t size;
} __attribute__((packed));

struct fat_lfndirentry {
    uint8_t order;
    uint16_t name1[5];
    uint8_t attribute;
    uint8_t reserved;
    uint8_t checksum;
    uint16_t name2[6];
    uint16_t zero;
    uint16_t name3[2];
} __attribute__((packed));

struct fat_filesysteminfo {
    uint32_t signature;
    uint32_t free;
    uint32_t startat;
    uint32_t reserved[3];
    uint32_t signature2;
} __attribute__((packed));

struct fat32fs {
    struct vfs_filesystem;
    struct fat_bootrecord br;
    struct fat_filesysteminfo fsinfo;
    struct vfs_node *device;
    size_t clustersize;
    size_t clustercount;
    off_t fatoffset;
    off_t dataoffset;
    ino_t currentinode;
};

struct fat32fs_resource {
    struct resource;
    struct fat32fs *fs;
    struct resource *dir;
    off_t diroffset;
    cluster_t cluster;
};

#define FINALCLUSTER 0xffffff8
#define IS_FINALCLUSTER(c) (c >= FINALCLUSTER)
#define DENT_GETCLUSTER(d) ((cluster_t)d->clusterlow | ((cluster_t)d->clusterhigh << 16));
#define DENT_SETCLUSTER(dent, val) \
(dent)->clusterlow = val & 0xffff;\
(dent)->clusterhigh = (val >> 16) & 0xffff;

#define ATTR_READONLY 0x1
#define ATTR_HIDDEN 0x2
#define ATTR_SYSTEM 0x4
#define ATTR_VOLUMEID 0x8
#define ATTR_DIR 0x10
#define ATTR_ARCHIVE 0x20
#define ATTR_DEVICE 0x40
#define ATTR_LFN (ATTR_VOLUMEID | ATTR_SYSTEM | ATTR_HIDDEN | ATTR_READONLY)

#define TIME_SECONDS(t) ((t & 0b11111) << 2)
#define TIME_MINUTES(t) (t & 0b11111100000)
#define TIME_HOUR(t) (t & 0b1111100000000000)
#define DATE_DAY(d) (t & 0b11111)
#define DATE_MONTH(d) (t & 0b111100000)
#define DATE_YEAR(d) (t & 0b1111111000000000)

static cluster_t fat32fs_next(struct fat32fs *fs, cluster_t cluster) {
    struct resource *res = fs->device->resource;
    ASSERT(res->read(res, NULL, &cluster, fs->fatoffset + cluster * 4, 4) > 0);
    return cluster;
}

static void fat32fs_setnext(struct fat32fs *fs, cluster_t cluster, cluster_t val) {
    struct resource *res = fs->device->resource;
    for (int fat = 0; fat < fs->br.fatcount; ++fat) { // change it on all FATs
        off_t currentfatoffset = fs->fatoffset + fs->br.sectorsperfat * fat * res->stat.st_blksize;
        ASSERT(res->write(res, NULL, &val, currentfatoffset + cluster * 4, 4) > 0);
    }
}

static cluster_t fat32fs_skip(struct fat32fs *fs, cluster_t cluster, size_t count, bool *end) {
    *end = false;
    for (size_t i = 0; i < count; ++i) {
        cluster_t next = fat32fs_next(fs, cluster);

        if (IS_FINALCLUSTER(next)) {
            *end = true;
            break;
        }

        cluster = next;
    }
    return cluster;
}

static size_t fat32fs_getchainsize(struct fat32fs *fs, cluster_t cluster) {
    size_t count = 0;
    while (IS_FINALCLUSTER(cluster) == false) {
        ++count;
        cluster = fat32fs_next(fs, cluster);
    }
    return count;
}

static off_t fat32fs_clusterdiskoffset(struct fat32fs *fs, cluster_t cluster) {
    return fs->dataoffset + fs->clustersize * (cluster-2);
}

static ssize_t fat32fs_rwcluster(struct fat32fs *fs, void *buffer, cluster_t cluster, off_t offset, size_t count, bool write) {
    return write ?
            fs->device->resource->write(fs->device->resource, NULL, buffer, fat32fs_clusterdiskoffset(fs, cluster) + offset, count) :
            fs->device->resource->read(fs->device->resource, NULL, buffer, fat32fs_clusterdiskoffset(fs, cluster) + offset, count);
}

static void fat32fs_updatefsinfo(struct fat32fs *fs) {
    struct resource *res = fs->device->resource;

    if (fs->fsinfo.free == 0xffffffff || fs->fsinfo.startat == 0xffffffff) {
        cluster_t *fatbuffer = alloc(res->stat.st_blksize);
        ASSERT(fatbuffer);

        size_t clustersperfatsector = res->stat.st_blksize / 4;
        fs->fsinfo.free = 0;

        // compute number of free clusters and the first free cluster
        for (size_t sector = 0; sector < fs->br.sectorsperfat; ++sector) {
            ASSERT(res->read(res, NULL, fatbuffer, fs->fatoffset + sector * res->stat.st_blksize, res->stat.st_blksize));

            for (size_t cluster = 0; cluster < clustersperfatsector; ++cluster) {
                if (fatbuffer[cluster] == 0) {
                    if (fs->fsinfo.startat == 0xffffffff) {
                        fs->fsinfo.startat = cluster + sector * clustersperfatsector;
                    }

                    ++fs->fsinfo.free;
                }
            }
        }

        free(fatbuffer);
    }

    ASSERT(res->write(res, NULL, &fs->fsinfo, res->stat.st_blksize * fs->br.fsinfosector + 484, sizeof(struct fat_filesysteminfo)));
};

static cluster_t nextfree(struct fat32fs *fs, cluster_t cluster) {
    while (cluster) {
        if (fat32fs_next(fs, cluster) == 0) {
            break;
        }
        ++cluster;
    }
    return cluster;
}

static void fat32fs_freeclusters(struct fat32fs *fs, cluster_t cluster) {
    while (!IS_FINALCLUSTER(cluster)) {
        cluster_t next = fat32fs_next(fs, cluster);
        fat32fs_setnext(fs, cluster, 0);
        fs->fsinfo.free++;
        if (cluster < fs->fsinfo.startat) {
            fs->fsinfo.startat = cluster;
        }
        cluster = next;
    }
}

static cluster_t fat32fs_allocateclusters(struct fat32fs *fs, size_t count) {
    if (count > fs->fsinfo.free) {
        return 0;
    }

    cluster_t ret = nextfree(fs, fs->fsinfo.startat);
    if (ret == 0) {
        return 0;
    }

    void *zerobuf = alloc(fs->clustersize);
    if (zerobuf == NULL) {
        return 0;
    }

    cluster_t cluster = ret;

    for (size_t i = 0; i < count-1; ++i) {
        cluster_t next = nextfree(fs, cluster + 1);
        if (next == 0) {
            fat32fs_setnext(fs, cluster, FINALCLUSTER);
            goto _fail;
        }

        fat32fs_setnext(fs, cluster, next);
        fat32fs_rwcluster(fs, zerobuf, cluster, 0, fs->clustersize, true); // zero the new allocated cluster
        cluster = next;
    }

    fat32fs_rwcluster(fs, zerobuf, cluster, 0, fs->clustersize, true);
    fat32fs_setnext(fs, cluster, FINALCLUSTER);
    fs->fsinfo.free -= count;
    fs->fsinfo.startat = cluster;

    free(zerobuf);
    return ret;
_fail:
    free(zerobuf);
    fat32fs_freeclusters(fs, ret);
    return 0;
}

static bool fat32fs_resize(struct fat32fs *fs, cluster_t cluster, size_t oldsize, size_t newsize, cluster_t *newstart) {
    ssize_t diff = (ssize_t)newsize - oldsize;

    if (diff == 0) {
        *newstart = cluster;
        return true;
    }

    if (cluster == 0) { // the file doesn't have any cluster associated with it so allocate them
        *newstart = fat32fs_allocateclusters(fs, newsize);
        return *newstart > 0;
    } else if (newsize == 0) { // the file will have all of its clusters unallocated
        fat32fs_freeclusters(fs, cluster);
        *newstart = 0;
        return true;
    } else { // the cluster chain will get resized
        *newstart = cluster;
        bool end;

        if (diff > 0){
            cluster = fat32fs_skip(fs, cluster, -1, &end);
            ASSERT_MSG(end, "fat32fs: bad cluster chain");

            cluster_t allocated = fat32fs_allocateclusters(fs, diff);
            if (allocated == 0) {
                return false;
            }

            fat32fs_setnext(fs, cluster, allocated);
            return true;
        } else {
            cluster = fat32fs_skip(fs, cluster, newsize-1, &end);
            ASSERT_MSG(!end, "fat32fs: trying to free cluster chain beyond limit");

            cluster_t next = fat32fs_next(fs, cluster);
            fat32fs_setnext(fs, cluster, FINALCLUSTER);

            fat32fs_freeclusters(fs, next);
            return true;
        }
    }
}

static ssize_t fat32fs_rwclusters(struct fat32fs *fs, void *buffer, cluster_t cluster, size_t count, cluster_t *endcluster, bool write) {
    size_t i = 0;
    for (; i < count; ++i) {
        ASSERT_MSG(cluster, "fat32fs: tried to read/write free cluster");
        if (IS_FINALCLUSTER(cluster)) {
            break;
        }
        ASSERT(cluster < fs->clustercount);

        off_t diskoffset = fat32fs_clusterdiskoffset(fs, cluster);
        ssize_t status = write ? fs->device->resource->write(fs->device->resource, NULL, (void *)((uintptr_t)buffer + fs->clustersize * i), diskoffset, fs->clustersize) :
                            fs->device->resource->read(fs->device->resource, NULL, (void *)((uintptr_t)buffer + fs->clustersize * i), diskoffset, fs->clustersize);
        if (status == -1) {
            return -1;
        }

        cluster = fat32fs_next(fs, cluster);
    }
    if (endcluster) {
        *endcluster = cluster;
    }

    return i;
}

static bool fat32fs_rwbytes(struct fat32fs *fs, cluster_t cluster, void *buffer, off_t offset, size_t count, bool write) {
    bool end;
    cluster = fat32fs_skip(fs, cluster, offset / fs->clustersize, &end);
    ASSERT(!end);

    // r/w the first cluster
    off_t clusteroffset = offset % fs->clustersize;
    if (clusteroffset > 0) {
        size_t rcount = count > fs->clustersize ? fs->clustersize - clusteroffset : count;
        if (fat32fs_rwcluster(fs, buffer, cluster, clusteroffset, rcount, write) == -1) {
            return false;
        }
        cluster = fat32fs_next(fs, cluster);
        buffer = (void*)((uintptr_t)buffer + rcount);
        count -= rcount;
    }

    // r/w the middle of the chain
    size_t clustersremaining = count / fs->clustersize;
    if (clustersremaining && fat32fs_rwclusters(fs, buffer, cluster, clustersremaining, &cluster, write) == -1) {
        return false;
    }

    // r/w the final cluster
    count -= clustersremaining * fs->clustersize;
    if (count > 0) {
        buffer = (void*)((uintptr_t)buffer + clustersremaining * fs->clustersize);
        if (fat32fs_rwcluster(fs, buffer, cluster, 0, count, write) == -1) {
            return false;
        }
    }

    return true;
}

static ssize_t fat32fs_reswrite(struct resource *_this, struct f_description *desc, const void *buffer, off_t offset, size_t count) {
    (void)desc;
    struct fat32fs_resource *this = (struct fat32fs_resource *)_this;
    spinlock_acquire(&this->lock);

    off_t endoffset = offset + count;

    if (endoffset > this->stat.st_size) {
        size_t newblkcount = DIV_ROUNDUP(endoffset, this->stat.st_blksize);
        cluster_t new;
        if (!fat32fs_resize(this->fs, this->cluster, this->stat.st_blocks, newblkcount, &new)) { 
            count = -1;
            errno = ENOSPC;
            goto cleanup;
        }

        fat32fs_updatefsinfo(this->fs);
        this->cluster = new;

        // update directory entry of the file if this resource is not the root directory
        struct fat_direntry dent;
        if (this->dir && this->dir->read(this->dir, NULL, &dent, this->diroffset, sizeof(struct fat_direntry)) < 0) {
            goto cleanup;
        }

        dent.size = S_ISDIR(this->stat.st_mode) ? 0 : endoffset; // directories have size 0
        DENT_SETCLUSTER(&dent, new);

        if (this->dir && this->dir->write(this->dir, NULL, &dent, this->diroffset, sizeof(struct fat_direntry)) < 0) {
            goto cleanup;
        }

        this->stat.st_blocks = newblkcount;
        this->stat.st_size = S_ISDIR(this->stat.st_mode) ? newblkcount * this->stat.st_blksize : (size_t)endoffset; // directories always have block aligned size
    }

    if (count == 0) {
        goto cleanup;
    }

    if (fat32fs_rwbytes(this->fs, this->cluster, (void *)buffer, offset, count, true) == false) {
        count = -1;
        goto cleanup;
    }

cleanup:
    spinlock_release(&this->lock);
    return count;
}

static void *fat32fs_resmmap(struct resource *_this, size_t file_page, int flags) {
    (void)flags;
    struct fat32fs_resource *this = (struct fat32fs_resource *)_this;

    void *ret = NULL;

    ret = pmm_alloc_nozero(1);
    if (ret == NULL) {
        return NULL;
    }

    if (this->read(_this, NULL, (void *)((uintptr_t)ret + VMM_HIGHER_HALF), file_page * PAGE_SIZE, PAGE_SIZE) == -1) {
        pmm_free(ret, 1);
        return NULL;
    }

    return ret;
}

static bool fat32fs_resmsync(struct resource *this, size_t file_page, void *phys, int flags) {
    (void)flags;

    if (this->write(this, NULL, (void *)((uintptr_t)phys + VMM_HIGHER_HALF), file_page * PAGE_SIZE, PAGE_SIZE) == -1) {
        return false;
    }

    return true;
}

static bool fat32fs_resunref(struct resource *_this, struct f_description *description) {
    (void)description;
    struct fat32fs_resource *this = (struct fat32fs_resource *)_this;
    spinlock_acquire(&this->lock);

    this->refcount--;
    if (this->refcount == 0) {
        // check if it is the last entry and decide which value to use for the first byte
        uint8_t byte;
        ssize_t count = this->dir->read(this->dir, NULL, &byte, this->diroffset + sizeof(struct fat_direntry), 1);
        if (byte == 0 || count == 0) { // last
            byte = 0;
        }
        else { // not last
            byte = 0xe5;
        }

        //free dir entry
        this->dir->write(this->dir, NULL, &byte, this->diroffset, 1);
        // free lfn entries
        if (this->diroffset > 0) {
            off_t offset = this->diroffset;

            while (offset) {
                offset -= sizeof(struct fat_lfndirentry);

                struct fat_lfndirentry e;
                this->dir->read(this->dir, NULL, &e, offset, sizeof(struct fat_lfndirentry));
                if (e.attribute == ATTR_LFN) {
                    this->dir->write(this->dir, NULL, &byte, offset, 1);
                } else {
                    break;
                }
            }
        }

        // free clusters used
        if (this->cluster) {
            fat32fs_freeclusters(this->fs, this->cluster);
        }
        fat32fs_updatefsinfo(this->fs);
    }

    spinlock_release(&this->lock);
    return true;
}

static bool fat32fs_restruncate(struct resource *_this, struct f_description *desc, size_t length) {
    (void)desc;
    struct fat32fs_resource *this = (struct fat32fs_resource *)_this;
    spinlock_acquire(&this->lock);

    size_t newblocksize = DIV_ROUNDUP(length, this->stat.st_blksize);
    cluster_t newcluster;

    bool status = false;
    if (!fat32fs_resize(this->fs, this->cluster, this->stat.st_blocks, newblocksize, &newcluster)) {
        errno = ENOSPC;
        goto cleanup;
    }

    this->cluster = newcluster;
    this->stat.st_blocks = newblocksize;
    this->stat.st_size = length;

    // update entry for this file on the directory
    struct fat_direntry dent;
    if (this->dir->read(this->dir, NULL, &dent, this->diroffset, sizeof(struct fat_direntry)) < 0) {
        goto cleanup;
    }

    // TODO mtime
    dent.size = length;
    DENT_SETCLUSTER(&dent, this->cluster);

    if (this->dir->write(this->dir, NULL, &dent, this->diroffset, sizeof(struct fat_direntry)) < 0) {
        goto cleanup;
    }

    fat32fs_updatefsinfo(this->fs);
    status = true;

cleanup:
    spinlock_release(&this->lock);
    return status;
}

static ssize_t fat32fs_resread(struct resource *_this, struct f_description *desc, void *buffer, off_t offset, size_t count) {
    (void)desc;
    struct fat32fs_resource *this = (struct fat32fs_resource *)_this;
    spinlock_acquire(&this->lock);
    off_t endoffset = offset + count;

    if (endoffset > this->stat.st_size) {
        endoffset = this->stat.st_size;
        count = offset >= endoffset ? 0 : endoffset - offset;
    }

    if (count == 0) {
        goto cleanup;
    }
    if (fat32fs_rwbytes(this->fs, this->cluster, buffer, offset, count, false) == false) {
        count = -1;
        goto cleanup;
    }

cleanup:
    spinlock_release(&this->lock);
    return count;
}

static uint8_t lfnchecksum(uint8_t *shortname) {
    uint8_t sum = 0;
    for (int i = 11; i; --i) {
        sum = ((sum & 1) << 7) + (sum >> 1) + *shortname++;
    }
    return sum;
}

static void fat32fs_copylfntostring(struct fat_lfndirentry *entry, char *str) {
    for (int i = 0; i < 5; ++i) {
        *str++ = entry->name1[i];
    }
    for (int i = 0; i < 6; ++i) {
        *str++ = entry->name2[i];
    }
    for (int i = 0; i < 2; ++i) {
        *str++ = entry->name3[i];
    }
}

static size_t copytolfn(const char *str, void *_lfn, size_t limit) {
    uint16_t *lfn = _lfn;
    bool putnull = false;
    size_t slen = strlen(str);
    size_t slensave = slen;
    for (size_t i = 0; i < limit; ++i) {
        if (slen == 0) {
            if (putnull) {
                lfn[i] = 0xffff;
            } else {
                lfn[i] = 0;
                putnull = true;
            }
        } else {
            lfn[i] = str[i];
            --slen;
        }
    }
    return limit > slensave ? slensave : limit;
}

static off_t insertdent(struct fat32fs_resource *dir, struct fat_direntry *entry, const char *name) {
    size_t entriesneeded = 2 + strlen(name) / 13;

    size_t found = 0;
    off_t diroffset = 0;
    for (; diroffset < dir->stat.st_blksize * dir->stat.st_blocks; diroffset += sizeof(struct fat_direntry)) {
        struct fat_direntry buffer;

        ssize_t bytesread = dir->read((struct resource *)dir, NULL, &buffer, diroffset, sizeof(struct fat_direntry));
        if (bytesread == -1) {
            return -1;
        }

        if (buffer.name[0] == 0 || bytesread == 0) { // hit end of the directory
            diroffset += (entriesneeded - found - 1) * sizeof(struct fat_direntry);
            // write the new end of dir
            if (dir->write((struct resource *)dir, NULL, &buffer, diroffset + sizeof(struct fat_direntry), sizeof(struct fat_direntry)) == -1) {
                return -1;
            }
            found = entriesneeded;
            break;
        }

        if (buffer.name[0] == 0xe5) { // unused entry, keep track of how much space we have
            ++found;
            if (found == entriesneeded) {
                break;
            }
            continue;
        }

        // reset number of free entries because an used entry was found
        found = 0;
    }

    // since we will only create long filenames, short filenames
    // are just the index of the entry into the directory
    // for example, the entry at offset 320 is named 00000000.010
    char shortnamebuf[12];
    snprintf(shortnamebuf, 12, "%011lu", diroffset / sizeof(struct fat_direntry));
    memcpy(entry->name, shortnamebuf, 11);

    if (dir->write((struct resource *)dir, NULL, entry, diroffset, sizeof(struct fat_direntry)) == -1) {
        return -1;
    }

    // walk backwards writing long filename entries
    off_t ret = diroffset;
    uint8_t checksum = lfnchecksum(entry->name);
    size_t i = 1;

    for (; i < entriesneeded; ++i) {
        struct fat_lfndirentry ent = {0};
        diroffset -= sizeof(struct fat_direntry);

        ent.attribute = ATTR_LFN;
        ent.order = i | ((i + 1 == entriesneeded) ? 0x40 : 0);
        ent.checksum = checksum;

        const char *str = name + (i - 1) * 13;
        str += copytolfn(str, ent.name1, 5);
        str += copytolfn(str, ent.name2, 6);
        str += copytolfn(str, ent.name3, 2);

        if (dir->write((struct resource *)dir, NULL, &ent, diroffset, sizeof(struct fat_lfndirentry)) == -1) {
            return -1;
        }
    }
    return ret;
}

static struct vfs_node *fat32fs_create(struct vfs_filesystem *_this, struct vfs_node *parent, const char *name, int mode) {
    struct fat32fs *this = (struct fat32fs *)_this;

    if (strlen(name) > 255) {
        errno = ENAMETOOLONG;
        return NULL;
    }

    if (!S_ISDIR(mode) && !S_ISREG(mode)) {
        // fat32 doesn't support special files
        errno = EPERM;
        return NULL;
    }

    struct fat32fs_resource *res = NULL;
    struct vfs_node *node = NULL;

    node = vfs_create_node(_this, parent, name, S_ISDIR(mode));
    if (node == NULL) {
        goto fail;
    }

    res = resource_create(sizeof(struct fat32fs_resource));
    if (res == NULL) {
        goto fail;
    }
    
    if (S_ISREG(mode)) {
        res->can_mmap = true;
    }

    res->read = fat32fs_resread;
    res->write = fat32fs_reswrite;
    res->truncate = fat32fs_restruncate;
    res->unref = fat32fs_resunref;
    res->mmap = fat32fs_resmmap;
    res->msync = fat32fs_resmsync;

    res->stat.st_uid = 0;
    res->stat.st_gid = 0;
    // allocate a cluster for directories, as they are going to have the dot entries anyways.
    res->stat.st_size = S_ISDIR(mode) ? this->clustersize : 0;
    res->stat.st_blocks = S_ISDIR(mode) ? 1 : 0;
    res->stat.st_blksize = this->clustersize;
    res->stat.st_dev = this->device->resource->stat.st_rdev;
    res->stat.st_mode = mode;
    res->stat.st_nlink = 1;
    res->stat.st_ino = this->currentinode++;
    res->refcount = 1;
    
    res->stat.st_atim = time_realtime;
    res->stat.st_ctim = time_realtime;
    res->stat.st_mtim = time_realtime;

    res->dir = parent->resource;
    res->fs = this;
    res->diroffset = -1;

    node->filesystem = _this;
    node->resource = (struct resource *)res;

    res->cluster = 0; 

    // TODO mtime ctime atime

    struct fat_direntry entry = {0};

    if (S_ISDIR(mode)) {
        cluster_t dcluster = fat32fs_allocateclusters(this, 1);
        if (!dcluster) {
            errno = ENOSPC;
            goto fail;
        }
        res->cluster = dcluster;
        entry.attributes = ATTR_DIR;
        DENT_SETCLUSTER(&entry, dcluster);
    }
    struct fat32fs_resource *parentres = (struct fat32fs_resource *)parent->resource;

    res->diroffset = insertdent(parentres, &entry, name);

    if (res->diroffset == -1) {
        goto fail;
    }

    if (S_ISDIR(mode)) {
        node->populated = true;
        memcpy(entry.name, ".          ", 11);
        res->write((struct resource *)res, NULL, &entry, 0, sizeof(struct fat_direntry));
        // if the .. entry points to the root directory, the cluster should be set to 0
        DENT_SETCLUSTER(&entry, parentres->stat.st_ino == 2 ? 0 : parentres->cluster);
        entry.name[1] = '.';
        res->write((struct resource *)res, NULL, &entry, sizeof(struct fat_direntry), sizeof(struct fat_direntry));
    }

    fat32fs_updatefsinfo(this);
    return node;
 
fail:
    if (node) {
        free(node); // TODO use vfs_destroy_node
    }
    if (res) {
        res->unref((struct resource *)res, NULL);
        free(res);
    }
    fat32fs_updatefsinfo(this);
    return NULL;
}

static size_t countspacepadding(uint8_t* str, size_t len) {
    size_t count = 0;
    str += len-1;
    while (len-- && *str-- == 0x20) {
        count++;
    }
    return count;
}

static void fat32fs_populate(struct vfs_filesystem *_this, struct vfs_node *node) {
    struct fat32fs *this = (struct fat32fs *)_this;
    struct fat32fs_resource *dres = (struct fat32fs_resource *)node->resource;

    char *namebuffer = alloc(sizeof(char) * 256);
    if (!namebuffer) {
        return;
    }

    struct fat_direntry *buffer = alloc(dres->stat.st_size);
    if (!buffer) {
        free(namebuffer);
        return;
    }

    size_t dentcount = dres->stat.st_size / sizeof(struct fat_direntry);

    if (fat32fs_rwclusters(this, buffer, dres->cluster, dres->stat.st_blocks, NULL, false) == -1) {
        goto cleanup;
    }

    bool islfn = false;
    uint8_t checksum;

    for (size_t i = 0; i < dentcount; ++i) {
        struct fat_direntry *entry = buffer + i;

        if (entry->name[0] == 0) { // end of directory
            break;
        }

        if (entry->name[0] == 0xe5) { // unused entry
            continue;
        }

        if (entry->attributes & ATTR_LFN) {
            struct fat_lfndirentry *lfn = (struct fat_lfndirentry *)entry;
            lfn->order &= 0x1f;

            if (islfn == false) {
                // get directory entry these LFNs belong to for checksum calculation
                struct fat_direntry *dent = entry + lfn->order;
                checksum = lfnchecksum(dent->name);
                islfn = true;
            } else if (lfn->checksum != checksum) {
                kernel_print("fat32fs: bad lfn checksum. Aborting populate.\n");
                goto cleanup;
            }

            fat32fs_copylfntostring(lfn, namebuffer + (lfn->order - 1) * 13);
            continue;
        }

        if (entry->attributes & ATTR_VOLUMEID) {
            continue;
        }

        if (islfn == false) {
            size_t namesize = 8 - countspacepadding(entry->name, 8);
            size_t extsize = 3 - countspacepadding(entry->name + 8, 3);
            if (namesize) {
                strncpy(namebuffer, (char *)entry->name, namesize);
            }
            if (extsize) {
                namebuffer[namesize] = '.';
                strncpy(namebuffer + namesize + 1, (char *)entry->name + 8, extsize);
            }
        }

        islfn = false;

        // the VFS already handles the creation of these
        if (strcmp(namebuffer, ".") == 0 || strcmp(namebuffer, "..") == 0) {
            continue;
        }

        // the permissions are hardcoded since fat doesn't support unix file permissions
        uint16_t mode = 0751 | (entry->attributes & ATTR_DIR ? S_IFDIR : S_IFREG);

        struct fat32fs_resource *res = resource_create(sizeof(struct fat32fs_resource));
        if (!res) {
            goto cleanup;
        }

        struct vfs_node *fnode = vfs_create_node((struct vfs_filesystem *)this, node, namebuffer, S_ISDIR(mode));
        if (!fnode) {
            free(res);
            goto cleanup;
        }

        if (S_ISREG(mode)) {
            res->can_mmap = true;
        }

        res->read = fat32fs_resread;
        res->truncate = fat32fs_restruncate;
        res->write = fat32fs_reswrite;
        res->unref = fat32fs_resunref;
        res->mmap = fat32fs_resmmap;
        res->msync = fat32fs_resmsync;

        res->cluster = DENT_GETCLUSTER(entry);

        res->stat.st_uid = 0;
        res->stat.st_gid = 0;
        res->stat.st_mode = mode;
        res->stat.st_ino = this->currentinode++;
        res->stat.st_blksize = this->clustersize;
        // the file size field in the dent for folders is 0, so we have to figure out the size by walking through the chain
        res->stat.st_size = S_ISDIR(mode) ? fat32fs_getchainsize(this, res->cluster) * res->stat.st_blksize : entry->size;
        res->stat.st_blocks = DIV_ROUNDUP(res->stat.st_size, this->clustersize);
        res->stat.st_nlink = 1;

        res->refcount = 1;
        res->diroffset = (uintptr_t)entry - (uintptr_t)buffer;
        res->dir = (struct resource *)dres;
        res->fs = this;

        // TODO atime ctime mtime

        fnode->filesystem = _this;
        fnode->resource = (struct resource *)res;

        HASHMAP_SINSERT(&fnode->parent->children, namebuffer, fnode);

        if (S_ISDIR(mode)) {
            fnode->populated = false;
            vfs_create_dotentries(fnode, fnode->parent);
        }
    }

    node->populated = true;
cleanup:
    free(namebuffer);
    free(buffer);
}

static struct fat32fs *fat32fs_new(void) {
    struct fat32fs *new_fs = alloc(sizeof(struct fat32fs));
    if (new_fs == NULL) {
        return NULL;
    }

    new_fs->populate = fat32fs_populate;
    new_fs->create = fat32fs_create;

    return new_fs;
}

static struct vfs_node *fat32fs_mount(struct vfs_node *parent, const char *name, struct vfs_node *device) {
    struct vfs_node *node = NULL;
    struct fat32fs_resource *res = NULL;
    struct fat32fs *fs = fat32fs_new();
    if (fs == NULL) {
        return NULL;
    }

    device->resource->read(device->resource, NULL, &fs->br, 0, sizeof(struct fat_bootrecord));

    if (fs->br.signature != 0x29) {
        kernel_print("fat32fs: bad boot record signature\n");
        goto fail;
    }

    if (strncmp((char *)fs->br.identifierstr, "FAT32   ", 8)) {
        kernel_print("fat32fs: bad identifier string\n");
        goto fail;
    }

    // read only the first signature at sector offset 0
    device->resource->read(device->resource, NULL, &fs->fsinfo, fs->br.fsinfosector * device->resource->stat.st_blksize, 4);
    
    if (fs->fsinfo.signature != 0x41615252) {
        kernel_print("fat32fs: bad fsinfo signature\n");
        goto fail;
    }

    // read the actual structure at sector offset 484
    device->resource->read(device->resource, NULL, &fs->fsinfo, fs->br.fsinfosector * device->resource->stat.st_blksize + 484, sizeof(struct fat_filesysteminfo));
    
    if (fs->fsinfo.signature != 0x61417272 || fs->fsinfo.signature2 != 0xaa550000) {
        kernel_print("fat32fs: bad fsinfo signature\n");
        goto fail;
    }

    off_t datasector = fs->br.reservedsectorcount + fs->br.fatcount * fs->br.sectorsperfat;

    fs->device = device;
    fs->clustersize = fs->br.sectorspercluster * device->resource->stat.st_blksize;
    fs->fatoffset = fs->br.reservedsectorcount * device->resource->stat.st_blksize;
    fs->dataoffset = datasector * device->resource->stat.st_blksize;
    fs->clustercount = (device->resource->stat.st_blocks - datasector) / fs->br.sectorspercluster;
    fs->currentinode = 3;
    fat32fs_updatefsinfo(fs);
    
    node = vfs_create_node((struct vfs_filesystem *)fs, parent, name, true);

    if (node == NULL) {
        kernel_print("fat32fs: failed to create vfs node for root directory\n");
        goto fail;
    }

    res = resource_create(sizeof(struct fat32fs_resource));

    if (res == NULL) {
        kernel_print("fat32fs: failed to create resource for root directory\n");
        goto fail;
    }

    res->read = fat32fs_resread;
    res->write = fat32fs_reswrite;

    res->stat.st_blocks = fat32fs_getchainsize(fs, fs->br.rootdircluster);
    res->stat.st_size = res->stat.st_blocks * fs->clustersize;
    res->stat.st_blksize = fs->clustersize;
    res->stat.st_dev = device->resource->stat.st_rdev;
    res->stat.st_mode = 0644 | S_IFDIR;
    res->stat.st_nlink = 1;
    res->stat.st_ino = 2;

    res->stat.st_atim = time_realtime;
    res->stat.st_ctim = time_realtime;
    res->stat.st_mtim = time_realtime;

    res->fs = fs;
    res->cluster = fs->br.rootdircluster;

    node->resource = (struct resource *)res;
    node->filesystem = (struct vfs_filesystem *)fs;

    return node;
    
fail:
    if (node) {
        free(node); // TODO use vfs_destroy_node
    }
    free(fs);
    return NULL;
}

void fat32fs_init(void) {
    vfs_add_filesystem(fat32fs_mount, "fat32");
}
