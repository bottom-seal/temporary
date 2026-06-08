#ifndef __MMAP_H__
#define __MMAP_H__

#include "thread.h"

unsigned long sys_mmap(void *addr,
                       unsigned long length,
                       int prot,
                       int flags);

int copy_mmap_regions(struct task_struct *parent,
                      struct task_struct *child);
//adv 2
int mmap_handle_page_fault(struct pt_regs *regs);
int add_user_region(struct task_struct *task,
                    unsigned long start,
                    unsigned long length,
                    int prot,
                    int flags,
                    enum vm_region_type type,
                    unsigned long backing,
                    unsigned long file_size);

#endif
