#include "page_alloc.h"
#include "list.h"
#include "uart.h"
#include "str.h"
#include "dt_parse.h"
#include "vm.h"
//overwritten in init_page_alloc
uintptr_t memory_base = 0;
uintptr_t memory_end = 0;
unsigned int managed_pages = 0;

//use pointer for advanced part 3, this should point to the first struct page object
static struct page *mem_map;

static struct list_head free_area[MAX_ORDER + 1];//free area is a list of list_head, a struct with ony prev and next pointer

static struct pool pools[NUM_POOLS] = {
    {16,   0},
    {32,   0},
    {64,   0},
    {128,  0},
    {256,  0},
    {512,  0},
    {1024, 0},
    {2048, 0},
};
//https://hackmd.io/@yuanC-L/HJMD9qub0
//statement expression, the value in last line will be returned
//since we are using intrusive linked list
//need to find the offset of the connecting node, so we can get other attributes
//the byte offset of MEMBER inside struct TYPE
//pretends addr 0 is a pointer to struct TYPE,
//if struct start at addr 0, what addr would MEMBER be, the addr is the offset
#define offsetof(TYPE, MEMBER) ((unsigned long)&(((TYPE *)0)->MEMBER))//unsigned long converts pointer addr to int value
//given a pointer to a member, find the pointer to the whole struct containing it
//page = ptr of node - its offset
//cast to char* so it will deal in bytes
//cast back to container's type before return
#define container_of(ptr, type, member) \
    ((type *)((char *)(ptr) - offsetof(type, member)))

//these 2 functions deals with metadata struct "page"'s addr, and the index if mem_map[]
unsigned int page_addr_to_idx(struct page *page) {
    return (unsigned int)(page - mem_map);//how many pages between param and mem_map base
}

struct page *page_idx_to_addr(unsigned int idx) {
    return &mem_map[idx];//idx'th page's addr
}

//order k block ha size of 2^k, which is 1 << order, flipping the bit to find the buddy of the idx
static unsigned int buddy_idx(unsigned int idx, unsigned int order) {
    return idx ^ (1U << order);
}

//ONLY FOR DEMO, NOT IMPORTANT
//a block head at page idx was added into free_area[order]
//eg. [+] Add page 5642 to order 1. Range of pages: [5642, 5643]
/*
static void log_add_block(unsigned int idx, unsigned int order) {
    uart_puts("[+] Add page ");
    uart_int(idx);
    uart_puts(" to order ");
    uart_int(order);
    uart_puts(". Range of pages: [");
    uart_int(idx);
    uart_puts(", ");
    uart_int(idx + (1U << order) - 1);
    uart_puts("]\n");
}
//a free block was removed from free_area[order]
// [-] Remove page 4 from order 2. Range of pages: [4, 7]
static void log_remove_block(unsigned int idx, unsigned int order) {
    uart_puts("[-] Remove page ");
    uart_int(idx);
    uart_puts(" from order ");
    uart_int(order);
    uart_puts(". Range of pages: [");
    uart_int(idx);
    uart_puts(", ");
    uart_int(idx + (1U << order) - 1);
    uart_puts("]\n");
}
// [*] Buddy found! buddy idx: 16898 for page 16899 with order 0
static void log_buddy_found(unsigned int idx, unsigned int order, unsigned int buddy) {
    uart_puts("[*] Buddy found! buddy idx: ");
    uart_int(buddy);
    uart_puts(" for page ");
    uart_int(idx);
    uart_puts(" with order ");
    uart_int(order);
    uart_puts("\n");
}
*/
// [Page] Allocate 0x4000 at order 1, page 4. Next address at order 1: 0x6000
// static void log_page_alloc(unsigned int idx, unsigned int order) {
//     uart_puts("[Page] Allocate ");
//     uart_hex(memory_base + (unsigned long)idx * PAGE_SIZE);
//     uart_puts(" at order ");
//     uart_int(order);
//     uart_puts(", page ");
//     uart_int(idx);
//     uart_puts(". Next block at order ");
//     uart_int(order);
//     uart_puts(": ");

//     if (list_empty(&free_area[order])) {
//         uart_puts("no next node");
//     } else {
//         struct list_head *node = free_area[order].next;//the node after head node has meaningful data
//         struct page *page = container_of(node, struct page, node);//page is a pointer 
//         unsigned int next_idx = page_addr_to_idx(page);//addr convert to idx
//         uart_hex(memory_base + (unsigned long)next_idx * PAGE_SIZE);//page index to physical address
//     }

