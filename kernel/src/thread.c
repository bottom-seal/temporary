#include "thread.h"
#include "page_alloc.h"
#include "trap.h"
#include "uart.h"
#include "list.h"
#include "vm.h"
//to solve the problem of sp saved in context switch of idle will be boot stack, treat boot job as a thread task
static struct task_struct boot_task;
static struct task_struct *boot_task_ptr = &boot_task;

static int nr_threads;//how many threads, for pid assign
static struct list_head all_tasks_queue;//all tasks, used for global pid lookup
static struct list_head run_queue;//runnable thread queue
static struct list_head zombie_queue;//finished threads waiting for another thread to free them

//switch_to saves prev context and restores next context, implemented in boot/start.S
extern void switch_to(struct task_struct *prev, struct task_struct *next);
extern void ret_from_exception(void);

unsigned long irq_save(void);
void irq_restore(unsigned long flags);

struct task_struct *get_current(void) {
    //register: variable should live in a CPU register.
    //asm("tp"): bind this variable to the RISC-V tp register.
    //tp points to the current running task_struct because switch_to updates it.
    register struct task_struct *current asm("tp");
    return current;
}

static void add_to_all_tasks(struct task_struct *task) {
    if (!task)
        return;

    list_add_tail(&task->all_tasks_list, &all_tasks_queue);
}

static struct task_struct *find_task_by_pid(int pid) {
    struct list_head *curr;

    for (curr = all_tasks_queue.next;
         curr != &all_tasks_queue;
         curr = curr->next) {
        struct task_struct *task =
            list_entry(curr, struct task_struct, all_tasks_list);

        if (task->pid == pid)
            return task;
    }

    return 0;
}

//add to tail for RR
static void enqueue_runnable(struct task_struct *task) {
    if (!task)
        return;

    //add this thread to the tail of run_queue for round-robin scheduling
    task->state = THREAD_RUNNABLE;
    list_add_tail(&task->run_list, &run_queue);
}

//get head of runnable queue
static struct task_struct *dequeue_runnable(void) {
    struct task_struct *task;

    if (list_empty(&run_queue))
        return 0;

    //take the first runnable thread from the queue
    task = list_entry(run_queue.next, struct task_struct, run_list);
    list_del(&task->run_list);
    INIT_LIST_HEAD(&task->run_list);
    return task;
}

//if we set ra directly to task, after the task function finished,
//ra doesn't contain meaningful return addr, may jump to wierd place
static void thread_bootstrap(void) {
    //new threads start here first, then call the real function stored in task->entry
    struct task_struct *current = get_current();


    //First-time kernel threads enter here through switch_to().
    //They did not return from an old schedule() call, enable interrupt before running the real thread function.
    //only inside critical section would irq be disabled, enable irq for UART, timer
    irq_enable();

    //entry declared void (*entry)(void);
    if (current && current->entry)
        current->entry();
        //same as writing task(), as entry points to the functions

    thread_exit();
}

