#include "vfs.h"
#include "page_alloc.h"
#include "str.h"

#define TMPFS_MAX_FILE_NAME 15
#define TMPFS_MAX_DIR_ENTRY 16
#define TMPFS_MAX_FILE_SIZE 4096

enum fsnode_type { FS_DIR, FS_FILE };

// internal data structure representing an inode exclusively for the tmpfs
struct tmpfs_vnode {
    enum fsnode_type type;//directory (FS_DIR) or a regular file (FS_FILE).
    char name[TMPFS_MAX_FILE_NAME];
    struct vnode* entry[TMPFS_MAX_DIR_ENTRY];//if this tmpfs_vnode is a directory, entry[] stores its children (vnodes)
    char* data;//mem addr
    size_t size;//mem size
};

//The table stores function addresses, so later code can call functions indirectly through the table
//designated initializer syntax, assign .field = value (vs positional)
struct file_operations tmpfs_file_ops = {
    .open = tmpfs_open,
    .close = tmpfs_close,
    .read = tmpfs_read,
    .write = tmpfs_write
};

struct vnode_operations tmpfs_vnode_ops = {
    .lookup = tmpfs_lookup,
    .create = tmpfs_create,
    .mkdir = tmpfs_mkdir,
    .is_dir = tmpfs_is_dir
};

//Allocate and fully initialize one inode, init VFS vnode too so it can point to FS vnode
struct vnode* tmpfs_create_vnode(enum fsnode_type type) {
    struct vnode* vnode = allocate(sizeof(struct vnode));
    if (!vnode)
        return 0;

    struct tmpfs_vnode* inode = allocate(sizeof(struct tmpfs_vnode));
    if (!inode) {
        free(vnode);
        return 0;
    }

    memset(vnode, 0, sizeof(struct vnode));
    memset(inode, 0, sizeof(struct tmpfs_vnode));

    inode->type = type;
    inode->data = 0;
    inode->size = 0;

    vnode->mount = 0;
    vnode->v_ops = &tmpfs_vnode_ops;
    vnode->f_ops = &tmpfs_file_ops;
    vnode->internal = inode;
    vnode->parent = 0;
    vnode->mounted_by = 0;

    return vnode;
}

//when tmpfs is mounted, creat root directory vnode, fill in fields of mount object
int tmpfs_setup_mount(struct filesystem* fs, struct mount* mnt) {
    if (!fs || !mnt)
        return -1;
    //allocate new vnode as root dir for new mounted fs
    mnt->root = tmpfs_create_vnode(FS_DIR);//root directory vnode of this tmpfs instance
    if (!mnt->root)
        return -1;

    mnt->fs = fs;
    mnt->root->parent = mnt->root;//root dir's parent is itself, prevent going above root
    mnt->root->mounted_by = mnt;//when path lookup on .. , it can find the struct mount, and check it to see mountpoint field
    return 0;
}

//handler allocated in vfs_open()
//initializes an opened file object for a tmpfs file (links handler to the vnode of the file)
//can modify pointer to file, (*f to Null -> *f to allocated mem)
int tmpfs_open(struct vnode* file_node, struct file** target) {
    if (!file_node || !target || !(*target))
        return -1;

    (*target)->vnode = file_node;
    (*target)->f_ops = &tmpfs_file_ops;
    (*target)->f_pos = 0;

    return 0;
}

//free file handler, doesn't free VFS vnode, tmpfs vnode, or data
int tmpfs_close(struct file* file) {
    if (!file)
        return -1;

    free(file);

    return 0;
}

//for a handler (accessing -> vnode -> inode), read len bytes from offset to buffer
int tmpfs_read(struct file* file, void* buf, size_t len) {
    if (!file || !buf)
        return -1;

    //inode is tmpfs vnode of the file
    struct tmpfs_vnode* inode = file->vnode->internal;

    //cannot read dir
    if (!inode || inode->type != FS_FILE)
        return -1;

    //need valid data
    if (!inode->data)
        return 0;

    //offset cannot exceed file size
    if (file->f_pos >= inode->size)
        return 0;

    //the data left that can be accessed
    size_t readable = inode->size - file->f_pos;

    //if len legal, read len, if exceed, read maximum readable
    if (len < readable)
        readable = len;

    //copy readable bytes, starting from offset of the handler
    memcpy(buf, inode->data + file->f_pos, readable);
    file->f_pos += readable;//update offset

    return readable;//return bytes read
}

