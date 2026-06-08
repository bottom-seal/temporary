#include "uart.h"
#include "sbi.h"
#include "initrd_parse.h"
#include "dt_parse.h"
#include "types.h"
#include "str.h"
#include "thread.h"
#include "vm.h"


#define MAX_CMD_LEN 64

//! the behavior is UART TX interrupt is that it will do interrupt when TX ready, and it doesn't know buffer has data or not
// this interrupt may not start immediately if TX ready was already 1, so we need to kick start the first byte, gurantees when TX ready again, i will send next byte

// THIS IMPLEMENTATION IS POLLING UART MMIO
char debug_uart_getc() {
  while ((*UART_LSR & LSR_DR) == 0)
        ;
  char c = (char)*UART_RBR;
  return c == '\r' ? '\n' : c;
}

void debug_uart_putc(char c) {
    if (c == '\n')
        debug_uart_putc('\r');

    while ((*UART_LSR & LSR_TDRQ) == 0)
        ;
    *UART_THR = c;
}

void debug_uart_puts(const char *s) {
    while (*s)
        debug_uart_putc(*s++);
}

void debug_uart_hex(unsigned long h) {
    debug_uart_puts("0x");
    for (int c = 60; c >= 0; c -= 4) {
        unsigned long n = (h >> c) & 0xf;
        debug_uart_putc((char)(n > 9 ? (n - 10 + 'a') : (n + '0')));
    }
}

// RX, TX BUFFER OPERATIONS

static struct ring_buf rx_buf = {0};
static struct ring_buf tx_buf = {0};

static inline int rb_empty(struct ring_buf *rb) {
    return rb->head == rb->tail;
}

static inline int rb_full(struct ring_buf *rb) {
    return ((rb->head + 1) % UART_BUF_SIZE) == rb->tail;
}

static inline int rb_push(struct ring_buf *rb, char c) {
    unsigned int next = (rb->head + 1) % UART_BUF_SIZE;
    //if full
    if (next == rb->tail)
        return 0;

    rb->buf[rb->head] = c;
    rb->head = next;
    return 1;
}

static inline int rb_pop(struct ring_buf *rb, char *out) {
    if (rb->head == rb->tail)//if empty
        return 0;

    *out = rb->buf[rb->tail];
    rb->tail = (rb->tail + 1) % UART_BUF_SIZE;
    return 1;
}

//END OF RX, TX BUFFER OPERATIONS

//UART PLIC

void uart_init() {
    //Interrupt Enable Register in UART
    // Enable RX interrupt (IER bit 0), bit 0 RDI
    *UART_IER |= (1 << 0);//enabling RX interrupt in IER, when a byte arrives, UART may request an interrupt.

    // TX interrupt starts off, bit 1 THRI
    *UART_IER &= ~(1 << 1);//mask it at the start, if on, it will keep interrupting when TX ready even when buffer had no data
    
    // Modem Control Register
    // Enable UART interrupt output (MCR bit 3 = OUT2)
    *UART_MCR |= (1 << 3);//enabling MCR bit 3 says, let UART’s interrupt signal actually go outward to interrupt controller
}

void uart_isr(void) {
    // RX side: move all available received bytes into rx buffer
    while ((*UART_LSR & LSR_DR) != 0) {//line status register's data ready bit must be 1
        char c = (char)*UART_RBR;//actually read MMIO, get 1 char, push to buffer
        if (c == '\r')
            c = '\n';

        rb_push(&rx_buf, c);
    }

    // TX side: while transmitter ready and software tx buffer not empty, send bytes
    while (((*UART_LSR & LSR_TDRQ) != 0) && !rb_empty(&tx_buf)) {
        char c;
        rb_pop(&tx_buf, &c);//get 1 char from buffer
        *UART_THR = c;//put to Transmit Hold Register
    }

    // If no more data to send, turn TX interrupt back off
    if (rb_empty(&tx_buf))
        *UART_IER &= ~(1 << 1);
}

// END 


// INTERRUPT BASED GETC/PUTC

char uart_getc() {
    while (rb_empty(&rx_buf))
        schedule();

    //modifies the rx buffer, need to disable interrupt so no race condition
    unsigned long flags = irq_save();
    char c;
    rb_pop(&rx_buf, &c);//pop 1 from rx buffer, assign to char c
    irq_restore(flags);//enable interrupt again

    return c;
}

void uart_putc(char c) {
    if (c == '\n')
        uart_putc('\r');

    while (rb_full(&tx_buf))//program can get stuck here with buffer too small
        schedule();

    unsigned long flags = irq_save();

    rb_push(&tx_buf, c);//push the char to tx buffer

    // enable TX interrupt, will disable in the ISR routine
    *UART_IER |= (1 << 1);

    // manually send out the first byte, TX interrupt will start immediately for second byte
    // else we might need to wait for TX itself to interrupt, result in delayed start
    if (((*UART_LSR & LSR_TDRQ) != 0) && !rb_empty(&tx_buf)) {
        char out;
        rb_pop(&tx_buf, &out);
        *UART_THR = out;
    }

    irq_restore(flags);
}

