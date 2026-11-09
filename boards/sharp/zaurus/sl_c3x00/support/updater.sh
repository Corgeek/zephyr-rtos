#!/bin/sh
#
# Copyright (c) 2026 Takayuki Goto
#
# SPDX-License-Identifier: Apache-2.0
#
# Write zephyr.bin to the kernel area of the NAND flash of a Sharp Zaurus
# SL-C3000, SL-C3100 or SL-C3200, where the boot loader looks for the Linux
# kernel it starts.
#
# The Sharp ROM updater (hold OK and press On/Off, then the update item and
# the card) runs this from the root of a FAT card that also holds
# zephyr.bin, or the same file named zImage, and passes the card's mount
# point as $1. It only runs the script in its own encoding: the build puts
# the encoded script next to zephyr.bin, as build/zephyr/updater.sh, and
#
#   zaurus_updater_codec.py updater.sh <encoded updater.sh>
#
# produces it by hand.
#
# Only the kernel area is written. The boot loader, the maintenance menu and
# the root and home file systems stay as they are. The Linux kernel comes
# back with the service menu's NAND restore.
#
# Messages are kept within the 30 columns of the updater's screen.

KERNEL_ADDR=0xe0000
# 0x13c000, the size of the kernel area
KERNEL_SIZE_MAX=1294336
# The Linux zImage magic number is 0x016f2818 at offset 0x24. Only its two
# middle bytes, "(o", are compared, as text. Comparing all four bytes, with
# the control characters 0x18 and 0x01 in shell strings, rejected a good
# image on an SL-C3200.
ZIMAGE_MAGIC_OFFSET=37
ZIMAGE_MAGIC='(o'

TMP_DIR=/tmp/zephyr-update

card=$1

fail()
{
    echo "Error: $1"
    echo 'Nothing was written.'
    exit 1
}

echo '--- Zephyr NAND updater ---'

/sbin/writerominfo
model=`cat /proc/deviceinfo/product`
case "$model" in
    SL-C3000|SL-C3100|SL-C3200)
        echo "Model: $model"
        ;;
    *)
        fail "unsupported $model"
        ;;
esac

image=""
for name in zephyr.bin ZEPHYR.BIN zImage zimage ZIMAGE
do
    if [ -f "$card/$name" ]
    then
        image="$card/$name"
        break
    fi
done
if [ "$image" = "" ]
then
    fail 'no zephyr.bin or zImage'
fi

size=`wc -c < "$image"`
size=`echo $size`
if [ "$size" -gt "$KERNEL_SIZE_MAX" ]
then
    fail "image too big: $size"
fi

mkdir -p "$TMP_DIR"
/sbin/bcut -a "$ZIMAGE_MAGIC_OFFSET" -s 2 -o "$TMP_DIR/magic" "$image"
if [ "`cat "$TMP_DIR/magic"`" != "$ZIMAGE_MAGIC" ]
then
    rm -rf "$TMP_DIR"
    fail "$name not a zImage"
fi

mtd_line=`grep smf /proc/mtd | tail -n 1`
if [ "$mtd_line" = "" ]
then
    rm -rf "$TMP_DIR"
    fail 'no smf partition'
fi
mtd=/dev/`echo $mtd_line | cut -d: -f1`

echo "Writing $size bytes..."
/sbin/nandlogical "$mtd" WRITE "$KERNEL_ADDR" "$size" "$image" > /dev/null
if [ $? -ne 0 ]
then
    rm -rf "$TMP_DIR"
    echo 'Error: NAND write failed.'
    echo 'Restore NAND from the'
    echo 'service menu.'
    exit 1
fi

# Read the area back where the updater has cmp to compare with.
if type cmp > /dev/null 2>&1
then
    /sbin/nandlogical "$mtd" READ "$KERNEL_ADDR" "$size" "$TMP_DIR/check" > /dev/null
    cmp -s "$image" "$TMP_DIR/check"
    if [ $? -ne 0 ]
    then
        rm -rf "$TMP_DIR"
        echo 'Error: verify failed.'
        echo 'Restore NAND from the'
        echo 'service menu.'
        exit 1
    fi
    echo 'Verified.'
fi

rm -rf "$TMP_DIR"
echo 'Done. Reset to boot Zephyr.'
exit 0
