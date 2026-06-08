#ifndef VFS_H
#define VFS_H

#include "types.h"

#define MAX_FS   16
#define MAX_FD   16
#define PATH_MAX 255
#define O_CREAT 00000100

struct mount;
struct filesystem;
struct vnode;
struct file;
struct vnode_operations;
struct file_operations;

struct vnode {
    struct mount* mount;
    struct vnode_operations* v_ops;
    struct file_operations* f_ops;
    void* internal;
};

struct file {
    struct vnode* vnode;
    size_t f_pos;
    struct file_operations* f_ops;
    int flags;

    int refcount;//for part 3 parent and child should share same handler
};

struct mount {
    struct vnode* root;
    struct filesystem* fs;
};

struct filesystem {
    const char* name;
    int (*setup_mount)(struct filesystem* fs, struct mount* mount);
};

struct file_operations {
    int (*open)(struct vnode* file_node, struct file** target);
    int (*close)(struct file* file);
    int (*read)(struct file* file, void* buf, size_t len);
    int (*write)(struct file* file, const void* buf, size_t len);
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
    int (*is_dir)(struct vnode* node);
};

extern struct mount* rootfs;

void vfs_init(void);

int register_filesystem(struct filesystem* fs);
int vfs_open(const char* pathname, int flags, struct file** target);
int vfs_close(struct file* file);
int vfs_read(struct file* file, void* buf, size_t len);
int vfs_write(struct file* file, const void* buf, size_t len);
int vfs_lookup(const char* pathname, struct vnode** target);
int vfs_mkdir(const char* pathname);
int vfs_mount(const char* target, const char* filesystem);

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
int tmpfs_mkdir(struct vnode* dir_node,
                struct vnode** target,
                const char* component_name);
int tmpfs_is_dir(struct vnode* node);

#endif