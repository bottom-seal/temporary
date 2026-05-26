#include "mmap.h"
#include "vm.h"
#include "thread.h"
#include "page_alloc.h"
#include "uart.h"
#include "list.h"
#include "str.h"


#define ALIGN_UP(x, a) (((x) + (a) - 1) & ~((a) - 1))//only if a = 2^n

static int is_page_aligned(unsigned long addr)
{
    return (addr & (PAGE_SIZE - 1)) == 0;//check if is page aligned, no change value
}

static unsigned long mmap_prot_to_pte(int prot)//prot can be combined, like PROT_READ | PROT_WRITE = 1 | 2 = 3
{
    unsigned long pte = PROT_USER_BASE;//minumum flag bits for user page
    //prot can be combined, so need to check each bit with &
    if (prot & MMAP_PROT_READ)
        pte |= PTE_R;

    if (prot & MMAP_PROT_WRITE)
        pte |= PTE_R | PTE_W;//write permission must come with read permission in RSICV

    if (prot & MMAP_PROT_EXEC)
        pte |= PTE_R | PTE_X;//follows definition in vm.h

    return pte;
}

static int range_overlap(unsigned long a_start, unsigned long a_end,
                         unsigned long b_start, unsigned long b_end)
{
    return a_start < b_end && b_start < a_end;//!(a_end <= b_start || b_end <= a_start)
}

//check if placing region at [start, end) overlaps with anything
//return 0 if no overlap
static int mmap_region_overlap(struct task_struct *task,
                               unsigned long start,
                               unsigned long end)
{
    struct list_head *curr;

    if (!task)
        return 1;

    // overlap with user program
    if (range_overlap(start, end,
                      USER_CODE_BASE,
                      USER_CODE_BASE + ALIGN_UP(task->user_program_size, PAGE_SIZE)))
        return 1;

    // overlap with normal user stack
    if (range_overlap(start, end, USER_STACK_BASE, USER_STACK_TOP))
        return 1;

    // overlap with signal stack
    if (range_overlap(start, end, USER_SIGNAL_STACK_BASE, USER_SIGNAL_STACK_TOP))
        return 1;

    // overlap with existing mmap regions
    for (curr = task->mmap_list.next; curr != &task->mmap_list; curr = curr->next) {//mmap_list is the list head
        struct mmap_region *region =
            list_entry(curr, struct mmap_region, list);//get the struct for the node 

        if (range_overlap(start, end,
                          region->start,
                          region->start + region->length))
            return 1;
    }

    return 0;
}

static unsigned long find_free_mmap_area_from(struct task_struct *task,
                                              unsigned long start,
                                              unsigned long length)
{
    unsigned long addr;
    unsigned long begin;

    if (!task)
        return 0;

    begin = ALIGN_UP(start, PAGE_SIZE);
    //must be in MMAP range, wrap around might exceed the range
    if (begin < USER_MMAP_BASE)
        begin = USER_MMAP_BASE;

    if (begin >= USER_MMAP_TOP)
        begin = ALIGN_UP(USER_MMAP_BASE, PAGE_SIZE);
    //we use saved likely next available 
    //first pass: [begin, USER_MMAP_TOP)
    for (addr = begin; addr + length <= USER_MMAP_TOP; addr += PAGE_SIZE) {
        if (!mmap_region_overlap(task, addr, addr + length))
            return addr;
    }

    //second pass: [USER_MMAP_BASE, begin)
    for (addr = ALIGN_UP(USER_MMAP_BASE, PAGE_SIZE);
         addr < begin && addr + length <= begin;
         addr += PAGE_SIZE) {
        if (!mmap_region_overlap(task, addr, addr + length))
            return addr;
    }

    return 0;
}


//return 0 on fail, return addr on success
static unsigned long find_free_mmap_area(struct task_struct *task,
                                         unsigned long length)
{
    unsigned long addr;

    addr = find_free_mmap_area_from(task, task->mmap_next, length);
    if (addr)
        task->mmap_next = addr + length;

    return addr;
}

static unsigned long find_free_mmap_area_hint(struct task_struct *task,
                                              unsigned long hint,
                                              unsigned long length)
{
    return find_free_mmap_area_from(task, hint, length);
}
//without COW, child should inherit paren't addr space, need to handle extra allocated memory
int copy_mmap_regions(struct task_struct *parent,
                             struct task_struct *child)
{
    struct list_head *curr;

