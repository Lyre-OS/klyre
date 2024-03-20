#ifndef LIB__PANIC_K_H_
#define LIB__PANIC_K_H_

#include <stdbool.h>
#include <stdnoreturn.h>
#include <sys/cpu.k.h>

noreturn void panic(struct cpu_ctx *ctx, bool trace, const char *fmt, ...);

#endif
