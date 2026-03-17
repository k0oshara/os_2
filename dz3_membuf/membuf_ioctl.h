#ifndef MEMBUF_IOCTL_H
#define MEMBUF_IOCTL_H

#include <linux/ioctl.h>
#include <linux/types.h>

#define MEMBUF_IOC_MAGIC 'm'

#define MEMBUF_IOCTL_GET_SIZE _IOR(MEMBUF_IOC_MAGIC, 1, __u64)
#define MEMBUF_IOCTL_SET_SIZE _IOW(MEMBUF_IOC_MAGIC, 2, __u64)

#endif /* MEMBUF_IOCTL_H */
