#include "dt_parse.h"
#include "types.h"
#include "str.h"
#include "uart.h"


#define FDT_BEGIN_NODE 0x00000001
#define FDT_END_NODE   0x00000002
#define FDT_PROP       0x00000003
#define FDT_NOP        0x00000004
#define FDT_END        0x00000009


static inline uint32_t bswap32(uint32_t x) {
    return ((x & 0x000000FFU) << 24) |
           ((x & 0x0000FF00U) << 8)  |
           ((x & 0x00FF0000U) >> 8)  |
           ((x & 0xFF000000U) >> 24);
}

static inline uint64_t bswap64(uint64_t x) {
    return ((x & 0x00000000000000FFULL) << 56) |
           ((x & 0x000000000000FF00ULL) << 40) |
           ((x & 0x0000000000FF0000ULL) << 24) |
           ((x & 0x00000000FF000000ULL) << 8)  |
           ((x & 0x000000FF00000000ULL) >> 8)  |
           ((x & 0x0000FF0000000000ULL) >> 24) |
           ((x & 0x00FF000000000000ULL) >> 40) |
           ((x & 0xFF00000000000000ULL) >> 56);
}

static inline const void* align_up(const void* ptr, size_t align) {
    return (const void*)(((uintptr_t)ptr + align - 1) & ~(align - 1));
}

int fdt_path_offset(const void* fdt, const char* path) {
    // TODO: Implement this function
    //return null on invalid inputs
    if (!fdt || !path) return -1;

    //treat starting addr as header structure (assume it has layout)
    const struct fdt_header* hdr = (const struct fdt_header*)fdt;

    /* Check magic */
    if (bswap32(hdr->magic) != 0xd00dfeed)
    {
        return -1;
    }
    //get staring addr of structure block 
    const char* struct_base = (const char*)fdt + bswap32(hdr->off_dt_struct);
    const char* p = struct_base;// p is cursor

    /* Keep node names of current path */
    const char* names[128];
    int depth = -1;//for tree depth, 0 when root

    while (1) {
        const char* token_pos = p;// need to return offset (token_pos - base)
        //read a 4 bytes each time (derefence at p)
        uint32_t token = bswap32(*(const uint32_t*)p);
        p += sizeof(uint32_t);//++ 4 bytes

        if (token == FDT_BEGIN_NODE) {
            const char* node_name = p;// The name is stored as a null-terminated string (spec)
            size_t name_len = strlen(node_name);

            depth++;//found new node, depth ++
            if (depth >= 128)
            {
                return -1;
            } 
            names[depth] = node_name;

            /* Build current full path */
            char cur_path[1024];
            size_t pos = 0;//writing the path

            if (depth == 0 && node_name[0] == '\0') {
                //root
                cur_path[pos++] = '/';
                cur_path[pos] = '\0';//result "/"
            } else {
                cur_path[pos++] = '/';//else need to add "/"
                for (int i = 1; i <= depth; i++) {// for all the heirachy, try build the path
                    size_t len = strlen(names[i]);
                    if (pos + len + 1 >= sizeof(cur_path))// if the name we about to past make overflow
                    {
                        return -1;
                    }
                    memcpy(cur_path + pos, names[i], len);// add name to the comparing string
                    pos += len;
                    if (i != depth)//if not last node
                        cur_path[pos++] = '/';
                }
                cur_path[pos] = '\0';//append null
            }
            /* Return first match */
            if (strcmp(cur_path, path) == 0)//if found
            {
                return (int)(token_pos - struct_base);//return offset
            }
            char cmp_path[1024];
            strcpy(cmp_path, cur_path);

            char* last_slash = strrchr(cmp_path, '/');
            if (last_slash) {
                char* at = strchr(last_slash + 1, '@');   // only search @ in last node name
                if (at) {
                    *at = '\0';   // temporarily cut "@..."
                    if (strcmp(cmp_path, path) == 0)
                    {
                        return (int)(token_pos - struct_base);
                    }
                }
            }
            p = align_up(p + name_len + 1, 4);//align since name has variable length, 1 byte for null
        } else if (token == FDT_END_NODE) {
            depth--;// no need to clear, will not use the node
        } else if (token == FDT_PROP) {
            uint32_t len = bswap32(*(const uint32_t*)p);
            p += sizeof(uint32_t);              /* len */
            p += sizeof(uint32_t);              /* nameoff */
            p = align_up(p + len, 4);           /* data + padding */
        } else if (token == FDT_NOP) {
            /* ignore */
        } else if (token == FDT_END) {
            break;
        } else {
            return -1;
        }
    }
    return -1;
}