//creates boot_task, to pretent to be first thread( pid = 0), so first switch_to has prev
void thread_init(void) {
    // init queue heads before creating any thread
    nr_threads = 0; // for pid
    INIT_LIST_HEAD(&all_tasks_queue);
    INIT_LIST_HEAD(&run_queue);
    INIT_LIST_HEAD(&zombie_queue);

    /*
     * boot_task represents the context that is already running:
     * start.S -> start_kernel().
     *
     * It is not created by kthread_create(), and it should not be put
     * into run_queue or freed by kill_zombies().
     */
    boot_task.pid = nr_threads++;
    boot_task.state = THREAD_RUNNING;
    boot_task.type = TASK_KERNEL;
    boot_task.entry = 0;

    boot_task.kernel_stack_base = 0;
    boot_task.kernel_sp = 0;
    //bug fix: sepc was in kernel addr when I print debug in trap.c, kernel threads need to use global pgd, so context switch won't switch to uninitialized thing
    boot_task.pgd = kernel_pgd();
    boot_task.user_stack_base = 0;
    boot_task.user_sp = 0;

    boot_task.user_program_base = 0;
    boot_task.user_program_size = 0;
    boot_task.image = 0;

    boot_task.parent = 0;
    boot_task.wait_pid = -1;
    boot_task.exit_status = 0;

    boot_task.thread.ra = 0;
    boot_task.thread.sp = 0;
    for (int i = 0; i < 12; i++)
        boot_task.thread.s[i] = 0;

    boot_task.pending_signal = 0;
    boot_task.handling_signal = 0;
    boot_task.signal_stack_base = 0;

    for (int i = 0; i < MAX_SIGNAL; i++)
        boot_task.signal_handler[i] = 0;

    INIT_LIST_HEAD(&boot_task.all_tasks_list);
    INIT_LIST_HEAD(&boot_task.run_list);
    INIT_LIST_HEAD(&boot_task.zombie_list);
    INIT_LIST_HEAD(&boot_task.child_list);
    INIT_LIST_HEAD(&boot_task.sibling_list);
    //lab6
    INIT_LIST_HEAD(&boot_task.mmap_list);
    boot_task.mmap_next = USER_MMAP_BASE;
    add_to_all_tasks(&boot_task);

    // current task = boot_task
    asm volatile("mv tp, %0" : : "r"(&boot_task) : "tp");
}

//kthread create set pu everything and enqueues
struct task_struct *kthread_create(void (*entry)(void)) {
    struct task_struct *task;
    unsigned long flags;

    //need meaningful function
    if (!entry)
        return 0;

    //allocate space for the struct
    task = (struct task_struct *)allocate(sizeof(struct task_struct));
    if (!task)
        return 0;

    //each thread owns one kernel stack, stack grows down from stack + STACK_SIZE
    task->kernel_stack_base = (unsigned long)allocate(STACK_SIZE);
    if (!task->kernel_stack_base) {
        free(task);
        return 0;
    }

    task->state = THREAD_RUNNABLE;
    task->type = TASK_KERNEL;
    task->entry = entry;

    task->kernel_sp = task->kernel_stack_base + STACK_SIZE;//top
    task->user_stack_base = 0;//don't use user space
    task->user_sp = 0;
    task->pgd = kernel_pgd();
    task->user_program_base = 0;
    task->user_program_size = 0;
    task->image = 0;

    task->parent = 0;
    task->wait_pid = -1;
    task->exit_status = 0;
    //for advance part
    task->pending_signal = 0;
    task->handling_signal = 0;
    task->signal_stack_base = 0;

    for (int i = 0; i < MAX_SIGNAL; i++)
        task->signal_handler[i] = 0;
    //new thread doesn't have saved ra, sp. need to init them so later ret actually jumps to ra
    task->thread.ra = (unsigned long)thread_bootstrap;//init run bootstrap
    task->thread.sp = task->kernel_sp;//set to its stack
    for (int i = 0; i < 12; i++)
        task->thread.s[i] = 0;

    INIT_LIST_HEAD(&task->all_tasks_list);
    INIT_LIST_HEAD(&task->run_list);
    INIT_LIST_HEAD(&task->zombie_list);
    INIT_LIST_HEAD(&task->child_list);
    INIT_LIST_HEAD(&task->sibling_list);
    INIT_LIST_HEAD(&task->mmap_list);
    task->mmap_next = USER_MMAP_BASE;
    //nr_threads, all_tasks_queue, and run_queue are shared scheduler state.
    flags = irq_save();

    task->pid = nr_threads++;
    add_to_all_tasks(task);
    enqueue_runnable(task);

    irq_restore(flags);//because irq could already be disabled before, just do irq_enable here might introduce bug

    return task;
}

