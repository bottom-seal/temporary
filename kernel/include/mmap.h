#ifndef __MMAP_H__
#define __MMAP_H__

#include "thread.h"

unsigned long sys_mmap(void *addr,
                       unsigned long length,
                       int prot,
                       int flags);

int copy_mmap_regions(struct task_struct *parent,
                      struct task_struct *child);

#endif