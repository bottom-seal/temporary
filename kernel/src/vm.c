#include "vm.h"
#include "dt_parse.h"
#include "page_alloc.h"
#include "str.h"
static inline unsigned long align_down(unsigned long x)
{
    return x & ~(PAGE_SIZE - 1);
}
static inline unsigned long align_up(unsigned long x)
{
    return (x + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1);
}
//initialize pgd table
static unsigned long __attribute__((section(".data"), aligned(PAGE_SIZE)))
    pgd[ENTRIES_PER_TABLE] = { 0 };
//initialize pmd tables
static unsigned long __attribute__((section(".data"), aligned(PAGE_SIZE)))
    pmd[LINEAR_MAP_GIB][ENTRIES_PER_TABLE] = { { 0 } };
static unsigned long __attribute__((section(".data"), aligned(PAGE_SIZE)))
    early_pte_tables[MAX_EARLY_PTE_TABLES][ENTRIES_PER_TABLE] = { { 0 } };
static int __attribute__((section(".data"))) early_pte_used = 0;//bug fixed: this was placed in bss, and shell was just broken
//9bits of 38:30
static inline unsigned long pgd_index(unsigned long va)
{
    return (va >> PGD_SHIFT) & 0x1ff;
}
static inline unsigned long pmd_index(unsigned long va)
{
    return (va >> PMD_SHIFT) & 0x1ff;
}
static inline unsigned long pte_index(unsigned long va)
{
    return (va >> PTE_SHIFT) & 0x1ff;
}
static inline unsigned long pte_to_pa(unsigned long pte)
{
    return (pte >> 10) << 12;
}
static inline int is_leaf_pte(unsigned long pte)
{
    return pte & (PTE_R | PTE_W | PTE_X);
}

static unsigned long *early_alloc_pte_table(void)
{
    if (early_pte_used >= MAX_EARLY_PTE_TABLES) {
        while (1) {
            asm volatile("wfi");
        }
    }

    return early_pte_tables[early_pte_used++];
}

//input is va of a 1 Gib block
static unsigned long *split_pmd_leaf(unsigned long va)
{
    //get the entry for 1 Gib block
    unsigned long pgd_entry = pgd[pgd_index(va)];
    //pgd_entry would save starting addr of the pmd table, convert to PA
    unsigned long *pmd_table = (unsigned long *)pte_to_pa(pgd_entry);
    //to the entry of VA, covers 2mb
    unsigned long *pmd_entry = &pmd_table[pmd_index(va)];
    //check if leaf (V == 1 and RWX == 000)
    //already split to pte table, return the starting addr
    if (!is_leaf_pte(*pmd_entry)) {
        return (unsigned long *)pte_to_pa(*pmd_entry);
    }

    unsigned long old = *pmd_entry;
    unsigned long old_pa_base = pte_to_pa(old);//PA of the target block
    unsigned long old_flags = old & 0x3ff;//the target block's permissions

    unsigned long *pte_table = early_alloc_pte_table();
    //create 512 PTE level mapping, each maps to 4 KB page
    for (int i = 0; i < ENTRIES_PER_TABLE; i++) {
        unsigned long pa = old_pa_base + i * PAGE_SIZE;
        pte_table[i] = MAKE_PTE(pa, old_flags);
    }
    //replace PMD leaf (mapped to physical memory) to PTE table, with only V = 1 indicating non leaf
    *pmd_entry = MAKE_PTE(virt_to_phys((unsigned long)pte_table), PTE_V);

    return pte_table;
}
//change prot of a page
static void map_page_4k(unsigned long va,
                        unsigned long pa,
                        unsigned long prot)
{
    unsigned long *pte_table = split_pmd_leaf(va);
    pte_table[pte_index(va)] = MAKE_PTE(pa, prot);
}
//change all pages in the range
static void map_pages_4k(unsigned long va,
                         unsigned long pa,
                         unsigned long size,
                         unsigned long prot)
{
    if (size == 0)
        return;

    for (unsigned long off = 0; off < size; off += PAGE_SIZE) {
        map_page_4k(va + off, pa + off, prot);
    }
}
//maps base + size, for UART, PLIC
static void map_mmio_region(unsigned long pa, unsigned long size)
{
    if (size == 0)
        return;

    unsigned long start_pa = align_down(pa);
    unsigned long end_pa = align_up(pa + size);

    map_pages_4k(phys_to_virt(start_pa),
                 start_pa,
                 end_pa - start_pa,
                 PROT_MMIO);
}
void setup_vm(unsigned long dtb_pa)
{

    //maps 2 area:
    //temporary direct mapping: VA = PA, so execution around setup_vm call doesn't crash
    //permanent high addr mapping: VA = PA + PAGE_OFFSET, after VM is on, CPU expects to run on VA
    for (int i = 0; i < LINEAR_MAP_GIB; i++) {//LINEAR_MAP_GIB is 4, loop runs 4 times
        //PA for 
        unsigned long pa_base = (unsigned long)i * PGD_SIZE;//pa should be start if each 1 GiB block.
        //for each entry in pmd table,
        for (int j = 0; j < ENTRIES_PER_TABLE; j++) {//ENTRIES_PER_TABLE is 512
            unsigned long pa = pa_base + (unsigned long)j * PMD_SIZE;//pa should be start if each 2 Mb block in 512 entries

            //currently all mapped to coarsed grained control
            pmd[i][j] = MAKE_PTE(pa, PROT_KERNEL);
        }
        //if MMU not enabled, virt_to_phys just returns same value
        unsigned long pmd_pa = virt_to_phys((unsigned long)pmd[i]);//addr of pmd[i][0]
        //
        //identity mapping, maps pgd entry to pmd table
        pgd[pgd_index(pa_base)] = MAKE_PTE(pmd_pa, PTE_V);

        //high addr mapping
        pgd[pgd_index(phys_to_virt(pa_base))] = MAKE_PTE(pmd_pa, PTE_V);
    }

    //for fine grained control
    const void *dtb = (const void *)dtb_pa;

    unsigned long uart_pa = get_uart_base(dtb);
    unsigned long uart_size = get_uart_size(dtb);

    /*
     * If get_plic_base/get_plic_size use debug_uart_puts(),
     * uart_base must be temporarily physical here because MMU is not enabled yet.
     */
    if (uart_pa)
        uart_base = uart_pa;

    unsigned long plic_pa = get_plic_base(dtb);
    unsigned long plic_size = get_plic_size(dtb);

    if (uart_pa && uart_size)
        map_mmio_region(uart_pa, uart_size);

    if (plic_pa && plic_size)
        map_mmio_region(plic_pa, plic_size);

    //Enables sv39 VM, Mode = sv39(8), ASID = 0, PPN = PA of pgd table
    unsigned long satp = MAKE_SATP(virt_to_phys((unsigned long)pgd));
    //after writing satp, flush TLB to ensure no stale
    asm volatile(
        "csrw satp, %0\n"
        "sfence.vma zero, zero\n"
        :
        : "r"(satp)
        : "memory"
    );
}

