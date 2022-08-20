# Comment/uncomment the following line to disable/enable debugging
DEBUG = y

#Add your debugging flag (or not) to CFLAGS
ifeq ($(DEBUG),y)
  DEBFLAGS = -O -g -DMY_SCULL_DEBUG # "-O" is needed to expand inlines
else
  DEBFLAGS = -O2
endif

EXTRA_CFLAGS += $(DEBFLAGS)

# If KERNELRELEASE is defined, we've been invoked from the
# kernel build sysatem and can use its language
ifneq ($(KERNELRELEASE),)
	obj-m := my_scull.o

# Otherwise we were called directly from the command
# line; invoke the kernel build system
else
	KERNELDIR ?= /lib/modules/$(shell uname -r)/build
	PWD := $(shell pwd)

default:
	$(MAKE) -C $(KERNELDIR) M=$(PWD) modules
endif

.PHONY: clean
clean:
	rm -rf *.o *~ .*.cmd *.ko *.mod.c .tmp_versions Module.symvers
