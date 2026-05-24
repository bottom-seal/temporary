#include "syscall.h"
#include "trap.h"
#include "uart.h"
#include "str.h"
#include "dt_parse.h"
#include "timer.h"
#include "thread.h"
#include "page_alloc.h"
#include "framebuffer.h"
#include "exec.h"
#include "signal.h"
#include "vm.h"

#define SYS_GETPID     0
#define SYS_UART_READ  1
#define SYS_UART_WRITE 2
#define SYS_EXEC       3
#define SYS_FORK       4
#define SYS_WAITPID    5
#define SYS_EXIT       6
#define SYS_STOP       7
#define SYS_DISPLAY    8
#define SYS_USLEEP     9
#define SYS_SIGNAL 10
#define SYS_SIGRETURN 11
#define SYS_KILL 12

static void wake_sleeping_task(void *arg) {
    thread_wake((struct task_struct *)arg);
}

void handle_syscall(struct pt_regs *regs) {
    //regs is saved user context iin trap frame 
    unsigned long num = regs->a7;
    //system call always execute next instruction on return
    regs->sepc += 4;
    //return pid
    if (num == SYS_GETPID) {
        regs->a0 = get_current()->pid;
    }
    //
    else if (num == SYS_UART_READ) {
        char *buf = (char *)regs->a0;   // user buffer
        long count = (long)regs->a1;    // number of bytes to read
        long i;

        // invalid count
        if (count < 0) {
            regs->a0 = (unsigned long)-1;
            return;
        }

        // reading 0 bytes is valid
        if (count == 0) {
            regs->a0 = 0;
            return;
        }

        // invalid buffer
        if (!buf) {
            regs->a0 = (unsigned long)-1;
            return;
        }

        /*
        * uart_getc() may block and call schedule().
        * Syscall entry disables interrupt, so enable interrupt while waiting
        * for UART input. Otherwise RX interrupt may never wake us.
        */
        irq_enable();

        for (i = 0; i < count; i++) {
            buf[i] = uart_getc();
        }

        irq_disable();

        // return number of bytes read
        regs->a0 = (unsigned long)i;
    }
    else if (num == SYS_UART_WRITE) {
        const char *buf = (const char *)regs->a0;
        long count = (long)regs->a1;
        long i;

        if (count < 0) {
            regs->a0 = (unsigned long)-1;
            return;
        }

        if (count == 0) {
            regs->a0 = 0;
            return;
        }

        if (!buf) {
            regs->a0 = (unsigned long)-1;
            return;
        }
        // only put to software buffer, no need for interrupt(if not full)
        for (i = 0; i < count; i++)
            uart_putc(buf[i]);

        uart_flush_tx();
        regs->a0 = (unsigned long)i;
    }
    else if (num == SYS_EXEC) {
        //Replace the current user process's program with a new program from initramfs.
        //Keep the same task_struct / pid / kernel stack.
        //Reset the user stack.
        //When returning from the syscall, jump into the new program instead of old program.
        uintptr_t initrd_start = get_initrd_start((const void *)phys_to_virt(boot_dtb_pa));
        const char *path = (const char *)regs->a0;
        unsigned long new_base;
        unsigned long new_size;
        struct user_image *new_image;
        struct user_image *old_image;
        struct task_struct *current = get_current();

        if (!current || current->type != TASK_USER || !path) {
            regs->a0 = (unsigned long)-1;//return
            return;
        }

        if (!initrd_start) {
            regs->a0 = (unsigned long)-1;//return
            return;
        }
        //gets program base, size
        if (initrd_load_program((const void *)initrd_start,
                                path,
                                &new_base,
                                &new_size) != 0) {
            regs->a0 = (unsigned long)-1;//return
            return;
        }
        //user image structure for task_struct
        new_image = (struct user_image *)allocate(sizeof(struct user_image));
        if (!new_image) {
            free((void *)new_base);
            regs->a0 = (unsigned long)-1;//return
            return;
        }
        
        new_image->base = new_base;
        new_image->size = new_size;
        new_image->refcount = 1;
        //save old image
        old_image = current->image;
        //replace thread field  
        current->image = new_image;
        current->user_program_base = new_base;
        current->user_program_size = new_size;
        //old program might be shared, this thread no longer runs old program, refcount--;
        if (old_image) {
            old_image->refcount--;
            if (old_image->refcount == 0) {//free if no one uses old program
                free((void *)old_image->base);
                free(old_image);
            }
        }
        //to run new program, should use clean stack
        if (current->user_stack_base)
            memset((void *)current->user_stack_base, 0, STACK_SIZE);
        //sp back to start of stack                                                                                               
        current->user_sp = current->user_stack_base + STACK_SIZE;

        //Do not return to the old program after exec().
        //Return from trap into the new program entry.
        regs->sepc = current->user_program_base;
        regs->sp = current->user_sp;
        regs->tp = (unsigned long)current;
        regs->a0 = 0;
    }
    else if (num == SYS_FORK) {
        //Create a new child user task.
        //The child continues from the same user instruction as the parent.
        //Parent sees fork() return child pid.
        //Child sees fork() return 0
        struct task_struct *parent = get_current();//caller of fork()
        struct task_struct *child;
        struct pt_regs *child_regs;
        unsigned long parent_stack_top;
        unsigned long used;
        //only user thread with user stack and program should run (system call)
        if (!parent || parent->type != TASK_USER ||
            !parent->user_stack_base || !parent->image) {
            regs->a0 = (unsigned long)-1;
            return;
        }
        
        child = uthread_create(parent->user_program_base,
                               parent->user_program_size);
        if (!child) {
            regs->a0 = (unsigned long)-1;
            return;
        }
        //advance part: child inherit handler
        for (int i = 0; i < MAX_SIGNAL; i++)
            child->signal_handler[i] = parent->signal_handler[i];

        child->pending_signal = 0;
        child->handling_signal = 0;
        child->signal_stack_base = 0;
        //copy the user program metadata that parent runs
        if (child->image) {
            free(child->image);
            child->image = parent->image;
            child->image->refcount++;//1 more ref
            child->user_program_base = child->image->base;
            child->user_program_size = child->image->size;
        }
        //copy whole stack for continue execution 
        memcpy((void *)child->user_stack_base,
               (void *)parent->user_stack_base,
               STACK_SIZE);

        parent_stack_top = parent->user_stack_base + STACK_SIZE;
        if (regs->sp <= parent_stack_top &&
            regs->sp >= parent->user_stack_base) {
            used = parent_stack_top - regs->sp;//stack grows down
            child->user_sp = child->user_stack_base + STACK_SIZE - used;
        } else {//fallback, doesn't happen
            child->user_sp = child->user_stack_base + STACK_SIZE;
        }

        if (setup_user_context(child) != 0) {//prepare trampoline
            regs->a0 = (unsigned long)-1;
            return;
        }

        child_regs = (struct pt_regs *)child->thread.sp;//copies parent's trap frame, gets same user mode state as parent: sepc, 
        memcpy(child_regs, regs, sizeof(struct pt_regs));
        //those registers is different for child
        child_regs->sp = child->user_sp;//user stack pointer
        child_regs->tp = (unsigned long)child;//child's current task current pointer is itself
        child_regs->a0 = 0;//return 0 on success for child, child see returned pid = 0

        thread_wake(child);//changes the child from waiting/not-runnable to runnable and puts it into the run queue
        //because user thread must set up context before adding to running queue
        regs->a0 = (unsigned long)child->pid;//parent return value, child pid 
    }
    else if (num == SYS_WAITPID) {
        //a0 was pid to wait for
        //if child is zombie -> free child, return child pid
        //if child is running -> current parent becomes THREAD_WAITING -> schedule() -> later child exits and wakes parent ->  parent resumes and re-checks child
        regs->a0 = (unsigned long)process_waitpid((long)regs->a0);
    }
    else if (num == SYS_EXIT) {
        //a0 exit status
        //doesnt set regs->a0, because exit will not return to user mode
        thread_exit_status((int)regs->a0);
        ///orphan current's children -> mark current as THREAD_ZOMBIE, add queue -> wake parent if parent is waiting  -> schedule
    }
    else if (num == SYS_STOP) {
        //a0 is pid of thread to stop, kills another process
        regs->a0 = (unsigned long)process_stop((long)regs->a0);
        //pid thread becomes zombie, return 0 on success
        //return -1 on fail (already zombie, kernel thread)
    }
    else if (num == SYS_DISPLAY) {
        unsigned int *bmp_image = (unsigned int *)regs->a0;//pointer to 1D pixel array
        unsigned int width = (unsigned int)regs->a1;//2nd argument
        unsigned int height = (unsigned int)regs->a2;//3rd
        //0 on success, -1 on fail
        framebuffer_display(bmp_image, width, height);
        //scheduler should handle
        //schedule();
    }
    else if (num == SYS_USLEEP) {
        //Put the current user task to sleep for us
        struct task_struct *current = get_current();
        unsigned int usec = (unsigned int)regs->a0;//a0 is us
        unsigned long flags;

        if (!current || current->type != TASK_USER) {//only valid user thread
            regs->a0 = (unsigned long)-1;
            return;
        }

        /*
        * Protect state change + timer insertion.
        * Otherwise a very short timer may fire before current becomes WAITING,
        * causing the wakeup to be missed.
        */
        flags = irq_save();

        current->state = THREAD_WAITING;//change self to waiting

        //executing thread should not be in, for safety
        if (!list_empty(&current->run_list)) {
            list_del(&current->run_list);
            INIT_LIST_HEAD(&current->run_list);
        }

        if (add_timer_us(wake_sleeping_task, current, usec) != 0) {//next timer interrupt calls wake_sleeping_task
            current->state = THREAD_RUNNING;
            irq_restore(flags);
            regs->a0 = (unsigned long)-1;
            return;
        }

        schedule();

        irq_restore(flags);

        regs->a0 = 0;//on success
    }
    else if (num == SYS_SIGNAL) {
        // a0 = signum, a1 = handler
        regs->a0 = (unsigned long)sys_signal((int)regs->a0, regs->a1);
    }
    else if (num == SYS_SIGRETURN) {
        sys_sigreturn(regs);
    }
    else if (num == SYS_KILL) {
        int ret = process_kill((int)regs->a0, (int)regs->a1);
        regs->a0 = (unsigned long)(long)ret;
    }
}
