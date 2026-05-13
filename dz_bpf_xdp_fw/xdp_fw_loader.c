#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <signal.h>
#include <errno.h>
#include <string.h>
#include <stdint.h>
#include <arpa/inet.h>
#include <net/if.h>
#include <linux/if_link.h>

#include <bpf/libbpf.h>
#include <bpf/bpf.h>

static volatile sig_atomic_t exiting = 0;

static void handle_signal(int sig)
{
    (void)sig;
    exiting = 1;
}

static int setup_signals(void)
{
    struct sigaction sa;

    sa.sa_handler = handle_signal;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0;

    if (sigaction(SIGINT, &sa, NULL) == -1 ||
        sigaction(SIGTERM, &sa, NULL) == -1 ||
        sigaction(SIGQUIT, &sa, NULL) == -1)
        return -1;

    return 0;
}

static int parse_port(const char *s, uint16_t *port)
{
    char *end;
    unsigned long value;

    errno = 0;
    value = strtoul(s, &end, 10);

    if (errno || *end != '\0' || value == 0 || value > 65535)
        return -1;

    *port = (uint16_t)value;
    return 0;
}

static int block_port(int map_fd, uint16_t port)
{
    __be16 key;
    __u8 value;

    key = htons(port);
    value = 1;

    if (bpf_map_update_elem(map_fd, &key, &value, BPF_ANY) != 0)
    {
        fprintf(stderr, "failed to block port %u: %s\n",
                port, strerror(errno));
        return -1;
    }

    printf("blocked port: %u\n", port);
    return 0;
}

int main(int argc, char **argv)
{
    const char *ifname;
    int ifindex;
    struct bpf_object *obj;
    struct bpf_program *prog;
    struct bpf_map *map;
    int prog_fd;
    int map_fd;
    int flags;
    int err;

    if (argc < 3)
    {
        fprintf(stderr, "usage: %s IFACE PORT [PORT...]\n", argv[0]);
        fprintf(stderr, "example: sudo %s enp0s3 8080\n", argv[0]);
        return EXIT_FAILURE;
    }

    ifname = argv[1];
    ifindex = if_nametoindex(ifname);
    if (ifindex == 0)
    {
        fprintf(stderr, "bad interface %s: %s\n", ifname, strerror(errno));
        return EXIT_FAILURE;
    }

    if (setup_signals())
    {
        perror("sigaction");
        return EXIT_FAILURE;
    }

    obj = bpf_object__open_file("xdp_fw.bpf.o", NULL);
    if (!obj)
    {
        perror("bpf_object__open_file");
        return EXIT_FAILURE;
    }

    if (bpf_object__load(obj))
    {
        perror("bpf_object__load");
        bpf_object__close(obj);
        return EXIT_FAILURE;
    }

    prog = bpf_object__find_program_by_name(obj, "xdp_firewall");
    if (!prog)
    {
        fprintf(stderr, "failed to find BPF program xdp_firewall\n");
        bpf_object__close(obj);
        return EXIT_FAILURE;
    }

    map = bpf_object__find_map_by_name(obj, "blocked_ports");
    if (!map)
    {
        fprintf(stderr, "failed to find BPF map blocked_ports\n");
        bpf_object__close(obj);
        return EXIT_FAILURE;
    }

    prog_fd = bpf_program__fd(prog);
    map_fd = bpf_map__fd(map);

    if (prog_fd < 0 || map_fd < 0)
    {
        fprintf(stderr, "bad BPF fd\n");
        bpf_object__close(obj);
        return EXIT_FAILURE;
    }

    for (int i = 2; i < argc; i++)
    {
        uint16_t port;

        if (parse_port(argv[i], &port))
        {
            fprintf(stderr, "bad port: %s\n", argv[i]);
            bpf_object__close(obj);
            return EXIT_FAILURE;
        }

        if (block_port(map_fd, port))
        {
            bpf_object__close(obj);
            return EXIT_FAILURE;
        }
    }

    flags = XDP_FLAGS_SKB_MODE;

    bpf_xdp_detach(ifindex, flags, NULL);

    err = bpf_xdp_attach(ifindex, prog_fd, flags, NULL);
    if (err)
    {
        fprintf(stderr, "failed to attach XDP to %s: %s\n",
                ifname, strerror(-err));
        bpf_object__close(obj);
        return EXIT_FAILURE;
    }

    printf("XDP firewall attached to %s\n", ifname);
    printf("press Ctrl+C to detach\n");

    while (!exiting)
        sleep(1);

    printf("detaching XDP from %s\n", ifname);

    bpf_xdp_detach(ifindex, flags, NULL);
    bpf_object__close(obj);

    return EXIT_SUCCESS;
}