//uthread:
//create: initialize fields, sets up conext, but thread not ready to be queued
//expects user program is allocated before this function
struct task_struct *uthread_create(unsigned long user_pc,///allocted outside, pass program base, this is now VA, maps to PA backing the actual binary
                                   unsigned long user_program_size) {
    struct task_struct *task;
    struct task_struct *parent;
    struct user_image *image;
    unsigned long flags;

    if (!user_pc)
        return 0;
    //user image:
    //to track if the program code is shared by other thread
    image = (struct user_image *)allocate(sizeof(struct user_image));
    if (!image)
        return 0;

    image->base = user_pc;//starts from start of program
    image->size = user_program_size;
    image->refcount = 1;
    //allocate task struct
    task = (struct task_struct *)allocate(sizeof(struct task_struct));
    if (!task) {
        free(image);
        return 0;
    }
    //allocate kernel stack, need for trap 
    task->kernel_stack_base = (unsigned long)allocate(STACK_SIZE);
    if (!task->kernel_stack_base) {
        free(image);
        free(task);
        return 0;
    }
    //allocate user stack for function call and stuff
    task->user_stack_base = (unsigned long)allocate(STACK_SIZE);//now this store VA mapping to allocated PA
    if (!task->user_stack_base) {
        free((void *)task->kernel_stack_base);
        free(image);
        free(task);
        return 0;
    }
    //each uthread has its own pgd, it should have shared kernel upper half mapping
    //and private lower half user mapping
    task->pgd = create_user_pgd();//this function creates a copy of global pgd, with lower half init to 0
    if (!task->pgd) {//just previously allocated stuff
        free((void *)task->user_stack_base);
        free((void *)task->kernel_stack_base);
        free(image);
        free(task);
        return 0;
    }
    //Map user program
    //User VA 0x0 -> physical page containing image->base
    //image->base is a kernel VA, so convert it to PA before putting it to page table
    map_pages(task->pgd,
              USER_CODE_BASE,//va
              user_program_size,
              virt_to_phys(image->base),//image->base is return by allocator, is VA
              PROT_USER_RX);//program can't be written
    //Map user stack:
    //User VA 0x3ffffff000 -> physical page containing task->user_stack_base
    //Initial user SP should be 0x4000000000.
     map_pages(task->pgd,
              USER_STACK_BASE,//va
              USER_STACK_TOP - USER_STACK_BASE,
              virt_to_phys(task->user_stack_base),//user stack base is from allocator, is VA
              PROT_USER_RW);//stack is RW
    //a thread must run uthread_create
    parent = get_current();//get the tp of the calling thread

    task->state = THREAD_WAITING;//need to set up trampoline, routine to enter program
    task->type = TASK_USER;
    task->entry = 0;//runs user program, not kernel C function

    task->kernel_sp = task->kernel_stack_base + STACK_SIZE;
    //user_sp is now a user VA, set to fixed addr, not kernel allocated address.
    task->user_sp = USER_STACK_TOP;
    task->image = image;
    //user_program_base is now user VA 0x0. The actual physical backing memory is image->base.
    task->user_program_base = USER_CODE_BASE;
    task->user_program_size = image->size;

    task->parent = parent;
    task->wait_pid = -1;
    task->exit_status = 0;

    task->pending_signal = 0;
    task->handling_signal = 0;
    task->signal_stack_base = 0;

    for (int i = 0; i < MAX_SIGNAL; i++)
        task->signal_handler[i] = 0;

    task->thread.ra = 0;//set later in trampoline
    task->thread.sp = task->kernel_sp;
    for (int i = 0; i < 12; i++)
        task->thread.s[i] = 0;

    INIT_LIST_HEAD(&task->all_tasks_list);
    INIT_LIST_HEAD(&task->run_list);
    INIT_LIST_HEAD(&task->zombie_list);
    INIT_LIST_HEAD(&task->child_list);
    INIT_LIST_HEAD(&task->sibling_list);
    INIT_LIST_HEAD(&task->mmap_list);
    task->mmap_next = USER_MMAP_BASE;
    //nr_threads, all_tasks_queue, and parent's child_list are shared task state.
    flags = irq_save();

    task->pid = nr_threads++;
    add_to_all_tasks(task);

    if (parent)
        list_add_tail(&task->sibling_list, &parent->child_list);

    irq_restore(flags);

    //doesnt add to run queue yet
    return task;
}

