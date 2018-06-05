/*
 * fuse part(ition)fs
 *
 * partfs allows one to access partitions within a device or file.
 * the main purpose of partfs is to allow the creation of disk
 * images without superuser privileges. this can be useful for the
 * enabling automatic partition discovery for containers or for
 * building disk images for embedded software.
 *
 * the mounted directory presents the partitions as files, allowing
 * mkfs.* to be used to create file systems on the partitions
 *
 * to create a disk image with a single bootable
 * partition with an ext4 file system:
 *
 * $ dd if=/dev/zero of=disk.image bs=1M count=4
 * 4+0 records in
 * 4+0 records out
 * 4194304 bytes (4.2 MB, 4.0 MiB) copied, 0.00470867 s, 891 MB/s
 * $ parted --script disk.image \
 *     mktable msdos mkpart primary 2048s 100% set 1 boot on
 * $ mkdir mntdir
 * $ partfs -o dev=disk.image mntdir
 * $ mkfs.ext4 mntdir/part_0
 * mke2fs 1.42.13 (17-May-2015)
 * Creating filesystem with 3072 1k blocks and 768 inodes
 *
 * Allocating group tables: done
 * Writing inode tables: done
 * Creating journal (1024 blocks): done
 * Writing superblocks and filesystem accounting information: done
 *
 * $ fusermount -u mntdir
 *
 * to verify that the partition and file
 * system were successfully created:
 *
 * $ sudo kpartx -a -v disk.image
 * add map loop0p1 (253:0): 0 6144 linear 7:0 2048
 * $ sudo fsck.ext4 /dev/mapper/loop0p1
 * e2fsck 1.42.13 (17-May-2015)
 * /dev/mapper/loop0p1: clean, 11/768 files, 1150/3072 blocks
 * $ sudo kpartx -d disk.image
 * loop deleted : /dev/loop0
 */

#define FUSE_USE_VERSION        26
#include <fuse/fuse.h>

#include <libfdisk/libfdisk.h>

/*
 * the standard version of this function in libfdisk returns
 * the size in sectors. this returns the size in bytes.
 */
static off_t __fdisk_partition_get_size(
    struct fdisk_context * const ctx,
    struct fdisk_partition * const pa)
{
    return fdisk_partition_has_size(pa) ?
        (fdisk_partition_get_size(pa) * fdisk_get_sector_size(ctx)) : 0;
}

#include <unistd.h>
#include <stdlib.h>
#include <stddef.h>
#include <string.h>
#include <stdio.h>
#include <errno.h>

#include <sys/param.h>

/*
 * file representations of partitions are named as "part_X"
 * where X is the integer id of the partition (generally 0-4)
 */
#define PARTFS_NAME_PREFIX      "part_"

/*
 * options retrieved from the command line
 */
struct partfs_options
{
    /* the device file name as supplied on the command line */
    const char * device;

    /* whether or not help should be displayed */
    int help;
};

/*
 * data structure associated with the mounted "device"
 */
struct partfs_device
{
    /* absolute path to the device file */
    const char * name;
    /* partition context associated with the device file */
    struct fdisk_context * ctx;
    /* stat information about the device file */
    struct stat st;
};

/*
 * data structure associated with each open of a partition
 */
struct partfs_file
{
    /* file descriptor for device file */
    int desc;

    /*
     * starting offset and size of the partition (in bytes)
     * within the device file. these are initialized in open call
     * and should not be modified after.
     */
    off_t start, size;
};

/* supported command line options */
static const struct fuse_opt partfs_optspec[] =
{
    /* dev is the name/path of the device file */
    { "dev=%s", offsetof(struct partfs_options, device), 1 },

    /* display help */
    { "--help", offsetof(struct partfs_options, help), 1 },
    { "-h", offsetof(struct partfs_options, help), 1 },

    FUSE_OPT_END,
};

/*
 * extract the partition number from the path name of a partition
 *
 * returns -1 on error
 */
static ssize_t __partfs_parse_path(const char * const path)
{
    size_t n;
    int r, c;

    r = sscanf(path, "/" PARTFS_NAME_PREFIX "%zu%n", &n, &c);
    return (r == 1 && c == (int)strlen(path)) ? (ssize_t)n : -1;
}

/*
 * populate a stat buffer
 *
 * ownership and time stamps are populated from the template
 */
static void __partfs_stat(struct stat * const st,
                          const mode_t mode,
                          const nlink_t nlink,
                          const off_t size,
                          const struct stat * const tmpl)
{
    memset(st, 0, sizeof(*st));

    st->st_mode     = mode;
    st->st_nlink    = nlink;

    st->st_uid      = tmpl->st_uid;
    st->st_gid      = tmpl->st_gid;

    st->st_size     = size;

    st->st_atime    = tmpl->st_atime;
    st->st_mtime    = tmpl->st_mtime;
    st->st_ctime    = tmpl->st_ctime;
}

