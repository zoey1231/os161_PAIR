#include <types.h>
#include <vfs.h>
#include <current.h>
#include <syscall.h>
#include <filetable.h>
#include <proc.h>
#include <uio.h>
#include <vnode.h>
#include <lib.h>
#include <kern/stat.h>
#include <kern/fcntl.h>
#include <kern/errno.h>
#include <vm.h>
#include <array.h>
#include <synch.h>
#include <copyinout.h>
#include <limits.h>
#include <kern/seek.h>
#include <kern/stattypes.h>
#include <machine/trapframe.h>
// #include <addrspace.h>

/*
 * opens the file, device, or other kernel object named by the pathname filename. 
 * The flags argument specifies how to open the file.
 * On success, open returns a nonnegative file descriptor at retVal. 
 * On error, the corresponding error code is returned.
*/
int sys_open(const userptr_t filename, int flags, unsigned int *retVal)
{
    struct vnode *vn;
    char *path;
    size_t *path_len;
    int err;
    struct file *file;
    struct fd_entry *fe;
    unsigned int fd = 0;

    path = kmalloc(PATH_MAX);
    path_len = kmalloc(sizeof(int));

    // Copy the filename into kernel space's address path
    err = copyinstr((const_userptr_t)filename, path, PATH_MAX, path_len);
    if (err)
    {
        kfree(path);
        kfree(path_len);
        return err;
    }

    //check if too many files have opened already

    lock_acquire(curproc->p_fdArray->fda_lock);
    if (array_num(curproc->p_fdArray->fdArray) >= OPEN_MAX)
    {
        lock_release(curproc->p_fdArray->fda_lock);
        return EMFILE;
    }

    // update fd to the number of files have opened. fd should be associated to
    // the new file after the loop
    while (fd_get(curproc->p_fdArray->fdArray, fd, NULL) != NULL)
    {
        fd += 1;
    }
    // Open file and grab the vnode associated with it
    err = vfs_open(path, flags, 0, &vn);

    kfree(path);
    kfree(path_len);

    if (err)
    {
        lock_release(curproc->p_fdArray->fda_lock);
        return err;
    }
    file = kmalloc(sizeof(struct file));
    if (file == NULL)
    {
        lock_release(curproc->p_fdArray->fda_lock);
        return ENOMEM;
    }
    file->refcount = 1;
    file->file_lock = lock_create("file_lock");
    file->status = flags;
    file->vn = vn;
    file->valid = 1;

    //since O_APPEND causes all writes to the file to occur at the end of file, set file's offset to file's size
    if (flags & O_APPEND)
    {
        struct stat *stat;
        stat = kmalloc(sizeof(struct stat));
        if (stat == NULL)
        {
            lock_release(curproc->p_fdArray->fda_lock);

            return ENOMEM;
        }
        VOP_STAT(vn, stat);
        file->offset = stat->st_size;
    }
    else
    {
        file->offset = 0;
    }

    //add the file's kernel representation to the process's file table
    fe = kmalloc(sizeof(struct fd_entry));
    if (fe == NULL)
    {
        lock_release(curproc->p_fdArray->fda_lock);

        return EMFILE;
    }

    fe->fd = fd;
    fe->file = file;
    *retVal = fd;
    lock_acquire(curproc->p_filetable->ft_lock);
    err = filetable_add(curproc->p_filetable, file);
    if (err)
    {
        lock_release(curproc->p_fdArray->fda_lock);
        lock_release(curproc->p_filetable->ft_lock);
        return err;
    }
    // build file entry and save it to the fd_Array
    err = array_add(curproc->p_fdArray->fdArray, fe, NULL);
    {
        lock_release(curproc->p_fdArray->fda_lock);
        lock_release(curproc->p_filetable->ft_lock);
        return err;
    }

    lock_release(curproc->p_fdArray->fda_lock);
    lock_release(curproc->p_filetable->ft_lock);
    return 0;
}

/**
 * Close the file handle fd. The same file handle may then be returned again from open, dup2, pipe, 
 * or similar calls. 
 * On success,return 0.
 * On error, the corresponding error code is returned.
 */
