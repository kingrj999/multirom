#!/sbin/sh
BOOT_DEV="$(cat /tmp/bootdev)"

if [ ! -e "$BOOT_DEV" ]; then
    echo "BOOT_DEV \"$BOOT_DEV\" does not exist!"
    return 1
fi

chmod 755 /tmp/multirom/trampoline
chmod 755 /tmp/multirom/busybox
chmod 755 /tmp/multirom/lz4
/tmp/multirom/trampoline --inject="$BOOT_DEV" --mrom_dir="/tmp/multirom" -f

# Bump the boot image
mkdir /tmp/bump

dd if=$BOOT_DEV of=/tmp/bump/original-boot.img

/sbin/livebump.sh

dd if=/dev/zero of=$BOOT_DEV bs=4096 count=4096
dd if=/tmp/bump/image_bumped.img of=$BOOT_DEV

rm -rf /tmp/bump

return $?
