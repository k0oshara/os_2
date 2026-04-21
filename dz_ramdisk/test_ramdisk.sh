#!/usr/bin/env bash
set -euo pipefail

MODULE_NAME="${MODULE_NAME:-ramdisk}"
MODULE_KO="${MODULE_KO:-./ramdisk.ko}"
DEV="${DEV:-/dev/myblock0}"
MNT="${MNT:-/mnt/myblock_test}"
SIZE_MB="${SIZE_MB:-1024}"
RUN_BADBLOCKS="${RUN_BADBLOCKS:-0}"

need() {
    command -v "$1" >/dev/null 2>&1 || {
        echo "missing command: $1" >&2
        exit 1
    }
}

cleanup() {
    set +e
    sync
    mountpoint -q "$MNT" && umount "$MNT"
    rmmod "$MODULE_NAME" 2>/dev/null
}
trap cleanup EXIT

need insmod
need rmmod
need grep
need awk
need mknod
need mount
need umount
need sync
need dd
need sha256sum
need mkfs.ext4
need mkfs.btrfs

if [[ $EUID -ne 0 ]]; then
    echo "run as root: sudo $0" >&2
    exit 1
fi

mkdir -p "$MNT"

echo "[1/6] loading module"
rmmod "$MODULE_NAME" 2>/dev/null || true
insmod "$MODULE_KO" disk_size_mb="$SIZE_MB"

if [[ ! -b "$DEV" ]]; then
    major="$(awk '$2=="mybdev"{print $1}' /proc/devices)"
    if [[ -z "${major:-}" ]]; then
        echo "cannot find major for mybdev in /proc/devices" >&2
        exit 1
    fi
    mknod "$DEV" b "$major" 0
fi

echo "[2/6] ext4 test"
mkfs.ext4 -F "$DEV"
mount "$DEV" "$MNT"

echo "hello ext4" > "$MNT/hello.txt"
dd if=/dev/urandom of="$MNT/blob.bin" bs=4K count=256 status=none
sha_before_ext4="$(sha256sum "$MNT/blob.bin" | awk '{print $1}')"

sync
umount "$MNT"
mount "$DEV" "$MNT"

sha_after_ext4="$(sha256sum "$MNT/blob.bin" | awk '{print $1}')"
[[ "$sha_before_ext4" == "$sha_after_ext4" ]]
grep -q "hello ext4" "$MNT/hello.txt"
umount "$MNT"

echo "[3/6] btrfs test"
mkfs.btrfs -f "$DEV"
mount "$DEV" "$MNT"

echo "hello btrfs" > "$MNT/hello.txt"
dd if=/dev/urandom of="$MNT/blob.bin" bs=4K count=256 status=none
sha_before_btrfs="$(sha256sum "$MNT/blob.bin" | awk '{print $1}')"

sync
umount "$MNT"
mount "$DEV" "$MNT"

sha_after_btrfs="$(sha256sum "$MNT/blob.bin" | awk '{print $1}')"
[[ "$sha_before_btrfs" == "$sha_after_btrfs" ]]
grep -q "hello btrfs" "$MNT/hello.txt"
umount "$MNT"

echo "[4/6] raw dd test"
tmp_in="$(mktemp /tmp/ramdisk_in.XXXXXX)"
tmp_out="$(mktemp /tmp/ramdisk_out.XXXXXX)"
dd if=/dev/urandom of="$tmp_in" bs=4K count=1024 status=none
dd if="$tmp_in" of="$DEV" bs=4K oflag=direct,sync status=progress
dd if="$DEV" of="$tmp_out" bs=4K count=1024 iflag=direct status=progress

sha_in="$(sha256sum "$tmp_in" | awk '{print $1}')"
sha_out="$(sha256sum "$tmp_out" | awk '{print $1}')"
[[ "$sha_in" == "$sha_out" ]]

rm -f "$tmp_in" "$tmp_out"

echo "[5/6] optional badblocks"
if [[ "$RUN_BADBLOCKS" == "1" ]]; then
    need badblocks
    badblocks -wsv "$DEV"
else
    echo "skipped badblocks (set RUN_BADBLOCKS=1 to enable)"
fi

echo "[6/6] success"
echo "all basic tests passed"
