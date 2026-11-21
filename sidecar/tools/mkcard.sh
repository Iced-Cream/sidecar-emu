#!/bin/bash
# mkcard.sh IMG: a 256 MB card image, MBR with one FAT32 (LBA, 0x0c) partition from sector 2048, as a Sidecar card
set -e
IMG=$1; rm -f "$IMG"; truncate -s 256M "$IMG"
echo 'label: dos
2048,,c' | sfdisk -q "$IMG"
SECT=$(( 256*1024*1024/512 - 2048 ))
mkfs.vfat -F 32 -n SIDECAR --offset 2048 "$IMG" $((SECT/2)) >/dev/null
echo "made $IMG"; sfdisk -l "$IMG" | tail -1
