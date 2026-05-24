#include "signal.h"
#include "thread.h"
#include "trap.h"
#include "page_alloc.h"
#include "uart.h"
#include "str.h"

#define SYS_SIGRETURN 11

 //trampoline
 /*
 *     addi a7, zero, 11
 *     ecall
 */
extern char __sigreturn_trampoline_start[];
extern char __sigreturn_trampoline_end[];

#define SSTATUS_SPP (1UL << 8)//Supervisor Previous Privilege, what was previous mode that entered trap routine, only user mode should run handler

static int pick_pending_signal(unsigned long pending_signal) {
    for (int signum = 1; signum < MAX_SIGNAL; signum++) {//0 is invalid, start from 1
        // find the smallest pending signal number, 
        if (pending_signal & (1UL << signum))//if both 1, pending signal is bitmask
            return signum;
    }

    return -1;
}
//a page is allocated before this function call, we allocate a page, put user code from low addr, put stack in high addr
//so user function doesn't jump to kernel text (assembly function)
static unsigned long install_sigreturn_trampoline(unsigned long stack_base) {
    unsigned long trampoline_addr = stack_base;
    unsigned long trampoline_size =
        (unsigned long)__sigreturn_trampoline_end -
        (unsigned long)__sigreturn_trampoline_start;

    memcpy((void *)trampoline_addr,
           __sigreturn_trampoline_start,
           trampoline_size);

    //We copied instructions into memory with data path
    //instruction-fetch path may have already cached, prefetched, make sure Icache or pipeline state gets newest data
    asm volatile("fence.i" ::: "memory");

    return trampoline_addr;
}

long sys_signal(int signum, unsigned long handler) {
    struct task_struct *current = get_current();
    unsigned long old_handler;
    //only user thread
    if (!current || current->type != TASK_USER)
        return -1;
    //signal 0 is invalid
    if (signum <= 0 || signum >= MAX_SIGNAL)
        return -1;
    //don't need to care for old_hnalder
    old_handler = current->signal_handler[signum];
    //register addr of the handler to signal_handler[signum]
    current->signal_handler[signum] = handler;

    return old_handler;
}
//restore user context after handler finishes, called by trampoline after handler finishes
void sys_sigreturn(struct pt_regs *regs) {
    struct task_struct *current = get_current();
    unsigned long signal_stack_base;
    //only user thread should call
    if (!regs || !current || current->type != TASK_USER)
        return;
    //only if thread is inside signal handler, should only call it from the trampoline
    if (!current->handling_signal)
        return;
    //print something
    uart_puts("sigreturn\n");
    //record allocated addr for later free
    signal_stack_base = current->signal_stack_base;
    //task has done handling signal, clear fields
    current->handling_signal = 0;
    current->signal_stack_base = 0;
    //regs is the trap frame created by the trampoline’s ecall 11. It represents the state after the handler finished
    //now repalce it with trap frame before entering handler
    *regs = current->saved_signal_regs;
    //sp is restored after this, safely free signal stack
    if (signal_stack_base)
        free((void *)signal_stack_base);
}

int sys_kill(int pid, int signum) {
    return process_kill(pid, signum);
}
//decide if it should run handler before going back to user mode
void handle_signal_if_needed(struct pt_regs *regs) {//passed sp, points to the trap frame that would be restored
    struct task_struct *current = get_current();
    int signum;
    unsigned long trampoline_addr;
    unsigned long handler;
    unsigned long signal_stack_base;
    unsigned long signal_stack_top;
    //no trap frame or thread
    if (!regs || !current)
        return;


    //Only user tasks have user-space signal handlers.
 
    if (current->type != TASK_USER)
        return;

    //only deliver signals when this trap frame is about to return to U-mode. if SPP == 1, this frame returns to S-mode, so do nothing.
    if (regs->sstatus & SSTATUS_SPP)
        return;

    //avoid nested handler
    if (current->handling_signal)
        return;
    
    //if not pending set by kill(), should do nothing
    if (!current->pending_signal)
        return;

    //get the smallest pending signal
    signum = pick_pending_signal(current->pending_signal);
    if (signum < 0)
        return;

    //set the signal bit in bitmask to 0, already handled
    current->pending_signal &= ~(1UL << signum);

    //stores the addr of the handler function
    handler = current->signal_handler[signum];

    //if not registered, default to stop()
    if (!handler) {
        thread_exit_status(0);
        return;
    }

    //allocate a page to put trampoline code from low addr, and use top as stack
    signal_stack_base = (unsigned long)allocate(STACK_SIZE);
    if (!signal_stack_base) {
        //if cannot create stack, safe fallback is to kill the thread
        thread_exit_status(0);
        return;
    }

    //initialize to 0
    memset((void *)signal_stack_base, 0, STACK_SIZE);

    //put the trampoline code at the low addr of allocated page
    trampoline_addr = install_sigreturn_trampoline(signal_stack_base);//copies the trampoline and run fence

    signal_stack_top = signal_stack_base + STACK_SIZE;

    //align top down to 16 bit aligned, so each saved registers are aligned
    signal_stack_top &= ~0xFUL;

    //copy whole user trap frame (before handler)
    current->saved_signal_regs = *regs;
    //to avoid nested handling
    current->handling_signal = 1;
    //to run free later
    current->signal_stack_base = signal_stack_base;

    /*
     * Rewrite the trap frame.
     *
     * After ret_from_exception restores this frame and executes sret:
     *
     *     pc = handler
     *     sp = signal_stack_top
     *     ra = trampoline
     *     a0 = signum
     *
     * So the user handler runs in U-mode. When it returns, it jumps to
     * trampoline, which calls sigreturn().
     */
    regs->sepc = handler;//first return: sret sees this so it enters handler routine
    regs->sp = signal_stack_top;
    regs->ra = trampoline_addr;//second return: when handler (normal c function) ends, it execute ret, ret jumps to ra
    //trampoline just calls sigreturn, in that function we restore saved trap frame (before handler)
    //3rd return, restores orignal user context before handler, so it resume to orignal user instructions
    regs->a0 = signum;//maybe user function need this

    //for safety
    regs->tp = (unsigned long)current;
}