int sys_close(int fd)
{
    struct file *file;

    lock_acquire(curproc->p_fdArray->fda_lock);
    int index = -1;
    struct fd_entry *fe = fd_get(curproc->p_fdArray->fdArray, fd, &index);
    if (index == -1)
    {
        lock_release(curproc->p_fdArray->fda_lock);
        return EBADF;
    }

    file = fe->file;
    lock_acquire(file->file_lock);

    //decrement the reference count of the file, if refcount is 0 after the decrement,
    //remove the file completely since there is no one has opened the file
    //otherwise, simply remove the file with fd from the filetable
    KASSERT(file->refcount >= 1);
    file->refcount -= 1;
    if (file->refcount == 0)
    {
        vfs_close(file->vn);
        file->vn = NULL;
        file->offset = 0;
        file->valid = 0;
        lock_release(file->file_lock);
        lock_destroy(file->file_lock);
        fe->file = NULL;
        // remove file entry from the kernel representation
        array_remove(curproc->p_fdArray->fdArray, (unsigned)index);
        kfree(fe);
    }
    else
    {
        fe->file = NULL;
        // remove file entry from the kernel representation
        array_remove(curproc->p_fdArray->fdArray, (unsigned)index);
        kfree(fe);
        lock_release(file->file_lock);
    }

    lock_release(curproc->p_fdArray->fda_lock);
    return 0;
}
/**
 * reads up to buflen bytes from the file specified by fd, at the location in the file specified by 
 * the current seek position of the file, and stores them in the space pointed to by buf. 
 * The file must be open for reading.
 * The count of bytes read is returned in retVal.
 * On success,return 0.
 * On error, the corresponding error code is returned.
 */
int sys_read(int fd, userptr_t buf, size_t buflen, int *retVal)
{
    struct iovec iovec;
    struct uio uio;
    struct file *file;
    int err;

    char *kbuf = kmalloc(sizeof(char *));
    // checking to see if buf provided is in user space
    err = copyin((const_userptr_t)buf, (void *)kbuf, sizeof(char *));
    if (err)
    {
        kfree(kbuf);
        return EFAULT;
    }

    int index = -1;
    lock_acquire(curproc->p_fdArray->fda_lock);
    struct fd_entry *fe = fd_get(curproc->p_fdArray->fdArray, fd, &index);
    if (index == -1)
    {
        lock_release(curproc->p_fdArray->fda_lock);
        return EBADF;
    }

    file = fe->file;
    lock_acquire(file->file_lock);
    //check if fd was opened for reading
    if (file->status & O_WRONLY)
    {
        lock_release(file->file_lock);
        lock_release(curproc->p_fdArray->fda_lock);
        return EBADF;
    }

    //set up uio structure for read
    iovec.iov_ubase = buf;
    iovec.iov_len = buflen;
    uio.uio_iov = &iovec;
    uio.uio_iovcnt = 1;
    uio.uio_resid = buflen;
    uio.uio_offset = file->offset;
    uio.uio_segflg = UIO_USERSPACE;
    uio.uio_rw = UIO_READ;
    uio.uio_space = curproc->p_addrspace;

    err = VOP_READ(file->vn, &uio);
    if (err)
    {
        lock_release(file->file_lock);
        lock_release(curproc->p_fdArray->fda_lock);
        return err;
    }

    file->offset += (off_t)(buflen - uio.uio_resid);
    lock_release(file->file_lock);
    lock_release(curproc->p_fdArray->fda_lock);
    *retVal = (int)buflen - uio.uio_resid;
    kfree(kbuf);

    return 0;
}
/**
 * writes up to buflen bytes to the file specified by fd, at the location in the file specified by the current 
 * seek position of the file, taking the data from the space pointed to by buf. 
 * The file must be open for writing.
 * The count of bytes written is returned in retVal.
 * On success,return 0.
 * On error, the corresponding error code is returned.
 */
