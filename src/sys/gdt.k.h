#ifndef SYS__GDT_K_H_
#define SYS__GDT_K_H_

#include <stdint.h>
#include <sys/cpu.k.h>

void gdt_init(void);
void gdt_reload(void);
void gdt_load_tss(struct tss *tss);

#endif
