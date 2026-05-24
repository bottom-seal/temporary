#include "initrd_parse.h"
#include "uart.h"
#include "str.h"


static int hextoi(const char* s, int n) {
    int r = 0;
    while (n-- > 0) {//loop for all n
        r = r << 4;//*16 for next digit
        if (*s >= 'A')
            r += *s++ - 'A' + 10;//A is 10, so add 10 to get correct value
        else if (*s >= '0' && *s <= '9')
            r += *s++ - '0';
    }
    return r;
}

static int align(int n, int byte) {
    //byte-1 gets bits to 1, invert for 0 mask
    //add byte-1 to n to round up, then mask out the last bits to align down
    return (n + byte - 1) & ~(byte - 1);
}

//this function is for formatting ls
int num_digits(unsigned long x) {
    //count how many digits in x
    int d = 1;
    while (x >= 10) {
        x /= 10;
        d++;
    }
    return d;
}

//put many space until align 
void put_spaces(int n) {
    while (n-- > 0) {
        uart_putc(' ');
    }
}


//added this function because ls need to show total files
void initrd_scan(const void *rd, int *count, int *max_size) {
    const char *p = (const char *)rd;//cursor
    *count = 0;
    *max_size = 0;

    while (1) {
        const struct cpio_t *hdr = (const struct cpio_t *)p;

        if (strncmp(hdr->magic, "070701", 6) != 0) {
            return;
        }

        int namesize = hextoi(hdr->namesize, 8);
        int filesize = hextoi(hdr->filesize, 8);
        const char *name = p + sizeof(struct cpio_t);

        if (strcmp(name, "TRAILER!!!") == 0) {//check for end of archive
            return;
        }

        (*count)++;//not end, count ++
        if (filesize > *max_size) {//get biggest file size for formatting
            *max_size = filesize;
        }

        const char *data = p + align(sizeof(struct cpio_t) + namesize, 4);//name is string, need alignment
        p = data + align(filesize, 4);//skip file data, align for next header
    }
}

void initrd_list(const void* rd) {
    // TODO: Implement this function
    if (!rd) {
        uart_puts("ramdisk false\n");
        return;
    }

    //new part to get file num, and format
    int total = 0;
    int max_size = 0;
    initrd_scan(rd, &total, &max_size);

    int width = num_digits((unsigned long)max_size);//check how many digits is max size, for formatting

    uart_puts("Total ");
    uart_int(total);
    uart_puts(" files.\n");

    //orignal part from exercise
    const char *p = (const char *)rd;

    while (1) {
        const struct cpio_t *hdr = (const struct cpio_t *)p;

        /* magic is char[6], not a C string pointer */
        if (strncmp(hdr->magic, "070701", 6) != 0) {
            uart_puts("magic false\n");
            return;
        }

        int namesize = hextoi(hdr->namesize, 8);
        int filesize = hextoi(hdr->filesize, 8);

        /* pathname starts right after header */
        const char *name = p + sizeof(struct cpio_t);

        /* end of archive */
        if (strcmp(name, "TRAILER!!!") == 0) {
            //uart_puts("TRAILER detected at ");
            //uart_puts(" ");
            //uart_puts("\n");
            return;
        }
        //print like: 337    penguin.txt
        uart_int(filesize);
        put_spaces(width - num_digits((unsigned long)filesize) + 4);//4 spaces between size and name
        uart_puts(name);
        uart_puts("\n");
        //structure
        //[file header][pathname][padding][file data][padding][next file header]...
        // move to file data: header + pathname, then align to 4 bytes
        const char *data = p + align(sizeof(struct cpio_t) + namesize, 4);

        // move to next entry: file data + align to 4 bytes
        p = data + align(filesize, 4);
    }
}

void initrd_cat(const void* rd, const char* filename) {
    // TODO: Implement this function
    if (!rd || !filename) {
        uart_puts("invalid argument\n");
        return;
    }

    const char *p = (const char *)rd;

    while (1) {
        const struct cpio_t *hdr = (const struct cpio_t *)p;

        /* check magic */
        if (strncmp(hdr->magic, "070701", 6) != 0) {
            uart_puts("magic false\n");
            return;
        }

        int namesize = hextoi(hdr->namesize, 8);
        int filesize = hextoi(hdr->filesize, 8);

        /* pathname starts right after header */
        const char *name = p + sizeof(struct cpio_t);

        /* end of archive */
        if (strcmp(name, "TRAILER!!!") == 0) {
            uart_puts("initrd_cat: ");
            uart_puts(filename);
            uart_puts(" not found\n");
            return;
        }

        /* file data starts after header + pathname, aligned to 4 bytes */
        const char *data = p + align(sizeof(struct cpio_t) + namesize, 4);

        /* found target file */
        if (strcmp(name, filename) == 0) {
            for (int i = 0; i < filesize; i++) {
                uart_putc(data[i]);
            }
            uart_puts("\n");
            return;
        }

        /* move to next entry */
        p = data + align(filesize, 4);
    }
}

