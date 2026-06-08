#ifndef DT_PARSE_H
#define DT_PARSE_H

#include "types.h"

#define FDT_BEGIN_NODE 0x00000001
#define FDT_END_NODE   0x00000002
#define FDT_PROP       0x00000003
#define FDT_NOP        0x00000004
#define FDT_END        0x00000009

#define MAX_RESERVED_REGIONS 32

struct fdt_header {
    uint32_t magic;
    uint32_t totalsize;
    uint32_t off_dt_struct;
    uint32_t off_dt_strings;
    uint32_t off_mem_rsvmap;
    uint32_t version;
    uint32_t last_comp_version;
    uint32_t boot_cpuid_phys;
    uint32_t size_dt_strings;
    uint32_t size_dt_struct;
};

struct reserved_region {
    char name[64];
    uintptr_t start;
    uintptr_t end;
};


int fdt_path_offset(const void* fdt, const char* path);

const void* fdt_getprop(const void* fdt,
                        int nodeoffset,
                        const char* name,
                        int* lenp);

uintptr_t get_uart_base(const void *fdt);
uintptr_t get_uart_size(const void *fdt);
uintptr_t get_mem_start(const void *fdt);
uintptr_t get_mem_end(const void *fdt);
uintptr_t get_initrd_start(const void *fdt);
uintptr_t get_initrd_end(const void *fdt);
unsigned long get_timebase_frequency(const void *fdt);
uintptr_t get_plic_base(const void *fdt);
int fdt_find_plic_under_soc(const void *fdt);
void dtb_debug_print(const void *fdt);
void dtb_debug_print_reserved_memory_children(const void *fdt);
int fdt_get_size(const void* fdt);
int fdt_get_reserved_regions(const void *fdt,
                             struct reserved_region *regions,
                             int max_regions);
uintptr_t get_plic_size(const void *fdt);
#endif