const void* fdt_getprop(const void* fdt,
                        int nodeoffset,
                        const char* name,
                        int* lenp) {
    // TODO: Implement this function
    if (!fdt || !name || nodeoffset < 0) {
        if (lenp) *lenp = -1;//if lenp pointer is not null, write -1 to len (*lenp)
        return NULL;
    }

    const struct fdt_header* hdr = (const struct fdt_header*)fdt;

    /* Check magic */
    if (bswap32(hdr->magic) != 0xd00dfeed) {
        if (lenp) *lenp = -1;
        return NULL;
    }

    const char* struct_base  = (const char*)fdt + bswap32(hdr->off_dt_struct);//need to find property offset from structure block
    const char* strings_base = (const char*)fdt + bswap32(hdr->off_dt_strings);//find the actual name in string block
    const char* p = struct_base + nodeoffset;

    /* nodeoffset should point to a BEGIN_NODE */
    uint32_t token = bswap32(*(const uint32_t*)p);//should jump to being_node token
    if (token != FDT_BEGIN_NODE) {
        if (lenp) *lenp = -1;
        return NULL;
    }

    p += sizeof(uint32_t);//move past node
    p = align_up(p + strlen(p) + 1, 4);//skip name

    int depth = 0;//depth tracks only target node

    while (1) {
        token = bswap32(*(const uint32_t*)p);//read token of current pos
        p += sizeof(uint32_t);//move forward 1 token
        // read next token in structure
        if (token == FDT_PROP) {
            // [FDT_PROP]
            // [len] : length of property value in bytes
            // [nameoff] : offset into strings block for the property name
            // [data bytes] : a byte string of length len
            // [padding] : actual property name string from strings block
            uint32_t len = bswap32(*(const uint32_t*)p);//len 
            p += sizeof(uint32_t);//move forward 32 bit len

            uint32_t nameoff = bswap32(*(const uint32_t*)p);//name offset
            p += sizeof(uint32_t);//move forward 32 bit nameoff

            const void* data = p;//pointer to the a byte string of length len
            const char* prop_name = strings_base + nameoff;// for string block
            if (depth == 0 && strcmp(prop_name, name) == 0) {//currently in target node, current prop name == target name
                if (lenp) *lenp = (int)len;//store length in byte in property
                return data;// data was pointed to the [data bytes] section
            }

            p = align_up(p + len, 4);//data might not align
        } else if (token == FDT_BEGIN_NODE) {
            const char* child_name = p;
            p = align_up(p + strlen(child_name) + 1, 4);
            depth++;
        } else if (token == FDT_END_NODE) {
            if (depth == 0)
                break;   /* end of this node */
            depth--;
        } else if (token == FDT_NOP) {
            /* ignore */
        } else if (token == FDT_END) {
            break;
        } else {
            if (lenp) *lenp = -1;
            return NULL;
        }
    }
    if (lenp) *lenp = -1;
    return NULL;
}

//new for lab3: parsing subnode in reserved_memory

