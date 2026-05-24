#ifndef PLIC_H
#define PLIC_H

#include "types.h"

extern uintptr_t plic_base;
extern unsigned long boot_hartid;

//PLIC may have many interrupt sources at once, priority helps decide which pending interrupt should be reported first.
#define PLIC_BASE plic_base
#define PLIC_PRIORITY(irq)   (PLIC_BASE + (irq) * 4)//gives the address of the priority register for one interrupt source, each source has its own 32-bit priority register, software can write priority to them
#define PLIC_ENABLE(hart)    (PLIC_BASE + 0x002080 + (hart) * 0x0100)//gives the base address of the interrupt enable bits for one hart/context. This register is a bitmask. Each bit corresponds to one interrupt source ID
#define PLIC_THRESHOLD(hart) (PLIC_BASE + 0x201000 + (hart) * 0x2000)//priority threshold register for that hart/context, PLIC will only deliver interrupts whose priority is strictly greater than the threshold.
#define PLIC_CLAIM(hart)     (PLIC_BASE + 0x201004 + (hart) * 0x2000)//the claim/complete register for that hart/context.
//used for two different operations: read it → claim interrupt, write interrupt ID back → complete interrupt

#define UART_IRQ  0x2a


void plic_init(void);
int plic_claim(void);
void plic_complete(int irq);

#endif