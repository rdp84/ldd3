#include <linux/init.h>
#include <linux/module.h>
#include <linux/moduleparam.h>

#include <linux/kernel.h>   /* printk, container_of */
#include <linux/slab.h>     /* kmalloc */
#include <linux/fs.h>       /* everything...*/
#include <linux/types.h>    /* size_t, dev_t, MAJOR, MINOR, MKDEV */
#include <linux/proc_fs.h>  /* writing to /proc for debugging */
#include <linux/seq_file.h> /* writing to /proc for debugging */
#include <linux/string.h>   /* memset */
#include <linux/fcntl.h>    /* O_ACCMODE */
#include <linux/cdev.h>     /* cdev */

#include <asm/uaccess.h>  /* copy_*_user */

#include "my_scull.h"

/*
 * Our parameters which can be set at load time.
 * Make them available for insmod to assign values at load time.
 */
int my_scull_major   = MY_SCULL_MAJOR;
int my_scull_minor   = 0;
int my_scull_nr_devs = MY_SCULL_NR_DEVS;
int my_scull_quantum = MY_SCULL_QUANTUM;
int my_scull_qset    = MY_SCULL_QSET;

module_param(my_scull_major, int, S_IRUGO);
module_param(my_scull_minor, int, S_IRUGO);
module_param(my_scull_nr_devs, int, S_IRUGO);
module_param(my_scull_quantum, int, S_IRUGO);
module_param(my_scull_qset, int, S_IRUGO);

MODULE_LICENSE("Dual BSD/GPL");
MODULE_AUTHOR("Bobby Pearson");
MODULE_DESCRIPTION("My Scull - Simple Character Utility for Loading Localities");
MODULE_VERSION("1.0");

struct my_scull_dev *scull_devices;  /* allocated in my_scull_init_module */

/*
 * Empty out the device
 */
int my_scull_trim(struct my_scull_dev *dev)
{
  struct my_scull_qset *next, *dataptr;
  int qset = dev->qset;
  int i;

  for (dataptr = dev->data; dataptr; dataptr = next) {
    if (dataptr->data) {
      for (i = 0; i < qset; i++)
        kfree(dataptr->data[i]); /* free the quantum */
      kfree(dataptr->data);      /* free the pointers */
      dataptr->data = NULL;
    }
    next = dataptr->next;
    kfree(dataptr);              /* free the allocation of the qset struct */
  }
  dev->size = 0;
  dev->quantum = my_scull_quantum;
  dev->qset = my_scull_qset;
  dev->data = NULL;

  return 0; /* success */
}

#ifdef MY_SCULL_DEBUG /* use proc only if debugging */

/*
 * The proc filesystem: function to read and entry
 */

int my_scull_read_procmem(char *buf, char **start, off_t offset,
                          int count, int *eof, void *data)
{
  int i, j, len = 0;
  int limit = count - 80; /* Don't print more than this */

  for (i = 0; i < my_scull_nr_devs && len <= limit; i++) {
    struct my_scull_dev *d = &scull_devices[i];
    struct my_scull_qset *qs = d->data;

    /* wait until we can obtain the semaphore */
    if (down_interruptible(&d->sem))
      return -ERESTARTSYS;

    len += sprintf(buf + len, "\nDevice %i: qset %i, q % i, sz %li\n",
                   i, d->qset, d->quantum, d->size);
    for (; qs && len <= limit; qs = qs->next) { /* scan the list */
      len += sprintf(buf + len, "  item at %p, qset at %p\n",
                     qs, qs->data);
      if (qs->data && !qs->next) /* dump on the last item */
        for (j = 0; j < d->qset; j++) {
          if (qs->data[j])
            len += sprintf(buf + len,
                           "    % 4i: %8p\n",
                           j, qs->data[j]);
        }
    }
    up(&d->sem); /* release the semaphore no matter what has happened */
  }
  *eof = 1;
  return len;
}

/*
 * For now the seq_file implementation will exist in parallel. The
 * older my_scull_read_procmem function should maybe go away though.
 */

/*
 * Here are our sequence iteration methods. Our "position" is
 * simply the device number.
 */

static void *my_scull_seq_start(struct seq_file *s, loff_t *pos)
{
  if (*pos >= my_scull_nr_devs)
    return NULL;   /* no more to read */
  return scull_devices + *pos;
}