//helper to get name
const char *fdt_get_name(const void *fdt, int nodeoffset, int *lenp) {
    const struct fdt_header *hdr;
    const char *struct_base;
    const char *p;

    if (!fdt || nodeoffset < 0) {
        if (lenp) *lenp = -1;
        return NULL;
    }

    hdr = (const struct fdt_header *)fdt;
    if (bswap32(hdr->magic) != 0xd00dfeed) {
        if (lenp) *lenp = -1;
        return NULL;
    }

    struct_base = (const char *)fdt + bswap32(hdr->off_dt_struct);
    p = struct_base + nodeoffset;
    //the offset should be NODE_START, so it should have name following it
    if (bswap32(*(const uint32_t *)p) != FDT_BEGIN_NODE) {
        if (lenp) *lenp = -1;
        return NULL;
    }

    p += sizeof(uint32_t);//skip the token

    if (lenp)
        *lenp = (int)strlen(p);

    return p;
}

//find the offset of the first sub node, -1 if not found or error
int fdt_first_subnode(const void *fdt, int parentoffset) {
    const struct fdt_header *hdr;
    const char *struct_base;
    const char *p;
    int depth = 0;

    if (!fdt || parentoffset < 0)
        return -1;

    hdr = (const struct fdt_header *)fdt;
    if (bswap32(hdr->magic) != 0xd00dfeed)
        return -1;

    struct_base = (const char *)fdt + bswap32(hdr->off_dt_struct);
    p = struct_base + parentoffset;
    //skip the parent node itself
    //parentoffset must point to a BEGIN_NODE
    if (bswap32(*(const uint32_t *)p) != FDT_BEGIN_NODE)
        return -1;

    //skip BEGIN_NODE token
    p += sizeof(uint32_t);

    //skip parent node name
    p = align_up(p + strlen(p) + 1, 4);

    while (1) {
        const char *token_pos = p;
        uint32_t token = bswap32(*(const uint32_t *)p);
        p += sizeof(uint32_t);

        if (token == FDT_PROP) {//skip prop
            uint32_t len = bswap32(*(const uint32_t *)p);
            p += sizeof(uint32_t);   /* len */
            p += sizeof(uint32_t);   /* nameoff */
            p = align_up(p + len, 4);
        } else if (token == FDT_BEGIN_NODE) {//found the first node inside the parent node, return
            if (depth == 0)
                return (int)(token_pos - struct_base);

            p = align_up(p + strlen(p) + 1, 4);
            depth++;
        } else if (token == FDT_END_NODE) {//whole parent node didn't find any child
            if (depth == 0)
                return -1;
            depth--;
        } else if (token == FDT_NOP) {
            //ignore
        } else if (token == FDT_END) {
            return -1;
        } else {
            return -1;
        }
        //shouldn't reach here
        if (token == FDT_BEGIN_NODE && depth == 0) {
            return (int)(token_pos - struct_base);
        }
    }
}

