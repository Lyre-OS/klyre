#ifndef _DEV__VIDEO__FBDEV_K_H
#define _DEV__VIDEO__FBDEV_K_H

void fbdev_init(void);

extern volatile struct limine_framebuffer_request framebuffer_request;

#endif
