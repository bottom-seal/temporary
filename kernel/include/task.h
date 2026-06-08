#ifndef _TASK_H
#define _TASK_H

#include "types.h"
#include "list.h"

#define TASK_PRIO_TIMER  200
#define TASK_PRIO_UART   300

//pointer to function task_callback_t that taks void pointer, return void
typedef void (*task_callback_t)(void *arg);

struct task {
    struct list_head list;          //node
    int priority;                   //high is earlier
    task_callback_t callback;       //function to run
    void *arg;                      //args for function
};

void task_init(void);
int task_empty(void);
int add_task(task_callback_t callback, void *arg, int priority);
void task_run_all(void);
void test_add_task(void);
#endif