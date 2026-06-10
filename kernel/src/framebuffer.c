#include "framebuffer.h"
#include "str.h"
#include "types.h"
#include "vm.h"
#include "vfs.h"
#include "page_alloc.h"

#define FB_BASE_PA 0x7f700000UL
#define FB_BASE    phys_to_virt(FB_BASE_PA)//use VA
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

static struct file_operations fbdev_file_ops;
//avoid writen data sit in cache, not actually written to memory
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
    unsigned int *fb = (unsigned int *)FB_BASE;//already VA
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
//same as tmpfs
static int fbdev_open(struct vnode* file_node, struct file** target) {
    if (!file_node || !target || !*target)
        return -1;

    (*target)->vnode = file_node;
    (*target)->f_pos = 0;
    (*target)->f_ops = &fbdev_file_ops;//use fb ops

    return 0;
}
//same as tmpfs
static int fbdev_close(struct file* file) {
    if (!file)
        return -1;

    free(file);
    return 0;
}

static int fbdev_read(struct file* file, void* buf, size_t len) {
    (void)file;
    (void)buf;
    (void)len;

    //write only
    return -1;
}

static int fbdev_write(struct file* file, const void* buf, size_t len) {
    unsigned long fb_size;
    unsigned long remain;
    void* dst;

    if (!file || !buf)
        return -1;

    fb_size = (unsigned long)FB_WIDTH * FB_HEIGHT * FB_BPP;

    if ((unsigned long)file->f_pos >= fb_size)
        return 0;

    remain = fb_size - (unsigned long)file->f_pos;

    if (len > remain)
        len = remain;

    dst = (void*)((char*)FB_BASE + file->f_pos);

    memcpy(dst, buf, len);
    flush_dcache(dst, len);//CPU reads it immediately, make sure is fresh

    file->f_pos += len;

    return (int)len;
}

//user use this to set offset
static long fbdev_lseek64(struct file* file, long offset, int whence) {//only SEEK_SET can be whence
    unsigned long fb_size;

    if (!file)
        return -1;

    if (whence != SEEK_SET)
        return -1;

    if (offset < 0)
        return -1;

    fb_size = (unsigned long)FB_WIDTH * FB_HEIGHT * FB_BPP;
    //try to move out of fb
    if ((unsigned long)offset > fb_size)
        return -1;
    //if legal, modifies offset of file handler
    file->f_pos = (size_t)offset;

    return offset;
}

//return framebuffer struct in arg, user use this to calculate where to write pixel (with lseek64)
static int fbdev_ioctl(struct file* file, unsigned long request, void* arg) {
    struct framebuffer_info* info;

    if (!file || !arg)
        return -1;
    //only work with FB_IOCTL_GET_INFO
    if (request != FB_IOCTL_GET_INFO)
        return -1;
    //return struct of framebuffer_info
    info = (struct framebuffer_info*)arg;
    info->width = FB_WIDTH;
    info->height = FB_HEIGHT;
    info->bpp = FB_BPP;

    return 0;
}

static struct file_operations fbdev_file_ops = {
    .open = fbdev_open,
    .close = fbdev_close,
    .read = fbdev_read,
    .write = fbdev_write,
    .lseek64 = fbdev_lseek64,
    .ioctl = fbdev_ioctl,
};

int fbdev_init(void) {
    return register_device("fb", &fbdev_file_ops);
}
