#include <filetable.h>
#include <array.h>
#include <kern/errno.h>
struct fileDescriptorArray *fdArray_create()
{
    struct fileDescriptorArray *fdArray;

    fdArray = kmalloc(sizeof(struct fileDescriptorArray));
    if (fdArray == NULL)
        return NULL;

    fdArray->fda_lock = lock_create("fda_lock");
    if (fdArray->fda_lock == NULL)
    {
        kfree(fdArray);
        return NULL;
    }
    fdArray->fdArray = array_create();
    array_init(fdArray->fdArray);
    array_preallocate(fdArray->fdArray, OPEN_MAX);

    return fdArray;
}
struct filetable *filetable_create()
{
    struct filetable *ft;

    ft = kmalloc(sizeof(struct filetable));
    if (ft == NULL)
        return NULL;

    ft->ft_lock = lock_create("ft_lock");
    if (ft->ft_lock == NULL)
    {
        kfree(ft);
        return NULL;
    }
    ft->entrys = array_create();
    array_init(ft->entrys);
    array_preallocate(ft->entrys, OPEN_MAX);

    return ft;
}

void filetable_destory(struct filetable *ft)
{
    KASSERT(ft != NULL);
    array_destroy(ft->entrys);
    lock_destroy(ft->ft_lock);
    kfree(ft);
}

/**
 * Add a file entry into the filetable. 
 * Add to the frontmost avaliable entry position.
 * Return 0 as success and -1 as error 
 */
int filetable_add(struct filetable *ft, struct file *file)
{
    int err = -1;
    KASSERT(ft != NULL);
    KASSERT(file != NULL);

    struct file *f;
    for (unsigned i = 0; i < array_num(ft->entrys); i++)
    {
        f = (struct file *)array_get(ft->entrys, i);
        if (!f->valid)
        {
            array_set(ft->entrys, i, file);
            return 0;
        }
    }
    err = array_add(ft->entrys, file, NULL);
    if (err)
        return err;
    return 0;
}
/**
 * Return the filedescriptor entry with fd in filetable in f 
 */
struct fd_entry *fd_get(struct array *arr, unsigned fd, int *index)
{
    struct fd_entry *fe;
    for (unsigned i = 0; i < array_num(arr); ++i)
    {
        fe = (struct fd_entry *)array_get(arr, i);
        if (fe->fd == fd && fe->file->valid)
        {
            if (index != NULL)
                *index = i;
            return fe;
        }
    }
    return NULL;
}
void filetable_remove(struct filetable *ft, unsigned fd)
{
    KASSERT(ft != NULL);
    KASSERT(fd < OPEN_MAX);
    array_remove(ft->entrys, fd);
}
// #include <types.h>
// #include <lib.h>
// #include <vfs.h>
// #include <filetable.h>
// #include <kern/fcntl.h>
// #include <fsyscall.h>
// #include <kern/errno.h>

// struct ft *ft_create()
// {
//     struct ft *ft;

//     ft = kmalloc(sizeof(struct ft));
//     if (ft == NULL)
//     {
//         return NULL;
//     }

//     ft->ft_lock = lock_create("fs_lock");
//     if (ft->ft_lock == NULL)
//     {
//         kfree(ft);
//         return NULL;
//     }

//     for (int i = 0; i < OPEN_MAX; i++)
//     {
//         ft->entries[i] = NULL;
//     }

//     return ft;
// }

// void ft_destroy(struct ft *ft)
// {
//     KASSERT(ft != NULL);

//     lock_destroy(ft->ft_lock);
//     kfree(ft);
// }

// /*
// Initializes the console of the filetable.
// */
// int ft_init_std(struct ft *ft)
// {

//     KASSERT(ft != NULL);

//     const char *cons = "con:";

//     if (ft->entries[0] == NULL)
//     {
//         struct vnode *stdin_v;
//         int ret;

//         ret = vfs_open(kstrdup(cons), O_RDONLY, 0, &stdin_v);
//         if (ret)
//         {
//             return ret;
//         }

//         ft->entries[0] = entry_create(stdin_v);
//         if (ft->entries[0] == NULL)
//         {
//             return ENOMEM;
//         }

//         ft->entries[0]->rwflags = O_RDONLY;
//         entry_incref(ft->entries[0]);
//         (void)ret;
//     }

//     if (ft->entries[1] == NULL)
//     {
//         struct vnode *stdout_v;
//         int ret;

//         ret = vfs_open(kstrdup(cons), O_WRONLY, 0, &stdout_v);
//         if (ret)
//         {
//             return ret;
//         }