unsigned char uart_get_byte() {
  while ((*UART_LSR & LSR_DR) == 0)
        ;
  char c = (char)*UART_RBR;
  return c;
}

unsigned int uart_get_u32_lil_end(void) {
    unsigned int b0 = uart_get_byte();
    unsigned int b1 = uart_get_byte();
    unsigned int b2 = uart_get_byte();
    unsigned int b3 = uart_get_byte();

    return b0 | (b1 << 8) | (b2 << 16) | (b3 << 24);
}

void uart_puts(const char* s) {
    while (*s)
        uart_putc(*s++);
}

void uart_flush_tx(void) {
    while (!rb_empty(&tx_buf)) {
        if (((*UART_LSR & LSR_TDRQ) != 0) && !rb_empty(&tx_buf)) {
            unsigned long flags = irq_save();
            char out;

            if (rb_pop(&tx_buf, &out))
                *UART_THR = out;

            irq_restore(flags);
        }
    }

    while ((*UART_LSR & LSR_TDRQ) == 0)
        ;
}

void uart_hex(unsigned long h) {
    uart_puts("0x");
    unsigned long n;
    for (int c = 60; c >= 0; c -= 4) {
        n = (h >> c) & 0xf;
        n += n > 9 ? 0x57 : '0';
        uart_putc(n);
    }
}

void uart_int(unsigned long x) {
    char buf[21];//64 bit has atmost 20 digits plus \0
    int i = 0;

    if (x == 0) {
        uart_putc('0');
        return;
    }

    while (x > 0) {
        buf[i] = '0' + (x % 10);//char code + last digit
        i++;
        x /= 10;
    }

    while (i > 0) {//print backward
        i--;
        uart_putc(buf[i]);
    }
}

//returns 1 if not match, returns 0 if match
int streq(const char *a, const char *b) {
    //check each character
    while (*a && *b) {
        if (*a != *b) 
          return 1;
        a++;
        b++;
    }
    //check if both are '\0'
    if(*a == *b)
      return 0;
    else
      return 1;
}

/* legacy: below removed to shell.c, keep it here in case i mess up
//prints stuff when given info command
void info_print() {
    uart_puts("   OpenSBI specification version:  ");
    uart_hex(sbi_get_spec_version());
    uart_puts("\n");
    uart_puts("   Implementation ID:  ");
    uart_hex(sbi_get_impl_id());
    uart_puts("\n");
    uart_puts("   Implementation version:  ");
    uart_hex(sbi_get_impl_version());
    uart_puts("\n");
}

void shell_execute(const char *cmd) {
    if (streq(cmd, "help") == 0)
    {
        uart_puts("Available commands:\n");
        uart_puts("   help  - show all commands.\n");
        uart_puts("   hello - print Hello World.\n");
        uart_puts("   info  - print system info.\n");
        uart_puts("   ls    - file list.\n");
        uart_puts("   cat   - show content of a txt.\n");
    } 
    else if (streq(cmd, "hello") == 0)
    {
        uart_puts("Hello World.\n");
    } 
    else if (streq(cmd, "info") == 0)
    {
        uart_puts("System information:\n");
        info_print();
    }
    else if (streq(cmd, "ls") == 0)
    {   
        uintptr_t initrd_start = get_initrd_start((const void *)phys_to_virt(boot_dtb_pa));
        //uart_hex(initrd_start);
        initrd_list((const void *)initrd_start);
    }
    else if (strncmp(cmd, "cat ", 4) == 0)
    {
        uintptr_t initrd_start = get_initrd_start((const void *)phys_to_virt(boot_dtb_pa));
        if (initrd_start == 0) {
            uart_puts("cat: initrd not found\n");
            return;
        }
        const char *filename = cmd + 4;   // skip "cat "
        if (*filename == '\0') {
            uart_puts("usage: cat <filename>\n");
        } else {
            initrd_cat((const void *)initrd_start, filename);
        }
    }
    else if (cmd[0] != '\0')
    {
        uart_puts("Unknown command: ");
        uart_puts(cmd);
        uart_puts("\r\n");
        uart_puts("Use help to get commands.\n");
    }
}

void shell_run(void) {
    char buf[MAX_CMD_LEN];
    int idx = 0;
    
    while(1)//loop for commands
    {
        uart_puts("opi-rv2> ");
        idx = 0;
        
        while ( 1)//look for each command
        {
            char c = uart_getc();
            if (c == '\r' || c == '\n') {//enter: see if command hit, then enter new command
                uart_puts("\r\n");
                buf[idx] = '\0';
                shell_execute(buf);
                break;
            }
            if (c == 8 || c == 127) {//backspace: cover with space then go back 1.
                if (idx > 0) {
                    idx--;
                    uart_puts("\b \b");
                }
                continue;
            }
            if (idx < MAX_CMD_LEN - 1) {//normal, append to *cmd, print to terminal
                buf[idx++] = c;
                uart_putc(c);
            }
        }
    }
}
*/
