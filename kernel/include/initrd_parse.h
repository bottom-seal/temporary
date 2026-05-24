#ifndef INITRD_PARSE_H
#define INITRD_PARSE_H

struct cpio_t {
    char magic[6];
    char ino[8];
    char mode[8];
    char uid[8];
    char gid[8];
    char nlink[8];
    char mtime[8];
    char filesize[8];
    char devmajor[8];
    char devminor[8];
    char rdevmajor[8];
    char rdevminor[8];
    char namesize[8];
    char check[8];
};


void initrd_list(const void *rd);
void initrd_cat(const void *rd, const char *filename);

#endif