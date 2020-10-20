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
