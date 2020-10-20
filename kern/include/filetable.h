#ifndef _FILETABLE_H_
#define _FILETABLE_H_

#include <types.h>
#include <vnode.h>
#include <limits.h>
#include <synch.h>
#include <array.h>
// entry of a file table
struct file
{
    struct vnode *vn;
    unsigned status;
    unsigned valid;
    off_t offset;
    unsigned refcount;

    struct lock *file_lock;
};
struct filetable_entry
{
    unsigned int fd;
    struct file *file;
};
struct filetable
{
    struct array *entrys; //file descriptor table array
    struct lock *ft_lock;
};
extern struct filetable filetable;

struct filetable *filetable_create(void);
void filetable_destory(struct filetable *ft);
int filetable_add(struct filetable *ft, struct file *f, unsigned *fd_ret);
struct file *filetable_get(struct filetable *ft, unsigned fd);
void filetable_remove(struct filetable *ft, unsigned fd);
#endif
// #ifndef _FILETABLE_H_
// #define _FILETABLE_H_

// #include <synch.h>
// #include <limits.h>
// #include <vnode.h>
// #include <lib.h>

// // File descriptor table
// struct ft
// {
//     struct lock *ft_lock;
//     struct ft_entry *entries[OPEN_MAX];
// };

// // Entry of a file table
// struct ft_entry
// {
//     int count;
//     struct lock *entry_lock;
//     struct vnode *file;
//     off_t offset;
//     const char *path;
//     int rwflags;
// };

// // Functions
// struct ft *ft_create(void);
// void ft_destroy(struct ft *);

// int ft_init_std(struct ft *);

// int add_entry(struct ft *, struct ft_entry *, int32_t *);
// void assign_fd(struct ft *, struct ft_entry *, int);
// void free_fd(struct ft *, int);
// bool fd_valid_and_used(struct ft *, int);
// bool fd_valid(int);
// void ft_copy(struct ft *, struct ft *);

// struct ft_entry *entry_create(struct vnode *);
// void entry_destroy(struct ft_entry *);

// /*
// At any state of the file table, the entry->count
// is the number of file descriptors it has. Once the
// count reaches zero, the entry must be destroyed.
// */

// void entry_incref(struct ft_entry *);
// void entry_decref(struct ft_entry *, bool);

// struct addrspace;
// struct vnode;

// #endif
