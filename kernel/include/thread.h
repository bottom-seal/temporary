#ifndef _THREAD_H
#define _THREAD_H

#include "list.h"
#include "trap.h"

#define STACK_SIZE 0x8000
#define MAX_SIGNAL 32

enum thread_state {
    THREAD_RUNNING,
    THREAD_RUNNABLE,
    THREAD_WAITING,
    THREAD_ZOMBIE,
};

enum task_type {
    TASK_KERNEL,
    TASK_USER,
};

struct user_image {
    unsigned long base; // demand paging: now points to initrd, not loaded kernel VA
    unsigned long size;
    int refcount;
    //lab 6
    int owned;            // 0 = do not free base
};

enum vm_region_type {
    VM_REGION_PROGRAM,
    VM_REGION_ANON,
    VM_REGION_STACK,
    VM_REGION_SIGNAL,
};

struct mmap_region {
    unsigned long start;      // user VA
    unsigned long length;     // rounded up to PAGE_SIZE

    unsigned long backing;    // kernel VA backing; only meaningful for program image
    unsigned long file_size;  // original program size for partial final page
    unsigned long *pages;     // kernel VAs for pages already allocated/mapped

    int prot;
    int flags;

    enum vm_region_type type;

    struct list_head list;
};

//Caller-saved registers: If the caller still needs this register after the call, the caller must save it before calling.
//Callee-saved registers: If the callee uses this register, the callee must restore its old value before returning.

//switch_to(prev, next) does
//prev->thread.ra
//prev->thread.sp
//prev->thread.s0 ~ prev->thread.s11
struct task_struct {
    struct thread_struct {//saved CPU state, enough registers to recover from context switch
        // Keep this layout in sync with switch_to() in boot/start.S.
        unsigned long ra;//return addr, where to continue when switched back
        //ra should contain addr after switch_to() call, when calls ret, it jumps to addr in ra
        unsigned long sp;//thread's own stack pointer
        //callee-saved registers, if a function uses them, it must restore them before returning
        unsigned long s[12];//callee-saved registers, if a function uses s0-s11, it must preserve their values before returning
    } thread;

    int pid;//thread id
    enum thread_state state;//scheduler uses this to know if the thread can run or should be recycled
    enum task_type type;
    void (*entry)(void);//function passed to kthread_create(), called when the thread first runs


    unsigned long kernel_stack_base;//the base address of the memory allocated for this thread's stack
    unsigned long kernel_sp;//from exercise

    unsigned long user_stack_base;//kernel VA of physical backing page
    unsigned long user_sp;//user VA, should be 0x4000000000

    //for lab6 
    unsigned long *pgd;//process page table root
    unsigned long user_program_base;//user VA, should be 0x0
    unsigned long user_program_size;
    struct user_image *image;//kernel VA of program backing memory

    struct task_struct *parent;
    int wait_pid;
    int exit_status;

    // Nodes for list.h queues. The queue heads live in thread.c.
    struct list_head all_tasks_list;
    struct list_head run_list;
    struct list_head zombie_list;
    struct list_head child_list;//head for the thread's child list
    struct list_head sibling_list;//used as node to connect to parent's list, with other siblings

    // for advanced part
    unsigned long signal_handler[MAX_SIGNAL]; // handler address for each signal, eg signal(2, handler); current->signal_handler[2] = (unsigned long)handler;
    unsigned long pending_signal;             // bitmask of pending signals, eg kill(pid, signum); target->pending_signal |= (1UL << signum); later check current->pending_signal &= ~(1UL << signum);

    int handling_signal;                      // avoid nested signal handler, if 1 don't enter handler
    struct pt_regs saved_signal_regs;         // original user context, because we need to overwrite fields to jump to handler, and handler would save new regs
    unsigned long signal_stack_base;          // temporary signal stack

    //
    struct list_head mmap_list;//head of the linked list of mmap regions
    unsigned long mmap_next;//help with request addr = NULL, need to find next available addr
};

struct task_struct *get_current(void);
void thread_init(void);
struct task_struct *kthread_create(void (*entry)(void));
struct task_struct *uthread_create(unsigned long user_pc,
                                   unsigned long user_program_size);
int setup_user_context(struct task_struct *task);
int thread_wake(struct task_struct *task);
int thread_wait(struct task_struct *child);
void thread_exit_status(int status);
long process_waitpid(long pid);
void schedule(void);
void thread_exit(void);
void kill_zombies(void);
void idle(void);
void foo(void);
void thread_test(void);
long process_stop(long pid);

int  process_kill(int  pid, int signum);
//lab6
void thread_destroy(struct task_struct *task);
void free_mmap_regions(struct task_struct *task);
#endif
