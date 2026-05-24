#ifndef PAGE_ALLOC_H
#define PAGE_ALLOC_H

#include "list.h"

#define PAGE_SIZE   4096UL
#define MAX_ORDER   10
//#define MEMORY_BASE 0x10000000UL
//#define MEMORY_SIZE 0x10000000UL
#define MAX_MANAGED_PAGES 2097152 //8 GiB / 4 KiB = 2097152 pages
#define NUM_POOLS 8
#define MAX_CHUNK_SIZE 2048UL
extern unsigned int managed_pages;
#define MAX_ALLOC_SIZE (PAGE_SIZE * (1U << MAX_ORDER))

#define ALLOC_TYPE_NONE   0
#define ALLOC_TYPE_PAGE   1
#define ALLOC_TYPE_CHUNK  2

extern unsigned long boot_hartid;
extern unsigned long boot_dtb_pa;
extern char _start[];
extern char _end[];

struct page {
    int order;          // -1 means not a head
    int refcount;       // 0 = free, 1 = allocated/reserved
    struct list_head node;

    int alloc_type;   // ALLOC_TYPE_NONE / ALLOC_TYPE_PAGE / ALLOC_TYPE_CHUNK
    int pool_idx;     // valid only when alloc_type == ALLOC_TYPE_CHUNK
    unsigned int allocated_chunks; //when 0 we can free the page back to buddy system
};

struct chunk {
    struct chunk *next;
};

struct pool {
    unsigned int chunk_size;
    struct chunk *free_list;
};


void *allocate(unsigned int size);
void free(void *ptr);
unsigned int page_addr_to_idx(struct page *page);
struct page *idx_to_page_addr(unsigned int idx);
int free_area_count(unsigned int order);
void page_alloc_init(const void *fdt);
struct page *alloc_pages(unsigned int order);
void free_pages(struct page *page);

#endif