//         ft->entries[1] = entry_create(stdout_v);
//         if (ft->entries[1] == NULL)
//         {
//             return ENOMEM;
//         }

//         ft->entries[1]->rwflags = O_WRONLY;
//         entry_incref(ft->entries[1]);
//         (void)ret;
//     }

//     if (ft->entries[2] == NULL)
//     {
//         struct vnode *stderr_v;
//         int ret;

//         ret = vfs_open(kstrdup(cons), O_WRONLY, 0, &stderr_v);
//         if (ret)
//         {
//             return ret;
//         }

//         ft->entries[2] = entry_create(stderr_v);
//         if (ft->entries[2] == NULL)
//         {
//             return ENOMEM;
//         }

//         ft->entries[2]->rwflags = O_WRONLY;
//         entry_incref(ft->entries[2]);
//         (void)ret;
//     }

//     return 0;
// }

// ////////////////////////////////////////////////////////////////////////////////////////////////

// /*
// Returns a EMFILE if OPEN_MAX files are already open.
// Otherwise, returns new file descriptor.
// */
// int add_entry(struct ft *ft, struct ft_entry *entry, int32_t *fd)
// {
//     KASSERT(ft != NULL);
//     KASSERT(entry != NULL);

//     for (int i = 3; i < OPEN_MAX; i++)
//     {
//         if (ft->entries[i] == NULL)
//         {
//             assign_fd(ft, entry, i);
//             *fd = i;
//             return 0;
//         }
//     }

//     return EMFILE;
// }

// void assign_fd(struct ft *ft, struct ft_entry *entry, int fd)
// {
//     KASSERT(ft != NULL);
//     KASSERT(entry != NULL);
//     KASSERT(fd_valid(fd));

//     ft->entries[fd] = entry;
//     entry_incref(entry);
// }

// void free_fd(struct ft *ft, int fd)
// {
//     KASSERT(ft != NULL);
//     KASSERT(fd_valid(fd));

//     if (ft->entries[fd] == NULL)
//     {
//         return;
//     }

//     entry_decref(ft->entries[fd]);
//     ft->entries[fd] = NULL;
// }

// bool fd_valid_and_used(struct ft *ft, int fd)
// {
//     KASSERT(ft != NULL);

//     if (!fd_valid(fd))
//     {
//         return false;
//     }
//     return ft->entries[fd] != NULL;
// }

// bool fd_valid(int fd)
// {
//     if (fd < 0 || fd >= OPEN_MAX)
//     {
//         return false;
//     }

//     return true;
// }

// //////////////////////////////////////////////////////////////////////////////////////////////

// struct ft_entry *
// entry_create(struct vnode *vnode)
// {
//     struct ft_entry *entry;

//     KASSERT(vnode != NULL);

//     entry = kmalloc(sizeof(struct ft_entry));
//     if (entry == NULL)
//     {
//         return NULL;
//     }
//     entry->entry_lock = lock_create("entry_lock");
//     if (entry->entry_lock == NULL)
//     {
//         kfree(entry);
//         return NULL;
//     }

//     entry->file = vnode;
//     entry->offset = 0;
//     entry->count = 0;

//     return entry;
// }

// void entry_destroy(struct ft_entry *entry)
// {
//     KASSERT(entry != NULL);

//     vfs_close(entry->file);
//     lock_destroy(entry->entry_lock);
//     kfree(entry);
// }

// void entry_incref(struct ft_entry *entry)
// {
//     KASSERT(entry != NULL);

//     entry->count += 1;
// }

// void entry_decref(struct ft_entry *entry)
// {
//     KASSERT(entry != NULL);

//     entry->count -= 1;
//     if (entry->count == 0)
//     {
//         entry_destroy(entry);
//     }
// }
// }

// void ft_destroy(struct ft *ft)
// {
//     KASSERT(ft != NULL);

//     lock_destroy(ft->ft_lock);
//     kfree(ft);
// }

// /*
// Initializes the console of the filetable.
// */
// int ft_init_std(struct ft *ft)
// {

//     KASSERT(ft != NULL);

//     const char *cons = "con:";

//     if (ft->entries[0] == NULL)
//     {
//         struct vnode *stdin_v;
//         int ret;

//         ret = vfs_open(kstrdup(cons), O_RDONLY, 0, &stdin_v);
//         if (ret)
//         {
//             return ret;
//         }

//         ft->entries[0] = entry_create(stdin_v);
//         if (ft->entries[0] == NULL)
//         {
//             return ENOMEM;
//         }

//         ft->entries[0]->rwflags = O_RDONLY;
//         entry_incref(ft->entries[0]);
//         (void)ret;
//     }

