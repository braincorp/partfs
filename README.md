# partfs
FUSE-based file system for accessing partitions on a disk

## Building
Run `make` at the top level.

Your system will need to have:
```
cmake
libfdisk (libraries and development headers)
libfuse (libraries and development headers)
```
On a Debian/Ubuntu system:
```
apt-get install cmake libfdisk1 libfdisk-dev libfuse2 libfuse-dev
```

## About
partfs allows one to access partitions within a device or file.
the main purpose of partfs is to allow the creation of disk
images without superuser privileges. this can be useful for the
enabling automatic partition discovery for containers or for
building disk images for embedded software.

the mounted directory presents the partitions as files, allowing
mkfs.* to be used to create file systems on the partitions

to create a disk image with a single bootable
partition with an ext4 file system:

```
$ dd if=/dev/zero of=disk.image bs=1M count=4
4+0 records in
4+0 records out
4194304 bytes (4.2 MB, 4.0 MiB) copied, 0.00470867 s, 891 MB/s
$ parted --script disk.image \
    mktable msdos mkpart primary 2048s 100% set 1 boot on
$ mkdir mntdir
$ partfs -o dev=disk.image mntdir
$ mkfs.ext4 mntdir/part_0
mke2fs 1.42.13 (17-May-2015)
Creating filesystem with 3072 1k blocks and 768 inodes

Allocating group tables: done
Writing inode tables: done
Creating journal (1024 blocks): done
Writing superblocks and filesystem accounting information: done

$ fusermount -u mntdir
```

to verify that the partition and file
system were successfully created:

```
$ sudo kpartx -a -v disk.image
add map loop0p1 (253:0): 0 6144 linear 7:0 2048
$ sudo fsck.ext4 /dev/mapper/loop0p1
e2fsck 1.42.13 (17-May-2015)
/dev/mapper/loop0p1: clean, 11/768 files, 1150/3072 blocks
$ sudo kpartx -d disk.image
loop deleted : /dev/loop0
```