//find the offset of next subnode, after the first subnode is found, -1 if error or not found
int fdt_next_subnode(const void *fdt, int nodeoffset) {
    const struct fdt_header *hdr;
    const char *struct_base;
    const char *p;
    int depth = 0;

    if (!fdt || nodeoffset < 0)
        return -1;

    hdr = (const struct fdt_header *)fdt;
    if (bswap32(hdr->magic) != 0xd00dfeed)
        return -1;

    struct_base = (const char *)fdt + bswap32(hdr->off_dt_struct);
    p = struct_base + nodeoffset;
    //skip the siblin itself
    //nodeoffset must point to a BEGIN_NODE
    if (bswap32(*(const uint32_t *)p) != FDT_BEGIN_NODE)
        return -1;

    //skip this node's BEGIN_NODE token 
    p += sizeof(uint32_t);

    //skip this node's name
    p = align_up(p + strlen(p) + 1, 4);

    while (1) {
        const char *token_pos = p;
        uint32_t token = bswap32(*(const uint32_t *)p);
        p += sizeof(uint32_t);

        if (token == FDT_PROP) {//skip siblin's prop
            uint32_t len = bswap32(*(const uint32_t *)p);
            p += sizeof(uint32_t);   /* len */
            p += sizeof(uint32_t);   /* nameoff */
            p = align_up(p + len, 4);
        } else if (token == FDT_BEGIN_NODE) {//skip the siblin's child, only look for nodes in same parent
            p = align_up(p + strlen(p) + 1, 4);
            depth++;
        } else if (token == FDT_END_NODE) {//2 cases for END_NODE, might be the end of the grandchild
            if (depth == 0) {//if it is the end of the orignal node, start finding siblin
                while (1) {
                    token_pos = p;
                    token = bswap32(*(const uint32_t *)p);
                    p += sizeof(uint32_t);

                    if (token == FDT_NOP) {
                        continue;
                    } else if (token == FDT_BEGIN_NODE) {//return the offset of the next siblin node
                        return (int)(token_pos - struct_base);
                    } else if (token == FDT_END_NODE || token == FDT_END) {
                        return -1;
                    } else if (token == FDT_PROP) {//skip whole prop
                        /* sibling region should not start with PROP */
                        uint32_t len = bswap32(*(const uint32_t *)p);
                        p += sizeof(uint32_t);
                        p += sizeof(uint32_t);
                        p = align_up(p + len, 4);
                    } else {
                        return -1;
                    }
                }
            }
            depth--;//not the end of the orignal node, keep parsing until reaching the END_NODE of the current node
        } else if (token == FDT_NOP) {
            /* ignore */
        } else if (token == FDT_END) {
            return -1;
        } else {
            return -1;
        }
    }
}

int fdt_get_size(const void* fdt) {
    if (!fdt) return -1;
    //
    const struct fdt_header* hdr = (const struct fdt_header*)fdt;
    // Check magic 
    if (bswap32(hdr->magic) != 0xd00dfeed)
    {
        return -1;
    }
    return (int)bswap32(hdr->totalsize);
}

uintptr_t get_uart_base(const void *fdt) {
    int len;
    int offset = fdt_path_offset(fdt, "/soc/serial");
    const void* prop = fdt_getprop(fdt, offset, "reg", &len);
    if (!prop || len < 8)
        return 0;
    const uint64_t* reg = (const uint64_t*)prop;
    //printf("offset = %d\n", offset);
    //printf("len = %d\n", len);
    //printf("prop addr = %p\n", prop);
    //printf("UART: base=0x%lx size=0x%lx\n", bswap64(reg[0]), bswap64(reg[1]));
    
    return bswap64(reg[0]);
};

uintptr_t get_uart_size(const void *fdt)
{
    int len;
    int offset = fdt_path_offset(fdt, "/soc/serial");
    const void* prop = fdt_getprop(fdt, offset, "reg", &len);

    if (!prop || len < 16)
        return 0;

    const uint64_t* reg = (const uint64_t*)prop;

    /*
     * reg[0] = base
     * reg[1] = size
     */
    return bswap64(reg[1]);
};

uintptr_t get_mem_start(const void *fdt) {
    // device tree is at fdt
    /* Find the node offset */
    int len;
    int offset = fdt_path_offset(fdt, "/memory");
    const void* prop = fdt_getprop(fdt, offset, "reg", &len);
    if (!prop || len < 16)
        return 0;
    const uint64_t* reg = (const uint64_t*)prop;
    //printf("offset = %d\n", offset);
    //printf("len = %d\n", len);
    //printf("prop addr = %p\n", prop);
    //printf("UART: base=0x%lx size=0x%lx\n", bswap64(reg[0]), bswap64(reg[1]));
    uintptr_t  mem_start_addr = bswap64(reg[0]);
    return mem_start_addr;
};

