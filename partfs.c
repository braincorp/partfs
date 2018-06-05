#define FUSE_USE_VERSION        26
#include <fuse/fuse.h>

#include <libfdisk/libfdisk.h>

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

#define PARTFS_NAME_PREFIX      "part_"

struct partfs_options
{
    const char * device;
    int help;
};

struct partfs_device
{
    const char * name;
    struct fdisk_context * ctx;
    struct stat st;
};

struct partfs_file
{
    /* file descriptor for disk */
    int desc;

    /*
     * initialized in open call
     * do not modify after
     */
    off_t start, size;
};

static const struct fuse_opt partfs_optspec[] =
{
    { "dev=%s", offsetof(struct partfs_options, device), 1 },
    { "--help", offsetof(struct partfs_options, help), 1 },
    { "-h", offsetof(struct partfs_options, help), 1 },
    FUSE_OPT_END,
};

static ssize_t __partfs_parse_path(const char * const path)
{
    size_t n;
    int r, c;

    r = sscanf(path, "/" PARTFS_NAME_PREFIX "%zu%n", &n, &c);
    return (r == 1 && c == (int)strlen(path)) ? (ssize_t)n : -1;
}

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

static int partfs_open_device(struct partfs_device * const pdev,
                              const char * const device)
{
    int err;

    err = -1;
    pdev->name = realpath(device, NULL);
    pdev->ctx = fdisk_new_context();

    if (pdev->ctx) {
        err = fdisk_assign_device(pdev->ctx, pdev->name, 1);
        if (err) {
            fdisk_unref_context(pdev->ctx);
            pdev->ctx = NULL;
        }
    }

    return err;
}

static void * partfs_init(struct fuse_conn_info * const conn)
{
    return fuse_get_context()->private_data;
}

static void partfs_destroy(void * const priv)
{
    struct partfs_device * const pdev = priv;

    fdisk_deassign_device(pdev->ctx, 0);
    fdisk_unref_context(pdev->ctx);
}

static int partfs_getattr(const char * const path, struct stat * const st)
{
    struct partfs_device * const pdev = fuse_get_context()->private_data;
    int ret;

    if (strcmp(path, "/") == 0) {
        /*
         * will need to increase nlink when
         * extended partitions are supported
         */
        __partfs_stat(st, S_IFDIR | 0755, 2, 0, &pdev->st);
        ret = 0;
    } else {
        ssize_t n;

        ret = -ENOENT;
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

static int partfs_readdir(const char * const path,
                          void * const buf,
                          fuse_fill_dir_t fill,
                          off_t offs,
                          struct fuse_file_info * const fi)
{
    struct partfs_device * const pdev = fuse_get_context()->private_data;

    int ret;

    ret = -EIO;
    if (strcmp(path, "/") == 0) {
        struct fdisk_table * tb;
        struct fdisk_iter * it;
        struct fdisk_partition * pa;

        struct stat st;

        __partfs_stat(&st, S_IFDIR | 0755, 2, 0, &pdev->st);
        fill(buf, ".",  &st, 0);

        fill(buf, "..", NULL, 0);

        tb = NULL;
        fdisk_get_partitions(pdev->ctx, &tb);

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

static int partfs_open(const char * const path,
                       struct fuse_file_info * const fi)
{
    struct partfs_device * const pdev = fuse_get_context()->private_data;
    struct partfs_file * const pfi = malloc(sizeof(*pfi));
    int err;

    err = -ENOMEM;
    if (pfi) {
        pfi->desc = open(pdev->name, fi->flags);
        if (pfi->desc < 0) {
            err = -errno;
        } else {
            const ssize_t n = __partfs_parse_path(path);

            struct fdisk_partition * pa;

            pa = NULL;
            fdisk_get_partition(pdev->ctx, n, &pa);

            pfi->start = fdisk_get_sector_size(pdev->ctx) *
                fdisk_partition_get_start(pa);
            pfi->size  = __fdisk_partition_get_size(pdev->ctx, pa);

            fdisk_unref_partition(pa);

            fi->fh = (uintptr_t)pfi;
            err = 0;
        }
    }

    return err;
}

static int partfs_lseek(const char * const path,
                        const off_t off,
                        struct fuse_file_info * const fi)
{
    struct partfs_file * const pfi = (void *)fi->fh;
    int ret;

    ret = -EINVAL;
    if (off >= 0 && off <= pfi->size) {
        ret = (lseek(pfi->desc, pfi->start + off, SEEK_SET) >= 0) ? 0 : -errno;
    }

    return ret;
}

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
    struct partfs_device pdev;
    int err;

    opts.device = NULL;
    opts.help   = 0;

    pdev.ctx     = NULL;

    err = fuse_opt_parse(&args, &opts, partfs_optspec, NULL);
    if (!err) {
        if (opts.device) {
            err = stat(opts.device, &pdev.st);
            if (err == 0) {
                err = partfs_open_device(&pdev, opts.device);
            }
            if (err) {
                fprintf(stderr,
                        "unable to read partitions on %s\n",
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
            free((void *)pdev.name);

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
