#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/fs.h>
#include <linux/blkdev.h>
#include <linux/bio.h>
#include <linux/blk_types.h>
#include <linux/highmem.h>
#include <linux/moduleparam.h>
#include <linux/vmalloc.h>
#include <linux/string.h>

MODULE_DESCRIPTION("RAM Disk");
MODULE_AUTHOR("Maksim Ivanov");
MODULE_LICENSE("GPL");
MODULE_VERSION("0.1");

#define MY_BLOCK_MAJOR 0
#define MY_BLKDEV_NAME "mybdev"
#define MY_BLOCK_MINORS 1
#define KERNEL_SECTOR_SIZE 512
#define SHIFT_SECTOR 9

static unsigned long disk_size_mb = 256;
module_param(disk_size_mb, ulong, 0444);
MODULE_PARM_DESC(disk_size_mb, "RAM disk size in MiB");

struct my_block_dev {
    struct gendisk *gd;
    u8 *data;
    u64 size;
    int major;
};

static struct my_block_dev g_dev;

static int my_block_open(struct gendisk *disk, blk_mode_t mode) 
{
    return 0;
}

static void my_block_release(struct gendisk *disk)
{
}

static void my_block_copy_data(struct my_block_dev *dev, sector_t sector,
                  unsigned int len, char *buffer, int dir)
{
    u64 offset = (u64)sector << SHIFT_SECTOR;

    if (offset > dev->size || len > dev->size - offset)
        return;

    if (dir == WRITE)
        memcpy(dev->data + offset, buffer, len);
    else
        memcpy(buffer, dev->data + offset, len);
}

static blk_status_t my_block_handle_bio(struct my_block_dev *dev, struct bio *bio)
{
    struct bio_vec bvec;
    struct bvec_iter iter;
    int dir = op_is_write(bio_op(bio)) ? WRITE : READ;

    bio_for_each_segment(bvec, bio, iter) {
        sector_t sector = iter.bi_sector;
        char *buffer;

        buffer = kmap_local_page(bvec.bv_page);
        my_block_copy_data(dev, sector, bvec.bv_len,
                  buffer + bvec.bv_offset, dir);
        kunmap_local(buffer);
    }

    return BLK_STS_OK;
}

static void my_block_submit_bio(struct bio *bio)
{
    struct my_block_dev *dev = bio->bi_bdev->bd_disk->private_data;
    u64 pos = (u64)bio->bi_iter.bi_sector << SHIFT_SECTOR;
    u64 len = bio->bi_iter.bi_size;

    if (pos > dev->size || len > dev->size - pos) {
        bio_io_error(bio);
        return;
    }

    switch (bio_op(bio)) {
    case REQ_OP_READ:
    case REQ_OP_WRITE:
        bio->bi_status = my_block_handle_bio(dev, bio);
        bio_endio(bio);
        return;
    case REQ_OP_FLUSH:
        bio_endio(bio);
        return;
    case REQ_OP_DISCARD:
    case REQ_OP_WRITE_ZEROES:
        memset(dev->data + pos, 0, len);
        bio_endio(bio);
        return;
    default:
        bio_io_error(bio);
        return;
    }
}

static const struct block_device_operations my_block_ops = {
    .owner = THIS_MODULE,
    .open = my_block_open,
    .release = my_block_release,
    .submit_bio = my_block_submit_bio,
};

static int create_block_device(struct my_block_dev *dev)
{
    struct queue_limits lim = {
        .logical_block_size = KERNEL_SECTOR_SIZE,
        .physical_block_size = KERNEL_SECTOR_SIZE,
        .io_min = KERNEL_SECTOR_SIZE,
        .max_hw_discard_sectors = ~0U,
        .max_discard_segments = 1,
        .discard_granularity = KERNEL_SECTOR_SIZE,
        .features = BLK_FEAT_SYNCHRONOUS,
    };
    int ret;

    if (!disk_size_mb || disk_size_mb > (ULONG_MAX >> 20))
        return -EINVAL;

    dev->size = (u64)disk_size_mb << 20;
    dev->size &= ~((u64)KERNEL_SECTOR_SIZE - 1);
    if (!dev->size)
        return -EINVAL;

    dev->data = vmalloc(dev->size);
    if (!dev->data)
        return -ENOMEM;
    memset(dev->data, 0, dev->size);

    ret = register_blkdev(MY_BLOCK_MAJOR, MY_BLKDEV_NAME);
    if (ret < 0) 
        goto out_vfree;
    dev->major = ret;

    dev->gd = blk_alloc_disk(&lim, NUMA_NO_NODE);
    if (IS_ERR(dev->gd)) {
        ret = PTR_ERR(dev->gd);
        dev->gd = NULL;
        goto out_unregister;
    }

    dev->gd->major = dev->major;
    dev->gd->first_minor = 0;
    dev->gd->minors = MY_BLOCK_MINORS;
    dev->gd->fops = &my_block_ops;
    dev->gd->private_data = dev;
    strscpy(dev->gd->disk_name, "myblock0", DISK_NAME_LEN);
    set_capacity(dev->gd, dev->size >> SECTOR_SHIFT);

    ret = add_disk(dev->gd);
    if (ret) 
        goto out_put_disk;

    pr_info("%s: created /dev/%s size=%llu MiB\n", MY_BLKDEV_NAME, dev->gd->disk_name, dev->size >> 20);
    return 0;

out_put_disk:
    put_disk(dev->gd);
    dev->gd = NULL;
out_unregister:
    unregister_blkdev(dev->major, MY_BLKDEV_NAME);
    dev->major = 0;
out_vfree:
    vfree(dev->data);
    dev->data = NULL;
    return ret;
}

static void delete_block_device(struct my_block_dev *dev)
{
    if (dev->gd) {
        del_gendisk(dev->gd);
        put_disk(dev->gd);
        dev->gd = NULL;
    }
    if (dev->major > 0) {
        unregister_blkdev(dev->major, MY_BLKDEV_NAME);
        dev->major = 0;
    }
    if (dev->data) {
        vfree(dev->data);
        dev->data = NULL;
    }
}

static int __init my_block_init(void)
{
    return create_block_device(&g_dev);
}

static void __exit my_block_exit(void)
{
    delete_block_device(&g_dev);
}

module_init(my_block_init);
module_exit(my_block_exit);