//Prepare this task so that when switch_to() chooses it later, it will eventually sret to user mode
int setup_user_context(struct task_struct *task) {
    struct pt_regs *regs;//fake trap frame
    unsigned long sstatus;//sret read this to know what to do when called
    unsigned long *p;//points to trap frame

    if (!task || task->type != TASK_USER)//only user thread need to do this
        return -1;
    //uthread_create should return 0 if those would fail, still check
    //lab 6 : removed !task->user_program_base, as it should now always point to addr 0 in VA
    if (!task->kernel_stack_base || !task->user_stack_base || !task->user_sp)
        return -1;

    //reserve space on the user task’s kernel stack for trap frame
    regs = (struct pt_regs *)(task->kernel_sp - sizeof(struct pt_regs));
    p = (unsigned long *)regs;
    for (unsigned long i = 0; i < sizeof(struct pt_regs) / sizeof(unsigned long); i++)
        p[i] = 0;//move by 8 bytes
    //read out sstatus and change 2 bits
    asm volatile("csrr %0, sstatus" : "=r"(sstatus));
    sstatus &= ~(1UL << 8); // SPP = 0, so sret enters U-mode.
    sstatus |= (1UL << 5);  // SPIE = 1, so interrupts are enabled after sret.

    //when ret_from_exception runs:
    //the 3 registers will be restored like this before sret
    regs->sp = task->user_sp;//user stack pointer
    regs->tp = (unsigned long)task;//keep get_current() valid after restoring user context
    regs->sepc = task->user_program_base;//user program counter / entry point, sret goes here to continue execution
    regs->sstatus = sstatus;// sret read return state

    //switch_to restores kernel scheduling context: ra, sp, s0-s11
    //then run ret that sees ra and sp
    //goes to execute ret_from_exception (ra), uses base of trap frame (sp)
    //ret_from_exception restores user/trap context: a0-a7, t0-t6, sp (saves user sp), sepc, sstatus, etc
    //restore sp (user_sp), tp, sepc (user_program_base), sstatus (for sret)
    task->thread.ra = (unsigned long)ret_from_exception;//when first scheduled , runs as trampoline
    //switch to : CPU sp = task->thread.sp = address of fake trap frame
    task->thread.sp = (unsigned long)regs;//ret_from_exception expects sp points to base of trap frame
    for (int i = 0; i < 12; i++)
        task->thread.s[i] = 0;

    task->kernel_sp = (unsigned long)regs;//for later interrupt routine
    return 0;
}
//modified
int thread_wake(struct task_struct *task) {
    unsigned long flags;

    if (!task)
        return -1;

    //state check and enqueue need to be atomic
    flags = irq_save();

    //only waiting tasks should be woken.
    //this prevents waking a zombie task later from an old usleep timer.
    if (task->state != THREAD_WAITING) {
        irq_restore(flags);
        return -1;
    }

    //doesnt really happen,
    if (!list_empty(&task->run_list)) {
        list_del(&task->run_list);
        INIT_LIST_HEAD(&task->run_list);
    }

    enqueue_runnable(task);//enqueue to running

    irq_restore(flags);
    return 0;
}

//no need to dequeue becuase only running thread will wait for a child
//itself is already dequeued
int thread_wait(struct task_struct *child) {
    struct task_struct *current = get_current();
    unsigned long flags;

    if (!current || !child || child->parent != current)
        return -1;
    //bug: return to kernel shell but uart stuck
    //parent stop at schedule, resume at next instruction
    //but switch_to doesn't restore sstatus
    //if thread_wait() was entered while interrupts were disabled, and UART cannot work for shell
    flags = irq_save();

    current->wait_pid = child->pid;
    current->state = THREAD_WAITING;
    schedule();//thread_exit_status() woke kernel shell

    irq_restore(flags);

    //irq_enable();//resumes here, interrupt might be disabled
    return child->pid;
}

static struct task_struct *find_child(struct task_struct *parent, int pid) {
    struct list_head *curr;

