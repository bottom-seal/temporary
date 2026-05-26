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
#include "mmap.h"

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
#define SYS_MMAP 13

//helper to copy from user page in kernel mode
//bit 18 in the RISC-V sstatus CSR: Supervisor User Memory access
//protects kernel from accidently dereferencing a user pointer, turning SUM up enables accessing temporary
#define SSTATUS_SUM (1UL << 18)

#define ALIGN_UP(x, a) (((x) + (a) - 1) & ~((a) - 1))


static unsigned long user_access_begin(void)
{
    unsigned long old;
    asm volatile("csrrs %0, sstatus, %1"//csr read and set, read sstatus to %0 for restore, set SUM bit to 1.
                 : "=r"(old)
                 : "r"(SSTATUS_SUM)
                 : "memory");
    return old;
}

static void user_access_end(unsigned long old)
{
    asm volatile("csrw sstatus, %0"//write the status before enabling SUM back
                 :
                 : "r"(old)
                 : "memory");
}

static int copy_string_from_user(char *dst, const char *src, unsigned long max_len)
{
    unsigned long old;
    unsigned long i;

    if (!dst || !src || max_len == 0)
        return -1;

    old = user_access_begin();//turn on SUM

    for (i = 0; i < max_len - 1; i++) {//copy from  user string to kpath buffer
        dst[i] = src[i];

        if (dst[i] == '\0') {
            user_access_end(old);//restore sstatus
            return 0;
        }
    }
    //path if exceeding length of 64
    dst[max_len - 1] = '\0';

    user_access_end(old);
    return 0;
}