//     uart_puts("\n");
// }
/*
//variant to print the same thing but says for chunk refill
static void log_chunk_refill_page(unsigned int idx, unsigned int order, int pool_idx) {
    uart_puts("[ChunkPage] Refill ");
    uart_hex(memory_base + (unsigned long)idx * PAGE_SIZE);
    uart_puts(" at order ");
    uart_int(order);
    uart_puts(" for pool ");
    uart_int(pool_idx);
    uart_puts(", page ");
    uart_int(idx);
    uart_puts(". Next block at order ");
    uart_int(order);
    uart_puts(": ");

    if (list_empty(&free_area[order])) {
        uart_puts("no next node");
    } else {
        struct list_head *node = free_area[order].next;
        struct page *page = container_of(node, struct page, node);
        unsigned int next_idx = page_addr_to_idx(page);
        uart_hex(memory_base + (unsigned long)next_idx * PAGE_SIZE);
    }

    uart_puts("\n");
}
// [Page] Free 0x4000 and add back to order 2, page 4. Next address at order 2: 0x904000
static void log_page_free(unsigned int idx, unsigned int order) {
    uart_puts("[Page] Free ");
    uart_hex(memory_base + (unsigned long)idx * PAGE_SIZE);
    uart_puts(" and add back to order ");
    uart_int(order);
    uart_puts(", page ");
    uart_int(idx);
    uart_puts(". Next block at order ");
    uart_int(order);
    uart_puts(": ");

    if (list_empty(&free_area[order])) {
        uart_puts("no next node");
    } else {
        struct list_head *node = free_area[order].next;
        struct page *next_page = container_of(node, struct page, node);
        unsigned int next_idx = page_addr_to_idx(next_page);
        uart_hex(memory_base + (unsigned long)next_idx * PAGE_SIZE);
    }

    uart_puts("\n");
}
*/
//  [Chunk] Allocate 0x3030 at chunk size 16
// static void log_chunk_alloc(void *ptr, unsigned int chunk_size) {
//     uart_puts("[Chunk] Allocate ");
//     uart_hex((unsigned long)ptr);
//     uart_puts(" at chunk size ");
//     uart_int(chunk_size);
//     uart_puts("\n");
// }
// //  [Chunk] Free 0x3030 at chunk size 16
// static void log_chunk_free(void *ptr, unsigned int chunk_size) {
//     uart_puts("[Chunk] Free ");
//     uart_hex((unsigned long)ptr);
//     uart_puts(" at chunk size ");
//     uart_int(chunk_size);
//     uart_puts("\n");
// }
/*
//  [Reserve] Reserve address [0x0, 0x1000). Range of pages: [0, 1)
static void log_reserve(unsigned long start, unsigned long end) {
    unsigned long start_page;
    unsigned long end_page;

    start_page = (start - memory_base) / PAGE_SIZE;
    end_page = (end - memory_base + PAGE_SIZE - 1) / PAGE_SIZE;//ceiling division, end need to be page that contains no reserved area
    //start <= x < end
    uart_puts("[Reserve] Reserve address [");
    uart_hex(start);
    uart_puts(", ");
    uart_hex(end);
    uart_puts("). Range of pages: [");
    uart_int((unsigned int)start_page);
    uart_puts(", ");
    uart_int((unsigned int)end_page);
    uart_puts(")\n");
}

static void log_buddy_free_area_state(const char *tag) {
    unsigned int order;

    uart_puts("[Buddy Free Lists] ");
    uart_puts(tag);
    uart_puts("\n");

    for (order = 0; order <= MAX_ORDER; order++) {
        uart_puts("  order ");
        uart_int(order);
        uart_puts(": ");
        uart_int(free_area_count(order));
        uart_puts(" blocks\n");
    }
}
*/
//end of logging code

//this files should have most of the LAB3
//I no longer adopt the data structure from lab page, now it uses structure from exercise
//order >= 0 && refcount == 0: this entry is the head of a free block of size 2^order * 4KB
//order >= 0 && refcount > 0: this entry is the head of an allocated/reserved block
//order == -1: this frame is inside a larger block, so not directly allocable

//the smallest pool that can hold a request of size bytes, returns pool index, or -1 if not found
static int size_to_pool_idx(unsigned int size) {
    int i;

    for (i = 0; i < NUM_POOLS; i++) {
        if (size <= pools[i].chunk_size)
            return i;
    }

    return -1;
}

//those 2 functions maps addr in mem_map to real RAM addr

//use idx to get the actual addr of the page, *page is pointer to some page struct in mem_map
static void *page_addr_to_phys_addr(struct page *page) {
    unsigned int idx = page_addr_to_idx(page);
    return (void *)(memory_base  + (unsigned long)idx * PAGE_SIZE);//unsigned long for 64 bit math
}//cast to void: change int addr to pointer

//find the index of a page that some addr belong to 
static struct page *phys_addr_to_page_addr(void *ptr) {
    unsigned long addr = (unsigned long)ptr;//cast pointer to integer 64bit value
    unsigned int idx;
    //memory_base <= addr < memory_end
    if (addr < memory_base || addr >= memory_end)//illegal access
        return 0;
    //find which page contains the addr
    idx = (unsigned int)((addr - memory_base ) / PAGE_SIZE);//get floor because the starting addr contains next 4kb
    if (idx >= managed_pages)//illegal if exceeding max page
        return 0;

    return &mem_map[idx];//returns pointer to the addr of page in mem_map
}




// for lab 6 add translator from page addr to v addr, enabling MMU, CPU expects all addr are VA
static void *page_addr_to_virt_addr(struct page *page)
{
    return (void *)phys_to_virt((unsigned long)page_addr_to_phys_addr(page));
}

static struct page *virt_addr_to_page_addr(void *ptr)
{
    unsigned long va = (unsigned long)ptr;
    unsigned long pa = virt_to_phys(va);

    return phys_addr_to_page_addr((void *)pa);
}

