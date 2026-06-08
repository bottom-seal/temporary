#include "vfs.h"
#include "page_alloc.h"
#include "str.h"

struct mount* rootfs;//root file system
struct filesystem fs_list[MAX_FS];//registered file systems

static struct filesystem tmpfs = {
    .name = "tmpfs",
    .setup_mount = tmpfs_setup_mount,//this func assigns fs field, allocate inode (and vnode) as DIR for root
};

void vfs_init(void) {
    int idx = register_filesystem(&tmpfs);
    if (idx < 0)
        return;

    rootfs = allocate(sizeof(struct mount));
    if (!rootfs)
        return;

    memset(rootfs, 0, sizeof(struct mount));

    fs_list[idx].setup_mount(&fs_list[idx], rootfs);//calls tmpfs_setup_mount(&fs_list[idx], rootfs)
    //rootfs (struct mount) now has fields point to filesystem tmpfs, and vnode to the root DIR type vnode
}

//find an empty entry in fs_list, and record new fs (only record, no mount)
//return list index
int register_filesystem(struct filesystem* fs) {
    if (!fs)
        return -1;

    for (int i = 0; i < MAX_FS; i++) {
        if (fs_list[i].name == 0) {
            fs_list[i].name = fs->name;
            fs_list[i].setup_mount = fs->setup_mount;
            return i;
        }
    }

    return -1;
}

//open a file by pathname, might need to create it first, output in target
int vfs_open(const char* pathname, int flags, struct file** target) {
    if (!pathname || !target)
        return -1;

    if (pathname[0] != '/')
        return -1;

    struct vnode* vnode = 0;

    //if file not exist path
    if (vfs_lookup(pathname, &vnode) != 0) {
        if (!(flags & O_CREAT))
            return -1;

        int pos = 0;

        //the loop finds the position of last '/'
        for (int i = 0; i < strlen(pathname); i++)
            if (pathname[i] == '/')
                pos = i;

        //split dir name and file name
        char dirname[PATH_MAX] = {0};

        //copy first pos chars to dirname
        for (int i = 0; i < pos; i++)
            dirname[i] = pathname[i];

        dirname[pos] = '\0';

        //file name starts after last '/'
        const char* filename = pathname + pos + 1;//just point to pathname string, not new buffer

        if (filename[0] == '\0')
            return -1;

        if (vfs_lookup(dirname, &vnode) != 0)//find the dir vnode, return in pointer vnode
            return -1;
        
        if (!vnode->v_ops || !vnode->v_ops->is_dir || !vnode->v_ops->is_dir(vnode))
            return -1;

        while (vnode->mount)//if the node is a mount point, jump to the root node of the mounted fs
            vnode = vnode->mount->root;

        if (!vnode->v_ops || !vnode->v_ops->create)
            return -1;

        if (vnode->v_ops->create(vnode, &vnode, filename) != 0)//allocate new vnode (and inode), copies name, register entry under dir (first argument), return new vnode with second argument
            return -1;
    }

    while (vnode->mount)
        vnode = vnode->mount->root;

    if (!vnode->f_ops || !vnode->f_ops->open)
        return -1;

    //allocate handler
    *target = allocate(sizeof(struct file));
    if (!(*target))
        return -1;

    (*target)->flags = flags;
    (*target)->refcount = 1;
    if (vnode->f_ops->open(vnode, target) != 0) {//init handler, link to vnode of the file
        free(*target);
        *target = 0;
        return -1;
    }

    return 0;
}

int vfs_close(struct file* file) {
    if (!file || !file->f_ops || !file->f_ops->close)
        return -1;

    return file->f_ops->close(file);
}

int vfs_read(struct file* file, void* buf, size_t len) {
    if (!file || !file->f_ops || !file->f_ops->read)
        return -1;

    return file->f_ops->read(file, buf, len);
}

int vfs_write(struct file* file, const void* buf, size_t len) {
    if (!file || !file->f_ops || !file->f_ops->write)
        return -1;

    return file->f_ops->write(file, buf, len);
}

