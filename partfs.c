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

#include <stddef.h>
#include <string.h>
#include <stdio.h>
#include <errno.h>

#define PARTFS_NAME_PREFIX      "part_"

struct partfs_options
{
    const char * device;
    int help;
};

struct partfs_device
{
    struct fdisk_context * ctx;
    struct stat st;
};

struct partfs_file
{
};

static const struct fuse_opt partfs_optspec[] =
{
    { "dev=%s", offsetof(struct partfs_options, device), 1 },
    { "--help", offsetof(struct partfs_options, help), 1 },
    { "-h", offsetof(struct partfs_options, help), 1 },
    FUSE_OPT_END,
};

static int partfs_open_device(struct partfs_device * const pfsdev,
                              const char * const device)
{
    int err;

    err = -1;
    pfsdev->ctx = fdisk_new_context();

    if (pfsdev->ctx) {
        err = fdisk_assign_device(pfsdev->ctx, device, 1);
        if (err) {
            fdisk_unref_context(pfsdev->ctx);
            pfsdev->ctx = NULL;
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
    struct partfs_device * const pfsdev = priv;

    fdisk_deassign_device(pfsdev->ctx, 0);
    fdisk_unref_context(pfsdev->ctx);
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

static int partfs_getattr(const char * const path, struct stat * const st)
{
    struct partfs_device * const pfsdev = fuse_get_context()->private_data;
    int ret;

    if (strcmp(path, "/") == 0) {
        /*
         * will need to increase nlink when
         * extended partitions are supported
         */
        __partfs_stat(st, S_IFDIR | 0755, 2, 0, &pfsdev->st);
        ret = 0;
    } else {
        size_t n;
        int r, c;

        ret = -ENOENT;

        r = sscanf(path, "/" PARTFS_NAME_PREFIX "%zu%n", &n, &c);
        if (r == 1 && c == (int)strlen(path)) {
            struct fdisk_partition * pa;

            pa = NULL;
            fdisk_get_partition(pfsdev->ctx, n, &pa);

            __partfs_stat(st, S_IFREG | 0644, 1,
                          __fdisk_partition_get_size(pfsdev->ctx, pa),
                          &pfsdev->st);

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
    struct partfs_device * const pfsdev = fuse_get_context()->private_data;

    int ret;

    ret = -EIO;
    if (strcmp(path, "/") == 0) {
        struct fdisk_table * tb;
        struct fdisk_iter * it;
        struct fdisk_partition * pa;

        struct stat st;

        __partfs_stat(&st, S_IFDIR | 0755, 2, 0, &pfsdev->st);
        fill(buf, ".",  &st, 0);

        fill(buf, "..", NULL, 0);

        tb = NULL;
        fdisk_get_partitions(pfsdev->ctx, &tb);

        it = fdisk_new_iter(FDISK_ITER_FORWARD);
        while (fdisk_table_next_partition(tb, it, &pa) == 0) {
            char num[16];

            snprintf(num, sizeof(num),
                     PARTFS_NAME_PREFIX "%zu",
                     fdisk_partition_get_partno(pa));

            __partfs_stat(&st, S_IFREG | 0644, 1,
                          __fdisk_partition_get_size(pfsdev->ctx, pa),
                          &pfsdev->st);

            fill(buf, num, &st, 0);
        }

        fdisk_free_iter(it);
        fdisk_unref_table(tb);

        ret = 0;
    }

    return ret;
}

static struct fuse_operations partfs_ops =
{
    .getattr        = partfs_getattr,
    .readdir        = partfs_readdir,

    .init           = partfs_init,
    .destroy        = partfs_destroy,
};

int main(const int argc, char * argv[])
{
    struct fuse_args args = FUSE_ARGS_INIT(argc, argv);
    struct partfs_options opts;
    struct partfs_device pfsdev;
    int err;

    opts.device = NULL;
    opts.help   = 0;

    pfsdev.ctx     = NULL;

    err = fuse_opt_parse(&args, &opts, partfs_optspec, NULL);
    if (!err) {
        if (opts.device) {
            err = stat(opts.device, &pfsdev.st);
            if (err == 0) {
                err = partfs_open_device(&pfsdev, opts.device);
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

            err = fuse_main(args.argc, args.argv, &partfs_ops, &pfsdev);

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