int retain_page(void *ptr)
{
    struct page *page;

    if (!ptr)
        return -1;

    page = virt_addr_to_page_addr(ptr);
    if (!page || page->alloc_type != ALLOC_TYPE_PAGE || page->refcount <= 0)
        return -1;

    page->refcount++;
    return 0;
}

//if dynamic allocator finds empty pool, ask buddy for a page, and partition that into chunks
static int request_page_for_chunk(int pool_idx) {
    struct page *page;//pointer to a page metadata from mem_map
    char *base;//start addr of the page in memory
    unsigned int chunk_size;//
    unsigned int n_chunks;//this page can give n chunks
    unsigned int i;//loop

    //check pool index valid
    if (pool_idx < 0 || pool_idx >= NUM_POOLS)
        return -1;

    //ask buddy for page of order 0 (1 page)
    page = alloc_pages(0);  // get pointer to one 4KB page in mem_map
    if (page == 0)//check if null pointer
        return -1;

    //log_chunk_refill_page(page_addr_to_idx(page), 0, pool_idx);
    
    chunk_size = pools[pool_idx].chunk_size;//struct pool saves chunksize attribute
    base = (char *)page_addr_to_virt_addr(page);//base points to first byte of the requested page, cast to char so math in bytes (smallest unit in addr)
    n_chunks = PAGE_SIZE / chunk_size;

    //mark page metadata
    page->alloc_type = ALLOC_TYPE_CHUNK;
    page->pool_idx = pool_idx;
    page->allocated_chunks = 0;//initially no chunk from this page is allocated yet

    //loop all chunks
    for (i = 0; i < n_chunks; i++) {
        struct chunk *c = (struct chunk *)(base + i * chunk_size);//chunk 0 starts at base + 0 * chunk_size, chunk 1 starts at base + 1 * chunk_size ...
        //make c point to the current first free chunk, then make c become the new first free chunk
        c->next = pools[pool_idx].free_list;//when chunk is free, the allocator uses the beginning of that chunk to store next pointer.
        pools[pool_idx].free_list = c;//new chunk added to the head of the list
        //chunk order is reversed but i think no problem
    }

    return 0;
}

//for request that is smaller than 2048, use this function to allocate a chunk, returns pointer to the chunk
static void *dynamic_chunk_allocate(unsigned int size) {
    int pool_idx;//the request size should be served from which pool
    struct chunk *c;
    struct page *page;//addr in mem_map

    //find the smallest chunk for request
    pool_idx = size_to_pool_idx(size);
    if (pool_idx < 0)//the function returns -1 on not found
        return 0;

    if (pools[pool_idx].free_list == 0) {//if free list is empty(nullptr)
        if (request_page_for_chunk(pool_idx) < 0)//if request fail returns -1
            return 0;
    }

    c = pools[pool_idx].free_list;//Let c be the current head of the pool’s free list
    pools[pool_idx].free_list = c->next;//Move head to the next free chunk, removes c from the front of the free list.

    page = virt_addr_to_page_addr((void *)c);//find which page this chunk belongs to, page is a pointer for mem_map
    if (page == 0)//check if fail
        return 0;

    page->allocated_chunks++;//one more allocated chunk in this page, keep this to know when to free page

    // log_chunk_alloc((void *)c, pools[pool_idx].chunk_size);
    return (void *)c;//return addr of the chunk
}

//for request bigger than 2048, use this to find the order needed, the function to allocate page takes order as param
static unsigned int size_to_order(unsigned int size) {
    unsigned int needed_pages;
    unsigned int order;
    unsigned int block_pages;

    needed_pages = (size + PAGE_SIZE - 1) / PAGE_SIZE;//ceiling, less than 4kb would become 1 page

    order = 0;
    block_pages = 1;
    //loop until the block exceeds needed pages
    while (block_pages < needed_pages) {//block_pages = 2^order
        block_pages <<= 1;
        order++;
    }

    return order;//returns order since page_allocate expects order as input
} 

//this function should only be called when size > biggest chunk (2048)
static void *dynamic_page_allocate(unsigned int size) {
    struct page *page;
    unsigned int order;
    unsigned int idx;

    //convert size > 2048 to order
    order = size_to_order(size);
    if (order > MAX_ORDER)//return exceeding order 10
        return 0;
    //request a block from buddy, returns pointer to that page in mem_map
    page = alloc_pages(order);
    if (page == 0)//return if alloc fail
        return 0;

    idx = page_addr_to_idx(page);//get the index in mem_map to udpate the metadata
    //only update metadata regarding chunk management
    mem_map[idx].alloc_type = ALLOC_TYPE_PAGE;
    mem_map[idx].pool_idx = -1;//doesn't belong to any chunk pool
    mem_map[idx].allocated_chunks = 0;

    //log_page_alloc(idx, order);
    return page_addr_to_virt_addr(page);//returns actual addr of the block
}


