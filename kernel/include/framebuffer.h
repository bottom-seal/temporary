#ifndef FRAMEBUFFER_H
#define FRAMEBUFFER_H

#define FB_IOCTL_GET_INFO 0

struct framebuffer_info {
    unsigned int width;
    unsigned int height;
    unsigned int bpp;
};

int framebuffer_display(unsigned int* bmp_image,
                        unsigned int width,
                        unsigned int height);

int fbdev_init(void);

#endif