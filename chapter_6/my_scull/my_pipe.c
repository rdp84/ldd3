#include <linux/module.h>
#include <linux/moduleparam.h>

#include <linux/kernel.h>  /* printk, container_of */
#include <linux/slab.h>    /* kmalloc */
#include <linux/fs.h>      /* everything... */
#include <linux/proc_fs.h> /* writing to /proc for debugging */
#include <linux/errno.h>   /* */
#include <linux/types.h>   /* size_t, dev_t, MAJOR, MINOR, MKDEV */
#include <linux/fcntl.h>   /* O_ACCMODE */
#include <linux/cdev.h>    /* cdev */
#include <linux/sched.h>
#include <asm/uaccess.h>

/* local definitions */
#include "my_scull.h"

struct my_scull_pipe {
  wait_queue_head_t inq, outq; /* read and write queues */
  char *buffer, *end;          /* begin of buf, end of buf */
  int buffersize;              /* used in pointer arithmetic*/
  char *rp, *wp;               /* where to read, where to write */
  int nreaders, nwriters;      /* number of openings for reading/writing */
  struct semaphore sem;        /* mututal exclusion semaphore */
  struct cdev cdev;            /* char device structure */
};

/* Parameters */
static int my_scull_p_nr_devs = MY_SCULL_P_NR_DEVS; /* number of pipe devices */
int my_scull_p_buffer = MY_SCULL_P_BUFFER;       /* buffer size */
dev_t my_scull_p_devno;                          /* Our first device number */

module_param(my_scull_p_nr_devs, int, 0);
module_param(my_scull_p_buffer, int, 0);

static struct my_scull_pipe *my_scull_p_devices;

static int space_free(struct my_scull_pipe *dev);

/*
 * Open and close
 */

static int my_scull_p_open(struct inode *inode, struct file *filp)
{
  struct my_scull_pipe *dev; /* device information */

  /*
   * Identify the device that is being opened
   * Can get the cdev from inode but we need the dev that this cdev is
   * contained in
   */
  dev = container_of(inode->i_cdev, struct my_scull_pipe, cdev);
  /* store pointer to make access easier in the future */
  filp->private_data = dev;

  if (down_interruptible(&dev->sem))
    return -ERESTARTSYS;

  /* Check if we need to allocate the buffer and do so */
  if (!dev->buffer) {
    dev->buffer = kmalloc(my_scull_p_buffer, GFP_KERNEL);
    if (!dev->buffer) {
      up(&dev->sem);
      return -ENOMEM;
    }
  }

  /* Set fields correctly */
  dev->buffersize = my_scull_p_buffer;
  dev->end = dev->buffer + dev->buffersize;
  dev->rp = dev->wp = dev->buffer;

  if (filp->f_mode & FMODE_READ)
    dev->nreaders++;
  if (filp->f_mode & FMODE_WRITE)
    dev->nwriters++;

  up(&dev->sem);
  /* Device does not support seeking */
  return nonseekable_open(inode, filp);
}

static int my_scull_p_release(struct inode *inode, struct file *filp)
{
  struct my_scull_pipe *dev = filp->private_data;

  down(&dev->sem);
  if (filp->f_mode & FMODE_READ)
    dev->nreaders--;
  if (filp->f_mode & FMODE_WRITE)
    dev->nwriters--;
  if (dev->nreaders + dev->nwriters == 0) {
    kfree(dev->buffer);
    dev->buffer = NULL; /* the other fields are not checked on open */
  }
  up(&dev->sem);
  return 0;
}

/*
 * Data management: read and write
 */

static ssize_t my_scull_p_read(struct file *filp, char __user *buf, size_t count,
                               loff_t *f_pos)
{
  struct my_scull_pipe *dev = filp->private_data;

  if (down_interruptible(&dev->sem))
    return -ERESTARTSYS;

  while (dev->rp == dev->wp) {          /* nothing to read */
    up(&dev->sem);                      /* release the lock */
    if (filp->f_flags & O_NONBLOCK)     /* if the process doesn't want to block, tell it to try again */
      return -EAGAIN;
    PDEBUG("\"%s\" reading: going to sleep\n", current->comm);
    if (wait_event_interruptible(dev->inq, (dev->rp != dev->wp)))
      return -ERESTARTSYS;             /* OS signal could have woken the process up: tell the fs layer to handle it */
    /* still not sure there is data for the taking, reacquire the lock and loop */
    if (down_interruptible(&dev->sem))
      return -ERESTARTSYS;
  }

  /* ok, data is there, return something */
  if (dev->wp > dev->rp)
    count = min(count, (size_t)(dev->wp - dev->rp));
  else /* the write pointer has wrapped, return data up to dev->end */
    count = min(count, (size_t)(dev->end - dev->rp));

  if (copy_to_user(buf, dev->rp, count)) {
    up(&dev->sem);
    return -EFAULT;
  }

  dev->rp += count;
  if (dev->rp == dev->end) /* wrapped so move to start of buffer */
    dev->rp = dev->buffer;
  up(&dev->sem);

  wake_up_interruptible(&dev->outq); /* awake any writers */
  PDEBUG("\"%s\" did read %li bytes\n", current->comm, (long) count);
  return count;
}

/*
 * Wait for space for writing; caller must hold device semaphore.
 * On error the semaphore is release before returning
 */
