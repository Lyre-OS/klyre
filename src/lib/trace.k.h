#ifndef LIB__TRACE_K_H_
#define LIB__TRACE_K_H_

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

struct symbol {
    uintptr_t address;
    char *name;
};

extern struct symbol symbol_table[];

bool trace_address(uintptr_t address, size_t *offset, struct symbol *symbol);
bool trace_printaddr(uintptr_t addr);
void trace_printstack(uintptr_t _base_ptr);

#endif
