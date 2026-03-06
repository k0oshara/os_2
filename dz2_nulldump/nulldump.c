#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/fs.h>
#include <linux/cdev.h>
#include <linux/device.h>
#include <linux/uaccess.h>
#include <linux/slab.h>
#include <linux/sched.h>
#include <linux/printk.h>

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Maksim Ivanov");
MODULE_DESCRIPTION("nulldump character device (/dev/null-like) with logging and hex dump");
MODULE_VERSION("0.1");

#define DEVICE_NAME "nulldump"
#define CLASS_NAME  "nulldump_class"
#define DUMP_CHUNK  256

static dev_t dev;
static struct cdev nulldump_cdev;
static struct class *nulldump_class;
static struct device *nulldump_dev;

static ssize_t nulldump_read(struct file *file, char __user *buf, size_t len, loff_t *off)
{
    pr_info("nulldump: read request=%zu pid=%d comm=%s\n", len, current->pid, current->comm);
    return 0;
}

static ssize_t nulldump_write(struct file *file, const char __user *buf, size_t len, loff_t *off)
{
    u8 *kbuf;
    size_t cap;
    size_t pos = 0;

    pr_info("nulldump: write size=%zu pid=%d comm=%s\n", len, current->pid, current->comm);

    if (len == 0) return 0;

    cap = min_t(size_t, len, DUMP_CHUNK);
    kbuf = kmalloc(cap, GFP_KERNEL);
    if (!kbuf) {
        pr_info("nulldump: dump skipped (kmalloc failed) pid=%d comm=%s\n", current->pid, current->comm);
        return len;
    }

    while (pos < len) {
        size_t n = min_t(size_t, len - pos, cap);
        size_t not_copied;
        size_t copied;

        not_copied = copy_from_user(kbuf, buf + pos, n);
        copied = n - not_copied;

        if (copied) {
            pr_info("nulldump: dump off=%zu len=%zu pid=%d comm=%s\n", pos, copied, current->pid, current->comm);
            print_hex_dump(KERN_INFO, "nulldump: ", DUMP_PREFIX_OFFSET, 16, 1, kbuf, copied, true);
        }

        if (not_copied) {
            pr_info("nulldump: copy_from_user fault off=%zu not_copied=%zu pid=%d comm=%s\n",pos + copied, not_copied, current->pid, current->comm);
            kfree(kbuf);
            return -EFAULT;
        }

        pos += copied;
    }

    kfree(kbuf);
    return len;
}

static const struct file_operations nulldump_fops = {
    .owner = THIS_MODULE,
    .read = nulldump_read,
    .write = nulldump_write,
    .llseek = noop_llseek,
};

static int __init nulldump_init(void)
{
    int ret;

    ret = alloc_chrdev_region(&dev, 0, 1, DEVICE_NAME);
    if (ret) {
        pr_err("nulldump: alloc_chrdev_region failed\n");
        return ret;
    }

    pr_info("nulldump: registered (major=%d, minor=%d)\n", MAJOR(dev), MINOR(dev));

    cdev_init(&nulldump_cdev, &nulldump_fops);

    ret = cdev_add(&nulldump_cdev, dev, 1);
    if (ret) {
        pr_err("nulldump: cdev_add failed\n");
        unregister_chrdev_region(dev, 1);
        return ret;
    }

    nulldump_class = class_create(CLASS_NAME);
    if (IS_ERR(nulldump_class)) {
        pr_err("nulldump: class_create failed\n");
        ret = PTR_ERR(nulldump_class);
        cdev_del(&nulldump_cdev);
        unregister_chrdev_region(dev, 1);
        return ret;
    }

    nulldump_dev = device_create(nulldump_class, NULL, dev, NULL, DEVICE_NAME);
    if (IS_ERR(nulldump_dev)) {
        pr_err("nulldump: device_create failed\n");
        ret = PTR_ERR(nulldump_dev);
        class_destroy(nulldump_class);
        cdev_del(&nulldump_cdev);
        unregister_chrdev_region(dev, 1);
        return ret;
    }

    pr_info("nulldump: module loaded (/dev/%s)\n", DEVICE_NAME);
    return 0;
}

static void __exit nulldump_exit(void)
{
    device_destroy(nulldump_class, dev);
    class_destroy(nulldump_class);
    cdev_del(&nulldump_cdev);
    unregister_chrdev_region(dev, 1);

    pr_info("nulldump: module unloaded\n");
}

module_init(nulldump_init);
module_exit(nulldump_exit);