//     if (ft->entries[1] == NULL)
//     {
//         struct vnode *stdout_v;
//         int ret;

//         ret = vfs_open(kstrdup(cons), O_WRONLY, 0, &stdout_v);
//         if (ret)
//         {
//             return ret;
//         }

//         ft->entries[1] = entry_create(stdout_v);
//         if (ft->entries[1] == NULL)
//         {
//             return ENOMEM;
//         }

//         ft->entries[1]->rwflags = O_WRONLY;
//         entry_incref(ft->entries[1]);
//         (void)ret;
//     }

//     if (ft->entries[2] == NULL)
//     {
//         struct vnode *stderr_v;
//         int ret;

//         ret = vfs_open(kstrdup(cons), O_WRONLY, 0, &stderr_v);
//         if (ret)
//         {
//             return ret;
//         }

//         ft->entries[2] = entry_create(stderr_v);
//         if (ft->entries[2] == NULL)
//         {
//             return ENOMEM;
//         }

//         ft->entries[2]->rwflags = O_WRONLY;
//         entry_incref(ft->entries[2]);
//         (void)ret;
//     }

//     return 0;
// }

// ////////////////////////////////////////////////////////////////////////////////////////////////

// /*
// Returns a EMFILE if OPEN_MAX files are already open.
// Otherwise, returns new file descriptor.
// */
// int add_entry(struct ft *ft, struct ft_entry *entry, int32_t *fd)
// {
//     KASSERT(ft != NULL);
//     KASSERT(entry != NULL);

//     for (int i = 3; i < OPEN_MAX; i++)
//     {
//         if (ft->entries[i] == NULL)
//         {
//             assign_fd(ft, entry, i);
//             *fd = i;
//             return 0;
//         }
//     }

//     return EMFILE;
// }

// void assign_fd(struct ft *ft, struct ft_entry *entry, int fd)
// {
//     KASSERT(ft != NULL);
//     KASSERT(entry != NULL);
//     KASSERT(fd_valid(fd));

//     ft->entries[fd] = entry;
//     entry_incref(entry);
// }

// void free_fd(struct ft *ft, int fd)
// {
//     KASSERT(ft != NULL);
//     KASSERT(fd_valid(fd));

//     if (ft->entries[fd] == NULL)
//     {
//         return;
//     }

//     entry_decref(ft->entries[fd]);
//     ft->entries[fd] = NULL;
// }

// bool fd_valid_and_used(struct ft *ft, int fd)
// {
//     KASSERT(ft != NULL);

//     if (!fd_valid(fd))
//     {
//         return false;
//     }
//     return ft->entries[fd] != NULL;
// }

// bool fd_valid(int fd)
// {
//     if (fd < 0 || fd >= OPEN_MAX)
//     {
//         return false;
//     }

//     return true;
// }

// //////////////////////////////////////////////////////////////////////////////////////////////

// struct ft_entry *
// entry_create(struct vnode *vnode)
// {
//     struct ft_entry *entry;

//     KASSERT(vnode != NULL);

//     entry = kmalloc(sizeof(struct ft_entry));
//     if (entry == NULL)
//     {
//         return NULL;
//     }
//     entry->entry_lock = lock_create("entry_lock");
//     if (entry->entry_lock == NULL)
//     {
//         kfree(entry);
//         return NULL;
//     }

//     entry->file = vnode;
//     entry->offset = 0;
//     entry->count = 0;

//     return entry;
// }

// void entry_destroy(struct ft_entry *entry)
// {
//     KASSERT(entry != NULL);

//     vfs_close(entry->file);
//     lock_destroy(entry->entry_lock);
//     kfree(entry);
// }

// void entry_incref(struct ft_entry *entry)
// {
//     KASSERT(entry != NULL);

//     entry->count += 1;
// }

// void entry_decref(struct ft_entry *entry)
// {
//     KASSERT(entry != NULL);

//     entry->count -= 1;
//     if (entry->count == 0)
//     {
//         entry_destroy(entry);
//     }
// }
// }

// void ft_destroy(struct ft *ft)
// {
//     KASSERT(ft != NULL);

//     lock_destroy(ft->ft_lock);
//     kfree(ft);
// }

// /*
// Initializes the console of the filetable.
// */
// int ft_init_std(struct ft *ft)
// {

//     KASSERT(ft != NULL);

//     const char *cons = "con:";

//     if (ft->entries[0] == NULL)
//     {
//         struct vnode *stdin_v;
//         int ret;

//         ret = vfs_open(kstrdup(cons), O_RDONLY, 0, &stdin_v);
//         if (ret)
//         {
//             return ret;
//         }

