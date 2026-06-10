#include "vfs.h"
#include "page_alloc.h"
#include "str.h"
#include "thread.h"
struct mount* rootfs;//root file system
struct filesystem fs_list[MAX_FS];//registered file systems

static struct filesystem tmpfs = {
    .name = "tmpfs",
    .setup_mount = tmpfs_setup_mount,//this func assigns fs field, allocate inode (and vnode) as DIR for root
};
static struct filesystem ramfs = {
    .name = "ramfs",
    .setup_mount = ramfs_setup_mount,
};
//given a path, return the node it point to through target
static int resolve_path(const char* path,
                        struct vnode** target,
                        int follow_final_mount) {
    struct task_struct* current = get_current();//get tp's task
    struct vnode* node;
    int i = 0;

    if (!path || !target || !rootfs || !rootfs->root)
        return -1;

    //if path starts with '/', it is an absolute path, so start from current root.
    //otherwise, it is relative path, so start from current working directory vnode.
    if (path[0] == '/') {
        //if current task exist and has root, use its root, else use rootfs's root
        node = current && current->root ? current->root : rootfs->root;
        i = 1;//skips /
    } else {
        //use cwd if has one, fallback to rootfs if none
        node = current && current->cwd ? current->cwd : rootfs->root;
        i = 0;
    }

    //If the starting vnode is a mount point, enter mounted filesystem.
    while (node && node->mount)
        node = node->mount->root;

    //Empty path means current working directory.
    if (path[0] == '\0') {
        *target = node;
        return 0;
    }

    //2. Split the path by '/', and walk vnode by vnode.
    while (path[i] != '\0') {
        char component[PATH_MAX];
        int idx = 0;

        //Skip slashes.
        while (path[i] == '/')
            i++;

        //If path ends after skipping slashes, lookup is done.
        if (path[i] == '\0')
            break;

        //build a component:
        //copy a name before / to component, use array because path is set as const now
        while (path[i] != '/' && path[i] != '\0') {
            if (idx >= PATH_MAX - 1)
                return -1;

            component[idx++] = path[i++];
        }
        component[idx] = '\0';

        //check current node can contain the component we found, only DIR node can contain other
        if (!vfs_is_dir(node))
            return -1;

        // Skip ".".
        if (strcmp(component, ".") == 0) {
            continue;
        }

        // Handle "..".
        // move to parent node
        if (strcmp(component, "..") == 0) {
            // If node is root of a mounted filesystem,
            // ".." should return to parent of the mount point.
            if (node->mounted_by && node->mounted_by->mountpoint) {
                struct vnode* mountpoint = node->mounted_by->mountpoint;

                if (mountpoint->parent)
                    node = mountpoint->parent;//should go to parent of mount point, not mount point itself
                else
                    node = mountpoint;
            } else if (node->parent) {
                node = node->parent;
            }

            //After moving upward, if the result is a mount point, enter it.
            //edge case of mount point is a dir containing another mount point, one .. would go first mount point, then we should enter root
            while (node && node->mount)
                node = node->mount->root;

            continue;
        }
        //component is a name
        if (!node->v_ops || !node->v_ops->lookup)//check function exists
            return -1;

        if (node->v_ops->lookup(node, &node, component) != 0)//tmpfs_lookup: find node under dir, return node addr
            return -1;
        //from now on node is the component
        
        // Check whether this component is the final meaningful component.
        //   "/mnt/"  -> "mnt" is final
        //   "/mnt/a" -> "mnt" is not final
        int j = i;//i is 1 element after component
        while (path[j] == '/')//skip /
            j++;

        int is_final_component = path[j] == '\0';//check if has next component

        // If this is not the final component, always enter mounted filesystem.
        // If this is the final component, follow_final_mount decides.
        //
        // vfs_lookup("/mnt", ...)
        //     follow_final_mount = 1, returns mounted root.
        //
        // vfs_mount("/mnt", ...)
        //     follow_final_mount = 0, returns mountpoint vnode itself.
        if (!is_final_component || follow_final_mount) {
            while (node && node->mount)
                node = node->mount->root;
        }
    }
    /*
     * A trailing slash requires the resolved vnode to be a directory.
     * For example, "file/" must not resolve to a regular file.
     */
    if (i > 0 && path[i - 1] == '/' && !vfs_is_dir(node))
        return -1;

    *target = node;
    return 0;
}
//on vfs_open(path, O_CREAT), vfs_mkdir(path)
//takes a path where the final component may not exist, and splits it into parent + name, return parent and name
static int resolve_parent(const char* pathname,
                          struct vnode** parent,
                          char* name) {
    char dirname[PATH_MAX];
    int len;
    int slash;
    int name_len;

    if (!pathname || !parent || !name)
        return -1;

    len = strlen(pathname);

    //remove trailing /'s
    while (len > 0 && pathname[len - 1] == '/')
        len--;

    if (len == 0)//path is only slashes
        return -1;

    //find the last /
    slash = len - 1;
    while (slash >= 0 && pathname[slash] != '/')//loop allow slash to go -1, need for later check
        slash--;
    //len - (slash+1) (name starts after last slash)
    name_len = len - slash - 1;

    if (name_len <= 0 || name_len >= PATH_MAX)
        return -1;
    //build name array
    for (int i = 0; i < name_len; i++)
        name[i] = pathname[slash + 1 + i];
    name[name_len] = '\0';
    // .. and . is not legal name
    if (strcmp(name, ".") == 0 || strcmp(name, "..") == 0)
        return -1;
    //slash would be the first slash before name
    //no slash before, parent is cwd, name pathname (eg. file)
    if (slash < 0) {
        dirname[0] = '\0';
    }
    //slash is at start, parent is root dir (eg. /file)
    else if (slash == 0) {
        dirname[0] = '/';
        dirname[1] = '\0';
    }
    //eg. "/a/b/file" -> dirname "/a/b"
    else {
        for (int i = 0; i < slash; i++)
            dirname[i] = pathname[i];

        dirname[slash] = '\0';
    }

    return vfs_lookup(dirname, parent);
}