//remove all free chunks that belong to this page from the pool free list
//needed before returning an empty chunk page back to buddy system
static void remove_page_chunks_from_pool(struct page *page) {
    int pool_idx;
    struct chunk *prev;
    struct chunk *curr;
    unsigned long page_start;
    unsigned long page_end;

    pool_idx = page->pool_idx;//this attribute added when a page is used for chunk pool, indicating which size of pool
    if (pool_idx < 0 || pool_idx >= NUM_POOLS)
        return;

    page_start = (unsigned long)page_addr_to_virt_addr(page);//start addr of this page in RAM
    page_end = page_start + PAGE_SIZE;//end addr of this page

    prev = 0;//nullptr, no prev yet
    curr = pools[pool_idx].free_list;//traverse the pool free list

    while (curr != 0) {//last node points to null
        struct chunk *next = curr->next;//next chunk, curr might be removed, need to know next first
        unsigned long addr = (unsigned long)curr;

        //if this chunk is inside the page range, remove it from free list
        if (addr >= page_start && addr < page_end) {//if the addr of curr chunk is inside this page, remove it from the list
            if (prev == 0)//if prev==0, curr is the head of list, move head of list to next node to remove it
                pools[pool_idx].free_list = next;
            else//curr node it not head, skips curr node by linking prev to next
                prev->next = next;
        } else {//curr node is not in the list, move prev
            prev = curr;
        }

        curr = next;//move curr
    }
}

//if we are freeing a chunk, just add it to free list, need page metadata to know the pool index, if all chunks form a page is free, free the page also
//ptr = chunk addr
static void dynamic_chunk_free(void *ptr, struct page *page) {
    int pool_idx;
    struct chunk *c;
    unsigned int idx;//idx for mem_map

    pool_idx = page->pool_idx;
    if (pool_idx < 0 || pool_idx >= NUM_POOLS)
        return;

    c = (struct chunk *)ptr;//treat pointer as a chunk node, so it can be linked to free list
    //log_chunk_free(ptr, pools[pool_idx].chunk_size);
    c->next = pools[pool_idx].free_list;//c points to first chunk of free list -> push the chunk back to the head of free list
    pools[pool_idx].free_list = c;//now head points to c

    //error if number for allocaed chunk recorded is wrong
    if (page->allocated_chunks == 0)
        return;

    page->allocated_chunks--;//one chunk is free

    //if all chunks in this page are free, return this page back to buddy system
    if (page->allocated_chunks == 0) {
        idx = page_addr_to_idx(page);//idx in mem_map

        //remove all free chunks of this page from pool free list first
        //otherwise the pool free list would keep stale pointers
        remove_page_chunks_from_pool(page);

        //update metadata
        mem_map[idx].alloc_type = ALLOC_TYPE_NONE;
        mem_map[idx].pool_idx = -1;
        mem_map[idx].allocated_chunks = 0;

        free_pages(page);
        //log_page_free(idx, 0);//log moved to free_pages
    }
}

//just s wrapper function that cleanas metadata about dynamic memory management
static void dynamic_page_free(void *ptr, struct page *page) {
    unsigned int idx;
    //unsigned int order;

    (void)ptr;//make compiler stop complaining, passed ptr because chunk one did

    idx = page_addr_to_idx(page);
    //lab 6, for allocated program image page, more than 1 user VA might be mapped to it, do not free it if someone is still using
    if (mem_map[idx].refcount > 1) {
        mem_map[idx].refcount--;
        return;
    }

    //order = page->order;//use index to access mem_map

    //for block, only head metadata is meaningful
    mem_map[idx].alloc_type = ALLOC_TYPE_NONE;
    mem_map[idx].pool_idx = -1;
    mem_map[idx].allocated_chunks = 0;

    free_pages(page);
    //log_page_free(idx, order);//move to free_pages
}

//free will check the allocation type to decide which func to call
void free(void *ptr) {
    struct page *page;

    if (ptr == 0)//check input valid
        return;

    page = virt_addr_to_page_addr(ptr);//get the page addr in mem_map
    if (page == 0)
        return;
    //use alloc_type attribute to decide which free to use
    if (page->alloc_type == ALLOC_TYPE_CHUNK) {
        dynamic_chunk_free(ptr, page);
        return;
    }

    if (page->alloc_type == ALLOC_TYPE_PAGE) {
        if (page->order < 0)//not allocated
            return;
        dynamic_page_free(ptr, page);
        return;
    }
}

//
void *allocate(unsigned int size) {
    if (size == 0)//check input valid
        return 0;

    if (size > MAX_CHUNK_SIZE)//if > 2048, allocate page
        return dynamic_page_allocate(size);
    //else allocate chunks
    return dynamic_chunk_allocate(size);
}

//helper function for initial testing, might need it to run page_alloc_test.c 
int free_area_count(unsigned int order) {
    int cnt = 0;
    struct list_head *pos;//iterator to walk throught linked list
    //start with node after head, because head has no data
    //circular doubly linked list, when pointed to head means had traversed whole list
    for (pos = free_area[order].next; pos != &free_area[order]; pos = pos->next) {
        cnt++;
    }

    return cnt;
}

//basic exercise 1
//also for advanced exercise 1,
//lookup time = O(1) page_addr_to_idx or page_idx_to_addr, which are just conversions of constant time

//
struct page* alloc_pages(unsigned int order) {
    if (order > MAX_ORDER)//return nullptr if order > 10
        return 0;

