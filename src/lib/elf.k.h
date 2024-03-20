#ifndef LIB__ELF_K_H_
#define LIB__ELF_K_H_

#include <stdbool.h>
#include <stdint.h>
#include <elf.h>
#include <lib/resource.k.h>
#include <mm/vmm.k.h>

struct auxval {
    uint64_t at_entry;
    uint64_t at_phdr;
    uint64_t at_phent;
    uint64_t at_phnum;
};

bool elf_load(struct pagemap *pagemap, struct resource *res, uint64_t load_base,
              struct auxval *auxv, const char **ld_path);

#endif
