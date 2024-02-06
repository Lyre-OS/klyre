#include <stddef.h>
#include <stdint.h>
#include <limine.h>
#include <dev/video/fbdev.k.h>
#include <fs/devtmpfs.k.h>
#include <lib/alloc.k.h>
#include <lib/errno.k.h>
#include <lib/misc.k.h>
#include <lib/print.k.h>
#include <lib/resource.k.h>
#include <mm/vmm.k.h>
#include <linux/fb.h>
#include <printf/printf.h>

volatile struct limine_framebuffer_request framebuffer_request = {
    .id = LIMINE_FRAMEBUFFER_REQUEST,
    .revision = 0
};

struct framebuffer_device {
    struct resource;
    struct limine_framebuffer *framebuffer;
    struct fb_var_screeninfo variable;
    struct fb_fix_screeninfo fixed;
};

static ssize_t fbdev_read(struct resource *this_, struct f_description *description, void *buf, off_t offset, size_t count) {
    (void)description;

    struct framebuffer_device *this = (struct framebuffer_device *)this_;

    if (count == 0) {
        return 0;
    }

    size_t actual_count = count;
    if (offset + count > this->fixed.smem_len) {
        actual_count = count - ((offset + count) - this->fixed.smem_len);
    }

    memcpy(buf, this->framebuffer->address + offset, actual_count);

    return actual_count;
}

static ssize_t fbdev_write(struct resource *this_, struct f_description *description, const void *buf, off_t offset, size_t count) {
    (void)description;

    struct framebuffer_device *this = (struct framebuffer_device *)this_;

    if (count == 0) {
        return 0;
    }

    size_t actual_count = count;
    if (offset + count > this->fixed.smem_len) {
        actual_count = count - ((offset + count) - this->fixed.smem_len);
    }

    memcpy(this->framebuffer->address + offset, buf, actual_count);

    return actual_count;
}

static int fbdev_ioctl(struct resource *this_, struct f_description *description, uint64_t request, uint64_t arg) {
    struct framebuffer_device *this = (struct framebuffer_device *)this_;

    switch (request) {
        case FBIOGET_VSCREENINFO:
            *(struct fb_var_screeninfo *)arg = this->variable;
            return 0;
        case FBIOGET_FSCREENINFO:
            *(struct fb_fix_screeninfo *)arg = this->fixed;
            return 0;
        case FBIOPUT_VSCREENINFO:
            this->variable = *(struct fb_var_screeninfo *)arg;
            return 0;
        case FBIOBLANK:
            return 0;
    }

    return resource_default_ioctl(this_, description, request, arg);
}

static void *fbdev_mmap(struct resource *this_, size_t file_page, int flags) {
    (void)flags;

    struct framebuffer_device *this = (struct framebuffer_device *)this_;

    size_t offset = file_page * PAGE_SIZE;

    if (offset >= this->fixed.smem_len) {
        return NULL;
    }

    return (this->framebuffer->address + offset) - VMM_HIGHER_HALF;
}

static bool fbdev_msync(struct resource *_this, size_t file_page, void *phys, int flags) {
    (void)_this;
    (void)file_page;
    (void)phys;
    (void)flags;
    return true;
}

void fbdev_init(void) {
    struct limine_framebuffer_response *framebuffer_response = framebuffer_request.response;
    if (framebuffer_response == NULL || framebuffer_response->framebuffer_count == 0) {
        kernel_print("fbdev: No framebuffers available\n");
    }

    kernel_print("fbdev: %d framebuffer(s) available\n", framebuffer_response->framebuffer_count);

    for (uint64_t i = 0; i < framebuffer_response->framebuffer_count; i++) {
        struct limine_framebuffer *framebuffer = framebuffer_response->framebuffers[i];
        struct framebuffer_device *device = resource_create(sizeof(struct framebuffer_device));

        kernel_print("fbdev: Framebuffer #%d with mode %lux%lu (bpp=%lu, stride=%lu bytes)\n",
            i + 1, framebuffer->width, framebuffer->height, framebuffer->bpp, framebuffer->pitch);

        device->can_mmap = true;
        device->read = fbdev_read;
        device->write = fbdev_write;
        device->ioctl = fbdev_ioctl;
        device->mmap = fbdev_mmap;
        device->msync = fbdev_msync;
        device->framebuffer = framebuffer;

        device->stat.st_size = 0;
        device->stat.st_blocks = 0;
        device->stat.st_blksize = 4096;
        device->stat.st_rdev = resource_create_dev_id();
        device->stat.st_mode = 0666 | S_IFCHR;

        device->fixed.smem_len = framebuffer->pitch * framebuffer->height;
        device->fixed.mmio_len = framebuffer->pitch * framebuffer->height;
        device->fixed.line_length = framebuffer->pitch;
        device->fixed.type = FB_TYPE_PACKED_PIXELS;
        device->fixed.visual = FB_VISUAL_TRUECOLOR;

        device->variable.xres = framebuffer->width;
        device->variable.yres = framebuffer->height;
        device->variable.xres_virtual = framebuffer->width;
        device->variable.yres_virtual = framebuffer->height;
        device->variable.bits_per_pixel = framebuffer->bpp;
        device->variable.red = (struct fb_bitfield) {
            .offset = framebuffer->red_mask_shift,
            .length = framebuffer->red_mask_size
        };
        device->variable.green = (struct fb_bitfield) {
            .offset = framebuffer->green_mask_shift,
            .length = framebuffer->green_mask_size
        };
        device->variable.blue = (struct fb_bitfield) {
            .offset = framebuffer->blue_mask_shift,
            .length = framebuffer->blue_mask_size
        };
        device->variable.activate = FB_ACTIVATE_NOW;
        device->variable.vmode = FB_VMODE_NONINTERLACED;
        device->variable.width = -1;
        device->variable.height = -1;

        snprintf(device->fixed.id, sizeof(device->fixed.id), "limine-fb%lu", i);

        char device_name[32];
        snprintf(device_name, sizeof(device_name) - 1, "fb%lu", i);
        devtmpfs_add_device((struct resource *)device, device_name);
    }
}
