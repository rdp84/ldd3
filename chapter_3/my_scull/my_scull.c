#include <linux/init.h>
#include <linux/module.h>
#include <linux/moduleparam.h>

#include <linux/kernel.h> /* printk, container_of */
#include <linux/slab.h>   /* kmalloc */
#include <linux/fs.h>     /* everything...*/
#include <linux/types.h>  /* size_t, dev_t, MAJOR, MINOR, MKDEV */
#include <linux/string.h> /* memset */
#include <linux/fcntl.h>  /* O_ACCMODE */
#include <linux/cdev.h>   /* cdev */

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
  if ((filp->f_flags & O_ACCMODE) == O_WRONLY)
    my_scull_trim(dev); /* ignore errors */

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

  if (*f_pos >= dev->size)
    goto out;
  if (*f_pos + count > dev->size)
    count = dev->size - *f_pos;

  item = (long)*f_pos / itemsize; /* listitem - index into my_scull_qset list */
  rest = (long)*f_pos % itemsize; /* number of bytes left and allocated into quantum set */
  s_pos = rest / quantum;         /* index into quantum set */
  q_pos = rest % quantum;         /* index into quantum */

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

  item = (long)*f_pos / itemsize; /* listitem - index into my_scull_qset list */
  rest = (long)*f_pos % itemsize; /* number of bytes left and allocated into quantum set */
  s_pos = rest / quantum;         /* index into quantum set */
  q_pos = rest % quantum;         /* index into quantum */

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

  // Free device numbers since they are no longer in use
  unregister_chrdev_region(devno, my_scull_nr_devs);
  printk(KERN_INFO "Goodbye cruel world\n");
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
    printk(KERN_NOTICE "Error %d adding scull %d", err, index);
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
    printk(KERN_WARNING "my_scull: can't get major %d\n", my_scull_major);
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
    my_scull_setup_cdev(&scull_devices[i], i);
  }

  return 0; /* succeed */

 fail:
  my_scull_cleanup_module();
  return result;
}

module_init(my_scull_init_module);
module_exit(my_scull_cleanup_module);