void drop_identity_map(void)
{
    //remove identity mapping, clear the entrie (also sets Valid 0)
    for (int i = 0; i < LINEAR_MAP_GIB; i++) {
        unsigned long pa_base = (unsigned long)i * PGD_SIZE;
        pgd[pgd_index(pa_base)] = 0;
    }

    asm volatile("sfence.vma zero, zero" ::: "memory");
}

//basic part 2
//return the orignal pgd that only has kernel pgd
unsigned long *kernel_pgd(void)
{
    return pgd;
}
//copy the kernel part to kernel pgd, all user addr space share same kernel mapping
unsigned long *create_user_pgd(void)
{
    unsigned long *new_pgd = allocate(PAGE_SIZE);
    if (!new_pgd)
        return 0;

    memset(new_pgd, 0, PAGE_SIZE);

    /*
     * Copy upper-half kernel mappings.
     * Sv39 high-half PAGE_OFFSET index is usually 256.
     */
    for (int i = 256; i < 512; i++)
        new_pgd[i] = pgd[i];

    return new_pgd;
}
//need to pass root instead of global pgd
static void pagewalk(unsigned long *root,
                     unsigned long va,
                     unsigned long pa,
                     unsigned long prot)
{
    unsigned long vpn[3];

    vpn[0] = (va >> 12) & 0x1ff;
    vpn[1] = (va >> 21) & 0x1ff;
    vpn[2] = (va >> 30) & 0x1ff;

    unsigned long *table = root;

    for (int level = 2; level > 0; level--) {
        unsigned long *pte = &table[vpn[level]];

        if (!(*pte & PTE_V)) {
            unsigned long *new_table = allocate(PAGE_SIZE);
            if (!new_table)
                return; // later maybe return int for error

            memset(new_table, 0, PAGE_SIZE);
            *pte = MAKE_PTE(virt_to_phys((unsigned long)new_table), PTE_V);
        }

        table = (unsigned long *)phys_to_virt(pte_to_pa(*pte));
    }

    table[vpn[0]] = MAKE_PTE(pa, prot);
}

void map_pages(unsigned long *root,
               unsigned long va,
               unsigned long size,
               unsigned long pa,
               unsigned long prot)
{
    for (unsigned long off = 0; off < size; off += PAGE_SIZE)
        pagewalk(root, va + off, pa + off, prot);
}

//helper for context switch changing satp
void switch_pgd(unsigned long *next_pgd)
{
    if (!next_pgd)
        next_pgd = kernel_pgd();

    asm volatile(
        "csrw satp, %0\n"
        "sfence.vma zero, zero\n"
        :
        : "r"(MAKE_SATP(virt_to_phys((unsigned long)next_pgd)))
        : "memory"
    );
}

//free pgd related stuff
static void free_pagetable_level(unsigned long *table, int level)
{
    for (int i = 0; i < ENTRIES_PER_TABLE; i++) {
        unsigned long pte = table[i];

        if (!(pte & PTE_V))
            continue;

        /*
         * Leaf PTE maps real memory: program, stack, signal stack, etc.
         * Do NOT free the physical page here.
         * free_task() already frees those owners separately.
         */
        if (is_leaf_pte(pte))
            continue;

        /*
         * Non-leaf PTE points to another page-table page.
         */
        unsigned long *next =
            (unsigned long *)phys_to_virt(pte_to_pa(pte));

        if (level > 0)
            free_pagetable_level(next, level - 1);

        free(next);
    }
}

void free_user_pgd(unsigned long *root)
{
    if (!root)
        return;

    /*
     * Never free the global kernel pgd.
     */
    if (root == kernel_pgd())
        return;

    /*
     * Only free lower-half user mappings.
     * Kernel upper-half mappings are copied from kernel pgd entries 256..511.
     */
    for (int i = 0; i < 256; i++) {
        unsigned long pte = root[i];

        if (!(pte & PTE_V))
            continue;

        if (is_leaf_pte(pte))
            continue;

        unsigned long *next =
            (unsigned long *)phys_to_virt(pte_to_pa(pte));

        /*
         * root -> level 1 -> level 0
         */
        free_pagetable_level(next, 1);

        free(next);
    }

    free(root);
}
//no free leaf because they are freed somewhere else