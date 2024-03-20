#ifndef DEV__VIDEO__FBDEV_K_H_
#define DEV__VIDEO__FBDEV_K_H_

void fbdev_init(void);

extern volatile struct limine_framebuffer_request framebuffer_request;

#endif
