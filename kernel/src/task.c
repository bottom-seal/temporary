#include "task.h"
#include "trap.h"
#include "page_alloc.h"
#include "list.h"
#include "uart.h"
static struct list_head task_list;
static int current_task_priority = -1;  //tracker, need for preempt

//init list head
void task_init(void) {
    INIT_LIST_HEAD(&task_list);
    current_task_priority = -1;//later be replaced by first task priority
}

int task_empty(void) {
    return list_empty(&task_list);
}
//
int add_task(task_callback_t callback, void *arg, int priority) {
    struct task *new_task;
    struct list_head *curr;//used to walk through list
    int inserted = 0;

    //check function valid
    if (!callback)
        return -1;

    //cehck allocation valid
    new_task = (struct task *)allocate(sizeof(struct task));
    if (new_task == 0)
        return -1;

    new_task->priority = priority;
    new_task->callback = callback;
    new_task->arg = arg;
    INIT_LIST_HEAD(&new_task->list);

    // larger priority value goes earlier in list
    for (curr = task_list.next; curr != &task_list; curr = curr->next) {
        struct task *entry = list_entry(curr, struct task, list);
        //if new task priorty larger, add curr task to its tail
        if (new_task->priority > entry->priority) {
            list_add_tail(&new_task->list, curr);   // insert before curr
            inserted = 1;
            break;
        }
    }
    //if not inserted whole loop, means priority smallest
    if (!inserted) {
        list_add_tail(&new_task->list, &task_list);
    }

    return 0;
}

void task_run_all(void) {
    //this is run when the critical work of whatever called the trap routine is done
    //we should run all deferred task before returning to restore part
    //run the whole list, this loop is meant to be interruptable
    while (!list_empty(&task_list)) {
        struct task *first;
        task_callback_t cb;
        void *arg;
        int prev_priority;

        first = list_entry(task_list.next, struct task, list);

        // preemption rule:
        // only run this queued task now if it is higher priority
        // than the currently interrupted/running task.
        // current_task_priority is static global
        // if task was running and interrupted, the new task has to be higher priority to run, else return to the interrupted task
        if (current_task_priority >= 0 &&
            first->priority <= current_task_priority) {
            break;
        }

        //order by the priority, first is always the highest
        cb = first->callback;
        arg = first->arg;
        //remove the one to run, so next interrupt would not compare already running task or run it again
        //on interrupt, after higher priority tasks finish, it should return to the interrupted callback func
        list_del(&first->list);
        
        //current_task_priority wont be modified if new task cannot preempt, if it can,
        //we need to save previous, because cb could be interrupted, need to ensure the restored current_task_priority is for the current running task
        prev_priority = current_task_priority;
        current_task_priority = first->priority;//this will be checked to see if new task can preempt

        irq_enable();//enables interrupt with sstatus.SIE
        if (cb)
            cb(arg);
        irq_disable();//after the routine

        current_task_priority = prev_priority;//after finishing higher priorty task, restore the previous priority

        free(first);
    }
}

//below tests API
struct demo_arg {
    int prio;
};

static struct demo_arg demo_args[20];

void test_task_cb(void *arg) {
    struct demo_arg *d = (struct demo_arg *)arg;

    uart_puts("[Task] Executing Priority ");
    uart_int(d->prio);
    uart_puts("\n");

    unsigned long current_sstatus;
    asm volatile("csrr %0, sstatus" : "=r"(current_sstatus));

    if (current_sstatus & (1 << 1)) {
        uart_puts("SIE=1\n");
    } else {
        uart_puts("SIE=0\n");
    }

    for (volatile int j = 0; j < 100000000; j++) {
    }
    uart_puts("[Task] End Priority ");
    uart_int(d->prio);
    uart_puts("\n");
}

void test_add_task(void){
    for (int i = 0; i < 20; i++) {
            demo_args[i].prio = i + 10;
            add_task(test_task_cb, &demo_args[i], i + 10);
        }
}