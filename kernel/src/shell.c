#include "shell.h"
#include "uart.h"
#include "sbi.h"
#include "initrd_parse.h"
#include "exec.h"
#include "dt_parse.h"
#include "types.h"
#include "str.h"
#include "page_alloc_test.h"
#include "timer.h"
#include "task.h"
#include "thread.h"
#include "vm.h"
#define MAX_CMD_LEN 64


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
        uart_puts("   help                 - show this help message.\n");
        uart_puts("   hello                - print Hello World.\n");
        uart_puts("   info                 - print system / OpenSBI info.\n");
        uart_puts("   ls                   - list files in initrd.\n");
        uart_puts("   cat <file>           - print a text file from initrd.\n");
        uart_puts("   exec <file>          - execute a user program from initrd.\n");
        uart_puts("   setTimeout <sec> <msg> - print <msg> after <sec> seconds.\n");
        uart_puts("   restart              - force a crash for trap testing.\n");
        uart_puts("   test_page_alloc      - run page allocator test.\n");
        uart_puts("   test                 - run whatever is in test function.\n");
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
    else if (strncmp(cmd, "exec ", 5) == 0)
    {
        uintptr_t initrd_start = get_initrd_start((const void *)phys_to_virt(boot_dtb_pa));
        if (initrd_start == 0) {
            uart_puts("exec: initrd not found\n");
            return;
        }
        const char *filename = cmd + 5;
        if (*filename == '\0') {
            uart_puts("usage: exec <filename>\n");
            return;
        }
        unsigned long program_base;
        unsigned long program_size;
        struct task_struct *task;

        if (initrd_load_program((const void *)initrd_start,
                                filename,
                                &program_base,
                                &program_size) != 0) {
            uart_puts("exec: file not found or invalid image\n");
            return;
        }
        //if found program, create thread context, program already allocated in load_program
        task = uthread_create(program_base, program_size);//program base is where pc goes when program runs
        if (!task) {
            uart_puts("exec: failed to create user task\n");
            return;
        }

        if (setup_user_context(task) != 0) {
            uart_puts("exec: failed to setup user context\n");
            return;
        }
        uart_puts("exec: created user task pid ");
        uart_int(task->pid);
        uart_puts(", runnable\n");
        uart_flush_tx();//force clean before scheulde, fix bug where user shell prints half of runnable
        if (thread_wake(task) != 0) {
            uart_puts("exec: failed to wake user task\n");
            return;
        }
        thread_wait(task);
    }
    else if (strncmp(cmd, "setTimeout ", 11) == 0)
    {
        const char *p = cmd + 11;

        while (*p == ' ')
            p++;

        if (*p == '\0') {
            uart_puts("usage: setTimeout <seconds> <message>\n");
            return;
        }

        int sec = 0;
        while (*p >= '0' && *p <= '9') {
            sec = sec * 10 + (*p - '0');
            p++;
        }

        if (*p != ' ') {
            uart_puts("usage: setTimeout <seconds> <message>\n");
            return;
        }

        while (*p == ' ')
            p++;

        if (*p == '\0') {
            uart_puts("usage: setTimeout <seconds> <message>\n");
            return;
        }

        if (add_timeout_message(p, sec) < 0) {
            return;
        }
    }
    else if (streq(cmd, "restart") == 0)
    {
        uart_puts("forcing crash...\n");
        void (*bad)(void) = (void (*)(void))0x0;
        bad();
    }
    else if (streq(cmd, "test_page_alloc") == 0)
    {
        test_alloc_1();
    }
    else if (streq(cmd, "test") == 0)
    {
        thread_test();
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
