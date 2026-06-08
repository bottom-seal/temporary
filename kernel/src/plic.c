#include "plic.h"
#include "uart.h"
//this sets up CPU side itnerrupt with PLIC
void enable_external_interrupt() {
    //sie = Supervisor Interrupt Enable CSR
    //CSR controls which kinds of supervisor interrupts are allowed
    //Bit 9 is SEIE: Supervisor External Interrupt Enable, allow external interrupts to be taken in supervisor mode
    asm volatile(
        "li t0, (1 << 9);"
        "csrs sie, t0;");
}
//initializes the PLIC so UART external interrupts can reach the CPU
void plic_init() {
    //MMIO pointers to PLIC registers
    volatile unsigned int *priority  = (volatile unsigned int *)PLIC_PRIORITY(UART_IRQ);//priority register, for UART
    volatile unsigned int *enable    = (volatile unsigned int *)PLIC_ENABLE(boot_hartid);//the enable bitmap array for this hart
    volatile unsigned int *threshold = (volatile unsigned int *)PLIC_THRESHOLD(boot_hartid);//the threshold register for this hart

    // (1) Set UART interrupt priority to 1
    //A PLIC interrupt source with priority 0 is effectively non-interrupting; only sources with priority above zero can be delivered.
    *priority = 1;

    // (2) Enable UART interrupt for the boot hart
    //PLIC enable register is many 32-bit words, each bit correspond to 1 IRQ, IRQ 0..31 live in enable[0], IRQ 32..63 live in enable[1]....
    enable[UART_IRQ / 32] |= (1U << (UART_IRQ % 32));//array index + bit position, OR with mask with only that bit set

    // (3) Set threshold to 0
    *threshold = 0;

    // (4) Enable supervisor external interrupts
    enable_external_interrupt();
}

int plic_claim() {
    //read the claim means accpeting the interrupt, the interrupt is the one with highest priority (PLIC does it itself)
    volatile unsigned int *claim = (volatile unsigned int *)PLIC_CLAIM(boot_hartid);
    return *claim;
}

void plic_complete(int irq) {
    //write to claim means interrupt accepted, this tells the PLIC to enable next interrupt from same source
    volatile unsigned int *claim = (volatile unsigned int *)PLIC_CLAIM(boot_hartid);
    *claim = irq;
}