    //find smallest free list with order >= requested
    for (unsigned int current_order = order; current_order <= MAX_ORDER; current_order++) {
        if (!list_empty(&free_area[current_order])) {//free_area[current_order] is the head node, list_empty want a pointer
            struct list_head *node;
            struct page *page;
            unsigned int idx;

            //first remove the selected block

            //take first block from free list
            node = free_area[current_order].next;
            list_del(node);
            //get addr of the page struct
            //free list stores nodes, not pages, so we get the page addr
            page = container_of(node, struct page, node);//page is the pointer to addr of the struct
            idx = page_addr_to_idx(page);
            //log_remove_block(idx, current_order);

            //split until requested size

            while (current_order > order) {
                struct page *right_buddy;
                unsigned int right_idx;

                current_order--;

                //right half starts at idx + 2^current_order, current_order already -1
                right_idx = idx + (1U << current_order);
                right_buddy = page_idx_to_addr(right_idx);//addr to the mem_map struct

                //when split, left buddy and right buddy both free, we can chhose to use left buddy

                //left half becomes smaller free block
                mem_map[idx].order = current_order;
                mem_map[idx].refcount = 0;
                mem_map[idx].alloc_type = ALLOC_TYPE_NONE;
                mem_map[idx].pool_idx = -1;

                //right half becomes smaller free block
                right_buddy->order = current_order;
                right_buddy->refcount = 0;
                right_buddy->alloc_type = ALLOC_TYPE_NONE;
                right_buddy->pool_idx = -1;

                //initialize buddy's node, and add it to free list, only for right buddy, left buddy continue splitting
                INIT_LIST_HEAD(&right_buddy->node);//new block created by splitting, not connected to any list, calling init is okay
                list_add_tail(&right_buddy->node, &free_area[current_order]);
                //log_add_block(right_idx, current_order);
            }

            //after the while loop, the idx should point to a block of request order
            //only first page has meaningful metadata
            mem_map[idx].order = order;
            mem_map[idx].refcount = 1;
            mem_map[idx].alloc_type = ALLOC_TYPE_NONE;//this is for dynamic allocator, will update in the function calling alloc_pages
            mem_map[idx].pool_idx = -1;//same^
            
            //log_buddy_free_area_state("after alloc_pages()");
            return &mem_map[idx];//return pointer to page metadata
        }
    }
    
    return 0;//here means no block could be allocated
}


void free_pages(struct page *page) {
    unsigned int idx;
    unsigned int order;
    //check null page
    if (page == 0)
        return;

    idx = page_addr_to_idx(page);

    //check head metadata not allocated
    if (mem_map[idx].order < 0 || mem_map[idx].refcount == 0)
        return;

    order = mem_map[idx].order;

    //if block is free, refcount should say 0
    mem_map[idx].refcount = 0;
    mem_map[idx].alloc_type = ALLOC_TYPE_NONE;//clear attribute about dynamic allocator
    mem_map[idx].pool_idx = -1;
    INIT_LIST_HEAD(&mem_map[idx].node);//okay to init because an allocated block 's node is already broken from free list

    //merge upward
    while (order < MAX_ORDER) {//try to merge to highest possible order, break early if no buddy
        unsigned int bidx = buddy_idx(idx, order);

        if (bidx >= managed_pages)//check if buddy out of range
            break;

        //check if buddy is same order (could have been split to smaller order) and free
        if (mem_map[bidx].order != (int)order || mem_map[bidx].refcount != 0)
            break;//breaks merging if fail

        //remove from free list
        //log_buddy_found(idx, order, bidx);
        //log_remove_block(bidx, order);
        list_del(&mem_map[bidx].node);//remove from free list

        //this old buddy head need reset for data
        mem_map[bidx].order = -1;//the order might change or become invalid 
        mem_map[bidx].refcount = 1;
        mem_map[bidx].alloc_type = ALLOC_TYPE_NONE;
        mem_map[bidx].pool_idx = -1;

        //if buddy is the lower addr, clear the block at idx
        if (bidx < idx) {
            mem_map[idx].order = -1;
            mem_map[idx].refcount = 1;
            mem_map[idx].alloc_type = ALLOC_TYPE_NONE;
            mem_map[idx].pool_idx = -1;
            idx = bidx;
        }

        order++;//order increments, so after loop we know which free list to insert
        //now it is order +1
        //fill head of new block
        //need to update the block head so next iteration would get correct metadata to check for merge
        mem_map[idx].order = order;
        mem_map[idx].refcount = 0;
        mem_map[idx].alloc_type = ALLOC_TYPE_NONE;
        mem_map[idx].pool_idx = -1;
        INIT_LIST_HEAD(&mem_map[idx].node);
    }

    //insert final merged block into corresponding free list
    list_add_tail(&mem_map[idx].node, &free_area[order]);//insert new node to tail of free list
    //log_page_free(idx, order);
    //log_add_block(idx, order);
    //log_buddy_free_area_state("after free_pages()");
}


//end of basic exercise 1


