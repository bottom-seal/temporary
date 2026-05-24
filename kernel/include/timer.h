#ifndef TIMER_H
#define TIMER_H

#include "types.h"
#include "list.h"

extern unsigned long cpu_freq;

#define TIMER_MSG_LEN 64
//timer_callback_t is a pointer to function type, takes void *args and returns void
typedef void (*timer_callback_t)(void *arg); 

struct timer_event {
    struct list_head list;
    uint64_t expire_time;      //absolute
    timer_callback_t callback; //the function to run
    void *arg;                 //args if any, for string here
};

uint64_t rdtime(void);
void timer_init(void);
void timer_interrupt_handler(void);
int timer_take_schedule_tick(void);
int add_timer(timer_callback_t callback, void *arg, int sec);
int add_timer_us(timer_callback_t callback, void *arg, unsigned int usec);
int add_timeout_message(const char *msg, int sec);

void timeout_callback(void *arg);
void tick_callback(void *arg);

#endif
