#include <linux/cdev.h>
#include <linux/device.h>
#include <linux/fs.h>
#include <linux/init.h>
#include <linux/kernel.h>
#include <linux/kstrtox.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/rwsem.h>
#include <linux/slab.h>
#include <linux/uaccess.h>
#include <linux/version.h>
#include <linux/vmalloc.h>

#include "membuf_ioctl.h"

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Maksim Ivanov");
MODULE_DESCRIPTION("Memory-backed dynamic character devices: /dev/membufN");
MODULE_VERSION("0.1");

#define MEMBUF_NAME "membuf"
#define MEMBUF_MAX_DEVICES 4096

struct membuf_dev {
    char *buf;
    size_t size;
    unsigned int minor;
    struct device *dev;
    struct rw_semaphore lock;
    struct mutex open_lock;
    unsigned int open_count;
};

static dev_t membuf_devt;
static struct cdev membuf_cdev;
static struct class *membuf_class;

static DECLARE_RWSEM(membuf_tbl_lock);
static DEFINE_MUTEX(membuf_ctl_lock);
static struct membuf_dev *membuf_tbl[MEMBUF_MAX_DEVICES];

static unsigned int num_devices = 1;
static unsigned long default_buf_size = 4096;
static bool membuf_ready;

static int membuf_resize(struct membuf_dev *d, size_t new_size)
{
    char *n;
    size_t keep;

    if (!new_size) return -EINVAL;

    n = kvzalloc(new_size, GFP_KERNEL);
    if (!n) return -ENOMEM;

    down_write(&d->lock);
    keep = min(d->size, new_size);
    if (keep) memcpy(n, d->buf, keep);

    kvfree(d->buf);
    d->buf = n;
    d->size = new_size;
    up_write(&d->lock);

    return 0;
}

static int membuf_create_one(unsigned int minor, size_t size)
{
    struct membuf_dev *d;
    int err;

    if (minor >= MEMBUF_MAX_DEVICES || !size) return -EINVAL;
    if (membuf_tbl[minor]) return -EEXIST;

    d = kzalloc(sizeof(*d), GFP_KERNEL);
    if (!d) return -ENOMEM;

    d->buf = kvzalloc(size, GFP_KERNEL);
    if (!d->buf) {
        kfree(d);
        return -ENOMEM;
    }

    d->size = size;
    d->minor = minor;
    init_rwsem(&d->lock);
    mutex_init(&d->open_lock);

    membuf_tbl[minor] = d;

    d->dev = device_create(membuf_class, NULL, MKDEV(MAJOR(membuf_devt), minor), d, "%s%u", MEMBUF_NAME, minor);
    if (IS_ERR(d->dev)) {
        err = PTR_ERR(d->dev);
        membuf_tbl[minor] = NULL;
        kvfree(d->buf);
        kfree(d);
        return err;
    }

    return 0;
}

static int membuf_destroy_one(unsigned int minor)
{
    struct membuf_dev *d;

    if (minor >= MEMBUF_MAX_DEVICES) return -EINVAL;

    d = membuf_tbl[minor];
    if (!d) return 0;

    mutex_lock(&d->open_lock);
    if (d->open_count) {
        mutex_unlock(&d->open_lock);
        return -EBUSY;
    }
    mutex_unlock(&d->open_lock);

    membuf_tbl[minor] = NULL;

    device_destroy(membuf_class, MKDEV(MAJOR(membuf_devt), minor));
    kvfree(d->buf);
    kfree(d);
    return 0;
}

static int membuf_set_count(unsigned int new_count)
{
    unsigned int old_count;
    unsigned int i;
    unsigned long size_snapshot;
    int err = 0;

    if (new_count > MEMBUF_MAX_DEVICES) return -EINVAL;

    mutex_lock(&membuf_ctl_lock);
    old_count = num_devices;
    if (new_count == old_count) {
        mutex_unlock(&membuf_ctl_lock);
        return 0;
    }

    size_snapshot = READ_ONCE(default_buf_size);
    if (!size_snapshot) {
        mutex_unlock(&membuf_ctl_lock);
        return -EINVAL;
    }

    down_write(&membuf_tbl_lock);

    if (new_count > old_count) {
        for (i = old_count; i < new_count; ++i) {
            err = membuf_create_one(i, size_snapshot);
            if (err) break;
        }
        if (err) {
            while (i-- > old_count) membuf_destroy_one(i);
            goto out_unlock;
        }
    }
    else {
        for (i = old_count; i > new_count; --i) {
            struct membuf_dev *d = membuf_tbl[i - 1];
            if (!d) continue;
            mutex_lock(&d->open_lock);
            if (d->open_count) {
                mutex_unlock(&d->open_lock);
                err = -EBUSY;
                goto out_unlock;
            }
            mutex_unlock(&d->open_lock);
        }
        for (i = old_count; i > new_count; --i) {
            err = membuf_destroy_one(i - 1);
            if (err) goto out_unlock;
        }
    }

    num_devices = new_count;

out_unlock:
    up_write(&membuf_tbl_lock);
    mutex_unlock(&membuf_ctl_lock);
    return err;
}

