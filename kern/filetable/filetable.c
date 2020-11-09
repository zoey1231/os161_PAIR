#include <filetable.h>
#include <array.h>
#include <kern/errno.h>
#include <proc.h>
/**
 * Create a filedescriptor array to keep track of opened 
 * files and thier associated fd
 */
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

/**
 * Create a filetable keep track of opened files 
 */
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
/**
 * Destory a filetable
 */
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
/**
 * Remove a file from the filetable
 */
void filetable_remove(struct filetable *ft, unsigned fd)
{
    KASSERT(ft != NULL);
    KASSERT(fd < OPEN_MAX);
    array_remove(ft->entrys, fd);
}

int ft_copy(struct proc *proc_src, struct proc *proc_dst)
{
    int err;
    lock_acquire(proc_src->p_fdArray->fda_lock);
    lock_acquire(proc_dst->p_fdArray->fda_lock);
    for (unsigned int i = 0; i < array_num(proc_src->p_fdArray); i++)
    {
        struct fd_entry *fe_old = (struct fd_entry *)array_get(proc_src->p_fdArray, i);
        struct fd_entry *fe_new = kmalloc(sizeof(struct fd_entry));
        KASSERT(fe_new != NULL);

        lock_acquire(fe_old->file->file_lock);
        KASSERT(fe_old->file->valid && fe_old->file->refcount > 0);
        fe_old->file->refcount++;
        VOP_INCREF(fe_old->file->vn);
        fe_new->fd = fe_old->fd;
        fe_new->file = fe_old->file;
        lock_acquire(proc_dst->p_filetable->ft_lock);
        err = filetable_add(proc_dst->p_filetable, fe_new->file);
        if (err)
        {
            lock_release(proc_dst->p_filetable->ft_lock);
            lock_release(fe_old->file->file_lock);
            lock_release(proc_src->p_fdArray->fda_lock);
            lock_release(proc_dst->p_fdArray->fda_lock);
            return err;
        }

        lock_release(proc_dst->p_filetable->ft_lock);
        lock_release(fe_old->file->file_lock);
        array_add(proc_dst->p_fdArray, fe_new, NULL);
    }

    lock_release(proc_src->p_fdArray->fda_lock);
    lock_release(proc_dst->p_fdArray->fda_lock);
}