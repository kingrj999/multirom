#Livebump for LG G3 kernel
#by skin1980@xda

ROOT=$1
WORKING_DIR=$2
BUSYBOX=$ROOT/busybox
OUT_DIR=$WORKING_DIR/out

IMAGE=$WORKING_DIR/original-boot.img

if [ -z $IMAGE ]; then exit 1; fi
$BUSYBOX mkdir $OUT_DIR
$ROOT/unpackbootimg -i $IMAGE -o $OUT_DIR

if [ -e $OUT_DIR/original-boot.img-ramdisk.gz ]; then
	rdcomp=$OUT_DIR/original-boot.img-ramdisk.gz
elif [ -e $OUT_DIR/original-boot.img-ramdisk.lz4 ]; then
	rdcomp=$OUT_DIR/original-boot.img-ramdisk.lz4
else
	exit 1
fi

$ROOT/mkbootimg --kernel $OUT_DIR/original-boot.img-zImage --ramdisk $rdcomp --dt $OUT_DIR/original-boot.img-dt --cmdline "$($BUSYBOX cat $OUT_DIR/original-boot.img-cmdline)" --pagesize $($BUSYBOX cat $OUT_DIR/original-boot.img-pagesize) --base $($BUSYBOX cat $OUT_DIR/original-boot.img-base) --ramdisk_offset $($BUSYBOX cat $OUT_DIR/original-boot.img-ramdisk_offset) --tags_offset $($BUSYBOX cat $OUT_DIR/original-boot.img-tags_offset) --output $WORKING_DIR/image_bumped.img

echo "Bumping the boot.img..."
printf '\x41\xA9\xE4\x67\x74\x4D\x1D\x1B\xA4\x29\xF2\xEC\xEA\x65\x52\x79' >> $WORKING_DIR/image_bumped.img

exit 0