int sys_write(int fd, const_userptr_t buf, size_t nbytes, int *retVal)
{
    struct file *file;
    char *kbuf = kmalloc(sizeof(char *));
    int err;
    // checking to see if buf provided is in user space
    err = copyin(buf, (void *)kbuf, sizeof(char *));

    if (err)
    {
        kfree(kbuf);
        return EFAULT;
    }

    int index = -1;
    lock_acquire(curproc->p_fdArray->fda_lock);
    struct fd_entry *fe = fd_get(curproc->p_fdArray->fdArray, fd, &index);
    if (index == -1)
    {
        lock_release(curproc->p_fdArray->fda_lock);
        return EBADF;
    }

    file = fe->file;
    lock_acquire(file->file_lock);
    //check if fd was opened for writing
    if (!(file->status & (O_WRONLY | O_RDWR)))
    {
        lock_release(file->file_lock);
        lock_release(curproc->p_fdArray->fda_lock);
        return EBADF;
    }

    //set up uio structure for write
    struct uio uio;
    struct iovec iovec;
    iovec.iov_ubase = (userptr_t)buf;
    iovec.iov_len = nbytes;
    uio.uio_iov = &iovec;
    uio.uio_iovcnt = 1;
    uio.uio_resid = nbytes;
    uio.uio_offset = file->offset;
    uio.uio_segflg = UIO_USERSPACE;
    uio.uio_rw = UIO_WRITE;
    uio.uio_space = curproc->p_addrspace;

    err = VOP_WRITE(file->vn, &uio);
    if (err)
    {
        lock_release(file->file_lock);
        lock_release(curproc->p_fdArray->fda_lock);
        return err;
    }

    file->offset += (off_t)(nbytes - uio.uio_resid);
    lock_release(file->file_lock);
    lock_release(curproc->p_fdArray->fda_lock);
    *retVal = (int)nbytes - uio.uio_resid;
    kfree(kbuf);

    return 0;
}
/**
 * alter the current seek position of the file with file handle fd, seeking to a new position based on 
 * pos and whence.
 * On success,return the new position in retVal.
 * On error, the corresponding error code is returned.
 */
int sys_lseek(int fd, off_t pos, userptr_t whence, int64_t *retVal)
{
    int err;
    struct stat *stat;
    off_t new_pos;
    int k_whence = 0;
    // bring in whence from userspace to kernel space
    err = copyin((const_userptr_t)whence, &k_whence, sizeof(int32_t));
    if (err)
    {
        return EINVAL;
    }

    // whence has to be either SEEK_SET or SEEK_CUR or SEEK_END. return EINVAL if anything else
    if (!(k_whence == SEEK_SET || k_whence == SEEK_CUR || k_whence == SEEK_END))
    {
        return EINVAL;
    }

    int index = -1;
    lock_acquire(curproc->p_fdArray->fda_lock);
    struct fd_entry *fe = fd_get(curproc->p_fdArray->fdArray, fd, &index);
    // return EBAD if can't find the file entry with given fd
    if (index == -1)
    {
        lock_release(curproc->p_fdArray->fda_lock);
        return EBADF;
    }
    lock_release(curproc->p_fdArray->fda_lock);

    struct file *file = fe->file;
    lock_acquire(file->file_lock);
    if (!file->valid)
    {
        lock_release(file->file_lock);
        return EBADF;
    }

    //check if object is seekable. i.e., is not console device etc.
    if (!VOP_ISSEEKABLE(file->vn))
    {
        lock_release(file->file_lock);
        return ESPIPE;
    }
    //seeking to a new position based on pos and whence
    switch (k_whence)
    {
    case SEEK_SET:
        new_pos = pos;
        break;
    case SEEK_CUR:
        new_pos = file->offset + pos;
        break;
    case SEEK_END:
        //get the file's original size
        stat = kmalloc(sizeof(struct stat));
        KASSERT(stat != NULL);
        VOP_STAT(file->vn, stat);
        new_pos = stat->st_size + pos;
        kfree(stat);
        break;
    }

    //Seek positions < zero are invalid. Seek positions beyond EOF are legal
    if (new_pos < 0)
    {
        lock_release(file->file_lock);
        return EINVAL;
    }

    file->offset = new_pos;
    lock_release(file->file_lock);
    *retVal = new_pos;

    return 0;
}
/**
 * dup2 clones the file handle oldfd onto the file handle newfd. 
 * The two handles refer to the same object and share the same seek pointer.
 * On success,return newfd in retVal.
 * On error, the corresponding error code is returned.
 */
