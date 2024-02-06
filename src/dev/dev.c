#include <dev/dev.k.h>
#include <dev/char/console.k.h>
#include <dev/char/streams.k.h>
#include <dev/char/mouse.k.h>
#include <dev/video/fbdev.k.h>
#include <dev/ps2.k.h>
#include <dev/pci.k.h>

void dev_init(void) {
    ps2_init();
    mouse_init();
    console_init();
    streams_init();
    pci_init();
    fbdev_init();
}