uintptr_t get_mem_end(const void *fdt) {
    // device tree is at fdt
    /* Find the node offset */
    int len;
    int offset = fdt_path_offset(fdt, "/memory");
    const void* prop = fdt_getprop(fdt, offset, "reg", &len);
    if (!prop || len < 16)
        return 0;
    const uint64_t* reg = (const uint64_t*)prop;
    //printf("offset = %d\n", offset);
    //printf("len = %d\n", len);
    //printf("prop addr = %p\n", prop);
    //printf("UART: base=0x%lx size=0x%lx\n", bswap64(reg[0]), bswap64(reg[1]));
    uintptr_t  mem_end_addr = bswap64(reg[0]) + bswap64(reg[1]);
    return mem_end_addr;
};

uintptr_t get_initrd_start(const void *fdt) {
    // device tree is at fdt
    /* Find the node offset */
    int len;
    int offset = fdt_path_offset(fdt, "/chosen");
    const void* prop = fdt_getprop(fdt, offset, "linux,initrd-start", &len);
    if (!prop || len < 8)
        return 0;
    const uint64_t* reg = (const uint64_t*)prop;
    //printf("offset = %d\n", offset);
    //printf("len = %d\n", len);
    //printf("prop addr = %p\n", prop);
    //printf("UART: base=0x%lx size=0x%lx\n", bswap64(reg[0]), bswap64(reg[1]));
    uintptr_t  ramdisk_start_addr = bswap64(reg[0]);
    return ramdisk_start_addr;
};

uintptr_t get_initrd_end(const void *fdt) {
    // device tree is at fdt
    /* Find the node offset */
    int len;
    int offset = fdt_path_offset(fdt, "/chosen");
    const void* prop = fdt_getprop(fdt, offset, "linux,initrd-end", &len);
    if (!prop || len < 8)
        return 0;
    const uint64_t* reg = (const uint64_t*)prop;
    //printf("offset = %d\n", offset);
    //printf("len = %d\n", len);
    //printf("prop addr = %p\n", prop);
    //printf("UART: base=0x%lx size=0x%lx\n", bswap64(reg[0]), bswap64(reg[1]));
    uintptr_t  ramdisk_end_addr = bswap64(reg[0]);
    return ramdisk_end_addr;
};

int fdt_get_reserved_regions(const void *fdt,
                             struct reserved_region *regions,
                             int max_regions)
{
    int parent;
    int child;
    int count = 0;

    parent = fdt_path_offset(fdt, "/reserved-memory");//get offset for parent
    if (parent < 0)
        return 0;

    child = fdt_first_subnode(fdt, parent);//get offset for children
    while (child >= 0 && count < max_regions) {//loop for 32 times, if child not found, the condition breaks
        int len;
        const char *name;
        const void *prop;

        name = fdt_get_name(fdt, child, 0);//get child name for logging
        prop = fdt_getprop(fdt, child, "reg", &len);

        if (prop && len >= 16) {
            const uint32_t *reg = (const uint32_t *)prop;
            uint64_t base;
            uint64_t size;
            //from parsing, each start addr and size has 2 32bits
            //reg[0] = base high 32 bits
            //reg[1] = base low 32 bits
            //reg[2] = size high 32 bits
            //reg[3] = size low 32 bits
            base = ((uint64_t)bswap32(reg[0]) << 32) | bswap32(reg[1]);//cast to 64 bit then OR 
            size = ((uint64_t)bswap32(reg[2]) << 32) | bswap32(reg[3]);

            if (size != 0) {//if parse returned meaningful stuff, add to metadata struct
                strcpy(regions[count].name, name);
                regions[count].start = (uintptr_t)base;
                regions[count].end = (uintptr_t)(base + size);
                count++;
            }
        }

        child = fdt_next_subnode(fdt, child);
    }

    return count;
}

