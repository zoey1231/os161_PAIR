#ifndef _FILETABLE_H_
#define _FILETABLE_H_

#include <types.h>
#include <vnode.h>
#include <limits.h>
#include <synch.h>
#include <array.h>

/**
 * structure to represent a file and related state in kernel
 */
struct file
{
    struct vnode *vn;
    unsigned status;
    unsigned valid;
    off_t offset;
    unsigned refcount;

    struct lock *file_lock;
};

/**
 * structure to represent filetable
 * Every entry is of type struct file
 */
struct filetable
{
    struct array *entrys;
    struct lock *ft_lock;
};
extern struct filetable filetable;

/**
 * structure to keep track of opened files and thier associated fd
 */
struct fd_entry
{
    unsigned int fd;
    struct file *file;
};

/**
 * Array to keep track of opened file with associated fd
 * Every entry is of type struct fd_entry
 */
struct fileDescriptorArray
{
    struct array *fdArray;
    struct lock *fda_lock;
};

struct fileDescriptorArray *fdArray_create(void);
struct filetable *filetable_create(void);
void filetable_destory(struct filetable *ft);
int filetable_add(struct filetable *ft, struct file *f);
struct fd_entry *fd_get(struct array *arr, unsigned fd, int *index);
void filetable_remove(struct filetable *ft, unsigned fd);
int ft_copy(struct proc *proc_src, struct proc *proc_dst);
#endif
