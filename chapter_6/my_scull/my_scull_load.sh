#!/bin/sh
module="my_scull"
device="my_scull"
mode="664"

# give appropriate group/permissions, and change the group.
# Not all distributions have staff, some have "wheel" instead.
group="staff"
grep -q '^staff:' /etc/group || group="wheel"

# invoke insmod with all arguments we got
# and use a pathname, as newer modutils don't look in . by default
/sbin/insmod ./$module.ko $* || exit 1

# retrieve major number
major=$(awk "\$2==\"$module\" {print \$1}" /proc/devices)

# Remove stale nodes and replace them, then give gid and perms
# Usually the script is shorter, it's scull that  has several devices in it

rm -f /dev/${device}[0-3]
mknod /dev/${device}0 c $major 0
mknod /dev/${device}1 c $major 1
mknod /dev/${device}2 c $major 2
mknod /dev/${device}3 c $major 3
ln -sf ${device}0 /dev/${device}
chgrp $group /dev/${device}[0-3]
chmod $mode  /dev/${device}[0-3]

rm -f /dev/${device}_pipe[0-3]
mknod /dev/${device}_pipe0 c $major 4
mknod /dev/${device}_pipe1 c $major 5
mknod /dev/${device}_pipe2 c $major 6
mknod /dev/${device}_pipe3 c $major 7
ln -sf ${device}_pipe0 /dev/${device}pipe
chgrp $group /dev/${device}_pipe[0-3]
chmod $mode  /dev/${device}_pipe[0-3]
