#ifndef VFS_H
#define VFS_H

#include "types.h"

#define MAX_DEV 16
#define MAX_FS   16
#define MAX_FD   16
#define PATH_MAX 255
#define O_CREAT 00000100

#define SEEK_SET 0

struct mount;
struct filesystem;
struct vnode;
struct file;
struct vnode_operations;
struct file_operations;

struct vnode {
    struct mount* mount;//the vnode with something mounted should have this, go down to mounted fs
    struct vnode_operations* v_ops;
    struct file_operations* f_ops;
    void* internal;
    //part 3
    struct vnode* parent;//parent dir for ..
    struct mount* mounted_by;//mounted root should have this, point back to mount point .. should go back to parent of mount point
};

struct file {
    struct vnode* vnode;
    size_t f_pos;
    struct file_operations* f_ops;
    int flags;

    int refcount;//for part 3 parent and child should share same handler
};

struct mount {
    struct vnode* root;//records root vnode (down)
    struct filesystem* fs;

    struct vnode* mountpoint;//records the vnode with mount field pointing to this struct mount
};

struct filesystem {
    const char* name;
    int (*setup_mount)(struct filesystem* fs, struct mount* mount);
};

struct device {
    const char* name;
    struct file_operations* f_ops;
};

struct file_operations {
    int (*open)(struct vnode* file_node, struct file** target);
    int (*close)(struct file* file);
    int (*read)(struct file* file, void* buf, size_t len);
    int (*write)(struct file* file, const void* buf, size_t len);

    long (*lseek64)(struct file* file, long offset, int whence);
    int (*ioctl)(struct file* file, unsigned long request, void* arg);
};

struct vnode_operations {
    int (*lookup)(struct vnode* dir_node,
                  struct vnode** target,
                  const char* component_name);
    int (*create)(struct vnode* dir_node,
                  struct vnode** target,
                  const char* component_name);
    int (*mkdir)(struct vnode* dir_node,
                 struct vnode** target,
                 const char* component_name);
    int (*mknod)(struct vnode* dir_node,
                 struct vnode** target,
                 const char* component_name,
                 int dev_id);

    int (*is_dir)(struct vnode* node);

    int (*get_dev_id)(struct vnode* node, int* dev_id);
};

extern struct mount* rootfs;
struct task_struct;//forward declaration, okay if just use pointer (size known)
void vfs_init(void);

int register_filesystem(struct filesystem* fs);
int vfs_open(const char* pathname, int flags, struct file** target);
int vfs_close(struct file* file);
int vfs_read(struct file* file, void* buf, size_t len);
int vfs_write(struct file* file, const void* buf, size_t len);
int vfs_lookup(const char* pathname, struct vnode** target);
int vfs_mkdir(const char* pathname);
int vfs_mount(const char* target, const char* filesystem);
int vfs_is_dir(struct vnode* node);
void vfs_file_increment_refcount(struct file* file);

int tmpfs_setup_mount(struct filesystem* fs, struct mount* mnt);
int tmpfs_open(struct vnode* file_node, struct file** target);
int tmpfs_close(struct file* file);
int tmpfs_read(struct file* file, void* buf, size_t len);
int tmpfs_write(struct file* file, const void* buf, size_t len);
int tmpfs_lookup(struct vnode* dir_node,
                 struct vnode** target,
                 const char* component_name);
int tmpfs_create(struct vnode* dir_node,
                 struct vnode** target,
                 const char* component_name);
int tmpfs_mkdir(struct vnode* dir_node,
                struct vnode** target,
                const char* component_name);
int tmpfs_is_dir(struct vnode* node);

int ramfs_setup_mount(struct filesystem* fs, struct mount* mnt);
long tmpfs_lseek64(struct file* file, long offset, int whence);

//for advance part
int register_device(const char* name, struct file_operations* f_ops);
struct file_operations* get_device_fops(int dev_id);

int vfs_mknod(const char* pathname, int dev_id);

int tmpfs_mknod(struct vnode* dir_node,
                struct vnode** target,
                const char* component_name,
                int dev_id);

int tmpfs_get_dev_id(struct vnode* node, int* dev_id);

int uartdev_init(void);

int open_stdio_for_task(struct task_struct* task);

//part 2
long vfs_lseek64(struct file* file, long offset, int whence);
int vfs_ioctl(struct file* file, unsigned long request, void* arg);

int fbdev_init(void);

#endif