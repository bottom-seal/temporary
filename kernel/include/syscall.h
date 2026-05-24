#ifndef _SYSCALL_H
#define _SYSCALL_H

#include "trap.h"

void handle_syscall(struct pt_regs *regs);

#endif