    if (!parent)
        return 0;
    //search parent's child list
    for (curr = parent->child_list.next;
         curr != &parent->child_list;
         curr = curr->next) {
        struct task_struct *child =
            list_entry(curr, struct task_struct, sibling_list);

        if (child->pid == pid)
            return child;
    }
    //not found
    return 0;
}

// Detach all children from a dying parent.
// Each child is linked in parent->child_list through child->sibling_list.
static void orphan_children(struct task_struct *parent) {
    while (parent && !list_empty(&parent->child_list)) {
        struct task_struct *child =
            list_entry(parent->child_list.next,
                       struct task_struct,
                       sibling_list);

        list_del(&child->sibling_list);
        INIT_LIST_HEAD(&child->sibling_list);
        child->parent = 0;
    }
}

//free all allocated resources
static void free_task(struct task_struct *task) {
    if (!task)
        return;
    orphan_children(task);
    if (!list_empty(&task->all_tasks_list)) {
        list_del(&task->all_tasks_list);
        INIT_LIST_HEAD(&task->all_tasks_list);
    }
    if (!list_empty(&task->run_list)) {
        list_del(&task->run_list);
        INIT_LIST_HEAD(&task->run_list);
    }
    if (!list_empty(&task->zombie_list)) {
        list_del(&task->zombie_list);
        INIT_LIST_HEAD(&task->zombie_list);
    }
    if (!list_empty(&task->sibling_list)) {
        list_del(&task->sibling_list);
        INIT_LIST_HEAD(&task->sibling_list);
    }
    //only user is allocated user stack,
    //image cnt--, so we know when we can free
    if (task->type == TASK_USER) {
        free_mmap_regions(task);
        free((void *)task->user_stack_base);
        if (task->image) {
            task->image->refcount--;
            if (task->image->refcount == 0) {
                free((void *)task->image->base);
                free(task->image);
            }
        }
        //free signal resource if needed
        if (task->signal_stack_base) {
            free((void *)task->signal_stack_base);
            task->signal_stack_base = 0;
        }
        //free pgd for user thread
        if (task->pgd) {
            free_user_pgd(task->pgd);
            task->pgd = 0;
        }
    }

    free((void *)task->kernel_stack_base);
    free(task);
}


long process_waitpid(long pid) {
    struct task_struct *current = get_current();
    struct task_struct *target;
    unsigned long flags;

    if (!current)
        return -1;

    while (1) {
        //check child state + set current WAITING.
        //Otherwise child may exit between the check and sleep, causing missed wakeup.
        flags = irq_save();

        target = find_child(current, (int)pid);
        //if didn't find child
        if (!target) {
            irq_restore(flags);
            return -1;
        }
        //if child exited, free it
        if (target->state == THREAD_ZOMBIE) {
            long ret = target->pid;
            free_task(target);
            irq_restore(flags);
            return ret;
        }
        //if child runnable, parent waiting (already left runnable queue)
        current->wait_pid = (int)pid;
        current->state = THREAD_WAITING;//doesn't enqueue in swtich_to
        //process might use systemcall to run waitpid
        //CPU traps into kernel mode. On RISC-V, entering the trap clears sstatus.SIE
        //so supervisor interrupts are disabled while the kernel handles the syscall.
        //later, when the child exits, the parent is woken and scheduled again
        //but switch_to() doesn't restore sstatus
        schedule();

        irq_restore(flags);

        //irq_enable();
    }
}

