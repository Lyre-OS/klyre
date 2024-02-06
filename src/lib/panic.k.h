#ifndef _LIB__PANIC_K_H
#define _LIB__PANIC_K_H

#include <stdbool.h>
#include <stdnoreturn.h>
#include <sys/cpu.k.h>

noreturn void panic(struct cpu_ctx *ctx, bool trace, const char *fmt, ...);

#endif