//memory reserve
void memory_reserve(unsigned long base, size_t size) {//input is RAM addr
    unsigned int start_pfn;
    unsigned int end_pfn;
    int order;
    unsigned long start;//for logging
    unsigned long end;

    if (size == 0)//check size valid
        return;

    start = base;
    end = base + size;

    //safe gaurds, should work only inside 2GB RAM
    if (end <= memory_base || start >= memory_end)
        return;
    if (start < memory_base)
        start = memory_base;
    if (end > memory_end)
        end = memory_end;

    //start_pfn, start_pfn+1, ..., end_pfn-1
    //floor division for start
    start_pfn = (unsigned int)((start - memory_base) / PAGE_SIZE);
    //end pfn should contain no reserved data, use cieling divison
    end_pfn = (unsigned int)((end - memory_base + PAGE_SIZE - 1) / PAGE_SIZE);

    //log_reserve(start, end);

    for (order = MAX_ORDER; order >= 0; order--) {//scan from largest block size
        struct list_head *pos = free_area[order].next;//start with first  free block in the list

        while (pos != &free_area[order]) {//until point back to head
            struct list_head *next = pos->next;//save next, pos might be removed later
            //get page struct
            struct page *page = container_of(pos, struct page, node);
            unsigned int idx = page_addr_to_idx(page);
            unsigned int block_start = idx;
            unsigned int block_end = idx + (1U << order);

            //no overlap
            if (block_end <= start_pfn || block_start >= end_pfn) {
                pos = next;//check next node
                continue;
            }

            //if partially overlapped or full overlapped, need to remove the block regardless
            //remove overlapped block from free list first
            //log_remove_block(idx, order);
            list_del(pos);//remove from list

            //fully covered by reserved range, whole block need to be reserved
            if (start_pfn <= block_start && block_end <= end_pfn) {
                mem_map[idx].order = order;
                mem_map[idx].refcount = 1;//refcount > 0 indicates used
                mem_map[idx].alloc_type = ALLOC_TYPE_NONE;
                mem_map[idx].pool_idx = -1;
                mem_map[idx].allocated_chunks = 0;

                pos = next;
                continue;
            }

            //partial overlap: split into two smaller buddies
            if (order > 0) {//check if can still split
                unsigned int half = 1U << (order - 1);
                unsigned int left_idx = idx;
                unsigned int right_idx = idx + half;

                //current big block head is no longer a valid head after split
                mem_map[idx].order = -1;
                mem_map[idx].refcount = 1;
                mem_map[idx].alloc_type = ALLOC_TYPE_NONE;
                mem_map[idx].pool_idx = -1;
                mem_map[idx].allocated_chunks = 0;

                //left block head
                mem_map[left_idx].order = order - 1;
                mem_map[left_idx].refcount = 0;
                mem_map[left_idx].alloc_type = ALLOC_TYPE_NONE;
                mem_map[left_idx].pool_idx = -1;
                mem_map[left_idx].allocated_chunks = 0;
                INIT_LIST_HEAD(&mem_map[left_idx].node);
                list_add_tail(&mem_map[left_idx].node, &free_area[order - 1]);//add to free list so next scan will find it, and check partially or fully cover again
                //log_add_block(left_idx, order - 1);

                //right block head
                mem_map[right_idx].order = order - 1;
                mem_map[right_idx].refcount = 0;
                mem_map[right_idx].alloc_type = ALLOC_TYPE_NONE;
                mem_map[right_idx].pool_idx = -1;
                mem_map[right_idx].allocated_chunks = 0;
                INIT_LIST_HEAD(&mem_map[right_idx].node);
                list_add_tail(&mem_map[right_idx].node, &free_area[order - 1]);
                //log_add_block(right_idx, order - 1);
            } else {
                //order 0 and partial overlap, cannot split, just reserve the page
                mem_map[idx].order = 0;
                mem_map[idx].refcount = 1;
                mem_map[idx].alloc_type = ALLOC_TYPE_NONE;
                mem_map[idx].pool_idx = -1;
                mem_map[idx].allocated_chunks = 0;
            }

            pos = next;//next block in the free list
        }
    }
}

//align to pages
static uintptr_t align_up_uintptr(uintptr_t x, uintptr_t a) {
    return (x + a - 1) & ~(a - 1);
}

static uintptr_t align_down_uintptr(uintptr_t x, uintptr_t a) {
    return x & ~(a - 1);
}


