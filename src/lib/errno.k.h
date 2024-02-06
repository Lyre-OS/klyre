#ifndef _LIB__ERRNO_K_H
#define _LIB__ERRNO_K_H

#include <errno.h>
#include <sched/proc.k.h>

#define errno (sched_current_thread()->errno)

#endif
