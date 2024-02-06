#ifndef _SYS__INT_EVENTS_K_H
#define _SYS__INT_EVENTS_K_H

#include <lib/event.k.h>

extern struct event int_events[256];

void int_events_init(void);

#endif
