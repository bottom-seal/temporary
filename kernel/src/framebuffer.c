#include "framebuffer.h"
#include "str.h"
#include "types.h"
#include "vm.h"

#define FB_BASE   0x7f700000UL//this is physical addr
#define FB_WIDTH  1920
#define FB_HEIGHT 1080
#define FB_BPP    4

#define CACHE_BLOCK_SIZE 64


//statement expression
//move argument start int a0
//.word emits 32-bit instruction cbo.flush
//Cache Block Operation: flush: flushes the cache block containing a given address, addr is provided in a0
//output empty
//input %0 = start
//clobber tells compiler that the instruction modifies a0, and memory
#define cbo_flush(start)                \
    ({                                  \
        asm volatile("mv a0, %0\n\t"    \
                     ".word 0x0025200F" \
                     :                  \
                     : "r"(start)       \
                     : "memory", "a0"); \
    })

static void flush_dcache(void *addr, unsigned long len) {
    unsigned long end;
    unsigned long line;

    if (len == 0)
        return;
    ////before flushing, rounds addr down to a 64-byte boundary, so last increment of CACHE_BLOCK_SIZE will not skip the last block
    line = (unsigned long)addr & ~(CACHE_BLOCK_SIZE - 1);//a mask whose lower log2(CACHE_BLOCK_SIZE) bits are 0, and whose higher bits are 1
    end = (unsigned long)addr + len;
    ////make sure previous memory writes happen before flush
    __sync_synchronize();
    while (line < end) {
        cbo_flush(line);
        line += CACHE_BLOCK_SIZE;
    }
    //make flush in the loop ordered
    __sync_synchronize();
}

int framebuffer_display(unsigned int *bmp_image,
                        unsigned int width,
                        unsigned int height) {
    unsigned int *fb = (unsigned int *)phys_to_virt(FB_BASE);
    unsigned int start_x;
    unsigned int start_y;

    if (!bmp_image || width == 0 || height == 0)
        return -1;

    if (width > FB_WIDTH || height > FB_HEIGHT)
        return -1;
    ////put half of unused space on the right/top
    start_x = (FB_WIDTH - width) / 2;
    start_y = (FB_HEIGHT - height) / 2;

    for (unsigned int y = 0; y < height; y++) {//loop through rows
        void *dst = fb + (start_y + y) * FB_WIDTH + start_x;//row * screen_width + column
        void *src = bmp_image + y * width;//from 1D pixel array
        unsigned long row_bytes = width * FB_BPP;

        memcpy(dst, src, row_bytes);
        flush_dcache(dst, row_bytes);//make sure new written data visible to display device
    }

    return 0;
}
