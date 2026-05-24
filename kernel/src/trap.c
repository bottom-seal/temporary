#include "uart.h"
#include "trap.h"
#include "plic.h"
#include "timer.h"
#include "task.h"
#include "thread.h"
#include "syscall.h"

#define SSTATUS_SIE (1UL << 1)

void irq_enable() {
    //csr set immediate, set the bit in the immediate position
    asm volatile("csrsi sstatus, 2");//bit 2 in sstatus is SIE, global interrupt
}

void irq_disable() {
    //csr clear immediate
    asm volatile("csrci sstatus, 2");
}

unsigned long irq_save(void) {
    unsigned long flags;

    asm volatile("csrrc %0, sstatus, %1"
                 : "=r"(flags)
                 : "r"(SSTATUS_SIE)
                 : "memory");

    return flags;
}


void irq_restore(unsigned long flags) {
    if (flags & SSTATUS_SIE)
        irq_enable();
    else
        irq_disable();
}


void do_trap(struct pt_regs *regs) {
    unsigned long cause = regs->scause;
    unsigned long is_interrupt = cause >> 63;
    unsigned long code = cause & 0xfffUL;
    unsigned long from_user = (regs->sstatus & (1UL << 8)) == 0;//sstatus.SPP, previous mode was user mode
    int need_schedule = 0;

    if (is_interrupt && code == 5) {
        timer_interrupt_handler();
        //need_schedule = timer_take_schedule_tick();
        need_schedule = timer_take_schedule_tick() && from_user;
    }
    else if (!is_interrupt && code == 8) {
        handle_syscall(regs);
    }
    else if (is_interrupt && code == 9) {
        int irq = plic_claim();

        if (irq == UART_IRQ)
            uart_isr();

        if (irq)
            plic_complete(irq);
    }

    task_run_all();

    if (need_schedule)
        schedule();
}