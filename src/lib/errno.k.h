#ifndef LIB__ERRNO_K_H_
#define LIB__ERRNO_K_H_

#include <errno.h>
#include <sched/proc.k.h>

#define errno (sched_current_thread()->errno)

#endif
