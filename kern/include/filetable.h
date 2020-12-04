#ifndef _FILETABLE_H_
#define _FILETABLE_H_

#include <types.h>
#include <vnode.h>
#include <limits.h>
#include <synch.h>
#include <array.h>
#include <proc.h>

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

//filetable is accessible for all processes
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

// struct fileDescriptorArray *fdArray_create(void);
int fdArray_create(struct proc *proc);
//struct filetable *filetable_create(void);
int filetable_create(struct filetable *ft);
void filetable_destory(struct filetable *ft);
void fdArray_destory(struct array *fd_arr);
int filetable_add(struct filetable *ft, struct file *f);
struct fd_entry *fd_get(struct array *arr, unsigned fd, int *index);
struct file *filetable_get(struct array *ft, struct array *fd_array, unsigned fd, int *index);
void filetable_remove(struct filetable *ft, struct array *fd_array, unsigned fd);
void ft_copy(struct proc *proc_src, struct proc *proc_dst);
#endif
