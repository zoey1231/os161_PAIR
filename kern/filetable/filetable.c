#include <filetable.h>
#include <array.h>
#include <kern/errno.h>
#include <proc.h>
/**
 * Create a filedescriptor array to keep track of opened 
 * files and thier associated fd
 */
int fdArray_create(struct proc *proc)
{
    proc->fdArray = array_create();
    if (proc->fdArray == NULL)
    {
        return 1;
    }
    array_init(proc->fdArray);
    proc->fda_lock = lock_create("fda_lock");
    if (proc->fda_lock == NULL)
    {
        return 1;
    }
    return 0;
}
// struct fileDescriptorArray *fdArray_create()
// {
//     struct fileDescriptorArray *fd_record;

//     fd_record = kmalloc(sizeof(struct fileDescriptorArray));
//     if (fd_record == NULL)
//         return NULL;

//     fd_record->fda_lock = lock_create("fda_lock");
//     if (fd_record->fda_lock == NULL)
//     {
//         kfree(fd_record);
//         return NULL;
//     }
//     fd_record->fdArray = array_create();
//     array_init(fd_record->fdArray);
//     array_preallocate(fd_record->fdArray, OPEN_MAX);

//     return fd_record;
// }
/**
 * Destory a fileDescriptorArray
 */
void fdArray_destory(struct array *fd_arr)
{
    KASSERT(fd_arr != NULL);
    int num = array_num(fd_arr);
    for (int i = 0; i < num; i++)
    {
        array_remove(fd_arr, 0);
    }
    KASSERT(array_num(fd_arr) == 0);
    array_destroy(fd_arr);
}
/**
 * Create a filetable keep track of opened files 
 */
int filetable_create(struct filetable *ft)
{
    ft->ft_lock = lock_create("ft_lock");
    if (ft->ft_lock == NULL)
    {
        return 1;
    }
    ft->entrys = array_create();
    array_init(ft->entrys);

    return 0;
}
// struct filetable *filetable_create()
// {
//     struct filetable *ft;

//     ft = kmalloc(sizeof(struct filetable));
//     if (ft == NULL)
//         return NULL;

//     ft->ft_lock = lock_create("ft_lock");
//     if (ft->ft_lock == NULL)
//     {
//         kfree(ft);
//         return NULL;
//     }
//     ft->entrys = array_create();
//     array_init(ft->entrys);
//     array_preallocate(ft->entrys, OPEN_MAX);

//     return ft;
// }
/**
 * Destory a filetable
 */
void filetable_destory(struct filetable *ft)
{
    KASSERT(ft != NULL);
    int num = array_num(ft->entrys);
    for (int i = 0; i < num; i++)
    {
        array_remove(ft->entrys, 0);
    }
    KASSERT(array_num(ft->entrys) == 0);
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
        // replace invalid file if possible
        if (!f->valid)
        {
            array_set(ft->entrys, i, file);
            return 0;
        }
    }
    // append to the end of the array
    err = array_add(ft->entrys, file, NULL);
    if (err)
        return err;
    return 0;
}
/**
 * Return the filedescriptor entry with fd in filetable 
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

struct file *filetable_get(struct array *ft, struct array *fd_array, unsigned fd, int *index)
{
    int idx;
    struct file *f;
    struct fd_entry *fe = fd_get(fd_array, fd, &idx);
    for (unsigned i = 0; i < array_num(ft); ++i)
    {
        f = (struct file *)array_get(ft, i);
        if (f->file_lock == fe->file->file_lock &&
            f->offset == fe->file->offset &&
            f->refcount == fe->file->refcount &&
            f->status == fe->file->status &&
            f->valid == fe->file->valid &&
            f->vn == fe->file->vn)
        {
            if (index != NULL)
                *index = i;
            return f;
        }
    }
    return NULL;
}
/**
 * Remove a file from the filetable
 * Unused
 */
void filetable_remove(struct filetable *ft, struct array *fd_array, unsigned fd)
{
    int index;
    KASSERT(ft != NULL);
    KASSERT(fd < OPEN_MAX);
    struct file *f = filetable_get(ft->entrys, fd_array, fd, &index);
    if (f != NULL)
    {
        array_remove(ft->entrys, index);
    }
}

/**
 * Helper function for sys_fork()
 * Copy proc_src's fileDescriptor array into proc_dst's fileDescriptor array
 */
void ft_copy(struct proc *proc_src, struct proc *proc_dst)
{
    KASSERT(proc_src != NULL);
    KASSERT(proc_dst != NULL);

    lock_acquire(proc_src->fda_lock);
    lock_acquire(proc_dst->fda_lock);

    //iterate over proc_src's filedescriptor array's entry and add each one into proc_dst's filedescriptor array
    for (unsigned int i = 0; i < array_num(proc_src->fdArray); i++)
    {
        struct fd_entry *fe_old = (struct fd_entry *)array_get(proc_src->fdArray, i);
        struct fd_entry *fe_new = kmalloc(sizeof(struct fd_entry));
        KASSERT(fe_new != NULL);

        lock_acquire(fe_old->file->file_lock);

        KASSERT(fe_old->file->valid && fe_old->file->refcount > 0);

        fe_old->file->refcount++;
        VOP_INCREF(fe_old->file->vn);

        fe_new->fd = fe_old->fd;
        fe_new->file = fe_old->file;

        lock_release(fe_old->file->file_lock);

        array_add(proc_dst->fdArray, fe_new, NULL);
    }

    lock_release(proc_src->fda_lock);
    lock_release(proc_dst->fda_lock);
}