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
unsigned long pte_to_pa(unsigned long pte)
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

//input is va, will find the PMD entry of the VA, then maps the 2MiB it points to to 512 4Kib entries.
static unsigned long *split_pmd_leaf(unsigned long va)
{
    //get the entry for 1 Gib block
    unsigned long pgd_entry = pgd[pgd_index(va)];//value of the pgd slot
    //pgd_entry would save starting addr of the pmd table, convert to PA
    unsigned long *pmd_table = (unsigned long *)pte_to_pa(pgd_entry);//identity not dropped yet
    //to the entry of VA, covers 2mb
    unsigned long *pmd_entry = &pmd_table[pmd_index(va)];//addr of the entry, we modify it later
    //check if leaf (V == 1 and RWX == 000)
    //already split to pte table, return the starting addr
    if (!is_leaf_pte(*pmd_entry)) {
        return (unsigned long *)pte_to_pa(*pmd_entry);
    }
    //is leaf branch : split to pte
    unsigned long old = *pmd_entry;
    unsigned long old_pa_base = pte_to_pa(old);//PA of the target block
    unsigned long old_flags = old & 0x3ff;//the target block's permissions

    unsigned long *pte_table = early_alloc_pte_table();
    //create 512 PTE level mapping, each maps to 4 KB page, inherit flags of PMD leaf for now
    //the 1 to 1 mapping of PMD leaf to PA is preserved, the whole block still maps to the same 2 Mib
    for (int i = 0; i < ENTRIES_PER_TABLE; i++) {
        unsigned long pa = old_pa_base + i * PAGE_SIZE;
        pte_table[i] = MAKE_PTE(pa, old_flags);
    }
    //replace PMD leaf entry (mapped to physical memory) to PTE table, with only V = 1 indicating non leaf
    *pmd_entry = MAKE_PTE(virt_to_phys((unsigned long)pte_table), PTE_V);//translation here should just return pte_table, since MMU is not on yet

    return pte_table;
}
//change prot of a page, will map PME entry to PTE table if not already, this works only on a page
static void map_page_4k(unsigned long va,
                        unsigned long pa,
                        unsigned long prot)
{
    //maps 1 2 Mib leaf to 512 4kb leaves
    unsigned long *pte_table = split_pmd_leaf(va);
    pte_table[pte_index(va)] = MAKE_PTE(pa, prot);
}
//maps VA to PA for size( 0x1000 for a page), with prot ( set to MMIO)
static void map_pages_4k(unsigned long va,
                         unsigned long pa,
                         unsigned long size,
                         unsigned long prot)
{
    if (size == 0)
        return;
    //loop change 1 page's prot at once
    for (unsigned long off = 0; off < size; off += PAGE_SIZE) {
        map_page_4k(va + off, pa + off, prot);
    }
}
//maps an MMIO physical region into the kernel’s high-half virtual address space
//PROT_MMIO    (PTE_V | PTE_R | PTE_W | PTE_G | PTE_A | PTE_D)
static void map_mmio_region(unsigned long pa, unsigned long size)
{
    if (size == 0)
        return;

    unsigned long start_pa = align_down(pa);
    unsigned long end_pa = align_up(pa + size);

    map_pages_4k(phys_to_virt(start_pa),//if used PA, still maps to the same PMD, use VA here because we should look up by VA
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
    //MMU not on, okay to use PA of dtb
    const void *dtb = (const void *)dtb_pa;
    //okay to use PA for bases
    unsigned long uart_pa = get_uart_base(dtb);
    unsigned long uart_size = get_uart_size(dtb);

    //If get_plic_base/get_plic_size use debug_uart_puts(),
    //uart_base must be temporarily physical here because MMU is not enabled yet.
    if (uart_pa)//added for debug else nothing really prints 
        uart_base = uart_pa;//uart_base is global, should be replaced by start_kernel() with VA of uart base

    unsigned long plic_pa = get_plic_base(dtb);
    unsigned long plic_size = get_plic_size(dtb);
    //maps to high addr
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
        pgd[pgd_index(pa_base)] = 0;//just clears PGD entry
    }

    asm volatile("sfence.vma zero, zero" ::: "memory");
}

