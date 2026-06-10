#include "vfs.h"
#include "uart.h"
#include "trap.h"
#include "page_alloc.h"

static struct file_operations uartdev_fops;//for compiler

//init file handler object
static int uartdev_open(struct vnode* file_node, struct file** target) {
    if (!file_node || !target || !*target)
        return -1;

    (*target)->vnode = file_node;
    (*target)->f_pos = 0;
    (*target)->f_ops = &uartdev_fops;

    return 0;
}
//free handler
static int uartdev_close(struct file* file) {
    if (!file)
        return -1;

    free(file);
    return 0;
}
//read(0, buf, len), goes fd_table 0, records /dev/uart, f_ops is this
static int uartdev_read(struct file* file, void* buf, size_t len) {
    //kernel buffer addr
    char* out = (char*)buf;
    unsigned long flags;

    if (!file)
        return -1;

    if (len == 0)
        return 0;//read 0 bytes so return 0

    if (!buf)
        return -1;

    //uart_getc() depends on UART interrupt-driven buffering in your kernel, need to enable interrupt
    flags = irq_save();
    irq_enable();

    for (size_t i = 0; i < len; i++)
        out[i] = uart_getc();

    irq_restore(flags);

    return (int)len;
}

static int uartdev_write(struct file* file, const void* buf, size_t len) {
    const char* in = (const char*)buf;

    if (!file)
        return -1;

    if (len == 0)
        return 0;

    if (!buf)
        return -1;
    //output existing byte, no need to wait
    for (size_t i = 0; i < len; i++)
        uart_putc(in[i]);
    //clear tx buffer so thing appear complete
    uart_flush_tx();

    return (int)len;
}

static struct file_operations uartdev_fops = {
    .open = uartdev_open,
    .close = uartdev_close,
    .read = uartdev_read,
    .write = uartdev_write,
};

int uartdev_init(void) {
    return register_device("uart", &uartdev_fops);//register device to device table, device has attribute name and f_ops
}