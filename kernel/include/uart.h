#ifndef UART_H
#define UART_H


#define UART_BASE uart_base
#define UART_RBR  (unsigned char*)(UART_BASE + 0x0)
#define UART_THR  (unsigned char*)(UART_BASE + 0x0)
#define UART_IER  (unsigned char*)(UART_BASE + 0x4)
#define UART_IIR  (unsigned char*)(UART_BASE + 0x8)
#define UART_MCR  (unsigned char*)(UART_BASE + 0x10)
#define UART_LSR  (unsigned char*)(UART_BASE + 0x14)
#define LSR_DR    (1 << 0)
#define LSR_TDRQ  (1 << 5)

#include "types.h"

#define UART_BUF_SIZE 4096

struct ring_buf {
    char buf[UART_BUF_SIZE];
    volatile unsigned int head;
    volatile unsigned int tail;
};


extern uintptr_t uart_base;
extern unsigned long boot_dtb_pa;

char debug_uart_getc();
void debug_uart_putc(char c);
void debug_uart_puts(const char *s);
void debug_uart_hex(unsigned long h);

void uart_init();
void uart_isr(void);
char uart_getc(void);
unsigned char uart_get_byte(void);
unsigned int uart_get_u32_lil_end(void);
void uart_putc(char c);
void uart_puts(const char *s);
void uart_flush_tx(void);
void uart_hex(unsigned long h);
void uart_int(unsigned long x);
int streq(const char *a, const char *b);



void shell_run(void);

#endif