//For uart 
//read 1 user char with SUM enabled
static char get_user_char(const char *user_ptr)
{
    char c;
    unsigned long old = user_access_begin();

    c = *user_ptr;

    user_access_end(old);
    return c;
}
//write 1 byte char to user VA
static void put_user_char(char *user_ptr, char c)
{
    unsigned long old = user_access_begin();

    *user_ptr = c;

    user_access_end(old);
}

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

        
        //uart_getc() may block and call schedule().
        //Syscall entry disables interrupt, so enable interrupt while waiting
        //for UART input. Otherwise RX interrupt may never wake us.
        irq_enable();

        for (i = 0; i < count; i++) {
            char c = uart_getc();

            //Store into user buffer.
            //buf is a user virtual address.
            //enable SUM to put c in user VA buffer
            put_user_char(&buf[i], c);
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
        {   
            //enable SUM to get char from user VA buffer
            char c = get_user_char(&buf[i]);
            uart_putc(c);
        }

        uart_flush_tx();
        regs->a0 = (unsigned long)i;
    }
    //after VM, progame image base and stack base is all the same (not addr kernel assigned)
    //now should just replace user mappings
    else if (num == SYS_EXEC) {
        //Replace the current user process's program with a new program from initramfs.
        //Keep the same task_struct / pid / kernel stack.
        //Reset the user stack.
        //When returning from the syscall, jump into the new program instead of old program.
        uintptr_t initrd_start = get_initrd_start((const void *)phys_to_virt(boot_dtb_pa));
        //kernel buffer to store *path
        char kpath[64];

        unsigned long new_base;
        unsigned long new_size;
        unsigned long *new_pgd;

        struct user_image *new_image;
        struct user_image *old_image;
        unsigned long *old_pgd;

        struct task_struct *current = get_current();

        if (!current || current->type != TASK_USER) {
            regs->a0 = (unsigned long)-1;
            return;
        }
        //need to enable SUM to get the string in user VA
        if (copy_string_from_user(kpath, (const char *)regs->a0, sizeof(kpath)) < 0) {
            regs->a0 = (unsigned long)-1;
            return;
        }

        if (!initrd_start) {
            regs->a0 = (unsigned long)-1;
            return;
        }
        // gets program base, size
        // new_base = kernel virtual address of the newly loaded program memory
        if (initrd_load_program((const void *)phys_to_virt(initrd_start),
                                kpath,
                                &new_base,//allocator always return VA mapping to PA actually alloated for this program
                                &new_size) != 0) {
            regs->a0 = (unsigned long)-1;
            return;
        }

        // user image structure for task_struct
        new_image = (struct user_image *)allocate(sizeof(struct user_image));
        if (!new_image) {
            free((void *)new_base);
            regs->a0 = (unsigned long)-1;
            return;
        }
        //new_image->base is just VA of the program backing memory, not user entry, entry is now always 0x0
        new_image->base = new_base;
        new_image->size = new_size;
        new_image->refcount = 1;
        //lab 6: exec now also need to create new pgd
        //create user pgd
        new_pgd = create_user_pgd();//copies upper half kernel mapping, leaving bottom half empty
        if (!new_pgd) {
            free((void *)new_base);
            free(new_image);
            regs->a0 = (unsigned long)-1;
            return;
        }

        //maps new program: VA of code base to actaul allocated PA for the program size
        map_pages(new_pgd,
                USER_CODE_BASE,//0x0
                new_size,
                virt_to_phys(new_base),//new base is kernel VA return from allocate(), need to convert to PA
                PROT_USER_RX);//program is RX

        //reuse stack
        if (!current->user_stack_base) {//should not reach here, will if stack allocation fail it shouldn't be a new process
            free_user_pgd(new_pgd);
            free((void *)new_base);
            free(new_image);
            // ideally also free new_pgd page-table pages
            regs->a0 = (unsigned long)-1;
            return;
        }
        //do not free the stack, just clear it
        memset((void *)current->user_stack_base, 0, STACK_SIZE);
        //map the stack to fixed VA, the mapping doesn't exist for the new pgd
        map_pages(new_pgd,
                USER_STACK_BASE,
                PAGE_SIZE,
                virt_to_phys(current->user_stack_base),
                PROT_USER_RW);

        //save old resources for later free
        old_image = current->image;
        old_pgd = current->pgd;
        
        free_mmap_regions(current);

        //replace thread fields
        current->image = new_image;

        //should be set to user VA
        current->user_program_base = USER_CODE_BASE;//should be VA entry addr
        current->user_program_size = new_size;
        current->user_sp = USER_STACK_TOP;//should be VA stack top

        //new pgd
        current->pgd = new_pgd;

        //Since exec() continues in the same task, activate the new page table now.
        //Otherwise sret would still use the old address space.
        switch_pgd(current->pgd);

        //check if can free image, might still shared
        if (old_image) {
            old_image->refcount--;
            if (old_image->refcount == 0) {
                free((void *)old_image->base);
                free(old_image);
            }
        }

        //free old pgd
        if (old_pgd)
            free_user_pgd(old_pgd);

        //Return from trap into the new program entry
        //sepc/sp are USER VIRTUAL addresses now
        regs->sepc = USER_CODE_BASE;   // 0x0, all user procress use same entry
        regs->sp = USER_STACK_TOP;     // 0x4000000000, all use same sp
        regs->tp = (unsigned long)current;// process is still this one
        regs->a0 = 0;//return 0 on success
    }
    else if (num == SYS_FORK) {
        //Create a new child user task.
        //The child continues from the same user instruction as the parent.
        //Parent sees fork() return child pid.
        //Child sees fork() return 0
        struct task_struct *parent = get_current();//caller of fork()
        struct task_struct *child;
        struct pt_regs *child_regs;
        unsigned long child_prog;
        unsigned long prog_alloc_size;

        // Only user thread with user stack and program should run fork().
        if (!parent || parent->type != TASK_USER ||
            !parent->user_stack_base || !parent->image) {
            regs->a0 = (unsigned long)-1;
            return;
        }
        prog_alloc_size = ALIGN_UP(parent->user_program_size, PAGE_SIZE);

        child_prog = (unsigned long)allocate(prog_alloc_size);
        if (!child_prog) {
            regs->a0 = (unsigned long)-1;
            return;
        }
        //just allocate new program image for now
        memset((void *)child_prog, 0, prog_alloc_size);
        //should copy from image->base, actaul VA mapping to allocated PA, not user_program_base
        memcpy((void *)child_prog,
            (void *)parent->image->base,
            parent->user_program_size);

        //uthread_create should use USER_CODE_BASE and USER_STACK_BASE and mapping them
        child = uthread_create(child_prog, parent->user_program_size);
        if (!child) {
            free((void *)child_prog);
            regs->a0 = (unsigned long)-1;
            return;
        }
        //advance part: child inherit handler
        for (int i = 0; i < MAX_SIGNAL; i++)
            child->signal_handler[i] = parent->signal_handler[i];

        child->pending_signal = 0;
        child->handling_signal = 0;
        child->signal_stack_base = 0;
        //copy content with kernel VA that backs the memory
        memcpy((void *)child->user_stack_base,//user_stack_base is the returned kernel VA from allocator, mapping to actual backing memory
            (void *)parent->user_stack_base,
            STACK_SIZE);
        //child and parent still use user VA stack
        //modifed so now sp translation
        //becuse paretn and child share same VA kernel addr, can just resume
        child->user_sp = regs->sp;

        /*
        * Lab 6 mmap:
        * fork() must also inherit mmap regions.
        * Without this, the child page table has code + stack only,
        * so reading mmap address causes scause = 0xd.
        */
        if (copy_mmap_regions(parent, child) != 0) {
            thread_destroy(child);
            regs->a0 = (unsigned long)-1;
            return;
        }


        if (setup_user_context(child) != 0) {
             thread_destroy(child);
            regs->a0 = (unsigned long)-1;
            return;
        }

        child_regs = (struct pt_regs *)child->thread.sp;
        
        //same sepc, user sp, general resigers
        //copies parents trap frame so return from trap resume as if return from same fork() syscall
        memcpy(child_regs, regs, sizeof(struct pt_regs));

        child_regs->sp = regs->sp;//now use the same sp (VA for stack is same)
        child_regs->tp = (unsigned long)child;  // child's current task pointer
        child_regs->a0 = 0;                     // fork() returns 0 in child

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
        unsigned long old = user_access_begin();
        framebuffer_display(bmp_image, width, height);
        user_access_end(old);
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
    else if (num == SYS_MMAP) {
        regs->a0 = sys_mmap(
            (void *)regs->a0,
            (unsigned long)regs->a1,
            (int)regs->a2,
            (int)regs->a3
        );
    }
}