    if (!parent || !child)
        return -1;

    child->mmap_next = parent->mmap_next;//next possible available addr

    for (curr = parent->mmap_list.next;//for parent's mmap list
         curr != &parent->mmap_list;
         curr = curr->next) {
        struct mmap_region *src =
            list_entry(curr, struct mmap_region, list);

        struct mmap_region *dst;
        unsigned long new_backing;
        unsigned long pte_flags;

        new_backing = (unsigned long)allocate(src->length);//allocate the same region as parent
        if (!new_backing) {
            free_mmap_regions(child);//maybe allocated some regions before fail
            return -1;
        }

        //copy parent's region to child's region
        memset((void *)new_backing, 0, src->length);
        memcpy((void *)new_backing,
               (void *)src->backing,
               src->length);
        //allocate for structure
        dst = (struct mmap_region *)allocate(sizeof(struct mmap_region));
        if (!dst) {
            free((void *)new_backing);
            free_mmap_regions(child);
            return -1;
        }

        pte_flags = mmap_prot_to_pte(src->prot);//mmap_region->prot is user request value, not PTE, need translation

        //make child pgd
        map_pages(child->pgd,
                  src->start,
                  src->length,
                  virt_to_phys(new_backing),//new_backing is kernel VA
                  pte_flags);

        //copy parent struct, backing is new allocated kernel VA
        dst->start = src->start;
        dst->length = src->length;
        dst->backing = new_backing;
        dst->prot = src->prot;
        dst->flags = src->flags;
        INIT_LIST_HEAD(&dst->list);

        list_add_tail(&dst->list, &child->mmap_list);//add to child list
    }

    return 0;
}

unsigned long sys_mmap(void *addr,//user hinted addr, put user VA here if can
                              unsigned long length,
                              int prot,//prot, could be combination
                              int flags)
{
    struct task_struct *current = get_current();
    struct mmap_region *region;
    unsigned long user_va;
    unsigned long backing;
    unsigned long pte_flags;

    if (!current || current->type != TASK_USER)
        return (unsigned long)-1;

    if (length == 0)
        return (unsigned long)-1;

    // anonymous only for now.
    if (!(flags &  MAP_ANONYMOUS)) 
        return (unsigned long)-1;

    length = ALIGN_UP(length, PAGE_SIZE);//align to page

    // For Advanced 1, easiest design:
    // always allocate immediately, even if MAP_POPULATE is not set.
    // Demand paging can be added in Advanced 2.
    //if user requested addr is not NULL
    if (addr) {
        unsigned long hint = (unsigned long)addr;
        //if addr is available
        if (is_page_aligned(hint) &&
            !mmap_region_overlap(current, hint, hint + length)) {
            user_va = hint;
        } else {//use addr as hint
            user_va = find_free_mmap_area_hint(current, hint, length);
        }
    } else {//NULL path
        user_va = find_free_mmap_area(current, length);
    }

    // PROT_NONE is not useful before demand paging.
    // For now, reject it.
    if (prot == MMAP_PROT_NONE)
        return (unsigned long)-1;

    pte_flags = mmap_prot_to_pte(prot);

    backing = (unsigned long)allocate(length);//kernel VA for allocated memory
    if (!backing)
        return (unsigned long)-1;

    memset((void *)backing, 0, length);//init 0

    region = (struct mmap_region *)allocate(sizeof(struct mmap_region));//allocate for mmap struct
    if (!region) {
        free((void *)backing);
        return (unsigned long)-1;
    }
    //map the va to the backing PA( accessed by kernel VA)
    map_pages(current->pgd,
              user_va,
              length,
              virt_to_phys(backing),
              pte_flags);

    region->start = user_va;
    region->length = length;
    region->backing = backing;
    region->prot = prot;
    region->flags = flags;
    INIT_LIST_HEAD(&region->list);
    //add to user process's mmap list
    list_add_tail(&region->list, &current->mmap_list);

    uart_puts("[mmap] va=");
    uart_hex(user_va);
    uart_puts(" len=");
    uart_hex(length);
    uart_puts(" prot=");
    uart_hex((unsigned long)prot);
    uart_puts(" flags=");
    uart_hex((unsigned long)flags);
    uart_puts(" backing=");
    uart_hex(backing);
    uart_puts("\n");

    return user_va;//return user VA for the allocated mmap
}