int sys_dup2(int oldfd, int newfd, int *retVal)
{
    if (newfd < 0 || oldfd < 0 || newfd >= OPEN_MAX || oldfd >= OPEN_MAX)
        return EBADF;

    lock_acquire(curproc->p_fdArray->fda_lock);

    struct fd_entry *fe = fd_get(curproc->p_fdArray->fdArray, oldfd, NULL);

    /* check if the file that oldfd points to exists and valid */
    if (fe == NULL || fe->file == NULL)
    {
        lock_release(curproc->p_fdArray->fda_lock);
        return EBADF;
    }
    if (!fe->file->valid)
    {
        lock_release(curproc->p_fdArray->fda_lock);
        return EBADF;
    }

    // return the existing file if oldfd is the same as the newfd
    if (oldfd == newfd)
    {
        *retVal = newfd;
        lock_release(curproc->p_fdArray->fda_lock);
        return 0;
    }

    struct fd_entry *newfd_fe = fd_get(curproc->p_fdArray->fdArray, newfd, NULL);
    // check if file that newfd points to exists, kill it if it does
    if (newfd_fe != NULL)
    {
        lock_release(curproc->p_fdArray->fda_lock);
        sys_close(newfd);
        lock_acquire(curproc->p_fdArray->fda_lock);
    }

    if (array_num(curproc->p_fdArray->fdArray) >= OPEN_MAX)
    {
        lock_release(curproc->p_fdArray->fda_lock);
        return EMFILE;
    }

    struct fd_entry *new_fe = kmalloc(sizeof(struct fd_entry));
    if (new_fe == NULL)
    {
        lock_release(curproc->p_fdArray->fda_lock);
        return EMFILE;
    }

    new_fe->fd = newfd;
    new_fe->file = fe->file;

    lock_acquire(fe->file->file_lock);
    fe->file->refcount++;
    lock_release(fe->file->file_lock);

    array_add(curproc->p_fdArray->fdArray, new_fe, NULL);
    *retVal = newfd;
    lock_release(curproc->p_fdArray->fda_lock);
    return 0;
}
/**
 * The current directory of the current process is set to the directory named by pathname.
 * On success,return 0.
 * On error, the corresponding error code is returned.
 */
int sys_chdir(const char *pathname)
{
    if (!pathname)
    {
        return EFAULT;
    }

    char *path;
    path = kmalloc(PATH_MAX);
    size_t *path_len = kmalloc(sizeof(int));

    // Copy the filename into kernel space's address path
    int err = copyinstr((const_userptr_t)pathname, path, PATH_MAX, path_len);
    if (err)
    {
        kfree(path);
        kfree(path_len);
        return err;
    }

    kfree(path_len);

    err = vfs_chdir(path);
    if (err)
    {
        kfree(path);
        return err;
    }
    else
    {
        kfree(path);
        return 0;
    }
}
/**
 * Stores the current working directory name in buf, an area of size buflen.
 * On success, returns the actural length of the data returned in retVal.
 * On error, the corresponding error code is returned.
 */
int sys___getcwd(char *buf, size_t buflen, int *retVal)
{
    int err;
    struct uio uio;
    struct iovec iovec;

    if (!buf)
        return EFAULT;
    char *temp_buf = kmalloc(sizeof(char *));

    //initialize an uio structure to get cwd
    uio_kinit(&iovec, &uio, temp_buf, buflen, 0, UIO_READ);

    err = vfs_getcwd(&uio);
    if (err)   
    {
        return err;
    }

    //copy the the path from kernel to user space
    err = copyout((const void *)temp_buf, (userptr_t)buf, (size_t)(sizeof(char *)));
    kfree(temp_buf);

    if (err)
        return err;

    *retVal = buflen - uio.uio_resid; // either way should work
    // *retVal = uio.uio_offset;
    return 0;
}

void enter_usermode(void *data1, unsigned long data2)
{
    (void)data2;
    void *tf = (void *)curthread->t_stack + 16;

    memcpy(tf, (const void *)data1, sizeof(struct trapframe));
    kfree((struct trapframe *)data1);

    as_activate();
    mips_usermode(tf);
}
/**
 * Copys the currently running process.
 * The two copies are identical except the child process has a new, unique process id.
 * The two processes do not share memory or open file tables. However, the file handle objects the file 
 * tables point to are shared.
 * On success, fork returns twice, once in the parent process and once in the child process.
 * In the child process, 0 is returned. In the parent process, the new child process's pid is returned.
 * On error, no new process is created. fork only returns once with corresponding error code.
 */