void dtb_debug_print(const void *fdt) {
    int fdt_size;
    int reserved_count;
    uintptr_t fdt_addr;
    uintptr_t uart_base;
    uintptr_t mem_start;
    uintptr_t mem_end;
    uintptr_t initrd_start;
    uintptr_t initrd_end;
    struct reserved_region regions[MAX_RESERVED_REGIONS];

    fdt_addr = (uintptr_t)fdt;
    fdt_size = fdt_get_size(fdt);
    uart_base = get_uart_base(fdt);
    mem_start = get_mem_start(fdt);
    mem_end = get_mem_end(fdt);
    initrd_start = get_initrd_start(fdt);
    initrd_end = get_initrd_end(fdt);
    reserved_count = fdt_get_reserved_regions(fdt, regions, MAX_RESERVED_REGIONS);

    uart_puts("DTB debug info:\n");

    uart_puts("  fdt          = ");
    uart_hex((unsigned long)fdt_addr);
    uart_puts("\n");

    uart_puts("  fdt_size     = ");
    uart_int(fdt_size);
    uart_puts("\n");

    uart_puts("  fdt_end      = ");
    uart_hex((unsigned long)(fdt_addr + (uintptr_t)fdt_size));
    uart_puts("\n");

    uart_puts("  uart_base    = ");
    uart_hex((unsigned long)uart_base);
    uart_puts("\n");

    uart_puts("  mem_start    = ");
    uart_hex((unsigned long)mem_start);
    uart_puts("\n");

    uart_puts("  mem_end      = ");
    uart_hex((unsigned long)mem_end);
    uart_puts("\n");

    uart_puts("  initrd_start = ");
    uart_hex((unsigned long)initrd_start);
    uart_puts("\n");

    uart_puts("  initrd_end   = ");
    uart_hex((unsigned long)initrd_end);
    uart_puts("\n");

    uart_puts("  reserved_count = ");
    uart_int(reserved_count);
    uart_puts("\n");

    for (int i = 0; i < reserved_count; i++) {
        uart_puts("  child = ");
        uart_puts(regions[i].name);
        uart_puts("\n");

        uart_puts("    start = ");
        uart_hex((unsigned long)regions[i].start);
        uart_puts("\n");

        uart_puts("    end   = ");
        uart_hex((unsigned long)regions[i].end);
        uart_puts("\n");

        uart_puts("    size  = ");
        uart_hex((unsigned long)(regions[i].end - regions[i].start));
        uart_puts("\n");
    }
}

unsigned long get_timebase_frequency(const void *fdt) {
    int len;
    int offset = fdt_path_offset(fdt, "/cpus");
    const uint32_t *prop = (const uint32_t *)fdt_getprop(fdt, offset, "timebase-frequency", &len);

    if (!prop || len < 4)
        return 0;

    return bswap32(*prop);
}

static int fdt_find_plic_offset(const void *fdt)
{
    int offset = fdt_path_offset(fdt, "/soc/plic");

    if (offset < 0)
        offset = fdt_path_offset(fdt, "/plic");

    if (offset < 0)
        offset = fdt_path_offset(fdt, "/soc/interrupt-controller");

    if (offset < 0)
        offset = fdt_path_offset(fdt, "/soc/interrupt-controller@e0000000");

    return offset;
}

uintptr_t get_plic_base(const void *fdt) {
    int len;
    int offset = fdt_find_plic_offset(fdt);

    if (offset < 0)
        return 0;

    const void *prop = fdt_getprop(fdt, offset, "reg", &len);
    if (!prop || len < 8)
        return 0;

    const uint32_t *reg = (const uint32_t *)prop;
    return (uintptr_t)(((uint64_t)bswap32(reg[0]) << 32) | bswap32(reg[1]));
}

uintptr_t get_plic_size(const void *fdt)
{
    int len;
    int offset = fdt_find_plic_offset(fdt);

    if (offset < 0)
        return 0;

    const void *prop = fdt_getprop(fdt, offset, "reg", &len);

    if (!prop || len < 16)
        return 0;

    const uint32_t *reg = (const uint32_t *)prop;

    /*
     * reg[0] = base high 32 bits
     * reg[1] = base low 32 bits
     * reg[2] = size high 32 bits
     * reg[3] = size low 32 bits
     */
    return (uintptr_t)(((uint64_t)bswap32(reg[2]) << 32) | bswap32(reg[3]));
}