//basic part 2
//return the orignal root page table that only has kernel mapping, because all user root page table need to copy the kernel mapping
unsigned long *kernel_pgd(void)
{
    return pgd;
}
//copy the kernel part to kernel pgd, all user addr space share same kernel mapping
unsigned long *create_user_pgd(void)
{
    unsigned long *new_pgd = allocate(PAGE_SIZE);//512 * 8 = 4kb
    if (!new_pgd)
        return 0;

    memset(new_pgd, 0, PAGE_SIZE);//clear to 0 

    //Copy upper-half kernel mappings.
    for (int i = 256; i < 512; i++)
        new_pgd[i] = pgd[i];

    return new_pgd;
}
//need to pass root page table because each user process has its own
//creates mapping of VA -> PA with one 4kb page
// walk PGD -> PMD -> PTE, allocate missing table,  write final PTE = PA + prot
static void pagewalk(unsigned long *root,
                     unsigned long va,
                     unsigned long pa,
                     unsigned long prot)
{
    unsigned long vpn[3];

    vpn[0] = (va >> 12) & 0x1ff;//PTE
    vpn[1] = (va >> 21) & 0x1ff;//PMD
    vpn[2] = (va >> 30) & 0x1ff;//PGD

    unsigned long *table = root;
    //loop 1: table is PGD table, index uses 38:30 of VA
    //loop 2: table is PMD table, index uses 29:21 of VA
    //loop 3: table is PTE table, index uses 20:12 of VA
    for (int level = 2; level > 0; level--) {
        unsigned long *pte = &table[vpn[level]];//pte is an entry of current level
        //if the entry was a leaf (not a table V != 1)
        if (!(*pte & PTE_V)) {
            unsigned long *new_table = allocate(PAGE_SIZE);
            if (!new_table)
                return;
            //initialize table, V != 1, will be treated like leaf in next iter
            memset(new_table, 0, PAGE_SIZE);
            //an entry of current level, updated to the PA of the new table, plus V == 1 indicating table
            *pte = MAKE_PTE(virt_to_phys((unsigned long)new_table), PTE_V);
        }//skipped if already allocated a table
        //table is the next level table that contains the VA
        table = (unsigned long *)phys_to_virt(pte_to_pa(*pte));
    }
    //after the loop, it should point PTE table, just map the entry to 4kb page
    table[vpn[0]] = MAKE_PTE(pa, prot);
}

//maps a range of addr (pages) from VA to PA
void map_pages(unsigned long *root,
               unsigned long va,
               unsigned long size,
               unsigned long pa,
               unsigned long prot)
{
    for (unsigned long off = 0; off < size; off += PAGE_SIZE)
        pagewalk(root, va + off, pa + off, prot);
}
//given a va, get the entry in PTE level
unsigned long *get_pte(unsigned long *root, unsigned long va)
{
    unsigned long vpn[3];
    unsigned long *table;
    
    if (!root)
        return 0;

    vpn[0] = (va >> PTE_SHIFT) & 0x1ff;
    vpn[1] = (va >> PMD_SHIFT) & 0x1ff;
    vpn[2] = (va >> PGD_SHIFT) & 0x1ff;

    table = root;
    for (int level = 2; level > 0; level--) {
        unsigned long pte = table[vpn[level]];

        if (!(pte & PTE_V) || is_leaf_pte(pte))
            return 0;

        table = (unsigned long *)phys_to_virt(pte_to_pa(pte));
    }

    return &table[vpn[0]];
}

static int cow_copy_leaf(unsigned long *parent_pte,
                         unsigned long *child_pte)
{
    unsigned long pte = *parent_pte;
    unsigned long flags = pte & 0x3ffUL;

    if ((pte & PTE_U) && (flags & PTE_W)) {
        flags = (flags & ~PTE_W) | PTE_COW;
        pte = MAKE_PTE(pte_to_pa(pte), flags);
        *parent_pte = pte;
    }

    *child_pte = pte;
    return 0;
}

