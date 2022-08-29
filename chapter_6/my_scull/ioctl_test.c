#include <stdio.h>
#include <stdlib.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/ioctl.h>

#define MY_SCULL_IOCSQUANTUM 0x40046F01
#define MY_SCULL_IOCSQSET    0x40046F02
#define MY_SCULL_IOCTQUANTUM 0x6F03
#define MY_SCULL_IOCTQSET    0x6F04
#define MY_SCULL_IOCGQUANTUM 0x80046F05
#define MY_SCULL_IOCGQSET    0x80046F06
#define MY_SCULL_IOCQQUANTUM 0x6F07
#define MY_SCULL_IOCQQSET    0x6F08
#define MY_SCULL_IOCXQUANTUM 0xC0046F09
#define MY_SCULL_IOCXQSET    0xC0046F0A
#define MY_SCULL_IOCHQUANTUM 0x6F0B
#define MY_SCULL_IOCHQSET    0x6F0C

int
main(void)
{
  int fd, quantum, qset, retval;

  if ((fd = open("/dev/my_scull0", O_RDONLY)) > 0) {

    retval = ioctl(fd, MY_SCULL_IOCGQUANTUM, &quantum);    /* Get quantum by pointer */
    printf("Get quantum by pointer:      %i\n", quantum);

    quantum = ioctl(fd, MY_SCULL_IOCQQUANTUM);             /* Get quantum by return value */
    printf("Get quantum by return value: %i\n", quantum);

    retval = ioctl(fd, MY_SCULL_IOCGQSET, &qset);          /* Get qset by pointer */
    printf("Get qset by pointer:      %i\n", qset);

    qset = ioctl(fd, MY_SCULL_IOCQQSET);                   /* Get qset by return value */
    printf("Get qset by return value: %i\n", qset);


    quantum = 2000;
    retval = ioctl(fd, MY_SCULL_IOCSQUANTUM, &quantum);    /* Set quantum by pointer */
    printf("Set quantum by pointer retval: %i\n", retval);
    quantum = ioctl(fd, MY_SCULL_IOCQQUANTUM);             /* Get quantum by return value */
    printf("Get quantum by return value:   %i\n", quantum);

    quantum = 4000;
    retval = ioctl(fd, MY_SCULL_IOCTQUANTUM, quantum);     /* Set quantum by value */
    printf("Set quantum by value retval: %i\n", retval);
    retval = ioctl(fd, MY_SCULL_IOCGQUANTUM, &quantum);    /* Get quantum by pointer */
    printf("Get quantum by pointer:      %i\n", quantum);

    qset = 2000;
    retval = ioctl(fd, MY_SCULL_IOCSQSET, &qset);          /* Set qset by pointer */
    printf("Set qset by pointer retval: %i\n", retval);
    qset = ioctl(fd, MY_SCULL_IOCQQSET);                   /* Get qset by return value */
    printf("Get qset by return value:   %i\n", qset);

    qset = 1000;
    retval = ioctl(fd, MY_SCULL_IOCTQSET, qset);           /* Set qset by value */
    printf("Set qset by value retval: %i\n", retval);
    retval = ioctl(fd, MY_SCULL_IOCGQSET, &qset);          /* Get qset by pointer */
    printf("Get qset by pointer:      %i\n", qset);

    quantum = 2000;
    retval = ioctl(fd, MY_SCULL_IOCXQUANTUM, &quantum);    /* Exhange quantum by pointer */
    printf("Exchange quantum, retval: %i, quantum: %i\n", retval, quantum);

    quantum = 4000;
    quantum = ioctl(fd, MY_SCULL_IOCHQUANTUM, quantum);    /* Shift quantum by value */
    printf("Shift quantum, quantum: %i\n", quantum);

    qset = 2000;
    retval = ioctl(fd, MY_SCULL_IOCXQSET, &qset);          /* Exhange qset by pointer */
    printf("Exchange qset, retval: %i, qset: %i\n", retval, qset);

    qset = 1000;
    qset = ioctl(fd, MY_SCULL_IOCHQSET, qset);             /* Shift qset by value */
    printf("Shift qset, qset: %i\n", qset);
    
    retval = close(fd);
    printf("retval from close: %i\n", retval);
  }
  else
    printf("Couldn't open the device file!: %i\n", fd);

  return EXIT_SUCCESS;
}
