#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include <lib/trace.k.h>
#include <lib/misc.k.h>
#include <lib/print.k.h>

bool trace_address(uintptr_t address, size_t *offset, struct symbol *sym) {
    struct symbol prev_sym = symbol_table[0];

    for (size_t i = 0; ; i++) {
        if (symbol_table[i].address == (uintptr_t)-1) {
            return false;
        }

        if (symbol_table[i].address >= address) {
            *offset = address - prev_sym.address;
            *sym = prev_sym;
            return true;
        }

        prev_sym = symbol_table[i];
    }
}

bool trace_printaddr(uintptr_t address) {
    size_t offset;
    struct symbol sym;
    if (!trace_address(address, &offset, &sym)) {
        debug_print(0, "  [%016lx] Failed to resolve symbol", address);
        return false;
    }
    debug_print(0, "  [%016lx] <%s+0x%lx>", address, sym.name, offset);
    return true;
}

void trace_printstack(uintptr_t _base_ptr) {
    uintptr_t *base_ptr = (uintptr_t *)_base_ptr;

    if (base_ptr == NULL) {
        asm volatile ("mov %%rbp, %0" : "=g"(base_ptr) :: "memory");
    }

    if (base_ptr == NULL) {
        return;
    }

    for (;;) {
        uintptr_t *old_bp = (uintptr_t *)base_ptr[0];
        uintptr_t *ret_addr = (uintptr_t *)base_ptr[1];
        if (ret_addr == NULL || old_bp == NULL || (uintptr_t)ret_addr < 0xffffffff80000000) {
            break;
        }
        if (!trace_printaddr((uintptr_t)ret_addr)) {
            break;
        }
        base_ptr = old_bp;
    }
}