//         ft->entries[0] = entry_create(stdin_v);
//         if (ft->entries[0] == NULL)
//         {
//             return ENOMEM;
//         }

//         ft->entries[0]->rwflags = O_RDONLY;
//         entry_incref(ft->entries[0]);
//         (void)ret;
//     }

//     if (ft->entries[1] == NULL)
//     {
//         struct vnode *stdout_v;
//         int ret;

//         ret = vfs_open(kstrdup(cons), O_WRONLY, 0, &stdout_v);
//         if (ret)
//         {
//             return ret;
//         }

//         ft->entries[1] = entry_create(stdout_v);
//         if (ft->entries[1] == NULL)
//         {
//             return ENOMEM;
//         }

//         ft->entries[1]->rwflags = O_WRONLY;
//         entry_incref(ft->entries[1]);
//         (void)ret;
//     }

//     if (ft->entries[2] == NULL)
//     {
//         struct vnode *stderr_v;
//         int ret;

//         ret = vfs_open(kstrdup(cons), O_WRONLY, 0, &stderr_v);
//         if (ret)
//         {
//             return ret;
//         }

//         ft->entries[2] = entry_create(stderr_v);
//         if (ft->entries[2] == NULL)
//         {
//             return ENOMEM;
//         }

//         ft->entries[2]->rwflags = O_WRONLY;
//         entry_incref(ft->entries[2]);
//         (void)ret;
//     }

//     return 0;
// }

// ////////////////////////////////////////////////////////////////////////////////////////////////

// /*
// Returns a EMFILE if OPEN_MAX files are already open.
// Otherwise, returns new file descriptor.
// */
// int add_entry(struct ft *ft, struct ft_entry *entry, int32_t *fd)
// {
//     KASSERT(ft != NULL);
//     KASSERT(entry != NULL);

//     for (int i = 3; i < OPEN_MAX; i++)
//     {
//         if (ft->entries[i] == NULL)
//         {
//             assign_fd(ft, entry, i);
//             *fd = i;
//             return 0;
//         }
//     }

//     return EMFILE;
// }

// void assign_fd(struct ft *ft, struct ft_entry *entry, int fd)
// {
//     KASSERT(ft != NULL);
//     KASSERT(entry != NULL);
//     KASSERT(fd_valid(fd));

//     ft->entries[fd] = entry;
//     entry_incref(entry);
// }

// void free_fd(struct ft *ft, int fd)
// {
//     KASSERT(ft != NULL);
//     KASSERT(fd_valid(fd));

//     if (ft->entries[fd] == NULL)
//     {
//         return;
//     }

//     entry_decref(ft->entries[fd]);
//     ft->entries[fd] = NULL;
// }

// bool fd_valid_and_used(struct ft *ft, int fd)
// {
//     KASSERT(ft != NULL);

//     if (!fd_valid(fd))
//     {
//         return false;
//     }
//     return ft->entries[fd] != NULL;
// }

// bool fd_valid(int fd)
// {
//     if (fd < 0 || fd >= OPEN_MAX)
//     {
//         return false;
//     }

//     return true;
// }

// //////////////////////////////////////////////////////////////////////////////////////////////

// struct ft_entry *
// entry_create(struct vnode *vnode)
// {
//     struct ft_entry *entry;

//     KASSERT(vnode != NULL);

//     entry = kmalloc(sizeof(struct ft_entry));
//     if (entry == NULL)
//     {
//         return NULL;
//     }
//     entry->entry_lock = lock_create("entry_lock");
//     if (entry->entry_lock == NULL)
//     {
//         kfree(entry);
//         return NULL;
//     }

//     entry->file = vnode;
//     entry->offset = 0;
//     entry->count = 0;

//     return entry;
// }

// void entry_destroy(struct ft_entry *entry)
// {
//     KASSERT(entry != NULL);

//     vfs_close(entry->file);
//     lock_destroy(entry->entry_lock);
//     kfree(entry);
// }

// void entry_incref(struct ft_entry *entry)
// {
//     KASSERT(entry != NULL);

//     entry->count += 1;
// }

// void entry_decref(struct ft_entry *entry)
// {
//     KASSERT(entry != NULL);

//     entry->count -= 1;
//     if (entry->count == 0)
//     {
//         entry_destroy(entry);
//     }
// }
// }

// void ft_destroy(struct ft *ft)
// {
//     KASSERT(ft != NULL);

//     lock_destroy(ft->ft_lock);
//     kfree(ft);
// }

// /*
// Initializes the console of the filetable.
// */
// int ft_init_std(struct ft *ft)
// {

//     KASSERT(ft != NULL);