static int membuf_open(struct inode *ino, struct file *f)
{
    struct membuf_dev *d;
    unsigned int minor = iminor(ino);

    if (minor >= MEMBUF_MAX_DEVICES) return -ENODEV;

    down_read(&membuf_tbl_lock);
    d = membuf_tbl[minor];
    if (!d) {
        up_read(&membuf_tbl_lock);
        return -ENODEV;
    }

    mutex_lock(&d->open_lock);
    d->open_count++;
    mutex_unlock(&d->open_lock);
    f->private_data = d;
    up_read(&membuf_tbl_lock);

    return 0;
}

static int membuf_release(struct inode *ino, struct file *f)
{
    struct membuf_dev *d = f->private_data;

    if (!d) return 0;

    mutex_lock(&d->open_lock);
    if (d->open_count) d->open_count--;
    mutex_unlock(&d->open_lock);

    return 0;
}

static ssize_t membuf_read(struct file *f, char __user *ubuf, size_t cnt, loff_t *ppos)
{
    struct membuf_dev *d = f->private_data;
    size_t left;
    size_t not_copied;

    if (!d) return -ENODEV;
    if (*ppos < 0) return -EINVAL;
    if (!cnt) return 0;

    down_read(&d->lock);
    if (*ppos >= d->size) {
        up_read(&d->lock);
        return 0;
    }

    left = d->size - *ppos;
    if (cnt > left) cnt = left;

    not_copied = copy_to_user(ubuf, d->buf + *ppos, cnt);
    cnt -= not_copied;
    *ppos += cnt;
    up_read(&d->lock);

    if (!cnt && not_copied) return -EFAULT;

    return cnt;
}

static ssize_t membuf_write(struct file *f, const char __user *ubuf, size_t cnt, loff_t *ppos)
{
    struct membuf_dev *d = f->private_data;
    size_t left;
    size_t not_copied;

    if (!d) return -ENODEV;
    if (*ppos < 0) return -EINVAL;
    if (!cnt) return 0;

    down_write(&d->lock);
    if (*ppos >= d->size) {
        up_write(&d->lock);
        return -ENOSPC;
    }

    left = d->size - *ppos;
    if (cnt > left) cnt = left;

    not_copied = copy_from_user(d->buf + *ppos, ubuf, cnt);
    cnt -= not_copied;
    *ppos += cnt;
    up_write(&d->lock);

    if (!cnt && not_copied) return -EFAULT;

    return cnt;
}

static loff_t membuf_llseek(struct file *f, loff_t off, int whence)
{
    struct membuf_dev *d = f->private_data;
    loff_t pos;
    size_t size;

    if (!d) return -ENODEV;

    down_read(&d->lock);
    size = d->size;
    up_read(&d->lock);

    switch (whence) {
        case SEEK_SET: pos = off; break;
        case SEEK_CUR: pos = f->f_pos + off; break;
        case SEEK_END: pos = size + off; break;
        default:       return -EINVAL;
    }

    if (pos < 0 || pos > (loff_t)size) return -EINVAL;

    f->f_pos = pos;
    return pos;
}