int sys_fork(struct trapframe *tf, int *retval)
{

    // declare a new process:
    struct proc *new_proc;
    new_proc = proc_create("new process");
    if (new_proc == NULL)
    {
        return ENOMEM;
    }

    // add the new process to the pid table
    int ret = pidtable_add(new_proc, &new_proc->pid);
    if (ret)
    {
        proc_destroy(new_proc);
        return ret;
    }

    // copy address space from the parent process
    ret = as_copy(curproc->p_addrspace, &new_proc->p_addrspace, new_proc->pid);
    if (ret)
    { // if copy is not successful, free new process
        pidtable_freepid(new_proc->pid);
        proc_destroy(new_proc);
        return ret;
    }

    spinlock_acquire(&curproc->p_lock);
    if (curproc->p_cwd != NULL)
    {
        VOP_INCREF(curproc->p_cwd);
        new_proc->p_cwd = curproc->p_cwd;
    }
    spinlock_release(&curproc->p_lock);

    // copy filetable from the parent process
    ret = ft_copy(curproc, new_proc);
    if (ret)
    {
        pidtable_freepid(new_proc->pid);
        proc_destroy(new_proc);
        return ret;
    }

    // copy and tweak trapframe
    struct trapframe *tf_dup = (struct trapframe *)kmalloc(sizeof(struct trapframe));
    if (tf_dup == NULL)
    {
        return ENOMEM;
    }
    memcpy((void *)tf_dup, (const void *)tf, sizeof(struct trapframe));
    tf_dup->tf_v0 = 0;
    tf_dup->tf_v1 = 0;
    tf_dup->tf_a3 = 0;
    tf_dup->tf_epc += 4;

    *retval = new_proc->pid;
    ret = thread_fork("new_thread", new_proc, enter_usermode, tf_dup, 1);
    if (ret)
    {
        proc_destroy(new_proc);
        pidtable_freepid(new_proc->pid);
        kfree(tf_dup);
        return ret;
    }
    return 0;
}

/**
 * Replaces the currently executing program with a newly loaded program image.
 * The pathname of the program to run is passed as program. The args argument is an array of 0-terminated strings.
 * The array itself should be terminated by a NULL pointer.The argument strings should be copied into the new 
 * process as the new process's argv[] array. In the new process, argv[argc] must be NULL. 
 * By convention, argv[0] in new processes contains the name that was used to invoke the program. 
 * This is not necessarily the same as program, and furthermore is only a convention and should not be enforced by the kernel. 
 * The process file table and current working directory are not modified by execv.
 * On success, execv does not return; instead, the new program begins executing.
 * On failure, the corresponding error code is returned.
 */
int sys_execv(const char *program, char **args)
{
    int ret;
    int strSize_total;
    char *progname;
    if (program == NULL || args == NULL)
        return EFAULT; // indicates that one of the argument is an invalid pointer

    //copy the arguments from the old address space
    size_t *path_len = kmalloc(sizeof(size_t));
    ret = string_in(program, &progname, PATH_MAX, path_len);
    if (ret)
    {
        kfree(path_len);
        return ret;
    }
    kfree(path_len);

    //get number of arguments
    int argc;
    ret = get_argc(args, &args);
    if (ret)
    {
        kfree(progname);
        return ret;
    }

    char **args_copy = kmalloc(argc * sizeof(char *));
    int *size_arr = kmalloc(argc * sizeof(int));
    ret = copy_in_args(argc, args, args_copy, size_arr, &strSize_total);
    if (ret)
    {
        kfree(args_copy);
        kfree(size_arr);
        kfree(progname);
        return ret;
    }

    //get a new address space
    struct addrspace *as_old = proc_getas();
    struct addrspace *as_new = as_create();
    if (as_new == NULL)
    {
        kfree(progname);
        free_copied_args(argc, size_arr, args_copy);
        return ENOMEM; // insufficient virtual memory is available
    }

    struct vnode *vn;
    vaddr_t entrypoint, stackptr;
    ret = vfs_open(progname, O_RDONLY, 0, &vn);
    if (ret)
    {
        kfree(progname);
        as_destroy(as_new);
        free_copied_args(argc, size_arr, args_copy);
        return ENOMEM;
    }

    //switch to the new address space
    proc_setas(as_new);
    as_activate();

    //load a new executable
    ret = load_elf(vn, &entrypoint);
    if (ret)
    {
        //if err, switch back to old address space
        proc_setas(as_old);
        as_activate();
        as_destroy(as_new);

        vfs_close(vn);
        kfree(progname);
        free_copied_args(argc, size_arr, args_copy);
        return ret;
    }
    vfs_close(vn);
    //define a new stack region
    ret = as_define_stack(as_new, &stackptr);
    if (ret)
    {
        proc_setas(NULL);
        as_deactivate();

        proc_setas(as_old);
        as_activate();

        as_destroy(as_new);
        kfree(progname);
        free_copied_args(argc, size_arr, args_copy);
        return ret;
    }

    //copy the arguments to the new address space, properly arranging them
    userptr_t argv;
    copy_out_args(argc, args_copy, size_arr, strSize_total, &stackptr, &argv);

    kfree(progname);
    free_copied_args(argc, size_arr, args_copy);

    //clean up the old address space and
    as_destroy(as_old);
    //wrap to user mode
    enter_new_process(argc, argv, NULL, stackptr, entrypoint);

    // enter_new_process should not return
    panic("enter_new_process returned in sys_execv\n");
    return EINVAL;
}
/**
 * Returns the process id of the current process.
 * sys_getpid does not fail.
 */
