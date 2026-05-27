#include "mmap.h"
#include "vm.h"
#include "thread.h"
#include "page_alloc.h"
#include "uart.h"
#include "list.h"
#include "str.h"


#define ALIGN_UP(x, a) (((x) + (a) - 1) & ~((a) - 1))//only if a = 2^n
#define ALIGN_DOWN(x, a) ((x) & ~((a) - 1))

//adv part 2

//given a user VA, return which user region for the thread contains it
static struct vm_region *find_mmap_region(struct task_struct *task,
                                            unsigned long addr)
{
    struct list_head *curr;

    if (!task)
        return 0;
    //loop through user vm regions
    for (curr = task->vm_regions.next;
         curr != &task->vm_regions;
         curr = curr->next) {
        struct vm_region *region =
            list_entry(curr, struct vm_region, list);
        //check which region the target addr is in
        if (addr >= region->start &&
            addr < region->start + region->length)
            return region;
    }

    return 0;
}
//page fault happen for different reason
//input, a region struct that records prot, return 0 means illegal, 1 means legal
static int mmap_access_allowed(struct vm_region *region,
                               unsigned long scause_code)
{
    if (!region)
        return 0;

    if (region->prot == MMAP_PROT_NONE)
        return 0;

    //is instruction page fault, and region is executable
    if (scause_code == 12)
        return region->prot & MMAP_PROT_EXEC;

    //is load page fault, and region is readable
    if (scause_code == 13)
        return region->prot & MMAP_PROT_READ;

    //is store/AMO page fault, and region is writable
    if (scause_code == 15)
        return region->prot & MMAP_PROT_WRITE;

    return 0;
}

//adv part 1
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
//return 0 if no overlap, 1 if overlap
static int mmap_region_overlap(struct task_struct *task,
                               unsigned long start,
                               unsigned long end)
{
    struct list_head *curr;

    if (!task)
        return 1;
    // overlap with existing mmap regions
    for (curr = task->vm_regions.next; curr != &task->vm_regions; curr = curr->next) {//vm_regions is the list head
        struct vm_region *region =
            list_entry(curr, struct vm_region, list);//get the struct for the node

        if (range_overlap(start, end,
                          region->start,
                          region->start + region->length))
            return 1;
    }

    return 0;
}

//start searching from start to MMAP_TOP, then search from MMAP_BASE to start
//avoid holes before start
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


//find from recorded possible addr, if user requested NULL
static unsigned long find_free_mmap_area(struct task_struct *task,
                                         unsigned long length)
{
    unsigned long addr;

    addr = find_free_mmap_area_from(task, task->mmap_next, length);
    if (addr)
        task->mmap_next = addr + length;

    return addr;
}
//find from user requested addr
static unsigned long find_free_mmap_area_hint(struct task_struct *task,
                                              unsigned long hint,
                                              unsigned long length)
{
    return find_free_mmap_area_from(task, hint, length);//start from requested addr
}
//wait until COW
int copy_mmap_regions(struct task_struct *parent,
                             struct task_struct *child)
{
    struct list_head *curr;

    if (!parent || !child)
        return -1;

    child->mmap_next = parent->mmap_next;//next possible available addr

    for (curr = parent->vm_regions.next;//for parent's mmap list
         curr != &parent->vm_regions;
         curr = curr->next) {
        struct vm_region *src =
            list_entry(curr, struct vm_region, list);

        struct vm_region *dst;
        unsigned long nr_pages;

        //allocate for structure
        dst = (struct vm_region *)allocate(sizeof(struct vm_region));
        if (!dst) {
            free_mmap_regions(child);
            return -1;
        }

        //copy parent struct, backing is new allocated kernel VA
        dst->start = src->start;
        dst->length = src->length;
        dst->backing = src->backing;
        dst->file_size = src->file_size;
        dst->prot = src->prot;
        dst->flags = src->flags;
        dst->type = src->type;
        dst->pages = 0;
        INIT_LIST_HEAD(&dst->list);

        nr_pages = src->length / PAGE_SIZE;

        dst->pages = (unsigned long *)allocate(nr_pages * sizeof(unsigned long));
        if (!dst->pages) {
            free(dst);
            free_mmap_regions(child);
            return -1;
        }

        memset(dst->pages, 0, nr_pages * sizeof(unsigned long));

        for (unsigned long i = 0; i < nr_pages; i++) {
            if (!src->pages[i])
                continue;

            if (retain_page((void *)src->pages[i]) < 0) {
                for (unsigned long j = 0; j < i; j++) {
                    if (dst->pages[j])
                        free((void *)dst->pages[j]);
                }

                free(dst->pages);
                free(dst);
                free_mmap_regions(child);
                return -1;
            }

            dst->pages[i] = src->pages[i];
        }

        list_add_tail(&dst->list, &child->vm_regions);//add to child list
    }

