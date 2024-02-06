#include <stddef.h>
#include <stdint.h>
#include <limine.h>
#include <fs/initramfs.k.h>
#include <fs/vfs/vfs.k.h>
#include <lib/libc.k.h>
#include <lib/misc.k.h>
#include <lib/panic.k.h>
#include <lib/print.k.h>
#include <mm/pmm.k.h>
#include <mm/vmm.k.h>

static volatile struct limine_module_request module_request = {
    .id = LIMINE_MODULE_REQUEST,
    .revision = 0
};

#define TAR_FILE_TYPE_NORMAL '0'
#define TAR_FILE_TYPE_HARD_LINK '1'
#define TAR_FILE_TYPE_SYMLINK '2'
#define TAR_FILE_TYPE_CHAR_DEV '3'
#define TAR_FILE_TYPE_BLOCK_DEV '4'
#define TAR_FILE_TYPE_DIRECTORY '5'
#define TAR_FILE_TYPE_FIFO '6'
#define TAR_FILE_TYPE_GNU_LONG_PATH 'L'

struct tar {
    char name[100];
    char mode[8];
    char uid[8];
    char gid[8];
    char size[12];
    char mtime[12];
    char checksum[8];
    char type;
    char link_name[100];
    char magic[6];
    char version[2];
    char uname[32];
    char gname[32];
    char dev_major[8];
    char dev_minor[8];
    char prefix[155];
};

static inline uint64_t oct2int(const char *str, size_t len) {
    uint64_t value = 0;
    while (*str && len > 0) {
        value = value * 8 + (*str++ - '0');
        len--;
    }
    return value;
}

void initramfs_init(void) {
    struct limine_module_response *modules = module_request.response;
    if (modules == NULL || modules->module_count == 0) {
        panic(NULL, true, "No initramfs");
    }

    struct limine_file *module = modules->modules[0];

    kernel_print("initramfs: address=%lx, length=%lu\n", module->address, module->size);
    kernel_print("initramfs: Unpacking...\n", module->address, module->size);

    struct tar *current_file = (struct tar *)module->address;
    char *name_override = NULL;

    while (strncmp(current_file->magic, "ustar", 5) == 0) {
        char *name = current_file->name;
        char *link_name = current_file->link_name;
        if (name_override != NULL) {
            name = name_override;
            name_override = NULL;
        }

        if (strcmp(name, "./") == 0) {
            continue;
        }

        uint64_t mode = oct2int(current_file->mode, sizeof(current_file->mode));
        uint64_t size = oct2int(current_file->size, sizeof(current_file->size));
        uint64_t mtime = oct2int(current_file->mtime, sizeof(current_file->mtime));
        // uint64_t uid = oct2int(current_file->uid, sizeof(current_file->uid));
        // uint64_t gid = oct2int(current_file->gid, sizeof(current_file->gid));

        struct vfs_node *node = NULL;
        switch (current_file->type) {
            case TAR_FILE_TYPE_NORMAL: {
                node = vfs_create(vfs_root, name, mode | S_IFREG);
                if (node == NULL) {
                    panic(NULL, true, "Failed to allocate an initramfs node");
                }

                struct resource *resource = node->resource;
                ASSERT(resource->write(resource, NULL, (void *)current_file + 512, 0, size) == (ssize_t)size);
                break;
            }
            case TAR_FILE_TYPE_SYMLINK: {
                node = vfs_symlink(vfs_root, link_name, name);
                if (node == NULL) {
                    panic(NULL, true, "Failed to allocate an initramfs node");
                }
                break;
            }
            case TAR_FILE_TYPE_DIRECTORY: {
                node = vfs_create(vfs_root, name, mode | S_IFDIR);
                if (node == NULL) {
                    panic(NULL, true, "Failed to allocate an initramfs node");
                }
                break;
            }
            case TAR_FILE_TYPE_GNU_LONG_PATH:
                name_override = (void *)current_file + 512;
                name_override[size] = 0;
                break;
        }

        if (node != NULL) {
            node->resource->stat.st_mtim = (struct timespec){.tv_sec = mtime, .tv_nsec = 0};
        }

        // switch (current_file->type) {
        //     case TAR_FILE_TYPE_HARD_LINK:
        //         kernel_print("initramfs: Hard link '%s' to '%s', mode=%04o, uid=%lu, gid=%lu\n", name, link_name, mode, uid, gid);
        //         break;
        //     case TAR_FILE_TYPE_SYMLINK:
        //         kernel_print("initramfs: Symbolic link '%s' to '%s', mode=%04o, uid=%lu, gid=%lu\n", name, link_name, mode, uid, gid);
        //         break;
        //     case TAR_FILE_TYPE_CHAR_DEV: {
        //         uint64_t dev_major = oct2int(current_file->dev_major, sizeof(current_file->dev_major));
        //         uint64_t dev_minor = oct2int(current_file->dev_minor, sizeof(current_file->dev_minor));
        //         kernel_print("initramfs: Character device '%s', device=%lu:%lu, mode=%04o, uid=%lu, gid=%lu\n", name, dev_major, dev_minor, mode, uid, gid);
        //         break;
        //     }
        //     case TAR_FILE_TYPE_BLOCK_DEV: {
        //         uint64_t dev_major = oct2int(current_file->dev_major, sizeof(current_file->dev_major));
        //         uint64_t dev_minor = oct2int(current_file->dev_minor, sizeof(current_file->dev_minor));
        //         kernel_print("initramfs: Block device '%s', device=%lu:%lu, mode=%04o, uid=%lu, gid=%lu\n", name, dev_major, dev_minor, mode, uid, gid);
        //         break;
        //     }
        //     case TAR_FILE_TYPE_FIFO: {
        //         uint64_t dev_major = oct2int(current_file->dev_major, sizeof(current_file->dev_major));
        //         uint64_t dev_minor = oct2int(current_file->dev_minor, sizeof(current_file->dev_minor));
        //         kernel_print("initramfs: FIFO '%s', device=%lu:%lu, mode=%04o, uid=%lu, gid=%lu\n", name, dev_major, dev_minor, mode, uid, gid);
        //         break;
        //     }
        // }

        pmm_free((void *)current_file - VMM_HIGHER_HALF, (512 + ALIGN_UP(size, 512)) / PAGE_SIZE);

        current_file = (void *)current_file + 512 + ALIGN_UP(size, 512);
    }
}