//given a path (/smth/smth1), find the vnode representing that file, return through target
int vfs_lookup(const char* pathname, struct vnode** target) {
    if (!pathname || !target || !rootfs || !rootfs->root)
        return -1;

    //"" means root dir
    if (strlen(pathname) == 0 || strcmp(pathname, "/") == 0) {
        *target = rootfs->root;

        while ((*target)->mount)//if the node is a mount point, jump to the root node of the mounted fs
            *target = (*target)->mount->root;

        return 0;
    }

    if (pathname[0] != '/')
        return -1;

    //start from root ("/")
    struct vnode* node = rootfs->root;

    while (node->mount)//if the node is a mount point, jump to the root node of the mounted fs
        node = node->mount->root;

    char component[PATH_MAX] = {0};
    int idx = 0;

    //walk through each char
    for (int i = 1; i < strlen(pathname); i++) {
        //if encounter "/", makes a component
        if (pathname[i] == '/') {
            component[idx] = '\0';//no need to clear component array, since compare stops at /0

            if (idx == 0)
                return -1;

            if (!node->v_ops || !node->v_ops->lookup)
                return -1;

            if (node->v_ops->lookup(node, &node, component) != 0)//component name not found under dir, return error
                return -1;

            //after lookup call, node is updated to the node with matching name
            while (node->mount)//if the node is a mount point, jump to the root node of the mounted fs
                node = node->mount->root;

            idx = 0;//reset to create next component
        } else {
            if (idx >= PATH_MAX - 1)
                return -1;

            component[idx++] = pathname[i];//if current char is not '/', keep recording chars
        }
    }

    //loop until the last component
    component[idx] = '\0';

    if (idx == 0)
        return -1;

    //check if last component exists in last dir
    if (!node->v_ops || !node->v_ops->lookup)
        return -1;

    if (node->v_ops->lookup(node, &node, component) != 0)
        return -1;

    //file exist in current dir
    while (node->mount)
        node = node->mount->root;//return root node of the mounted fs

    *target = node;//return in target pointer

    return 0;//success
}

//create a directory by pathname
int vfs_mkdir(const char* pathname) {
    if (!pathname)
        return -1;

    if (pathname[0] != '/')
        return -1;

    struct vnode* vnode = 0;

    //if the path already exists, don't create duplicate dir
    if (vfs_lookup(pathname, &vnode) == 0)
        return -1;

    int pos = 0;

    //the loop finds the position of last '/'
    for (int i = 0; i < strlen(pathname); i++)
        if (pathname[i] == '/')
            pos = i;

    //split dir name and new directory name
    char dirname[PATH_MAX] = {0};

    //copy first pos chars to dirname
    for (int i = 0; i < pos; i++)
        dirname[i] = pathname[i];

    dirname[pos] = '\0';

    //directory name starts after last '/'
    const char* new_dirname = pathname + pos + 1;//just point to pathname string, not new buffer

    if (new_dirname[0] == '\0')
        return -1;

    if (vfs_lookup(dirname, &vnode) != 0)//find the parent dir vnode, return in pointer vnode
        return -1;

    while (vnode->mount)//if the node is a mount point, jump to the root node of the mounted fs
        vnode = vnode->mount->root;

    if (!vnode->v_ops || !vnode->v_ops->mkdir)
        return -1;

    if (vnode->v_ops->mkdir(vnode, &vnode, new_dirname) != 0)//allocate new dir vnode, copies name, register entry under parent dir
        return -1;

    return 0;
}
//some design choice: for VFS API, user doesn't know pointer to vnode or filesystem, so it takes string and does conversion internally
//takes existing dir node, modify mount field to a mount struct
int vfs_mount(const char* target, const char* filesystem) {
    if (!target || !filesystem)
        return -1;
    //pointer to filesystem
    struct filesystem* fs = 0;

    //find registered filesystem by name, in the fs_list
    for (int i = 0; i < MAX_FS; i++) {
        if (fs_list[i].name && strcmp(fs_list[i].name, filesystem) == 0) {
            fs = &fs_list[i];
            break;
        }
    }
    //return error on not found
    if (!fs)
        return -1;

    struct vnode* vnode = 0;//pointer to DIR vnode

    //find the target vnode to mount on
    if (vfs_lookup(target, &vnode) != 0)
        return -1;
    
    if (!vnode->v_ops || !vnode->v_ops->is_dir || !vnode->v_ops->is_dir(vnode))
        return -1;

    //do not mount again if this vnode is already a mount point
    if (vnode->mount)
        return -1;

    //allocate for new mount object
    struct mount* new_mount = allocate(sizeof(struct mount));
    if (!new_mount)
        return -1;

    memset(new_mount, 0, sizeof(struct mount));

    //setup this mounted filesystem, create its root vnode, fill the fields in mount
    //calls the set up function recorded in the fs
    if (fs->setup_mount(fs, new_mount) != 0) {
        free(new_mount);
        return -1;
    }

    //attach the new filesystem to this vnode
    vnode->mount = new_mount;

    return 0;
}