/*
 * initial open of the device file and parsing of the partitions
 *
 * pdev must point to an existing structure. existing members will
 * be overwritten without being evaluated. device is the name
 * of the device file to be opened
 */
static int partfs_open_device(struct partfs_device * const pdev,
                              const char * const device)
{
    int err;

    pdev->name = NULL;
    pdev->ctx  = NULL;

    /*
     * need the absolute path since fuse may not stay
     * in the same directory in which it was started
     */
    pdev->name = realpath(device, NULL);
    err = pdev->name ? 0 : -errno;

    if (!err) {
        err = stat(pdev->name, &pdev->st) ? -errno : 0;

        if (!err) {
            pdev->ctx = fdisk_new_context();
            err = pdev->ctx ? 0 : -ENOMEM;

            if (!err) {
                /* parse the partition table */
                err = fdisk_assign_device(pdev->ctx, pdev->name, 1);
                if (err) {
                    fdisk_unref_context(pdev->ctx);
                    pdev->ctx = NULL;
                }
            }
        }

        if (err) {
            free((void *)pdev->name);
            pdev->name = NULL;
        }
    }

    return err;
}

/*
 * called just before the main fuse loop starts
 *
 * private_data is the partfs_device
 */
static void * partfs_init(struct fuse_conn_info * const conn)
{
    return fuse_get_context()->private_data;
}

/* called just before fuse exits */
static void partfs_destroy(void * const priv)
{
    struct partfs_device * const pdev = priv;

    fdisk_deassign_device(pdev->ctx, 0);
    fdisk_unref_context(pdev->ctx);
    free((void *)pdev->name);
}

/*
 * get attributes--stat(2), essentially--for a file or directory
 */
static int partfs_getattr(const char * const path, struct stat * const st)
{
    struct partfs_device * const pdev = fuse_get_context()->private_data;
    int ret;

    if (strcmp(path, "/") == 0) {
        __partfs_stat(st, S_IFDIR | 0755, 2, 0, &pdev->st);
        ret = 0;
    } else {
        ssize_t n;

        ret = -ENOENT;

        /*
         * extract the partition number from the path name
         * and gather statistics for it
         */
        n = __partfs_parse_path(path);
        if (n >= 0) {
            struct fdisk_partition * pa;

            pa = NULL;
            fdisk_get_partition(pdev->ctx, n, &pa);

            __partfs_stat(st, pdev->st.st_mode, 1,
                          __fdisk_partition_get_size(pdev->ctx, pa),
                          &pdev->st);

            fdisk_unref_partition(pa);
            ret = 0;
        }
    }

    return ret;
}

/*
 * list directory contents
 */
static int partfs_readdir(const char * const path,
                          void * const buf,
                          fuse_fill_dir_t fill,
                          off_t offs,
                          struct fuse_file_info * const fi)
{
    struct partfs_device * const pdev = fuse_get_context()->private_data;

    int ret;

    ret = -ENOENT;
    if (strcmp(path, "/") == 0) {
        struct fdisk_table * tb;
        struct fdisk_iter * it;
        struct fdisk_partition * pa;

        struct stat st;

        /*
         * use ownership and time stamps from the device file
         * for the top level directory
         */
        __partfs_stat(&st, S_IFDIR | 0755, 2, 0, &pdev->st);
        fill(buf, ".",  &st, 0);

        /* fuse will fill in information for the parent directoy */
        fill(buf, "..", NULL, 0);

        /* load the entire partition table */
        tb = NULL;
        fdisk_get_partitions(pdev->ctx, &tb);

        /* return directory entries and information for each partition */
        it = fdisk_new_iter(FDISK_ITER_FORWARD);
        while (fdisk_table_next_partition(tb, it, &pa) == 0) {
            char num[16];

            snprintf(num, sizeof(num),
                     PARTFS_NAME_PREFIX "%zu",
                     fdisk_partition_get_partno(pa));

            __partfs_stat(&st, pdev->st.st_mode, 1,
                          __fdisk_partition_get_size(pdev->ctx, pa),
                          &pdev->st);

            fill(buf, num, &st, 0);
        }

        fdisk_free_iter(it);
        fdisk_unref_table(tb);

        ret = 0;
    }

    return ret;
}

/*
 * open a file (partition) within the device
 */
static int partfs_open(const char * const path,
                       struct fuse_file_info * const fi)
{
    struct partfs_device * const pdev = fuse_get_context()->private_data;
    struct partfs_file * const pfi = malloc(sizeof(*pfi));
    int err;

