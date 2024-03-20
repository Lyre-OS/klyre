#ifndef LIB__PRINT_K_H_
#define LIB__PRINT_K_H_

#include <stdarg.h>
#include <stddef.h>
#include <stdbool.h>
#include <lib/lock.k.h>

extern spinlock_t debug_print_lock;
extern bool debug_on;

void kernel_vprint(const char *fmt, va_list args);
void kernel_print(const char *fmt, ...);
void debug_vprint(size_t indent, const char *fmt, va_list args);
void debug_print(size_t indent, const char *fmt, ...);

#endif
