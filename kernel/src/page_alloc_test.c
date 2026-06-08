#include "page_alloc.h"
#include "uart.h"
#include "types.h"
#include "str.h"

void test_alloc_1() {
    uart_puts("Testing memory allocation...\n");
    char *ptr1 = (char *)allocate(4000);
    char *ptr2 = (char *)allocate(8000);
    char *ptr3 = (char *)allocate(4000);
    char *ptr4 = (char *)allocate(4000);

    free(ptr1);
    free(ptr2);
    free(ptr3);
    free(ptr4);

    /* Test kmalloc */
    uart_puts("Testing dynamic allocator...\n");
    char *kmem_ptr1 = (char *)allocate(16);
    char *kmem_ptr2 = (char *)allocate(32);
    char *kmem_ptr3 = (char *)allocate(64);
    char *kmem_ptr4 = (char *)allocate(128);

    free(kmem_ptr1);
    free(kmem_ptr2);
    free(kmem_ptr3);
    free(kmem_ptr4);

    char *kmem_ptr5 = (char *)allocate(16);
    char *kmem_ptr6 = (char *)allocate(32);

    free(kmem_ptr5);
    free(kmem_ptr6);

    // Test allocate new page if the cache is not enough
    void *kmem_ptr[102];
    for (int i=0; i<100; i++) {
        kmem_ptr[i] = (char *)allocate(128);
    }
    for (int i=0; i<100; i++) {
        free(kmem_ptr[i]);
    }

    // Test exceeding the maximum size
    char *kmem_ptr7 = (char *)allocate(MAX_ALLOC_SIZE + 1);
    if (kmem_ptr7 == NULL) {
        uart_puts("Allocation failed as expected for size > MAX_ALLOC_SIZE\n");
    }
    else {
        uart_puts("Unexpected allocation success for size > MAX_ALLOC_SIZE\n");
        free(kmem_ptr7);
    }
}

void dump(void) {
    int i;

    uart_puts("Buddy allocator state:\n");
    for (i = MAX_ORDER; i >= 0; i--) {
        uart_puts("free_area[");
        uart_int(i);
        uart_puts("] = ");
        uart_int(free_area_count(i));
        uart_puts("\n");
    }
}

void test_alloc_2(void) {
    struct page *p1;
    struct page *p2;
    struct page *p3;

    uart_puts("\n[p1 allocate order 1]\n");
    p1 = alloc_pages(1);
    uart_puts("p1 idx = ");
    uart_int(page_addr_to_idx(p1));
    uart_puts("\n");
    dump();

    uart_puts("\n[p2 allocate order 1]\n");
    p2 = alloc_pages(1);
    uart_puts("p2 idx = ");
    uart_int(page_addr_to_idx(p2));
    uart_puts("\n");
    dump();

    uart_puts("\n[p3 allocate order 1]\n");
    p3 = alloc_pages(1);
    uart_puts("p3 idx = ");
    uart_int(page_addr_to_idx(p3));
    uart_puts("\n");
    dump();
    
    uart_puts("\n[free p1]\n");
    free_pages(p1);
    dump();
    
    uart_puts("\n[free p2]\n");
    free_pages(p2);
    dump();

    uart_puts("\n[free p3]\n");
    free_pages(p3);
    dump();
    
}