static void *my_scull_seq_next(struct seq_file *s, void *v, loff_t *pos)
{
  (*pos)++;
  if (*pos >= my_scull_nr_devs)
    return NULL;
  return scull_devices + *pos;
}

static void my_scull_seq_stop(struct seq_file *s, void *v)
{
  /* Actually there's nothing to do here */
}

static int my_scull_seq_show(struct seq_file *s, void *v)
{
  struct my_scull_dev *dev = (struct my_scull_dev *) v;
  struct my_scull_qset *qs;
  int i;

  /* wait until we can obtain the semaphore */
  if (down_interruptible(&dev->sem))
    return -ERESTARTSYS;

  seq_printf(s, "\nDevice %i: qset %i, q %i, sz %li\n",
             (int) (dev - scull_devices), dev->qset,
             dev->quantum, dev->size);
  for (qs = dev->data; qs; qs = qs->next) { /* scan the list */
    seq_printf(s, "  item at %p, qset at %p\n", qs, qs->data);
    if (qs->data && !qs->next)              /* dump only the last item */
      for (i = 0; i < dev->qset; i++) {
        if (qs->data[i])
          seq_printf(s, "    % 4i: %8p\n",
                     i, qs->data[i]);
      }
  }
  up(&dev->sem); /* release the semaphore no matter what has happened */
  return 0;
}

/*
 * Tie the sequence operators up.
 */
static struct seq_operations my_scull_seq_ops = {
  .start = my_scull_seq_start,
  .next  = my_scull_seq_next,
  .stop  = my_scull_seq_stop,
  .show  = my_scull_seq_show
};

/*
 * Now to implement the /proc file we need only make an open
 * method which sets up the sequence operators.
 */
static int my_scull_proc_open(struct inode *inode, struct file *file)
{
  return seq_open(file, &my_scull_seq_ops);
}

/*
 * Create a set of file operations for our proc file.
 */
static struct file_operations my_scull_proc_ops = {
  .owner   = THIS_MODULE,
  .open    = my_scull_proc_open,
  .read    = seq_read,
  .llseek  = seq_lseek,
  .release = seq_release
};

/*
 * Actually create (and remove) the /proc file(s).
 */

static void my_scull_create_proc(void)
{
  struct proc_dir_entry *entry;
  create_proc_read_entry("myscullmem", 0 /* default mode */,
                         NULL /* parent dir */, my_scull_read_procmem,
                         NULL /* client data */);
  entry = create_proc_entry("myscullseq", 0, NULL);
  if (entry)
    entry->proc_fops = &my_scull_proc_ops;
}

static void my_scull_remove_proc(void)
{
  /* no problem if it was not regsitered */
  remove_proc_entry("myscullmem", NULL /* parent dir */);
  remove_proc_entry("myscullseq", NULL);
}


#endif /* MY_SCULL_DEBUG */

/*
 * Open and close
 */

/*
 * Do any initialization in preparation for later operations.
 */
int my_scull_open(struct inode *inode, struct file *filp)
{
  struct my_scull_dev *dev; /* device information */

  /*
   * Identify the device that is being opened
   * Can get the cdev from inode but we need the dev that this cdev is
   * contained in
   */
  dev = container_of(inode->i_cdev, struct my_scull_dev, cdev);
  /* Store pointer to make access easier in the future */
  filp->private_data = dev;

  /* now trim to 0 the length of the device if open was write-only */
  if ((filp->f_flags & O_ACCMODE) == O_WRONLY) {
    /* wait until we can obtain the semaphore */
    if (down_interruptible(&dev->sem))
      return -ERESTARTSYS;
    my_scull_trim(dev); /* ignore errors */
    up(&dev->sem); /* release the semaphore no matter what has happened */
  }
  return 0; /* success */
}

/*
 * Should perform the following tasks:
 * 1. Deallocate anything that open allocated in filp->private_data
 * 2. Shut down the device on last close
 *
 * This basic form has no hardware to shut down. Nothing was allocated
 * to filp->private_data in open
 */
int my_scull_release(struct inode *inode, struct file *filp)
{
  return 0;
}

/*
 * Follow the list
 */
