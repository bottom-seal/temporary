#include "exec.h"
#include "initrd_parse.h"
#include "uart.h"
#include "str.h"
#include "page_alloc.h"

static int align_page_up(int n)
{
    return (n + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1);
}

static int hextoi(const char *s, int n) {
    int r = 0;

    while (n-- > 0) {
        r = r << 4;
        if (*s >= 'A') {
            r += *s++ - 'A' + 10;
        } else if (*s >= '0' && *s <= '9') {
            r += *s++ - '0';
        }
    }

    return r;
}

static int align_up(int n, int byte) {
    return (n + byte - 1) & ~(byte - 1);
}
//in multithread support, the function just locates the file and allocate for program itself
int initrd_load_program(const void *rd,
                        const char *filename,
                        unsigned long *program_base,
                        unsigned long *program_size) {
    const char *p;

    if (!rd || !filename || !program_base || !program_size)
        return -1;

    p = (const char *)rd;

    while (1) {
        const struct cpio_t *hdr = (const struct cpio_t *)p;

        if (strncmp(hdr->magic, "070701", 6) != 0)
            return -1;

        int namesize = hextoi(hdr->namesize, 8);
        int filesize = hextoi(hdr->filesize, 8);
        const char *name = p + sizeof(struct cpio_t);
        const char *data;
        void *program;

        if (strcmp(name, "TRAILER!!!") == 0)
            return -1;

        data = p + align_up(sizeof(struct cpio_t) + namesize, 4);

        if (strcmp(name, filename) == 0) {
            if (filesize <= 0) {
                uart_puts("exec: invalid program image\n");
                return -1;
            }

            int alloc_size = align_page_up(filesize);

            program = allocate((unsigned int)alloc_size);

            if (!program) {
                uart_puts("exec: failed to allocate program memory\n");
                return -1;
            }

            memset(program, 0, (size_t)alloc_size);
            memcpy(program, data, (size_t)filesize);

            /*
            * We copied user code as data, then later execute it as instructions.
            * Make sure instruction fetch sees the copied bytes.
            */
            asm volatile("fence.i" ::: "memory");

            if (((unsigned long)program & (PAGE_SIZE - 1)) != 0) {
                uart_puts("BUG: loaded program not page aligned: ");
                uart_hex((unsigned long)program);
                uart_puts("\n");

                free(program);   // here

                return -1;
            }

            *program_base = (unsigned long)program;//now allocate() returns VA
            *program_size = (unsigned long)filesize;
            return 0;
        }

        p = data + align_up(filesize, 4);
    }
}
/*legacy: load program from initramfs without thread support
int initrd_exec(const void *rd, const char *filename) {
    //check input valid
    if (!rd || !filename) {
        return -1;
    }
    //should be start of cpio
    const char *p = (const char *)rd;

    //loop until finds file with the filename
    while (1) {
        //each file start with header, name + padding, data + padding
        const struct cpio_t *hdr = (const struct cpio_t *)p;

        if (strncmp(hdr->magic, "070701", 6) != 0) {
            return -1;
        }

        int namesize = hextoi(hdr->namesize, 8);
        int filesize = hextoi(hdr->filesize, 8);
        const char *name = p + sizeof(struct cpio_t);//skip header

        if (strcmp(name, "TRAILER!!!") == 0) {//end of cpio archive
            return -1;
        }

        const char *data = p + align_up(sizeof(struct cpio_t) + namesize, 4);//start of data

        if (strcmp(name, filename) == 0) {
            void *program;//pointer to addr of allocated space for user program
            void *stack_page = allocate(PAGE_SIZE);//same for user program stack
            unsigned long user_sp;
            unsigned long user_pc;
            unsigned long sstatus;

            if (filesize <= 0) {//check program valid
                uart_puts("exec: invalid program image\n");
                return -1;
            }

            program = allocate((unsigned int)filesize);
            //check allocation success
            if (!program) {
                uart_puts("exec: failed to allocate program memory\n");
                free(stack_page);
                return -1;
            }

            if (!stack_page) {
                uart_puts("exec: failed to allocate user stack\n");
                free(program);
                return -1;
            }

            user_sp = (unsigned long)stack_page + PAGE_SIZE;//use from high addr
            memcpy(program, data, (size_t)filesize);//copy the program out to the allocated space
            user_pc = (unsigned long)program;//start at the first insruction (program is binary)

            //read sstatus to the output sstatus (c variable)
            //put the result to some reg chosen by compiler, =r means output operand written by asm, general register
            asm volatile("csrr %0, sstatus" : "=r"(sstatus));
            sstatus &= ~(1UL << 8);//bit 8 is SPP, setting it to 0, sret will read it and goes user mode
            sstatus |= (1UL << 5);//bit 5 is SPIE, setting to 1, so interrupt will be enabled when sret to user mode

            asm volatile(
                "csrw sscratch, sp\n"//write current kernel stack pointer to sscratch, later ecall routine gets kernel_sp from here
                "mv   sp, %0\n"//write in user_sp as sp to run user program
                "csrw sepc, %1\n"//sepc is the pc to jump to on sret, set to the user program so it runs that
                "csrw sstatus, %2\n"// write in sstatus we prepared so it goes user mode and enables interrupt
                "sret\n"
                ://no output
                : "r"(user_sp), "r"(user_pc), "r"(sstatus)//%0 = user_sp; %1 = user_pc; %2 = sstatus
                : "memory"//affects memory, don't move memory instructions  around it
            );

            return 0;
        }

        p = data + align_up(filesize, 4);
    }
}
*/