//current process want to kill a process (no need to be parent)
long process_stop(long pid) {
    struct task_struct *current = get_current();
    struct task_struct *target;
    struct task_struct *parent;
    unsigned long flags;

    if (!current || pid < 0)
        return -1;


    //find_task_by_pid, run_queue removal, zombie_queue insertion,
    //orphan_children, and parent wakeup all modify shared task state.
    flags = irq_save();

    //thread exist
    target = find_task_by_pid((int)pid);
    if (!target) {
        irq_restore(flags);
        return -1;
    }

    //Do not stop boot task
    if (target == boot_task_ptr) {
        irq_restore(flags);
        return -1;
    }

    //If stopping itself, use normal exit path.
    //thread_exit_status() exits the CURRENT task, not arbitrary target.
    if (target == current) {
        irq_restore(flags);
        thread_exit_status(0);
        return 0;   // never reached
    }

     //Only allow stopping user tasks.
    if (target->type != TASK_USER) {
        irq_restore(flags);
        return -1;
    }
    //already stopped
    if (target->state == THREAD_ZOMBIE) {
        irq_restore(flags);
        return -1;
    }

    //Save parent before changing target state.
    //We must keep target attached to this parent so waitpid() can find it.
    parent = target->parent;

    //If target is runnable, remove it from run_queue.
    //If target is waiting, it is not in run_queue.
    if (target->state == THREAD_RUNNABLE) {
        if (!list_empty(&target->run_list)) {
            list_del(&target->run_list);
            INIT_LIST_HEAD(&target->run_list);
        }
    }

    
    //Mark target as zombie.
    //Do not free it here because parent may still waitpid() it,
    //and old timer callbacks may still hold its pointer.
    target->exit_status = 0;
    target->wait_pid = -1;
    target->state = THREAD_ZOMBIE;

    if (list_empty(&target->zombie_list))//if target's zombie node is self linked (not linked to zombie list)
        list_add_tail(&target->zombie_list, &zombie_queue);

    //The killed task's children become orphans.
    //But the killed task itself stays attached to its own parent.
    orphan_children(target);

    //Wake parent if it is waiting for this target.
    if (parent && parent->state == THREAD_WAITING &&
        parent->wait_pid == target->pid) {
        parent->wait_pid = -1;
        thread_wake(parent);//add back to running queue
    }

    irq_restore(flags);
    return 0;
}

//normal exit path for current running thread
void thread_exit_status(int status) {
    //get current thread
    struct task_struct *current = get_current();
    struct task_struct *parent;
    unsigned long flags;

    if (!current)
        return;

    //Exiting changes child list, zombie list, state, and may wake parent.
    flags = irq_save();

    //get current exit status
    current->exit_status = status;
    orphan_children(current);//orphan curren't child
    current->state = THREAD_ZOMBIE;//current exited
    //if not in zombie list, add to zombie list
    if (list_empty(&current->zombie_list))
        list_add_tail(&current->zombie_list, &zombie_queue);
    //wake up parent if waiting for this child
    parent = current->parent;
    if (parent && parent->state == THREAD_WAITING &&
        parent->wait_pid == current->pid) {
        parent->wait_pid = -1;
        thread_wake(parent);
    }

    schedule();

    irq_restore(flags);

    uart_puts("zombie run\n");//error if reached
    while (1)
        ;
}

void schedule(void) {
    //pick the next runnable thread and switch from current to next
    struct task_struct *prev;
    struct task_struct *next;
    unsigned long flags;

    //Critical section:
    //dequeue/enqueue/state update/switch_to must not be interrupted halfway.
    flags = irq_save();

    prev = get_current();//use tp for current thread
    next = dequeue_runnable();//first node in the list

    //only 1 thread runnable ( is running)
    if (!next) {
        irq_restore(flags);
        return;
    }

    //should never happen, for safety
    if (prev == next) {
        next->state = THREAD_RUNNING;
        irq_restore(flags);
        return;
    }

    //need to append the running thread back to list so RR will pick it again
    if (prev && prev->state == THREAD_RUNNING && prev != boot_task_ptr)
        enqueue_runnable(prev);

    //next is removed from run_queue, and is running
    next->state = THREAD_RUNNING;

    //context switch also switches satp
    switch_pgd(next->pgd);

    //context switch
    switch_to(prev, next);

    //task resumes here when it is scheduled again later.
    //Restore the interrupt state this task had before schedule().
    irq_restore(flags);
}