struct my_scull_qset *my_scull_follow(struct my_scull_dev *dev, int n)
{
  struct my_scull_qset *qs = dev->data;

  /* Allocate first qset explicitly if need be */
  if (!qs) {
    qs = dev->data = kmalloc(sizeof(struct my_scull_qset), GFP_KERNEL);
    if (qs == NULL)
      return NULL;
    memset(qs, 0, sizeof(struct my_scull_qset));
  }

  /* Then follow the list */
  while (n--) {
    if (!qs->next) {
      qs->next = kmalloc(sizeof(struct my_scull_qset), GFP_KERNEL);
      if (qs->next == NULL)
        return NULL;
      memset(qs->next, 0, sizeof(struct my_scull_qset));
    }
    qs = qs->next;
    continue;
  }
  return qs;
}

/*
 * Data management: read and write
 */

ssize_t my_scull_read(struct file *filp, char __user *buf, size_t count,
                   loff_t *f_pos)
{
  struct my_scull_dev *dev = filp->private_data;
  struct my_scull_qset *dataptr;                 /* the first listitem */
  int quantum = dev->quantum, qset = dev->qset;
  int itemsize = quantum * qset;                 /* how many bytes in the listitem */
  int item, s_pos, q_pos, rest;
  ssize_t retval = 0;

  /* wait until we can obtain the semaphore */
  if (down_interruptible(&dev->sem))
    return -ERESTARTSYS;
  if (*f_pos >= dev->size)
    goto out;
  if (*f_pos + count > dev->size)
    count = dev->size - *f_pos;

  item = (long)*f_pos / itemsize; /* listitem - index into my_scull_qset list */
  rest = (long)*f_pos % itemsize; /* number of bytes left and allocated into quantum set */
  s_pos = rest / quantum;         /* index into quantum set */
  q_pos = rest % quantum;         /* index into quantum */

  PDEBUG("read: s_pos=%i, q_pos=%i, f_pos=%li, count=%zi. %s:%i\n",
         s_pos, q_pos, (long) *f_pos, count, __FILE__, __LINE__);

  /* follow the list up to the right position */
  dataptr = my_scull_follow(dev, item);

  if (dataptr == NULL || !dataptr->data || !dataptr->data[s_pos])
    goto out;

  /* read only up to the end of this quantum */
  if (count > quantum - q_pos)
    count = quantum - q_pos;

  if (copy_to_user(buf, dataptr->data[s_pos] + q_pos, count)) {
    retval = -EFAULT;
    goto out;
  }

  *f_pos += count;
  retval = count;

 out:
  up(&dev->sem); /* release the semaphore no matter what has happened */
  return retval;
}

ssize_t my_scull_write(struct file *filp, const char __user *buf, size_t count,
                       loff_t *f_pos)
{
  struct my_scull_dev *dev = filp->private_data;
  struct my_scull_qset *dataptr;                 /* the first list item */
  int quantum = dev->quantum, qset = dev->qset;
  int itemsize = quantum * qset;                 /* how many bytes in the listitem */
  int item, s_pos, q_pos, rest;
  ssize_t retval = -ENOMEM;                      /* value used in "goto out" statements */

  /* wait until we can obtain the semaphore */
  if (down_interruptible(&dev->sem))
    return -ERESTARTSYS;
  
  item = (long)*f_pos / itemsize; /* listitem - index into my_scull_qset list */
  rest = (long)*f_pos % itemsize; /* number of bytes left and allocated into quantum set */
  s_pos = rest / quantum;         /* index into quantum set */
  q_pos = rest % quantum;         /* index into quantum */

  PDEBUG("write: s_pos=%i, q_pos=%i, f_pos=%li, count=%zi. %s:%i\n",
         s_pos, q_pos, (long) *f_pos, count, __FILE__, __LINE__);

  /* follow the list up to the right position */
  dataptr = my_scull_follow(dev, item);
  if (dataptr == NULL)
    goto out;

  /* allocate memory to store the pointers if need be */
  if (!dataptr->data) {
    dataptr->data = kmalloc(qset * sizeof(char *), GFP_KERNEL);
    if (!dataptr->data)
      goto out;
    memset(dataptr->data, 0, qset * sizeof(char *));
  }

  /* allocate memory for the quantum if need be */
  if (!dataptr->data[s_pos]) {
    dataptr->data[s_pos] = kmalloc(quantum, GFP_KERNEL);
    if (!dataptr->data[s_pos])
      goto out;
  }

  /* write only up to the end of this quantum */
  if (count > quantum - q_pos)
    count = quantum - q_pos;

  if (copy_from_user(dataptr->data[s_pos] + q_pos, buf, count)) {
    retval = -EFAULT;
    goto out;
  }

  *f_pos += count;
  retval = count;

  /* update the size */
  if (dev->size < *f_pos)
    dev->size = *f_pos;

 out:
  up(&dev->sem); /* release the semaphore no matter what has happened */
  return retval;
}