    return 0;
}
//part 1 allocate full backing memory, map full immediately
//part 2 create region metadata, pages[] array, map immediately if MAP_POPULATE is set
unsigned long sys_mmap(void *addr,//user hinted addr, put user VA here if can
                              unsigned long length,
                              int prot,//prot, could be combination
                              int flags)//maps right away if POPULATE set
{
    struct task_struct *current = get_current();
    struct vm_region *region;
    unsigned long user_va;
    unsigned long nr_pages;
    //check user thread
    if (!current || current->type != TASK_USER)
        return (unsigned long)-1;
    //check range valid
    if (length == 0)
        return (unsigned long)-1;

    if (!(flags & MAP_ANONYMOUS))//doesn't support file backed mapping

        return (unsigned long)-1;

    if (prot & ~(MMAP_PROT_READ | MMAP_PROT_WRITE | MMAP_PROT_EXEC))//only support 3 permission bits
        return (unsigned long)-1;

    length = ALIGN_UP(length, PAGE_SIZE);//align to page

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
    //after found user_va
    if (!user_va)
        return (unsigned long)-1;
    //allocate for vm_region struct
    region = (struct vm_region *)allocate(sizeof(struct vm_region));
    if (!region)
        return (unsigned long)-1;
    //init vm_region
    region->start = user_va;
    region->length = length;
    region->backing = 0;
    region->file_size = 0;
    region->prot = prot;
    region->flags = flags;
    region->type = VM_REGION_ANON;//mmap is always anonymous
    region->pages = 0;
    INIT_LIST_HEAD(&region->list);

    nr_pages = region->length / PAGE_SIZE;// how many pages needed
    //allocate the array to record page : 0 = not allocated, kervel VA = allocated at *
    region->pages = (unsigned long *)allocate(nr_pages * sizeof(unsigned long));
    if (!region->pages) {
        free(region);
        return (unsigned long)-1;
    }
    memset(region->pages, 0, nr_pages * sizeof(unsigned long));

    //if set POPULATE flag
    if ((flags & MAP_POPULATE) && prot != MMAP_PROT_NONE) {
        //translate flags
        unsigned long pte_flags = mmap_prot_to_pte(prot);
        //allocate all pages
        for (unsigned long i = 0; i < nr_pages; i++) {
            unsigned long page = (unsigned long)allocate(PAGE_SIZE);
            //if any allocation fail, free all previously allocated pages
            if (!page) {
                for (unsigned long j = 0; j < i; j++) {
                    if (region->pages[j])
                        free((void *)region->pages[j]);
                }

                free(region->pages);//free allocated array
                free(region);
                return (unsigned long)-1;
            }
            //init allocated 4kb
            memset((void *)page, 0, PAGE_SIZE);
            region->pages[i] = page;//pages array stores kernel VA if allocated & mapped
            //map the page
            map_pages(current->pgd,
                      user_va + i * PAGE_SIZE,
                      PAGE_SIZE,
                      virt_to_phys(page),
                      pte_flags);
        }
        //modifed page table, need to do sfence vma
        asm volatile("sfence.vma zero, zero" ::: "memory");
    }
    list_add_tail(&region->list, &current->vm_regions);

    return user_va;//return user VA for the allocated mmap
}


