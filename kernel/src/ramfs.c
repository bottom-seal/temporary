#include "vfs.h"
#include "page_alloc.h"
#include "str.h"
#include "initrd_parse.h"
#include "dt_parse.h"
#include "vm.h"

#define RAMFS_MAX_NAME       64
#define RAMFS_MAX_DIR_ENTRY  64

extern const void *boot_dtb_va;

enum ramfs_node_type {
    RAMFS_DIR,
    RAMFS_FILE,
};

struct ramfs_node {
    enum ramfs_node_type type;
    char name[RAMFS_MAX_NAME];

    const char* data;
    size_t size;

    struct vnode* entry[RAMFS_MAX_DIR_ENTRY];
};

static int ramfs_open(struct vnode* file_node, struct file** target);
static int ramfs_close(struct file* file);
static int ramfs_read(struct file* file, void* buf, size_t len);
static int ramfs_write(struct file* file, const void* buf, size_t len);

static int ramfs_lookup(struct vnode* dir_node,
                        struct vnode** target,
                        const char* component_name);
static int ramfs_create(struct vnode* dir_node,
                        struct vnode** target,
                        const char* component_name);
static int ramfs_mkdir(struct vnode* dir_node,
                       struct vnode** target,
                       const char* component_name);
static int ramfs_is_dir(struct vnode* node);

static struct file_operations ramfs_file_ops = {
    .open = ramfs_open,
    .close = ramfs_close,
    .read = ramfs_read,
    .write = ramfs_write,
};

static struct vnode_operations ramfs_vnode_ops = {
    .lookup = ramfs_lookup,
    .create = ramfs_create,
    .mkdir = ramfs_mkdir,
    .is_dir = ramfs_is_dir,
};

static int hextoi(const char* s, int n) {
    int r = 0;

    while (n-- > 0) {
        r <<= 4;

        if (*s >= 'A' && *s <= 'F')
            r += *s++ - 'A' + 10;
        else if (*s >= 'a' && *s <= 'f')
            r += *s++ - 'a' + 10;
        else if (*s >= '0' && *s <= '9')
            r += *s++ - '0';
        else
            return -1;
    }

    return r;
}

static int align4(int x) {
    return (x + 3) & ~3;
}
//different from tmpfs, inode should already have data
static struct vnode* ramfs_create_vnode(enum ramfs_node_type type,
                                        const char* name,
                                        const char* data,
                                        size_t size) {
    struct vnode* vnode = allocate(sizeof(struct vnode));
    if (!vnode)
        return 0;

    struct ramfs_node* node = allocate(sizeof(struct ramfs_node));
    if (!node) {
        free(vnode);
        return 0;
    }

    memset(vnode, 0, sizeof(struct vnode));
    memset(node, 0, sizeof(struct ramfs_node));

    node->type = type;
    node->data = data;
    node->size = size;
    //copy name with end of line
    if (name) {
        int i;

        for (i = 0; i < RAMFS_MAX_NAME - 1 && name[i]; i++)
            node->name[i] = name[i];

        node->name[i] = '\0';
    }

    vnode->v_ops = &ramfs_vnode_ops;
    vnode->f_ops = &ramfs_file_ops;
    vnode->internal = node;

    return vnode;
}
//only do insert to parent dir's entry 
static int ramfs_add_child(struct vnode* dir_vnode, struct vnode* child) {
    struct ramfs_node* dir;

    if (!dir_vnode || !child)
        return -1;

    dir = dir_vnode->internal;

    if (!dir || dir->type != RAMFS_DIR)
        return -1;

    for (int i = 0; i < RAMFS_MAX_DIR_ENTRY; i++) {
        if (!dir->entry[i]) {
            dir->entry[i] = child;
            child->parent = dir_vnode;
            return 0;
        }
    }

    return -1;
}
//get last component in a path (because ramfs doesn't preserve directory eg. bin/a.txt)
static const char* basename(const char* path) {
    const char* base = path;

    for (int i = 0; path[i]; i++) {
        if (path[i] == '/')
            base = &path[i + 1];
    }

    return base;
}
//for each file in CPIO, create a vnode with name, data addr and size, add it to ramfs root entry
static int ramfs_load_cpio(struct vnode* root, const void* rd) {
    const char* p = (const char*)rd;//rd is start of cpio

    if (!root || !rd)
        return -1;

    while (1) {
        //cpio file start with header
        const struct cpio_t* hdr = (const struct cpio_t*)p;
        //check is valid header cpio
        if (strncmp(hdr->magic, "070701", 6) != 0)
            return -1;
        //stores in hex, need to convert to int
        int namesize = hextoi(hdr->namesize, 8);
        int filesize = hextoi(hdr->filesize, 8);
        // check value legal
        if (namesize <= 0 || filesize < 0)
            return -1;
        //move past header
        const char* name = p + sizeof(struct cpio_t);
        //file name == means end of CPIO
        if (strcmp(name, "TRAILER!!!") == 0)
            return 0;
        //align to 4 bytes
        const char* data = p + align4(sizeof(struct cpio_t) + namesize);

        //remove preceding ./
        if (name[0] == '.' && name[1] == '/')
            name += 2;

        //get only file name (not bin/file)
        const char* file_name = basename(name);

        if (file_name[0] != '\0') {
            struct vnode* child =
                ramfs_create_vnode(RAMFS_FILE, file_name, data, filesize);

            if (!child)
                return -1;
            //add child to fs root's entry
            if (ramfs_add_child(root, child) != 0)
                return -1;
        }
        //move to next header
        p = data + align4(filesize);
    }
}