    err = -ENOMEM;
    if (pfi) {
        /* open the existing disk/device file */
        pfi->desc = open(pdev->name, fi->flags);

        if (pfi->desc < 0) {
            err = -errno;
        } else {
            const ssize_t n = __partfs_parse_path(path);

            struct fdisk_partition * pa;

            pa = NULL;
            fdisk_get_partition(pdev->ctx, n, &pa);

            /*
             * get/save the starting offset
             * and size of the partition
             */
            pfi->start = fdisk_get_sector_size(pdev->ctx) *
                fdisk_partition_get_start(pa);
            pfi->size  = __fdisk_partition_get_size(pdev->ctx, pa);

            fdisk_unref_partition(pa);

            /* save the file structure */
            fi->fh = (uintptr_t)pfi;
            err = 0;
        }
    }

    return err;
}

/*
 * this function is not actually part of the fuse api but
 * has an signature similar to what it would probably look
 * like if it was.
 *
 * path is the path name to the file
 * off is the offset to which to seek within the open
 *   file/partition; whence is assumed to be SEEK_SET
 * fi is the file information associated with the open() call
 *
 * returns 0 on success or a negative errno on failure
 */
static int partfs_lseek(const char * const path,
                        const off_t off,
                        struct fuse_file_info * const fi)
{
    struct partfs_file * const pfi = (void *)fi->fh;
    int ret;

    ret = -EINVAL;
    if (off >= 0 && off <= pfi->size) {
        /*
         * off refers to an offset within a partition; however,
         * the file system actually opens the device file itself
         * underneath the covers, so the lseek actually seeks to
         * the appropriate location within the device file
         */
        ret = (lseek(pfi->desc, pfi->start + off, SEEK_SET) >= 0) ? 0 : -errno;
    }

    return ret;
}

/*
 * read data from a partition
 */
static int partfs_read(const char * const path,
                       char * const buf, size_t len,
                       off_t off,
                       struct fuse_file_info * const fi)
{
    struct partfs_file * const pfi = (void *)fi->fh;

    int ret;

    ret = partfs_lseek(path, off, fi);
    if (ret == 0) {
        ret = read(pfi->desc, buf, MIN(pfi->size - off, len));
        if (ret < 0) {
            ret = -errno;
        }
    }

    return ret;
}

/*
 * write data to a partition
 */
static int partfs_write(const char * const path,
                        const char * const buf, const size_t len,
                        const off_t off,
                        struct fuse_file_info * const fi)
{
    struct partfs_file * const pfi = (void *)fi->fh;

    int ret;

    ret = partfs_lseek(path, off, fi);
    if (off >= 0) {
        if (ret != 0) {
            /*
             * if offset was positive, but the lseek failed, then
             * the offset is beyond the end of the partition
             */
            ret = -EFBIG;
        } else {
            ret = write(pfi->desc, buf, MIN(pfi->size - off, len));
            if (ret < 0) {
                ret = -errno;
            }
        }
    }

    return ret;
}

/*
 * release is called when a partition is closed
 */
static int partfs_release(const char * const path,
                          struct fuse_file_info * const fi)
{
    struct partfs_file * const pfi = (void *)fi->fh;
    const int desc = pfi->desc;

    free(pfi);

    /*
     * fuse ignores this error, but return it anyway
     */
    return close(desc) ? -errno : 0;
}

/* supported operations for the part(ition)fs */
static struct fuse_operations partfs_ops =
{
    .getattr        = partfs_getattr,
    .readdir        = partfs_readdir,

    .open           = partfs_open,
    .read           = partfs_read,
    .write          = partfs_write,
    .release        = partfs_release,

    .init           = partfs_init,
    .destroy        = partfs_destroy,
};

int main(const int argc, char * argv[])
{
    struct fuse_args args = FUSE_ARGS_INIT(argc, argv);
    struct partfs_options opts;
    int err;

    opts.device = NULL;
    opts.help   = 0;

    err = fuse_opt_parse(&args, &opts, partfs_optspec, NULL);
    if (!err) {
        struct partfs_device pdev;

        if (opts.device) {
            err = partfs_open_device(&pdev, opts.device);
            if (err) {
                fprintf(stderr,
                        "%s: unable to read partitions\n",
                        opts.device);
            }
        } else {
            opts.help = 1;
        }

        if (!err) {
            if (opts.help) {
                fuse_opt_add_arg(&args, "--help");
            }

            err = fuse_main(args.argc, args.argv, &partfs_ops, &pdev);

            if (opts.help) {
                fprintf(stderr, "\n");
                fprintf(stderr, "File system-specific options:\n");
                fprintf(stderr, "\n");
                fprintf(stderr, "    -o dev=FILE\n");
            }
        }
    }

    return err;
}

/*
 * local variables:
 * compile-command: "gcc -Wall -Wextra -Wno-unused-parameter -g \
 *      -D_FILE_OFFSET_BITS=64 -o partfs partfs.c -lfdisk -lfuse"
 * end:
 */