int my_scull_ioctl(struct inode *inode, struct file *filp,
                   unsigned int cmd, unsigned long arg)
{
  int err = 0, tmp;
  int retval = 0;

  /*
   * Extract the type and number bitfields; don't decode wrong
   * cmds, return ENOTTY (inappropriate ioctl) before access_ok()
   */
  if (_IOC_TYPE(cmd) != MY_SCULL_IOC_MAGIC) return -ENOTTY;
  if (_IOC_NR(cmd) > MY_SCULL_IOC_MAXNR) return -ENOTTY;

  /*
   * The direction is a bitmask. VERIFY_WRITE catches R/W transfers;
   * if it is safe to write to memory then it is safe to read.
   * 'Type' is user-orientated, while access_ok is kernel orientated.
   * If the user wishjes to read, then the kernel needs to write to userspace.
   * If they wish to write, then the kernel needs to read from userspace
   */
  if (_IOC_DIR(cmd) & _IOC_READ)
    err = !access_ok(VERIFY_WRITE, (void __user *)arg, _IOC_SIZE(cmd));
  else if (_IOC_DIR(cmd) & _IOC_WRITE)
    err = !access_ok(VERIFY_READ,  (void __user *)arg, _IOC_SIZE(cmd));
  if (err) return -EFAULT;

  switch(cmd) {

  case MY_SCULL_IOCRESET:
    my_scull_quantum = MY_SCULL_QUANTUM;
    my_scull_qset = MY_SCULL_QSET;
    break;

  case MY_SCULL_IOCSQUANTUM: /* Set: arg point to the value */
    if (!capable(CAP_SYS_ADMIN))
      return -EPERM;
    retval = __get_user(my_scull_quantum, (int __user *)arg);
    break;

  case MY_SCULL_IOCTQUANTUM: /* Tell: arg is the value */
    if (!capable(CAP_SYS_ADMIN))
      return -EPERM;
    my_scull_quantum = arg;
    break;

  case MY_SCULL_IOCGQUANTUM: /* Get: arg is pointer to the result */
    retval = __put_user(my_scull_quantum, (int __user *)arg);
    break;

  case MY_SCULL_IOCQQUANTUM: /* Query: return it (it's positive) */
    return my_scull_quantum;

  case MY_SCULL_IOCXQUANTUM: /* eXchange: use arg as pointer */
    if (!capable(CAP_SYS_ADMIN))
      return -EPERM;
    tmp = my_scull_quantum;
    retval = __get_user(my_scull_quantum, (int __user *)arg);
    if (retval == 0)
      retval = __put_user(tmp, (int __user *)arg);
    break;

  case MY_SCULL_IOCHQUANTUM: /* sHift: like Tell + Query */
    if (!capable(CAP_SYS_ADMIN))
      return -EPERM;
    tmp = my_scull_quantum;
    my_scull_quantum = arg;
    return tmp;

  case MY_SCULL_IOCSQSET:
    if (!capable(CAP_SYS_ADMIN))
      return -EPERM;
    retval = __get_user(my_scull_qset, (int __user *)arg);
    break;

  case MY_SCULL_IOCTQSET:
    if (!capable(CAP_SYS_ADMIN))
      return -EPERM;
    my_scull_qset = arg;
    break;

  case MY_SCULL_IOCGQSET:
    retval = __put_user(my_scull_qset, (int __user *)arg);
    break;

  case MY_SCULL_IOCQQSET:
    return my_scull_qset;

  case MY_SCULL_IOCXQSET:
    if (!capable(CAP_SYS_ADMIN))
      return -EPERM;
    tmp = my_scull_qset;
    retval = __get_user(my_scull_qset, (int __user *)arg);
    if (retval == 0)
      retval = __put_user(tmp, (int __user *)arg);
    break;

  case MY_SCULL_IOCHQSET:
    if (!capable(CAP_SYS_ADMIN))
      return -EPERM;
    tmp = my_scull_qset;
    my_scull_qset = arg;
    return tmp;

  default: /* redundant as cmd was checked against MAXNR */
    return -ENOTTY;
  }
  return retval;
}