//only running thread itself can call exit, and running thread is already dequeued from RUNNING
void thread_exit(void) {
    struct task_struct *current = get_current();
    unsigned long flags;

    if (!current)
        return;

    flags = irq_save();

    //a thread cannot free its own stack while still running on it, so it mark itself as zombie and idle or parent will recycle
    current->state = THREAD_ZOMBIE;//marked zombie, schedule() logic checks only RUNNING can execute

    if (list_empty(&current->zombie_list))
        list_add_tail(&current->zombie_list, &zombie_queue);

    schedule();

    irq_restore(flags);

    //this thread will never be scheduled, should not enter this inf loop
    uart_puts("zombie run\n");
    while (1)
        ;
}

void kill_zombies(void) {
    //idle or parent thread recycle zombie thread resources
    struct list_head *curr;
    unsigned long flags;


    //zombie_queue may be modified by exit/stop/waitpid.
    flags = irq_save();

    curr = zombie_queue.next;

    while (curr != &zombie_queue) {
        struct list_head *next = curr->next;
        struct task_struct *task =
            list_entry(curr, struct task_struct, zombie_list);

         //Only free orphan zombies here.
         //Child zombies are freed by waitpid().
        if (!task->parent)
            free_task(task);

        curr = next;
    }

    irq_restore(flags);
}

void idle(void) {
    struct task_struct *current = get_current();

    if (current)
        current->state = THREAD_RUNNING;

    while (1) {
        irq_enable();//

        kill_zombies();
        schedule();

        //schedule() may restore an interrupt-disabled state depending on
        //where idle resumed from. Enable before wfi so timer/UART can wake CPU.
        irq_enable();

        //Avoid spinning forever when all user tasks are sleeping.
        //Timer/UART interrupt will wake the CPU.
        asm volatile("wfi");
    }
}

void foo(void) {
    for (int i = 0; i < 5; i++) {
        uart_puts("Thread id: ");
        uart_int(get_current()->pid);
        uart_puts(" ");
        uart_int(i);
        uart_puts("\n");
        for (int j = 0; j < 100000000; j++);
        schedule();
    }
    thread_exit();
}

void thread_test(void) {
    for (int i = 0; i < 3; i++) {
        kthread_create(foo);
    }
    idle();
}

//advance part
int  process_kill(int  pid, int signum) {
    struct task_struct *target;
    //check pid
    if (pid < 0)
        return -1;
    //check signum valid
    if (signum <= 0 || signum >= MAX_SIGNAL)
        return -1;
    //check if thread exist
    target = find_task_by_pid((int)pid);
    if (!target)
        return -1;

    //Do not send signal to boot task
    if (target == boot_task_ptr)
        return -1;

    //Only user process needs signal handling.
    if (target->type != TASK_USER)
        return -1;

    //Already dead.
    if (target->state == THREAD_ZOMBIE)
        return -1;

    //if not registered by signal, default stop the thread
    if (!target->signal_handler[signum])
        return process_stop(pid);

    //Handler exists:
    //Mark the signal as pending. It will actually be delivered before the target returns to user mode in ret_from_exception().
    target->pending_signal |= (1UL << signum);//set bit # to 1

    //if the process is sleeping/waiting, wake it up so it can eventually return to user mode and run the signal handler.
    //if it is RUNNABLE, it will be scheduled, then timer interrupt
    //if target is RUNNING, later timer interrupt happen, handles
    if (target->state == THREAD_WAITING)
        thread_wake(target);

    return 0;
}

//for lab 6 clean up
void thread_destroy(struct task_struct *task)
{
    free_task(task);
}

void free_mmap_regions(struct task_struct *task)
{
    struct list_head *curr;

    if (!task)
        return;

    while (!list_empty(&task->mmap_list)) {//remove until list empty
        struct mmap_region *region;

        curr = task->mmap_list.next;
        region = list_entry(curr, struct mmap_region, list);

        list_del(&region->list);

        if (region->pages) {
            unsigned long nr_pages = region->length / PAGE_SIZE;

            for (unsigned long i = 0; i < nr_pages; i++) {
                if (region->pages[i])
                    free((void *)region->pages[i]);
            }

            free(region->pages);
        }


        free(region);//free mmap struct
    }

    task->mmap_next = USER_MMAP_BASE;//next possible available back to start
}