int find_mem_map_region(const void *fdt,
                        uintptr_t *out_start,
                        uintptr_t *out_end) {
    uintptr_t mem_start;
    uintptr_t mem_end;
    uintptr_t kernel_start;
    uintptr_t kernel_end;
    uintptr_t dtb_start;
    uintptr_t dtb_end;
    uintptr_t initrd_start;
    uintptr_t initrd_end;
    uintptr_t candidate;//used to find place to fit the mem_map
    uintptr_t candidate_end;
    unsigned int managed_pages;
    unsigned long mem_map_size;
    int fdt_size;

    struct reserved_region regions[MAX_RESERVED_REGIONS];//this is for /reserved_memory parsing result,  MAX_RESERVED_REGIONS is hardcoded to 32
    int reserved_count = 0;//how many iteration to run

    mem_start = align_up_uintptr(get_mem_start(fdt), PAGE_SIZE);
    mem_end = align_down_uintptr(get_mem_end(fdt), PAGE_SIZE);

    if (mem_end <= mem_start)//check RAM valid
        return 0;

    managed_pages = (unsigned int)((mem_end - mem_start) / PAGE_SIZE);//check if later work is meaningful
    if (managed_pages == 0)
        return 0;

    mem_map_size = (unsigned long)managed_pages * sizeof(struct page);//the area needed to place mem_map
    mem_map_size = align_up_uintptr(mem_map_size, PAGE_SIZE);//to 4kb


    //modified to use regions[] to handle reservation, or code looks too messy
    //lab6 this part was from linker, which modified to virt addr, but buddy system manages phys addr underlying
    kernel_start = virt_to_phys((uintptr_t)_start);//from linker, reserve physical RAM
    kernel_end = virt_to_phys((uintptr_t)_end);
    regions[reserved_count].start = kernel_start;
    regions[reserved_count].end = kernel_end;
    reserved_count++;
    //the passed DTB is now all virt, but buddy system need to see phys addr.
    dtb_start = virt_to_phys((uintptr_t)fdt);//from param, fdt pointer is virtual after VM
    fdt_size = fdt_get_size(fdt);
    if (fdt_size > 0) {
        dtb_end = dtb_start + (uintptr_t)fdt_size;
        regions[reserved_count].start = dtb_start;
        regions[reserved_count].end = dtb_end;
        reserved_count++;
    }

    initrd_start = get_initrd_start(fdt);//from dt_parse
    initrd_end = get_initrd_end(fdt);
    if (initrd_start && initrd_end && initrd_end > initrd_start) {//check valid
        regions[reserved_count].start = initrd_start;
        regions[reserved_count].end = initrd_end;
        reserved_count++;
    }

    reserved_count += fdt_get_reserved_regions(//this function adds subnode in /reserved_memory to the region[]
        fdt,
        &regions[reserved_count],
        MAX_RESERVED_REGIONS - reserved_count
    );

    candidate = align_up_uintptr(mem_start, PAGE_SIZE);//the addr to put mem_map, need to align to 4kb

    for (;;) {
        int conflict = 0;

        candidate = align_up_uintptr(candidate, PAGE_SIZE);
        candidate_end = candidate + mem_map_size;

        if (candidate_end > mem_end)//loop to end of RAM and not found
            return 0;

        for (int i = 0; i < reserved_count; i++) {//check every reserved memory in region
            uintptr_t rs = align_down_uintptr(regions[i].start, PAGE_SIZE);
            uintptr_t re = align_up_uintptr(regions[i].end, PAGE_SIZE);

            //if reserve end smaller than candicate start. or reserve start bigger than candidate end 
            //condition for no overlap
            if (re <= candidate || rs >= candidate_end)//can use <= and >= because both are half open
                continue;//check next reserved memory

            candidate = re;//move the candidate to the end of overlapping region, check this region in next iteration
            conflict = 1;//flag indicate current candidate overlaps
            break;
        }
        //if the check loop leave without raising conflict flag, the candidate is valid, return the candidate
        if (!conflict) {
            *out_start = candidate;
            *out_end = candidate_end;
            return 1;
        }
    }
}