//     const char *cons = "con:";

//     if (ft->entries[0] == NULL)
//     {
//         struct vnode *stdin_v;
//         int ret;

//         ret = vfs_open(kstrdup(cons), O_RDONLY, 0, &stdin_v);
//         if (ret)
//         {
//             return ret;
//         }

//         ft->entries[0] = entry_create(stdin_v);
//         if (ft->entries[0] == NULL)
//         {
//             return ENOMEM;
//         }

//         ft->entries[0]->rwflags = O_RDONLY;
//         entry_incref(ft->entries[0]);
//         (void)ret;
//     }

//     if (ft->entries[1] == NULL)
//     {
//         struct vnode *stdout_v;
//         int ret;

//         ret = vfs_open(kstrdup(cons), O_WRONLY, 0, &stdout_v);
//         if (ret)
//         {
//             return ret;
//         }

//         ft->entries[1] = entry_create(stdout_v);
//         if (ft->entries[1] == NULL)
//         {
//             return ENOMEM;
//         }

//         ft->entries[1]->rwflags = O_WRONLY;
//         entry_incref(ft->entries[1]);
//         (void)ret;
//     }

//     if (ft->entries[2] == NULL)
//     {
//         struct vnode *stderr_v;
//         int ret;

//         ret = vfs_open(kstrdup(cons), O_WRONLY, 0, &stderr_v);
//         if (ret)
//         {
//             return ret;
//         }

//         ft->entries[2] = entry_create(stderr_v);
//         if (ft->entries[2] == NULL)
//         {
//             return ENOMEM;
//         }

//         ft->entries[2]->rwflags = O_WRONLY;
//         entry_incref(ft->entries[2]);
//         (void)ret;
//     }

//     return 0;
// }

// ////////////////////////////////////////////////////////////////////////////////////////////////

// /*
// Returns a EMFILE if OPEN_MAX files are already open.
// Otherwise, returns new file descriptor.
// */
// int add_entry(struct ft *ft, struct ft_entry *entry, int32_t *fd)
// {
//     KASSERT(ft != NULL);
//     KASSERT(entry != NULL);

//     for (int i = 3; i < OPEN_MAX; i++)
//     {
//         if (ft->entries[i] == NULL)
//         {
//             assign_fd(ft, entry, i);
//             *fd = i;
//             return 0;
//         }
//     }

//     return EMFILE;
// }

// void assign_fd(struct ft *ft, struct ft_entry *entry, int fd)
// {
//     KASSERT(ft != NULL);
//     KASSERT(entry != NULL);
//     KASSERT(fd_valid(fd));

//     ft->entries[fd] = entry;
//     entry_incref(entry);
// }

// void free_fd(struct ft *ft, int fd)
// {
//     KASSERT(ft != NULL);
//     KASSERT(fd_valid(fd));

//     if (ft->entries[fd] == NULL)
//     {
//         return;
//     }

//     entry_decref(ft->entries[fd]);
//     ft->entries[fd] = NULL;
// }

// bool fd_valid_and_used(struct ft *ft, int fd)
// {
//     KASSERT(ft != NULL);

//     if (!fd_valid(fd))
//     {
//         return false;
//     }
//     return ft->entries[fd] != NULL;
// }

// bool fd_valid(int fd)
// {
//     if (fd < 0 || fd >= OPEN_MAX)
//     {
//         return false;
//     }

//     return true;
// }

// //////////////////////////////////////////////////////////////////////////////////////////////

// struct ft_entry *
// entry_create(struct vnode *vnode)
// {
//     struct ft_entry *entry;

//     KASSERT(vnode != NULL);

//     entry = kmalloc(sizeof(struct ft_entry));
//     if (entry == NULL)
//     {
//         return NULL;
//     }
//     entry->entry_lock = lock_create("entry_lock");
//     if (entry->entry_lock == NULL)
//     {
//         kfree(entry);
//         return NULL;
//     }

//     entry->file = vnode;
//     entry->offset = 0;
//     entry->count = 0;

//     return entry;
// }

// void entry_destroy(struct ft_entry *entry)
// {
//     KASSERT(entry != NULL);

//     vfs_close(entry->file);
//     lock_destroy(entry->entry_lock);
//     kfree(entry);
// }

// void entry_incref(struct ft_entry *entry)
// {
//     KASSERT(entry != NULL);

//     entry->count += 1;
// }

// void entry_decref(struct ft_entry *entry)
// {
//     KASSERT(entry != NULL);

//     entry->count -= 1;
//     if (entry->count == 0)
//     {
//         entry_destroy(entry);
//     }
// }