int ramfs_setup_mount(struct filesystem* fs, struct mount* mnt) {
    if (!fs || !mnt)
        return -1;
    //create root dir node
    struct vnode* root = ramfs_create_vnode(RAMFS_DIR, "", 0, 0);
    if (!root)
        return -1;

    uintptr_t initrd_start = get_initrd_start(boot_dtb_va);
    if (!initrd_start)
        return -1;
    //use VA
    if (ramfs_load_cpio(root, (const void*)phys_to_virt(initrd_start)) != 0)
        return -1;

    mnt->root = root;
    mnt->fs = fs;
    
    root->parent = root;//point to itself, when .. , it will check mnt struct to go back to tmpfs
    root->mounted_by = mnt;

    return 0;
}
//link file to a vnode
static int ramfs_open(struct vnode* file_node, struct file** target) {
    if (!file_node || !target || !(*target))
        return -1;

    (*target)->vnode = file_node;
    (*target)->f_ops = &ramfs_file_ops;
    (*target)->f_pos = 0;

    return 0;
}
//refcount checked in vfs layer
static int ramfs_close(struct file* file) {
    if (!file)
        return -1;

    free(file);
    return 0;
}
//same as tmpfs, read initrd addr
static int ramfs_read(struct file* file, void* buf, size_t len) {
    if (!file || !buf)
        return -1;

    struct ramfs_node* node = file->vnode->internal;

    if (!node || node->type != RAMFS_FILE)
        return -1;

    if (file->f_pos >= node->size)
        return 0;

    size_t readable = node->size - file->f_pos;

    if (len < readable)
        readable = len;

    memcpy(buf, node->data + file->f_pos, readable);
    file->f_pos += readable;

    return readable;
}

static int ramfs_write(struct file* file, const void* buf, size_t len) {
    //read only
    return -1;
}
//give dir and a component name, see if dir contains the node with component name
static int ramfs_lookup(struct vnode* dir_node,
                        struct vnode** target,
                        const char* component_name) {
    if (!dir_node || !target || !component_name)
        return -1;

    struct ramfs_node* dir = dir_node->internal;

    if (!dir || dir->type != RAMFS_DIR)
        return -1;

    for (int i = 0; i < RAMFS_MAX_DIR_ENTRY; i++) {
        if (!dir->entry[i])
            return -1;

        struct ramfs_node* child = dir->entry[i]->internal;

        if (child && strcmp(child->name, component_name) == 0) {
            *target = dir->entry[i];
            return 0;
        }
    }

    return -1;
}

static int ramfs_create(struct vnode* dir_node,
                        struct vnode** target,
                        const char* component_name) {
    return -1;
}

static int ramfs_mkdir(struct vnode* dir_node,
                       struct vnode** target,
                       const char* component_name) {
    return -1;
}

static int ramfs_is_dir(struct vnode* node) {
    if (!node || !node->internal)
        return 0;

    struct ramfs_node* rnode = node->internal;

    return rnode->type == RAMFS_DIR;
}