static long membuf_ioctl(struct file *f, unsigned int cmd, unsigned long arg)
{
    struct membuf_dev *d = f->private_data;
    __u64 sz64;

    if (!d) return -ENODEV;

    switch (cmd) {
    case MEMBUF_IOCTL_GET_SIZE:
        down_read(&d->lock);
        sz64 = d->size;
        up_read(&d->lock);
        if (copy_to_user((void __user *)arg, &sz64, sizeof(sz64))) return -EFAULT;
        return 0;

    case MEMBUF_IOCTL_SET_SIZE:
        if (copy_from_user(&sz64, (void __user *)arg, sizeof(sz64))) return -EFAULT;
        if (!sz64) return -EINVAL;
        if ((__u64)(size_t)sz64 != sz64) return -EOVERFLOW;
        return membuf_resize(d, (size_t)sz64);

    default:
        return -ENOTTY;
    }
}

static const struct file_operations membuf_fops = {
    .owner = THIS_MODULE,
    .open = membuf_open,
    .release = membuf_release,
    .read = membuf_read,
    .write = membuf_write,
    .llseek = membuf_llseek,
    .unlocked_ioctl = membuf_ioctl,
};

static int param_set_num_devices(const char *val, const struct kernel_param *kp)
{
    unsigned int v;
    int err;

    err = kstrtouint(val, 0, &v);
    if (err) return err;
    if (v > MEMBUF_MAX_DEVICES) return -EINVAL;

    if (!membuf_ready) {
        *(unsigned int *)kp->arg = v;
        return 0;
    }

    err = membuf_set_count(v);
    if (!err) *(unsigned int *)kp->arg = v;
    return err;
}

static int param_set_default_buf_size(const char *val, const struct kernel_param *kp)
{
    unsigned long v;
    int err;

    err = kstrtoul(val, 0, &v);
    if (err) return err;
    if (!v) return -EINVAL;

    *(unsigned long *)kp->arg = v;
    return 0;
}

static const struct kernel_param_ops num_devices_ops = {
    .set = param_set_num_devices,
    .get = param_get_uint,
};

static const struct kernel_param_ops default_buf_size_ops = {
    .set = param_set_default_buf_size,
    .get = param_get_ulong,
};

module_param_cb(num_devices, &num_devices_ops, &num_devices, 0644);
MODULE_PARM_DESC(num_devices, "Current number of active membuf devices");

module_param_cb(default_buf_size, &default_buf_size_ops, &default_buf_size, 0644);
MODULE_PARM_DESC(default_buf_size, "Default buffer size for newly created devices");

static int __init membuf_init(void)
{
    int err;
    unsigned int i;

    if (!default_buf_size) return -EINVAL;
    if (num_devices > MEMBUF_MAX_DEVICES) return -EINVAL;

    err = alloc_chrdev_region(&membuf_devt, 0, MEMBUF_MAX_DEVICES, MEMBUF_NAME);
    if (err) return err;

    cdev_init(&membuf_cdev, &membuf_fops);

    err = cdev_add(&membuf_cdev, membuf_devt, MEMBUF_MAX_DEVICES);
    if (err) goto err_unreg;

    membuf_class = class_create(MEMBUF_NAME);
    if (IS_ERR(membuf_class)) {
        err = PTR_ERR(membuf_class);
        goto err_cdev;
    }

    down_write(&membuf_tbl_lock);
    for (i = 0; i < num_devices; ++i) {
        err = membuf_create_one(i, default_buf_size);
        if (err) goto err_devices;
    }
    up_write(&membuf_tbl_lock);

    membuf_ready = true;
    pr_info("membuf: registered major=%d count=%u default_buf_size=%lu\n", MAJOR(membuf_devt), num_devices, default_buf_size);
    return 0;

err_devices:
    while (i-- > 0) membuf_destroy_one(i);
    up_write(&membuf_tbl_lock);
    class_destroy(membuf_class);
err_cdev:
    cdev_del(&membuf_cdev);
err_unreg:
    unregister_chrdev_region(membuf_devt, MEMBUF_MAX_DEVICES);
    return err;
}

static void __exit membuf_exit(void)
{
    unsigned int i;

    membuf_ready = false;

    down_write(&membuf_tbl_lock);
    for (i = num_devices; i > 0; --i) {
        int err = membuf_destroy_one(i - 1);
        if (err) pr_warn("membuf: failed to destroy minor %u: %d\n", i - 1, err);
    }
    up_write(&membuf_tbl_lock);

    class_destroy(membuf_class);
    cdev_del(&membuf_cdev);
    unregister_chrdev_region(membuf_devt, MEMBUF_MAX_DEVICES);
    pr_info("membuf: unloaded\n");
}

module_init(membuf_init);
module_exit(membuf_exit);
