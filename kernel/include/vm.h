#ifndef VM_H
#define VM_H

#include <types.h>
extern uintptr_t uart_base;

#define PAGE_OFFSET   0xffffffc000000000UL

#ifndef PAGE_SIZE
#define PAGE_SIZE (1UL << 12)
#endif

#define PMD_SIZE      (1UL << 21)
#define PGD_SIZE      (1UL << 30)

#define PGD_SHIFT     30
#define PMD_SHIFT     21
#define PTE_SHIFT     12

#define ENTRIES_PER_TABLE 512

#define LINEAR_MAP_GIB 4

#define PTE_V  (1UL << 0)
#define PTE_R  (1UL << 1)
#define PTE_W  (1UL << 2)
#define PTE_X  (1UL << 3)
#define PTE_U  (1UL << 4)
#define PTE_G  (1UL << 5)
#define PTE_A  (1UL << 6)
#define PTE_D  (1UL << 7)
#define PTE_COW (1UL << 8)
//basic part 2 USER addr space
#define USER_CODE_BASE   0x0UL
#define USER_STACK_TOP   0x4000000000UL
#define USER_STACK_BASE  (USER_STACK_TOP - 8 * PAGE_SIZE)
//signal is broken now, trying to fix it
//1 page below user stack
#define USER_SIGNAL_STACK_TOP  USER_STACK_BASE
#define USER_SIGNAL_STACK_BASE (USER_SIGNAL_STACK_TOP - 1 * PAGE_SIZE)
// mmap user region
// keep it below normal user stack / signal stack
#define USER_MMAP_TOP  USER_SIGNAL_STACK_BASE
#define USER_MMAP_BASE 0x1000000000UL
// mmap prot flags from lab spec
#define MMAP_PROT_NONE  0
#define MMAP_PROT_READ  1
#define MMAP_PROT_WRITE 2
#define MMAP_PROT_EXEC  4
// mmap flags from lab spec
#define MAP_ANONYMOUS 0x20
#define MAP_POPULATE  0x8000

#define PROT_USER_BASE   (PTE_V | PTE_U | PTE_A | PTE_D)
#define PROT_USER_RX     (PROT_USER_BASE | PTE_R | PTE_X)
#define PROT_USER_RW     (PROT_USER_BASE | PTE_R | PTE_W)
#define PROT_USER_RWX     (PROT_USER_BASE | PTE_R | PTE_W | PTE_X)


#define PROT_KERNEL  (PTE_V | PTE_R | PTE_W | PTE_X | PTE_G | PTE_A | PTE_D)
#define PROT_MMIO    (PTE_V | PTE_R | PTE_W | PTE_G | PTE_A | PTE_D)

#define MAX_EARLY_PTE_TABLES 64
//Mode sv39 is 8, mode bit is 63:60
#define SATP_SV39    (8UL << 60)
//PPN should have 12 bits of 0 (offset)
#define MAKE_SATP(pgd_pa) \
    (SATP_SV39 | ((unsigned long)(pgd_pa) >> 12))
//9:0 is flag bits
#define MAKE_PTE(pa, flags) \
    ((((unsigned long)(pa)) >> 12) << 10 | (flags))

static inline unsigned long phys_to_virt(unsigned long pa)
{
    return pa + PAGE_OFFSET;
}

static inline unsigned long virt_to_phys(unsigned long va)
{
    if (va >= PAGE_OFFSET)//before MMU enabled, it will just return the same value
        return va - PAGE_OFFSET;
    return va;
}

void setup_vm(unsigned long dtb_pa);
void drop_identity_map(void);
unsigned long *kernel_pgd(void);
unsigned long *create_user_pgd(void);
void map_pages(unsigned long *root,
               unsigned long va,
               unsigned long size,
               unsigned long pa,
               unsigned long prot);
void switch_pgd(unsigned long *next_pgd);
void free_user_pgd(unsigned long *root);
unsigned long pte_to_pa(unsigned long pte);
unsigned long *get_pte(unsigned long *root, unsigned long va);
int cow_copy_user_pagetable(unsigned long *parent_pgd,
                            unsigned long *child_pgd);
#endif