//write len bytes from buffer to inode data starting from off set
int tmpfs_write(struct file* file, const void* buf, size_t len) {
    if (!file || !buf)
        return -1;

    struct tmpfs_vnode* inode = file->vnode->internal;

    //cannot write dir
    if (!inode || inode->type != FS_FILE)
        return -1;

    //check curr offset legal
    if (file->f_pos >= TMPFS_MAX_FILE_SIZE)
        return 0;

    //if inode doesn't points to valid data addr, allocate a file size
    if (!inode->data) {
        inode->data = allocate(TMPFS_MAX_FILE_SIZE);
        if (!inode->data)
            return -1;

        memset(inode->data, 0, TMPFS_MAX_FILE_SIZE);
    }

    //how much data still left
    size_t writable = TMPFS_MAX_FILE_SIZE - file->f_pos;

    //if len illgal, write only maximum left
    if (len < writable)
        writable = len;

    //write to data region starting from offset, from buffer, for len (or maximum left area)
    memcpy(inode->data + file->f_pos, buf, writable);
    file->f_pos += writable;//move offset

    //if writing exceeding orignal file size, update the file size too
    if (file->f_pos > inode->size)
        inode->size = file->f_pos;

    return writable;//return bytes written
}

//under a dir, check if file with component name exists, return with * target
int tmpfs_lookup(struct vnode* dir_node,//directory vnode to search for component_name
                 struct vnode** target,//return through target
                 const char* component_name) {
    if (!dir_node || !target || !component_name)
        return -1;

    struct tmpfs_vnode* dentry = dir_node->internal;//the inode for the dir

    if (!dentry || dentry->type != FS_DIR)
        return -1;

    for (int i = 0; i < TMPFS_MAX_DIR_ENTRY; i++) {//look for whole list
        if (!dentry->entry[i])//return not found if end of the list with value
            return -1;

        struct tmpfs_vnode* inode = dentry->entry[i]->internal;//entry saves vnode not inode

        if (!strcmp(inode->name, component_name)) {//on match, update pointer target to point to matched node
            *target = dentry->entry[i];
            return 0;
        }
    }

    return -1;
}

//helper for tmpfs_create and tmpfs_mkdir
//allocate vnode, inode, copy name to inode, register it under dir_node (entry list), return updated * target
static int tmpfs_create_child(struct vnode* dir_node,
                              struct vnode** target,
                              const char* component_name,
                              enum fsnode_type type) {//supports DIR and FILE
    //check input legal
    if (!dir_node || !target || !component_name)
        return -1;

    //check name correct
    size_t name_len = strlen(component_name);

    if (name_len == 0 || name_len >= TMPFS_MAX_FILE_NAME)
        return -1;

    //get dir inode
    struct tmpfs_vnode* dir = dir_node->internal;

    //check inode is dir type
    if (!dir || dir->type != FS_DIR)
        return -1;
                                
    //reject duplicate filename/dirname
    for (int i = 0; i < TMPFS_MAX_DIR_ENTRY; i++) {
        if (!dir->entry[i])//good: no duplicate for all valid entry
            break;

        //get the vnode->inode name, compare with component_name, if duplicate, return error -1
        struct tmpfs_vnode* child = dir->entry[i]->internal;

        if (child && strcmp(child->name, component_name) == 0)
            return -1;
    }

    //loop through dir inode's entry, until find available one
    for (int i = 0; i < TMPFS_MAX_DIR_ENTRY; i++) {
        if (!dir->entry[i]) {//available entry
            struct vnode* new_vnode = tmpfs_create_vnode(type);//vnode links to an inode
            if (!new_vnode)
                return -1;
            new_vnode->parent = dir_node;
            struct tmpfs_vnode* new_inode = new_vnode->internal;
            
            //modify inode's name
            for (int j = 0; j < TMPFS_MAX_FILE_NAME - 1; j++) {
                new_inode->name[j] = component_name[j];

                if (component_name[j] == '\0')
                    break;
            }

            new_inode->name[TMPFS_MAX_FILE_NAME - 1] = '\0';

            dir->entry[i] = new_vnode;//modify the entry
            *target = new_vnode;//return addr of new vnode

            return 0;
        }
    }

    //no available entry, return error
    return -1;
}

//allocate vnode, inode, copy name to inode, register it under dir_node (entry list), return updated * target
int tmpfs_create(struct vnode* dir_node,
                 struct vnode** target,
                 const char* component_name) {
    return tmpfs_create_child(dir_node, target, component_name, FS_FILE);
}

//allocate directory vnode, inode, copy name to inode, register it under dir_node (entry list), return updated * target
int tmpfs_mkdir(struct vnode* dir_node,
                struct vnode** target,
                const char* component_name) {
    return tmpfs_create_child(dir_node, target, component_name, FS_DIR);
}

//check if a vnode (->inode) is DIR type
int tmpfs_is_dir(struct vnode* node) {
    if (!node || !node->internal)
        return 0;

    struct tmpfs_vnode* inode = node->internal;

    return inode->type == FS_DIR;
}