void page_alloc_init(const void *fdt) {
    uintptr_t mem_start;
    uintptr_t mem_end;
    uintptr_t kernel_start;
    uintptr_t kernel_end;
    uintptr_t dtb_start;
    uintptr_t dtb_end;
    uintptr_t initrd_start;
    uintptr_t initrd_end;
    int fdt_size;

    struct reserved_region regions[MAX_RESERVED_REGIONS];
    int reserved_count = 0;

    uintptr_t mem_map_start;
    uintptr_t mem_map_end;

    if (!find_mem_map_region(fdt, &mem_map_start, &mem_map_end)) {//function returns 1 on success
        uart_puts("cannot find space for mem_map\n");
        while (1) { }
    }
    //later directly touches mem_map[i], so we need to use virt addr
    mem_map = (struct page *)phys_to_virt(mem_map_start);

    uart_puts("[MemMap] start = ");
    uart_hex((unsigned long)mem_map_start);
    uart_puts(", end = ");
    uart_hex((unsigned long)mem_map_end);
    uart_puts(", size = ");
    uart_hex((unsigned long)(mem_map_end - mem_map_start));
    uart_puts("\n");

    mem_start = get_mem_start(fdt);
    mem_end = get_mem_end(fdt);

    mem_start = align_up_uintptr(mem_start, PAGE_SIZE);//align down would touch illegal memory
    mem_end = align_down_uintptr(mem_end, PAGE_SIZE);//... up ...

    if (mem_end <= mem_start) {//check RAM valid
        uart_puts("invalid memory range from dtb\n");
        while (1) { }
    }

    memory_base = mem_start;
    memory_end = mem_end;
    managed_pages = (unsigned int)((memory_end - memory_base) / PAGE_SIZE);//total number of pages, later will do initialization

    if (managed_pages == 0) {
        uart_puts("no usable pages from dtb\n");
        while (1) { }
    }

    if (managed_pages > MAX_MANAGED_PAGES) {//I set 8GB worth of pages, should not exceed
        uart_puts("managed_pages exceeds MAX_MANAGED_PAGES\n");
        while (1) { }
    }

    //log memory
    uart_puts("[Mem] base = ");
    uart_hex((unsigned long)memory_base);
    uart_puts(", end = ");
    uart_hex((unsigned long)memory_end);
    uart_puts(", pages = ");
    uart_int((int)managed_pages);
    uart_puts("\n");

    //initialize region array so unused names are empty
    for (int k = 0; k < MAX_RESERVED_REGIONS; k++) {
        regions[k].name[0] = '\0';
        regions[k].start = 0;
        regions[k].end = 0;
    }

    //initialize all free-list heads
    for (int i = 0; i <= MAX_ORDER; i++) {
        INIT_LIST_HEAD(&free_area[i]);
    }

    //mark all frames as unavailable first
    for (int i = 0; i < managed_pages; i++) {
        mem_map[i].order = -1;
        mem_map[i].refcount = 1;
        mem_map[i].alloc_type = ALLOC_TYPE_NONE;
        mem_map[i].pool_idx = -1;
        mem_map[i].allocated_chunks = 0;
        INIT_LIST_HEAD(&mem_map[i].node);
    }

    //split the whole managed region into largest possible aligned blocks
    int i = 0;
    while ((unsigned int)i < managed_pages) {
        unsigned int order = MAX_ORDER;//try largest order first


        //2GB 2^31 aligns to 2^19 4kb's(2^12), should not need to cut to smaller block
        while (order > 0) {//try smaller order and see if completely fit
            unsigned int block_pages = 1U << order;

            if (((unsigned int)i % block_pages) == 0 &&// i must be multiple of block size for alignment
                (unsigned int)i + block_pages <= managed_pages) {//the whole block would not exceed
                break;
            }

            order--;
        }

        mem_map[i].order = (int)order;
        mem_map[i].refcount = 0;
        mem_map[i].alloc_type = ALLOC_TYPE_NONE;
        mem_map[i].pool_idx = -1;
        mem_map[i].allocated_chunks = 0;
        INIT_LIST_HEAD(&mem_map[i].node);
        list_add_tail(&mem_map[i].node, &free_area[order]);
        //log_add_block((unsigned int)i, order);

        i += (1 << order);//move i paste the sizse of block
    }
    //modified the code to use region[] to do stuff
    //MAX_RESERVED_REGIONS set to 32, theres like 14 or 15 regions to reserve only
    //collect kernel region 
    kernel_start = virt_to_phys((uintptr_t)_start);
    kernel_end = virt_to_phys((uintptr_t)_end);
    strcpy(regions[reserved_count].name, "kernel");
    regions[reserved_count].start = kernel_start;
    regions[reserved_count].end = kernel_end;
    reserved_count++;

    //collect dtb region
    dtb_start = virt_to_phys((uintptr_t)fdt);
    fdt_size = fdt_get_size(fdt);
    if (fdt_size > 0 && reserved_count < MAX_RESERVED_REGIONS) {
        dtb_end = dtb_start + (uintptr_t)fdt_size;
        strcpy(regions[reserved_count].name, "dtb");
        regions[reserved_count].start = dtb_start;
        regions[reserved_count].end = dtb_end;
        reserved_count++;
    }

    //collect initrd region
    initrd_start = get_initrd_start(fdt);
    initrd_end = get_initrd_end(fdt);
    if (initrd_start && initrd_end && initrd_end > initrd_start &&
        reserved_count < MAX_RESERVED_REGIONS) {
        strcpy(regions[reserved_count].name, "initrd");
        regions[reserved_count].start = initrd_start;
        regions[reserved_count].end = initrd_end;
        reserved_count++;
    }

    //collect /reserved-memory children
    if (reserved_count < MAX_RESERVED_REGIONS) {
        reserved_count += fdt_get_reserved_regions(
            fdt,
            &regions[reserved_count],
            MAX_RESERVED_REGIONS - reserved_count
        );
    }

    //collect mem_map
    if (reserved_count < MAX_RESERVED_REGIONS) {
        strcpy(regions[reserved_count].name, "frame_array");
        regions[reserved_count].start = mem_map_start;
        regions[reserved_count].end = mem_map_end;
        reserved_count++;
    }

    //reserve all collected regions exactly once
    for (int j = 0; j < reserved_count; j++) {//no need align, calculating pfn in the function does that already
        if (regions[j].start >= memory_end || regions[j].end <= memory_base)//if not in 2GB RAM don't reserve
            continue;

        memory_reserve((unsigned long)regions[j].start,//cast so it matches param
                       (size_t)(regions[j].end - regions[j].start));
    }

    //log all reserved memory
    uart_puts("[Reserved Regions]\n");

    uart_puts("  memory [");
    uart_hex((unsigned long)memory_base);
    uart_puts(", ");
    uart_hex((unsigned long)memory_end);
    uart_puts(")\n");

    for (int j = 0; j < reserved_count; j++) {
        uart_puts("  ");
        uart_int(j);
        uart_puts(": ");

        if (regions[j].name[0] != '\0') {
            uart_puts(regions[j].name);
            uart_puts(" ");
        }

        uart_puts("[");
        uart_hex((unsigned long)regions[j].start);
        uart_puts(", ");
        uart_hex((unsigned long)regions[j].end);
        uart_puts(")\n");
    }

    //log_buddy_free_area_state("initialized buddy list");
}
