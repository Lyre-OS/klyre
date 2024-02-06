#ifndef _SYS__IDT_K_H
#define _SYS__IDT_K_H

#include <stdint.h>

extern void *isr[];
extern uint8_t idt_panic_ipi_vector;

void idt_register_handler(uint8_t vector, void *handler, uint8_t flags);
uint8_t idt_allocate_vector(void);
void idt_set_ist(uint8_t vector, uint8_t ist);
uint8_t idt_get_ist(uint8_t vector);
void idt_set_flags(uint8_t vector, uint8_t flags);
void idt_reload(void);
void idt_init(void);

#endif
