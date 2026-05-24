#ifndef _SIGNAL_H
#define _SIGNAL_H

#include "trap.h"
#include "thread.h"

#define MAX_SIGNAL 32
#define SIGTERM 15

long sys_signal(int signum, unsigned long handler);
void sys_sigreturn(struct pt_regs *regs);
int  sys_kill(int pid, int signum);
void handle_signal_if_needed(struct pt_regs *regs);

#endif