int vfs_is_dir(struct vnode* node) {
    if (!node || !node->v_ops || !node->v_ops->is_dir)
        return 0;

    return node->v_ops->is_dir(node);
}

void vfs_file_increment_refcount(struct file* file) {
    if (file)
        file->refcount++;
}

void vfs_init(void) {
    int tmpfs_idx = register_filesystem(&tmpfs);

    if (tmpfs_idx < 0)
        return;

    if (register_filesystem(&ramfs) < 0)
        return;

    rootfs = allocate(sizeof(struct mount));

    if (!rootfs)
        return;

    memset(rootfs, 0, sizeof(struct mount));

    fs_list[tmpfs_idx].setup_mount(&fs_list[tmpfs_idx], rootfs);

    /*
     * Part 4:
     * rootfs is tmpfs.
     * Create /ramfs inside tmpfs.
     * Then mount read-only ramfs there.
     */
    if (vfs_mkdir("/ramfs") != 0)//created under root of tmpfs
        return;

    if (vfs_mount("/ramfs", "ramfs") != 0)//vfs_mount also calls setup_mount for ramfs
        return;
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

    //relative allowed, no longer check first char need to be /
    struct vnode* vnode = 0;

    //if file not exist path
    if (vfs_lookup(pathname, &vnode) != 0) {//not found pathname node, need to create it under parent
        if (!(flags & O_CREAT))
            return -1;

        struct vnode* parent = 0;
        char filename[PATH_MAX];

        //split pathname into parent vnode and final file name
        if (resolve_parent(pathname, &parent, filename) != 0)//return parent vnode, and file name
            return -1;

        //if the parent is a mount point, create inside mounted fs
        while (parent && parent->mount)
            parent = parent->mount->root;
        //check is dir
        if (!parent->v_ops || !parent->v_ops->is_dir || !parent->v_ops->is_dir(parent))
            return -1;
        //check has function
        if (!parent->v_ops || !parent->v_ops->create)
            return -1;

        //allocate new vnode and inode, register entry under parent dir
        if (parent->v_ops->create(parent, &vnode, filename) != 0)
            return -1;
    }

    //would not fall into this check, left for safety
    while (vnode->mount)
        vnode = vnode->mount->root;

    //do not open directory as regular file
    if (vfs_is_dir(vnode))
        return -1;

    if (!vnode->f_ops || !vnode->f_ops->open)
        return -1;

    //allocate handler
    *target = allocate(sizeof(struct file));
    if (!(*target))
        return -1;

    (*target)->flags = flags;
    (*target)->refcount = 1;//cause new handler

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

    file->refcount--;

    if (file->refcount > 0)//if someone still using, do nothing
        return 0;

    return file->f_ops->close(file);//if no one using, free the file handler
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
//part3, just wrapper for resolve_path
int vfs_lookup(const char* pathname, struct vnode** target) {
    return resolve_path(pathname, target, 1);
}

//create a directory by pathname
int vfs_mkdir(const char* pathname) {
    if (!pathname)
        return -1;

    struct vnode* vnode = 0;
    struct vnode* parent = 0;
    char new_dirname[PATH_MAX];

    //if the path already exists, don't create duplicate dir
    if (vfs_lookup(pathname, &vnode) == 0)
        return -1;

    //split pathname into parent vnode and new directory name
    if (resolve_parent(pathname, &parent, new_dirname) != 0)
        return -1;

    //if the parent is a mount point, create inside mounted fs
    while (parent && parent->mount)
        parent = parent->mount->root;

    if (!parent->v_ops || !parent->v_ops->is_dir || !parent->v_ops->is_dir(parent))
        return -1;

    if (!parent->v_ops || !parent->v_ops->mkdir)
        return -1;

    //allocate new dir vnode, copies name, register entry under parent dir
    if (parent->v_ops->mkdir(parent, &vnode, new_dirname) != 0)
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
    if (resolve_path(target, &vnode, 0) != 0)//last argument: 0 will return mountpoint, 1 will return mount root (if last component is mountpoint)
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

    ///records mount -> mountpoint, so .. can find its parent dir
    new_mount->mountpoint = vnode;

    if (new_mount->root) {
        new_mount->root->mounted_by = new_mount;

        //didn't really use this part
        if (vnode->parent)
            new_mount->root->parent = vnode->parent;//can do direct link back, but not really parent 
        else
            new_mount->root->parent = vnode;
    }

    //attach the new filesystem to this vnode
    vnode->mount = new_mount;

    return 0;
}
