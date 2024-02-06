#ifndef _DEV__LAPIC_K_H
#define _DEV__LAPIC_K_H

#include <stdint.h>

void lapic_init(void);
void lapic_eoi(void);
void lapic_timer_oneshot(uint64_t us, uint8_t vector);
void lapic_timer_stop(void);
void lapic_send_ipi(uint32_t lapic_id, uint32_t vec);
void lapic_timer_calibrate(void);

#endif
