#include <stdint.h>
#include <stddef.h>
#include <dev/char/serial.k.h>
#include <dev/lapic.k.h>
#include <dev/dev.k.h>
#include <dev/net/net.k.h>
#include <lib/elf.k.h>
#include <lib/print.k.h>
#include <lib/random.k.h>
#include <mm/pmm.k.h>
#include <mm/slab.k.h>
#include <mm/vmm.k.h>
#include <sys/gdt.k.h>
#include <sys/idt.k.h>
#include <sys/except.k.h>
#include <sys/int_events.k.h>
#include <fs/vfs/vfs.k.h>
#include <limine.h>
#include <fs/ext2fs.k.h>
#include <fs/fat32fs.k.h>
#include <fs/initramfs.k.h>
#include <fs/tmpfs.k.h>
#include <fs/devtmpfs.k.h>
#include <sched/sched.k.h>
#include <acpi/acpi.k.h>
#include <time/time.k.h>

void kmain_thread(void);

void _start(void) {
    serial_init();
    gdt_init();
    idt_init();
    except_init();
    int_events_init();
    pmm_init();
    slab_init();
    vmm_init();
    proc_init();
    sched_init();
    cpu_init();
    acpi_init();
    time_init();

    sched_new_kernel_thread(kmain_thread, NULL, true);
    sched_await();
}

void kmain_thread(void) {
    random_init();
    vfs_init();
    fat32fs_init();
    ext2fs_init();
    tmpfs_init();
    devtmpfs_init();
    vfs_mount(vfs_root, NULL, "/", "tmpfs");
    vfs_create(vfs_root, "/dev", 0755 | S_IFDIR);
    vfs_mount(vfs_root, NULL, "/dev", "devtmpfs");
    vfs_create(vfs_root, "/mnt", 0755 | S_IFDIR);
    dev_init();
    vfs_mount(vfs_root, "/dev/nvme0n1", "/mnt", "ext2fs");
    net_init();
    initramfs_init();

    struct pagemap *init_vm = vmm_new_pagemap();
    struct auxval init_auxv, ld_auxv;
    const char *ld_path;

    struct vfs_node *init_node = vfs_get_node(vfs_root, "/usr/bin/init", true);
    elf_load(init_vm, init_node->resource, 0x0, &init_auxv, &ld_path);

    struct vfs_node *ld = vfs_get_node(vfs_root, ld_path, true);
    elf_load(init_vm, ld->resource, 0x40000000, &ld_auxv, NULL);

    const char *argv[] = {"/usr/bin/init", NULL};
    const char *envp[] = {NULL};

    struct process *init_proc = sched_new_process(NULL, init_vm);
    struct vfs_node *console_node = vfs_get_node(vfs_root, "/dev/console", true);

    fdnum_create_from_resource(init_proc, console_node->resource, 0, 0, true);
    fdnum_create_from_resource(init_proc, console_node->resource, 0, 1, true);
    fdnum_create_from_resource(init_proc, console_node->resource, 0, 2, true);

    vfs_pathname(init_node, init_proc->name, sizeof(init_proc->name) - 1);
    sched_new_user_thread(init_proc, (void *)ld_auxv.at_entry, NULL, NULL, argv, envp, &init_auxv, true);

    sched_dequeue_and_die();
}