int sys_getpid(int *retval)
{
    lock_acquire(pidtable->pid_lock);
    *retval = curproc->pid;
    lock_release(pidtable->pid_lock);
    return 0;
}
/**
 * Cause the current process to exit. 
 * The exit code exitcode is reported back to other process(es) via the waitpid() call.
 * The process id of the exiting process should not be reused until all processes expected to collect 
 * the exit code with waitpid have done so.
 * sys__exit does not return.
 */
void sys__exit(int exitcode) {
    KASSERT(curproc != NULL);

	lock_acquire(pidtable->pid_lock);

    // update the status of children of current process accordingly 
	int num_child = array_num(curproc->children);

	for(int i = num_child-1; i >= 0; i--){

		struct proc *child = array_get(curproc->children, i);
		int child_pid = child->pid;

        // make child ORPHAN if it's actively running
		if(pidtable->pid_status[child_pid] == RUNNING){
			pidtable->pid_status[child_pid] = ORPHAN;
		}

        // ZOMBIE refers to a child proc "dead" but not yet "reaped" 
		else if (pidtable->pid_status[child_pid] == ZOMBIE){
			/* Update the next pid indicator, child's pid is free to use after being reaped*/
			if(child_pid < pidtable->pid_next){
				pidtable->pid_next = child_pid;
			}

            // reap the evil zombie child
			proc_destroy(child);
			clear_pid(child_pid);
		}
		else{
			panic("Tried to modify a child that did not exist.\n");
		}
	}

	/* Case: Signal the parent that the child ended with waitcode given. */
	if(pidtable->pid_status[curproc->pid] == RUNNING){
		pidtable->pid_status[curproc->pid] = ZOMBIE;
		pidtable->pid_waitcode[curproc->pid] = waitcode;
	}
	/* Case: Parent already exited. Reset the current pidtable spot for later use. */
	else if(pidtable->pid_status[curproc->pid] == ORPHAN){
		pid_t pid = curproc->pid;
		proc_destroy(curproc);
		clear_pid(pid);
	}
	else{
		panic("Tried to remove a bad process.\n");
	}

	/* Broadcast to any waiting processes. There is no guarentee that the processes on the cv are waiting for us */
	cv_broadcast(pidtable->pid_cv, pidtable->pid_lock);

	lock_release(pidtable->pid_lock);

	thread_exit();
}
/**
 * Wait for the process specified by pid to exit, and return an encoded exit status in the integer pointed 
 * to by status. If that process has exited already, waitpid returns immediately. 
 * If that process does not exist, waitpid fails.
 * It is explicitly allowed for status to be NULL, in which case waitpid operates normally but the status value
 * is not produced.
 * A process moves from "has exited already" to "does not exist" when every process that is expected to collect
 * its exit status with waitpid has done so.
 * If a parent process exits before one or more of its children, it can no longer be expected collect their exit status. 
 * The options argument should be 0. You are not required to implement any options. (However, the system should 
 * check to make sure that requests for options the system do not support are rejected.)
 * On success, returns the process id whose exit status is reported in status. 
 * On failure, the corresponding error code is returned.
 */