static int cow_copy_level(unsigned long *parent,
                          unsigned long *child,
                          int level)
{
    for (int i = 0; i < ENTRIES_PER_TABLE; i++) {
        unsigned long pte = parent[i];

        if (!(pte & PTE_V))
            continue;

        if (is_leaf_pte(pte)) {
            cow_copy_leaf(&parent[i], &child[i]);
            continue;
        }

        unsigned long *child_next = allocate(PAGE_SIZE);
        if (!child_next)
            return -1;

        memset(child_next, 0, PAGE_SIZE);
        child[i] = MAKE_PTE(virt_to_phys((unsigned long)child_next), PTE_V);

        if (level > 0) {
            unsigned long *parent_next =
                (unsigned long *)phys_to_virt(pte_to_pa(pte));

            if (cow_copy_level(parent_next, child_next, level - 1) < 0)
                return -1;
        }
    }

    return 0;
}

int cow_copy_user_pagetable(unsigned long *parent_pgd,
                            unsigned long *child_pgd)
{
    if (!parent_pgd || !child_pgd)
        return -1;

    for (int i = 0; i < 256; i++) {
        unsigned long pte = parent_pgd[i];

        if (!(pte & PTE_V))
            continue;

        if (is_leaf_pte(pte)) {
            cow_copy_leaf(&parent_pgd[i], &child_pgd[i]);
            continue;
        }

        unsigned long *child_pmd = allocate(PAGE_SIZE);
        if (!child_pmd)
            return -1;

        memset(child_pmd, 0, PAGE_SIZE);
        child_pgd[i] = MAKE_PTE(virt_to_phys((unsigned long)child_pmd), PTE_V);

        unsigned long *parent_pmd =
            (unsigned long *)phys_to_virt(pte_to_pa(pte));

        if (cow_copy_level(parent_pmd, child_pmd, 1) < 0)
            return -1;
    }

    asm volatile("sfence.vma zero, zero" ::: "memory");
    return 0;
}
//later called like : maps user VA to some PA backed by kernel VA, because allocation returns VA 
/*
map_pages(task->pgd,
          USER_CODE_BASE,
          program_size,
          virt_to_phys(image->base),
          PROT_USER_RX);
*/
//helper for context switch changing satp
void switch_pgd(unsigned long *next_pgd)//next_pgd is kernel VA pointer to next root page table (task->pgd)
{
    if (!next_pgd)
        next_pgd = kernel_pgd();//switch back to kernel mode

    asm volatile(
        "csrw satp, %0\n"
        "sfence.vma zero, zero\n"//flushes old TLB entries
        :
        : "r"(MAKE_SATP(virt_to_phys((unsigned long)next_pgd)))//satp want PA
        : "memory"
    );
    //safe because schedule running on kernel stack, the kernel stack mapping is the same for all processes
}

//free the tables allocated, not the memory object leaves map to
static void free_pagetable_level(unsigned long *table, int level)
{
    for (int i = 0; i < ENTRIES_PER_TABLE; i++) {
        unsigned long pte = table[i];
        //if entry does not save next level table, skip it
        if (!(pte & PTE_V))
            continue;

        //leaf PTE maps to memory allocated from dynamic allocator (stack, program image)
        //they should be freed on zombie reaping in free_task()
        if (is_leaf_pte(pte))
            continue;

        //non leaf points to next level table !!THIS FREES PTE (CHILD) TABLE
        unsigned long *next =
            (unsigned long *)phys_to_virt(pte_to_pa(pte));//PTE saves PA, need to translate to VA
        //if not last level, free the next level page table first.
        if (level > 0)
            free_pagetable_level(next, level - 1);
        //free the page, the page should contain 512 entries (4kb)
        free(next);
    }
}

void free_user_pgd(unsigned long *root)
{
    if (!root)
        return;

    //don't free global pgd, we need it to create user pgd, and kernel uses it
    if (root == kernel_pgd())
        return;

    //only free bottom half mapping, which should only be user level mapping
    //because PGD entry can map to same PMD table, it will also free kernel mapped stuff
    //Kernel upper-half mappings are copied from kernel pgd entries 256..511.
    for (int i = 0; i < 256; i++) {
        unsigned long pte = root[i];

        if (!(pte & PTE_V))
            continue;

        if (is_leaf_pte(pte))
            continue;
        //if is a table
        unsigned long *pmd_table  =
            (unsigned long *)phys_to_virt(pte_to_pa(pte));

        //always call on PMD table, calling on PGD tables clears kernel mapping too
        free_pagetable_level(pmd_table , 1);

        free(pmd_table );//!!THIS FREES PMD TABLE
    }

    free(root);
}
//no free leaf because they are freed somewhere else
