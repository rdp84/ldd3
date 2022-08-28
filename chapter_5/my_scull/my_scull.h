
#ifndef _MY_SCULL_H_
#define _MY_SCULL_H_

/*
 * Macros to help debugging
 */

#undef PDEBUG
#ifdef MY_SCULL_DEBUG
#  ifdef __KERNEL__
     /* This one if debugging is on and kernel space */
#    define PDEBUG(fmt, args...) printk(KERN_DEBUG "my_scull: " fmt, ## args)
#  else
     /* This one for user space */
#    define PDEBUG(fmt, args...) fprintf(stderr, fmt, ## args)
#  endif
#else
#  define PDEBUG(fmt, args...) /* not debugging: nothing */
#endif

#undef PDEBUGG
#define PDEBUGG(fmt, args...) /* nothing: it's a placeholder */


#ifndef MY_SCULL_MAJOR
#define MY_SCULL_MAJOR    0 /* dynamic major by default */
#endif

#ifndef MY_SCULL_NR_DEVS
#define MY_SCULL_NR_DEVS  4 /* number of devices, myscull0 through myscull3*/
#endif

/*
 * The bare device is a variable-length region of memory.
 * Use a linked list of indirect blocks.
 *
 * "my_scull_dev->data" points to an array of pointers, each
 * pointer refers to a memory area of SCULL_QUANTUM bytes.
 *
 * The array (quantum-set) is SCULL_QSET long.
 */

#ifndef MY_SCULL_QUANTUM
#define MY_SCULL_QUANTUM  4000
#endif

#ifndef MY_SCULL_QSET
#define MY_SCULL_QSET     1000
#endif

/*
 * Representation of scull quantum sets
 */
struct my_scull_qset {
  void **data;
  struct my_scull_qset *next;
};

struct my_scull_dev {
  struct my_scull_qset *data; /* Pointer to first quantum set */
  int quantum;                /* the current quantum size */
  int qset;                   /* the current array size */
  unsigned long size;         /* amount of data stored here */
  struct semaphore sem;       /* mutual exclusion semaphore */
  struct cdev cdev;           /* char device structure */
};

/*
 * The different configurable parameters
 * Defined in main.c
 */
extern int my_scull_major;
extern int my_scull_nr_devs;

/*
 * Prototypes for shared functions
 */
int     my_scull_open(struct inode *inode, struct file *filp);
int     my_scull_release(struct inode *inode, struct file *filp);
ssize_t my_scull_read(struct file *filp, char __user *buf, size_t count,
                      loff_t *fpos);
ssize_t my_scull_write(struct file *filp, const char __user *buf, size_t count,
                       loff_t *f_pos);
int     my_scull_trim(struct my_scull_dev *dev);

#endif /* _MY_SCULL_H_ */
