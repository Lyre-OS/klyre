#include <stdint.h>
#include <stdbool.h>
#include <sys/port.k.h>
#include <sched/sched.k.h>
#include <lib/event.k.h>
#include <sys/int_events.k.h>
#include <time/time.k.h>
#include <sys/idt.k.h>
#include <dev/ioapic.k.h>
#include <dev/lapic.k.h>
#include <lib/print.k.h>
#include <lib/resource.k.h>
#include <lib/errno.k.h>
#include <poll.h>
#include <fs/devtmpfs.k.h>

// mouse code primarily from https://github.com/mintsuki/MeME/blob/master/test/src/main.c

// For now this is for PS/2 mice, but it should eventually be abstracted to
// at least include USB mice in an abstracted manner.

static inline void mouse_wait(int type) {
    int timeout = 100000;

    if (type == 0) {
        while (timeout--) {
            if (inb(0x64) & (1 << 0)) {
                return;
            }
        }
    } else {
        while (timeout--) {
            if (!(inb(0x64) & (1 << 1))) {
                return;
            }
        }
    }
}

static inline void mouse_write(uint8_t val) {
    mouse_wait(1);
    outb(0x64, 0xd4);
    mouse_wait(1);
    outb(0x60, val);
}

static inline uint8_t mouse_read(void) {
    mouse_wait(0);
    return inb(0x60);
}

static int ps2_mouse_vector;

struct mouse_packet {
    uint8_t flags;
    int32_t x_mov;
    int32_t y_mov;
};

struct mouse {
    struct resource;
    bool packet_avl;
    struct mouse_packet packet;
};

static struct mouse *mouse_res;

static noreturn void mouse_handler(void) {
    int handler_cycle = 0;
    struct mouse_packet current_packet;
    bool discard_packet = false;

    for (;;) {
        struct event *events[] = { &int_events[ps2_mouse_vector] };
        event_await(events, 1, true);

        // we will get some spurious packets at the beginning and they will screw
        // up the alignment of the handler cycle so just ignore everything in
        // the first 250 milliseconds after boot
        if (time_monotonic.tv_sec == 0 && time_monotonic.tv_nsec < 250000000) {
            inb(0x60);
            continue;
        }

        switch (handler_cycle) {
            case 0:
                current_packet.flags = mouse_read();
                handler_cycle++;
                if (current_packet.flags & (1 << 6) || current_packet.flags & (1 << 7))
                    discard_packet = true;     // discard rest of packet
                if (!(current_packet.flags & (1 << 3)))
                    discard_packet = true;     // discard rest of packet
                continue;
            case 1:
                current_packet.x_mov = mouse_read();
                handler_cycle++;
                continue;
            case 2: {
                current_packet.y_mov = mouse_read();
                handler_cycle = 0;

                if (discard_packet) {
                    discard_packet = false;
                    continue;
                }

                break;
            }
        }

        // process packet
        if (current_packet.flags & (1 << 4)) {
            current_packet.x_mov = (int8_t)(uint8_t)current_packet.x_mov;
        }

        if (current_packet.flags & (1 << 5)) {
            current_packet.y_mov = (int8_t)(uint8_t)current_packet.y_mov;
        }

        spinlock_acquire(&mouse_res->lock);
        memcpy(&mouse_res->packet, &current_packet, sizeof(struct mouse_packet));
        mouse_res->packet_avl = true;
        spinlock_release(&mouse_res->lock);

        mouse_res->status |= POLLIN;
        event_trigger(&mouse_res->event, false);
    }
}

static ssize_t _mouse_read(struct resource *_this, struct f_description *description, void *_buf, off_t offset, size_t count) {
    (void)_this;
    (void)offset;

    if (count != sizeof(struct mouse_packet)) {
        errno = EINVAL;
        return -1;
    }

    spinlock_acquire(&mouse_res->lock);

    while (!mouse_res->packet_avl) {
        spinlock_release(&mouse_res->lock);

        if (description->flags & O_NONBLOCK) {
            errno = EWOULDBLOCK;
            return -1;
        }

        struct event *events[] = { &mouse_res->event };
        event_await(events, 1, true);
        spinlock_acquire(&mouse_res->lock);
    }

    memcpy(_buf, &mouse_res->packet, sizeof(struct mouse_packet));
    mouse_res->packet_avl = false;

    mouse_res->status &= ~POLLIN;

    spinlock_release(&mouse_res->lock);

    return sizeof(struct mouse_packet);
}

void mouse_init(void) {
    mouse_write(0xf6);
    mouse_read();

    mouse_write(0xf4);
    mouse_read();

    mouse_res = resource_create(sizeof(struct mouse));

    mouse_res->stat.st_size = 0;
    mouse_res->stat.st_blocks = 0;
    mouse_res->stat.st_blksize = 512;
    mouse_res->stat.st_rdev = resource_create_dev_id();
    mouse_res->stat.st_mode = 0644 | S_IFCHR;

    mouse_res->status |= POLLOUT;

    mouse_res->read = _mouse_read;

    devtmpfs_add_device((struct resource *)mouse_res, "mouse");

    ps2_mouse_vector = idt_allocate_vector();
    io_apic_set_irq_redirect(bsp_lapic_id, ps2_mouse_vector, 12, true);

    sched_new_kernel_thread(mouse_handler, NULL, true);
}
