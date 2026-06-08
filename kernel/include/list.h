//https://elixir.bootlin.com/linux/v6.0/source/include/linux/list.h
#ifndef LIST_H
#define LIST_H

struct list_head {
    struct list_head *next;
    struct list_head *prev;
};
//35
static inline void INIT_LIST_HEAD(struct list_head *list) {
    //if the list is empty, the head points to itself in both directions for circular doubly linked list
    list->next = list;
    list->prev = list;
}

//check if head points to itself
static inline int list_empty(struct list_head *head) {
    return head->next == head;
}

//add a node after head
static inline void list_add(struct list_head *node, struct list_head *head) {
    node->next = head->next;//node's next point to what was head's next
    node->prev = head;//node prev point to head
    head->next->prev = node;//the node after head's prev now point to new node
    head->next = node;//now head's next point to new node
}
//insert to tail == insert before head
static inline void list_add_tail(struct list_head *node, struct list_head *head) {
    node->next = head;//node inserted before head -> next points to head
    node->prev = head->prev;//node's prev points to what was head's prev
    head->prev->next = node;//what was head's prev 's next now points to the new node
    head->prev = node;//head prev now points to new node
}

static inline void list_del(struct list_head *node) {
    node->prev->next = node->next;//what was before the node now points to the next node
    node->next->prev = node->prev;//same idea
    node->next = node;//pointing to self making it isolated
    node->prev = node;//not point to null because the list itself is not null terminated
}

//decided to add after lab4 because we keep using those
#define offsetof(TYPE, MEMBER) ((unsigned long)&(((TYPE *)0)->MEMBER))
#define container_of(ptr, type, member) \
    ((type *)((char *)(ptr) - offsetof(type, member)))
#define list_entry(ptr, type, member) container_of(ptr, type, member)


#endif