static int my_scull_p_get_write_space(struct my_scull_pipe *dev, struct file *filp)
{
  while (space_free(dev) == 0) { /* full */
    DEFINE_WAIT(wait);           /* macro to provide us with a wait_queue_t */

    up(&dev->sem);
    if (filp->f_flags & O_NONBLOCK)
      return -EAGAIN;            /* if the process doesn't want to block, tell it to try again */

    PDEBUG("\"%s\" writing: going to sleep\n", current->comm);

    /* add queue entry to the queue and set the state of the process to asleep */
    prepare_to_wait(&dev->outq, &wait, TASK_INTERRUPTIBLE);
    /* check condition we're sleeping for to avoid race condition and miss a wakeup */
    if (space_free(dev) == 0)
      schedule(); /* yield the processor  */

    /*
     * cleanup: set state of process to runnable. schedule() does this for us but we
     * may not have called that function if we didn't need to sleep.
     */
    finish_wait(&dev->outq, &wait);

    if (signal_pending(current))
      return -ERESTARTSYS; /* OS signal: tell the fs layer to handle it */
    if (down_interruptible(&dev->sem))
      return -ERESTARTSYS;
  }
  return 0;
}

/* How much space is free */
static int space_free(struct my_scull_pipe *dev)
{
  if (dev->rp == dev->wp)
    return dev->buffersize - 1;
  return ((dev->rp + dev->buffersize - dev->wp) % dev->buffersize) - 1;
}

static ssize_t my_scull_p_write(struct file *filp, const char __user *buf, size_t count,
                                loff_t *f_pos)
{
  struct my_scull_pipe *dev = filp->private_data;
  int result;

  if (down_interruptible(&dev->sem))
    return -ERESTARTSYS;

  result = my_scull_p_get_write_space(dev, filp);
  if (result)
    return result; /* my_scull_p_get_write_space called up(&dev->sem) */

  /* ok, spoace is there, accept something */
  count = min(count, (size_t) space_free(dev));
  if (dev->wp >= dev->rp)
    count = min(count, (size_t) (dev->end - dev->wp));    /* to end-of-buffer */
  else
    count = min(count, (size_t) (dev->rp - dev->wp - 1)); /* wp has wrapped, fill up to rp-1 */

  PDEBUG("Going to accept %li bytes to %p from %p\n", (long) count, dev->wp, buf);
  if (copy_from_user(dev->wp, buf, count)) {
    up(&dev->sem);
    return -EFAULT;
  }
  dev->wp += count;
  if (dev->wp == dev->end)
    dev->wp = dev->buffer; /* wrapped */
  up(&dev->sem);

  /* finally, awake any reader */
  wake_up_interruptible(&dev->inq); /* blocked in read() */

  PDEBUG("\"%s\" did write %li bytes\n", current->comm, (long) count);
  return count;
}


struct file_operations my_scull_pipe_fops = {
  .owner =    THIS_MODULE,
  .llseek =   no_llseek,
  .read =     my_scull_p_read,
  .write =    my_scull_p_write,
  .ioctl =    my_scull_ioctl,
  .open =     my_scull_p_open,
  .release =  my_scull_p_release,
};

/*
 * Setup a cdev entry
 */
static void my_scull_p_setup_cdev(struct my_scull_pipe *dev, int index)
{
  int err, devno = my_scull_p_devno + index;

  cdev_init(&dev->cdev, &my_scull_pipe_fops);
  dev->cdev.owner = THIS_MODULE;
  err = cdev_add(&dev->cdev, devno, 1);
  /* Fail gracefully if need be */
  if (err)
    printk(KERN_NOTICE "Error %d adding my_scull_pipe%d\n", err, index);
}

/*
 * Initialize the pipe devs; return how many we did.
 */
int my_scull_p_init(dev_t firstdev)
{
  int i, result;

  result = register_chrdev_region(firstdev, my_scull_p_nr_devs, "my_scull_pipe");
  if (result < 0) {
    printk(KERN_NOTICE "Unable to get scullp region, error %d\n", result);
    return 0;
  }

  my_scull_p_devno = firstdev;
  my_scull_p_devices = kmalloc(my_scull_p_nr_devs * sizeof(struct my_scull_pipe), GFP_KERNEL);
  if (my_scull_p_devices == NULL) {
    unregister_chrdev_region(firstdev, my_scull_p_nr_devs);
    return 0;
  }

  memset(my_scull_p_devices, 0, my_scull_p_nr_devs * sizeof(struct my_scull_pipe));
  for (i = 0; i < my_scull_p_nr_devs; i++) {
    init_waitqueue_head(&(my_scull_p_devices[i].inq));
    init_waitqueue_head(&(my_scull_p_devices[i].outq));
    init_MUTEX(&(my_scull_p_devices[i].sem));
    my_scull_p_setup_cdev(my_scull_p_devices + i, i);
  }

  return my_scull_p_nr_devs;
}

/*
 * This is called by cleanup_module or on failure
 */
void my_scull_p_cleanup(void)
{
  int i;

  if (!my_scull_p_devices)
    return; /* nothing else to release */

  for (i = 0; i < my_scull_p_nr_devs; i++) {
    cdev_del(&(my_scull_p_devices[i].cdev));
    kfree(my_scull_p_devices[i].buffer);
  }
  kfree(my_scull_p_devices);
  unregister_chrdev_region(my_scull_p_devno, my_scull_p_nr_devs);
  my_scull_p_devices = NULL;
}