int mmap_handle_page_fault(struct pt_regs *regs)
{
    struct task_struct *current = get_current();
    struct vm_region *region;
    unsigned long addr;
    unsigned long page_va;
    unsigned long page_idx;
    unsigned long nr_pages;
    unsigned long page;
    unsigned long pte_flags;
    unsigned long code;

    if (!current || current->type != TASK_USER)//only accept user thread
        return -1;

    addr = regs->stval;//stval is the virtual addr that caused the interrupt
    code = regs->scause & 0xfffUL;//scause records interrupt number, only need to check if it is 12, 13, 15, just get lower bits
    page_va = ALIGN_DOWN(addr, PAGE_SIZE);

    region = find_mmap_region(current, addr);//the region in vm_region list, that contains the interrupting VA

    if (!region || !mmap_access_allowed(region, code)) {//not in recorded region OR access not allowed
        uart_puts("[Segmentation fault]: Kill Process\n");
        thread_exit_status(-1);
        return -1;
    }
    //would be caught earlier, just for safety
    nr_pages = region->length / PAGE_SIZE;
    page_idx = (page_va - region->start) / PAGE_SIZE;//index of page in a region, used to check

    if (page_idx >= nr_pages) {//exceeding the range
        uart_puts("[Segmentation fault]: Kill Process\n");
        thread_exit_status(-1);
        return -1;
    }

    if (region->pages[page_idx]) {
        unsigned long *pte;
        unsigned long old_page;
        unsigned long new_page;

        pte = get_pte(current->pgd, page_va);
        if (code != 15 || !pte || !(*pte & PTE_COW) ||
            !(region->prot & MMAP_PROT_WRITE)) {
            uart_puts("[Segmentation fault]: Kill Process\n");
            thread_exit_status(-1);
            return -1;
        }

        old_page = region->pages[page_idx];
        if (page_refcount((void *)old_page) > 1) {
            new_page = (unsigned long)allocate(PAGE_SIZE);
            if (!new_page) {
                uart_puts("[Segmentation fault]: Kill Process\n");
                thread_exit_status(-1);
                return -1;
            }

            memcpy((void *)new_page, (void *)old_page, PAGE_SIZE);
            free((void *)old_page);
            region->pages[page_idx] = new_page;
        } else {
            new_page = old_page;
        }

        pte_flags = mmap_prot_to_pte(region->prot);
        map_pages(current->pgd,
                  page_va,
                  PAGE_SIZE,
                  virt_to_phys(new_page),
                  pte_flags);

        asm volatile("sfence.vma %0, zero" :: "r"(page_va) : "memory");
        uart_puts("[Permission fault]: ");
        uart_hex(addr);
        uart_puts("\n");
        return 0;
    }

    //legal path: allocate a page and maps user VA

    //allocate 4kb page
    page = (unsigned long)allocate(PAGE_SIZE);
    if (!page) {
        uart_puts("[Segmentation fault]: Kill Process\n");
        thread_exit_status(-1);
        return -1;
    }

    memset((void *)page, 0, PAGE_SIZE);
    //if the accessed region is in image region
    //file backed path
    if (region->type == VM_REGION_PROGRAM) {
        unsigned long file_off = page_va - region->start;//offset starting from image base in initrd

        if (file_off < region->file_size) {//should always be inside the file range
            unsigned long copy_len = region->file_size - file_off;
            //we cannot copy a whole page if it contains stuff outside image
            //if copy_len >= a page, allocate and copy that page
            //if copy_len < a page, copy only offset ~ file_top
            if (copy_len > PAGE_SIZE)
                copy_len = PAGE_SIZE;

            memcpy((void *)page,//kernel VA for allocated page
                   (void *)(region->backing + file_off),
                   copy_len);//copy not exceed image
            asm volatile("fence.i" ::: "memory");//instruction modifed, CPU fetch need to see fresh updated content (I-cache can't have stale)
        }
    }
    //pages array element records the kernel VA allocated
    region->pages[page_idx] = page;

    pte_flags = mmap_prot_to_pte(region->prot);//translate requested prot to PTE flag
    //maps 1 page
    map_pages(current->pgd,
              page_va,
              PAGE_SIZE,
              virt_to_phys(page),
              pte_flags);

    asm volatile("sfence.vma %0, zero" :: "r"(page_va) : "memory");//page table updated, need to flush TLB
    //access legal just not mapped: translation fault
    uart_puts("[Translation fault]: ");
    uart_hex(addr);
    uart_puts("\n");

    return 0;
}

//init and add a region structure to task_struct's vm list
int add_user_region(struct task_struct *task,
                    unsigned long start,
                    unsigned long length,
                    int prot,
                    int flags,
                    enum vm_region_type type,
                    unsigned long backing,
                    unsigned long file_size)
{
    struct vm_region *region;

    if (!task || !length)
        return -1;
    //aligned to pages
    length = ALIGN_UP(length, PAGE_SIZE);
    
    if (start & (PAGE_SIZE - 1))//check page aligned, return -1 if lower 3 bit is not 000
        return -1;

    if (prot & ~(MMAP_PROT_READ | MMAP_PROT_WRITE | MMAP_PROT_EXEC))//user only has 3 mode, can be combinational
        return -1;

    region = (struct vm_region *)allocate(sizeof(struct vm_region));//allocate region struct
    if (!region)
        return -1;

    region->start = start;
    region->length = length;
    region->backing = backing;
    region->file_size = file_size;
    region->prot = prot;
    region->flags = flags;
    region->type = type;
    region->pages = (unsigned long *)allocate((length / PAGE_SIZE) *
                                              sizeof(unsigned long));
    if (!region->pages) {
        free(region);
        return -1;
    }
    memset(region->pages, 0, (length / PAGE_SIZE) * sizeof(unsigned long));

    INIT_LIST_HEAD(&region->list);
    list_add_tail(&region->list, &task->vm_regions);

    return 0;
}