int sys_waitpid(pid_t pid, int *status, int options, int *retval) {

    // not required to implement options in OS161.
    // by convention, options is 0
    if (options != 0) {
        return EINVAL; // The options argument requested invalid or unsupported options.
    }

    // the process pid represents is invalid
    if (pid < PID_MIN || pid > PID_MAX || pidtable->pid_status[pid] == READY) { 
        return ESRCH; // The pid argument named a nonexistent process.
    }

    // check if pid points to is a child process of the current process

    // bool var indicates if pid is a child of current process
    int flag = 0;
    // retrieve the process pid represents
    struct proc *proc_of_pid = pidtable->pid_procs[pid];
    int num_of_children = array_num(curproc->children);
    for (int i = 0; i < num_of_children; ++i) {
        // 
        struct proc *child = array_get(curproc->children, i);
        if (child == proc_of_pid) {
            flag = 1;
        } 
    }
    if (!flag) {
        return ECHILD; //	The pid argument named a process that was not a child of the current process.
    }

    int waitcode = 0;
    lock_acquire(pidtable->pid_lock);
    *status = pidtable->pid_status[pid];

    // wait until the child process to exit
    while(status != ZOMBIE) {
        cv_wait(pidtable->pid_cv, pidtable->pid_lock);
        status = pidtable->pid_status[pid];
    }
    waitcode = pidtabl e->pid_waitcode[pid];
    lock_release(pidtable->pid_lock);

    // a NULL value in retval means don't expect anything to be returned
    if (retval != NULL) {
        int ret = copyout(&waitcode, (userptr_t) retval, sizeof(int32_t));
        if (ret) return ret;
    }

    return 0;
}

static int
string_in(const char *user_src, char **kern_dest, size_t copy_len, size_t *actural_len)
{
    int ret;

    copy_len++;
    *kern_dest = kmalloc(copy_len * sizeof(char));

    ret = copyinstr((const_userptr_t)user_src, *kern_dest, copy_len, actural_len);
    if (ret)
    {
        kfree(*kern_dest);
        return ret;
    }

    return 0;
}

/**
 * Return the number of arguments in argc
 * Return error if there are more arguments than ARG_MAX
 */
static int get_argc(char **args, int *argc)
{
    int ret;
    int i = 0;
    char *next_arg;

    //count the argument number one by one until we meet the null terminator, also check if there are too many arguments
    do
    {
        i++; // i starts from 1
        ret = copyin((const_userptr_t)&args[i], (void *)&next_arg, (size_t)sizeof(char *));
        if (ret)
        {
            return ret;
        }
    } while (next_arg != NULL && (i + 1) * (sizeof(char *)) <= ARG_MAX);

    if (next_arg != NULL)
    {
        return E2BIG;
    }

    *argc = i;
    return 0;
}

/**
*Copies the user strings into the kernel, populating size[] with their respective lengths.
*Returns an error if an argument's length exceeds ARG_MAX.
*/
static int
copy_in_args(int argc, char **args, char **args_copy, int *size_arr, int *strSize_total) //TODO:this implementation might not pass bigexecv test
{
    int err;
    size_t str_len, aligned_strlen;
    char **kaddr = (char **)kmalloc(sizeof(char *));
    *strSize_total = 0;
    for (int i = 0; i < argc; i++)
    {
        err = string_in((const char *)args[i], &args_copy[i], ARG_MAX, &str_len);
        if (err)
        {
            for (int j = 0; j < i; j++)
            {
                kfree(args_copy[j]);
            }
            return err;
        }
        //align the string pointer to 4 if not already
        if (str_len % 4 != 0)
        {
            aligned_strlen = 4 * (str_len / 4) + 4;
        }
        else
        {
            aligned_strlen = strlen;
        }
        size_arr[i] = aligned_strlen;
        *strSize_total += aligned_strlen;
    }
    return 0;
}

static void free_copied_args(int argc, int *size, char **args_copy)
{
    for (int i = 0; i < argc; i++)
    {
        kfree(args_copy[i]);
    }
    kfree(size);
    kfree(args_copy);
}
static void copy_out_args(int argc, char **args_copy, int *size, int strSize_total, vaddr_t *stackptr, userptr_t *argv)
{
    size_t actual_len;
    int err;
    userptr_t argv_base_addr = (*stackptr - (argc + 1) * sizeof(userptr_t *) - strSize_total * sizeof(char));
    userptr_t str_base_addr = (*stackptr - strSize_total * sizeof(char));

    *stackptr = argv_base_addr;
    *argv = argv_base_addr;
    for (int i = 0; i < argc; i++)
    {
        //copy out argument's pointer to the new stack
        err = copyout(&args_copy[i], argv_base_addr, sizeof(char *));
        if (err)
        {
            return err;
        }
        //copy out argument string to the new stack
        err = copyoutstr(args_copy[i], str_base_addr, size[i], &actual_len);
        if (err)
        {
            return err;
        }

        //adjust pointers to point to the next argument's position on stack
        argv_base_addr += sizeof(userptr_t *);
        str_base_addr += size[i];
    }
}