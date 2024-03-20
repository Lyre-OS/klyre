#ifndef SYS__INT_EVENTS_K_H_
#define SYS__INT_EVENTS_K_H_

#include <lib/event.k.h>

extern struct event int_events[256];

void int_events_init(void);

#endif