/*
 * Initialize file operations
 */
static const struct file_operations my_scull_fops = {
  .owner   = THIS_MODULE,
  .open    = my_scull_open,
  .release = my_scull_release,
  .read    = my_scull_read,
  .write   = my_scull_write,
  .ioctl   = my_scull_ioctl,
};

/*
 * The cleanup function is used to handle initialization failures as well.
 * Therefore, it must be careful to work correctly even if some the items
 * have not been initialized
 */

static void __exit my_scull_cleanup_module(void)
{
  dev_t devno = MKDEV(my_scull_major, my_scull_minor);
  if (scull_devices)
    kfree(scull_devices);

#ifdef MY_SCULL_DEBUG /* use proc only if debugging */
  my_scull_remove_proc();
#endif

  // Free device numbers since they are no longer in use
  unregister_chrdev_region(devno, my_scull_nr_devs);
  PDEBUG("goodbye!. %s:%i\n", __FILE__, __LINE__);
}

/*
 * Set up the char_dev structure for this device
 */
static void my_scull_setup_cdev(struct my_scull_dev *dev, int index)
{
  int err, devno = MKDEV(my_scull_major, my_scull_minor + index);

  cdev_init(&dev->cdev, &my_scull_fops);
  dev->cdev.owner = THIS_MODULE;
  dev->cdev.ops = &my_scull_fops;
  err = cdev_add(&dev->cdev, devno, 1);
  /* Fail gracefully if need be */
  if (err)
    PDEBUG("error %d adding scull %d. %s:%i\n", err, index, __FILE__, __LINE__);
}

static int __init my_scull_init_module(void)
{
  int   result, i;
  dev_t dev = 0;

  /*
   * Get a range of minor numbers to work with.
   * my_scull_major is set to 0 but can be assigned another value at load
   * time via a parameter. Therefore by default we ask for a dynamic major
   * unless directed otherwise at load time.
   */
  if (my_scull_major) {
    dev = MKDEV(my_scull_major, my_scull_minor);
    result = register_chrdev_region(dev, my_scull_nr_devs, "my_scull");
  } else {
    result = alloc_chrdev_region(&dev, my_scull_minor, my_scull_nr_devs,
                                 "my_scull");
    my_scull_major = MAJOR(dev);
  }
  if (result < 0) {
    PDEBUG("can't get major %d. %s:%i\n", my_scull_major, __FILE__, __LINE__);
    return result;
  }

  /*
   * Allocate the devices - we can't have them static, i.e., as an array, as
   * the number can be specified at load time
   */
  scull_devices = kmalloc(my_scull_nr_devs * sizeof(struct my_scull_dev), GFP_KERNEL);
  if (!scull_devices) {
    result = -ENOMEM;
    goto fail;
  }
  memset(scull_devices, 0, my_scull_nr_devs * sizeof(struct my_scull_dev));

  for (i = 0; i < my_scull_nr_devs; i++) {
    scull_devices[i].quantum = my_scull_quantum;
    scull_devices[i].qset = my_scull_qset;

    /*
     * must be initialized before device is made available to rest of the system
     * to avaoid a race condition where the semaphore could be accessed before it's ready
     */
    init_MUTEX(&scull_devices[i].sem);

    my_scull_setup_cdev(&scull_devices[i], i);
  }

  PDEBUG("hello! %s:%i\n", __FILE__, __LINE__);

#ifdef MY_SCULL_DEBUG /* only when debugging */
  my_scull_create_proc();
#endif

  return 0; /* succeed */

 fail:
  my_scull_cleanup_module();
  return result;
}

module_init(my_scull_init_module);
module_exit(my_scull_cleanup_module);
