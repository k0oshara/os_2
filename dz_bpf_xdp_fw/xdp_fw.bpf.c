#include "vmlinux.h"

#include <bpf/bpf_helpers.h>
#include <bpf/bpf_endian.h>

#define MY_ETH_P_IP 0x0800
#define MY_IPPROTO_TCP 6
#define MY_IPPROTO_UDP 17

char LICENSE[] SEC("license") = "GPL";

struct eth_hdr {
    __u8 dst[6];
    __u8 src[6];
    __be16 proto;
} __attribute__((packed));

struct ipv4_hdr {
    __u8 version_ihl;
    __u8 tos;
    __be16 tot_len;
    __be16 id;
    __be16 frag_off;
    __u8 ttl;
    __u8 protocol;
    __be16 check;
    __be32 saddr;
    __be32 daddr;
} __attribute__((packed));

struct tcp_hdr {
    __be16 src;
    __be16 dst;
} __attribute__((packed));

struct udp_hdr {
    __be16 src;
    __be16 dst;
    __be16 len;
    __be16 check;
} __attribute__((packed));

struct {
    __uint(type, BPF_MAP_TYPE_HASH);
    __uint(max_entries, 1024);
    __type(key, __be16);
    __type(value, __u8);
} blocked_ports SEC(".maps");

static __always_inline int port_is_blocked(__be16 port)
{
    __u8 *blocked;

    blocked = bpf_map_lookup_elem(&blocked_ports, &port);
    return blocked && *blocked;
}

SEC("xdp")
int xdp_firewall(struct xdp_md *ctx)
{
    void *data = (void *)(long)ctx->data;
    void *data_end = (void *)(long)ctx->data_end;

    struct eth_hdr *eth;
    struct ipv4_hdr *ip;
    __u32 ip_hlen;
    __u16 frag_off;

    eth = data;
    if ((void *)(eth + 1) > data_end)
        return XDP_PASS;

    if (eth->proto != bpf_htons(MY_ETH_P_IP))
        return XDP_PASS;

    ip = (void *)(eth + 1);
    if ((void *)(ip + 1) > data_end)
        return XDP_PASS;

    if ((ip->version_ihl >> 4) != 4)
        return XDP_PASS;

    ip_hlen = (ip->version_ihl & 0x0f) * 4;
    if (ip_hlen < sizeof(*ip) || ip_hlen > 60)
        return XDP_PASS;

    if ((void *)ip + ip_hlen > data_end)
        return XDP_PASS;

    frag_off = bpf_ntohs(ip->frag_off);

    if (frag_off & 0x3fff)
        return XDP_PASS;

    if (ip->protocol == MY_IPPROTO_TCP) {
        struct tcp_hdr *tcp;

        tcp = (void *)ip + ip_hlen;
        if ((void *)(tcp + 1) > data_end)
            return XDP_PASS;

        if (port_is_blocked(tcp->dst))
            return XDP_DROP;
    }

    if (ip->protocol == MY_IPPROTO_UDP) {
        struct udp_hdr *udp;

        udp = (void *)ip + ip_hlen;
        if ((void *)(udp + 1) > data_end)
            return XDP_PASS;

        if (port_is_blocked(udp->dst))
            return XDP_DROP;
    }

    return XDP_PASS;
}
