#ifndef _DEV__PS2_K_H
#define _DEV__PS2_K_H

#include <stdint.h>

extern uint8_t ps2_keyboard_vector;

uint8_t ps2_read(void);
void ps2_write(uint16_t port, uint8_t value);
uint8_t ps2_read_config(void);
void ps2_write_config(uint8_t value);
void ps2_init(void);

#endif
