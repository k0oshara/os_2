#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>

#include "membuf_ioctl.h"

int main(int argc, char **argv)
{
    int fd;
    uint64_t sz;

    if (argc != 3) {
        fprintf(stderr, "usage: %s /dev/membufN new_size\n", argv[0]);
        return 1;
    }

    fd = open(argv[1], O_RDWR);
    if (fd < 0) {
        perror("open");
        return 1;
    }

    if (ioctl(fd, MEMBUF_IOCTL_GET_SIZE, &sz) < 0) {
        perror("GET_SIZE");
        close(fd);
        return 1;
    }
    printf("old size: %llu\n", (unsigned long long)sz);

    sz = strtoull(argv[2], NULL, 0);
    if (ioctl(fd, MEMBUF_IOCTL_SET_SIZE, &sz) < 0) {
        perror("SET_SIZE");
        close(fd);
        return 1;
    }

    if (ioctl(fd, MEMBUF_IOCTL_GET_SIZE, &sz) < 0) {
        perror("GET_SIZE");
        close(fd);
        return 1;
    }
    printf("new size: %llu\n", (unsigned long long)sz);

    close(fd);
    return 0;
}
