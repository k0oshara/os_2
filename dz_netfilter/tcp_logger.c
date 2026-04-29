#include <linux/init.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/netfilter.h>
#include <linux/netfilter_ipv4.h>
#include <linux/skbuff.h>
#include <linux/ip.h>
#include <linux/tcp.h>
#include <linux/in.h>
#include <net/net_namespace.h>

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Maksim Ivanov");
MODULE_DESCRIPTION("Log outgoing TCP connections with destination port filter");
MODULE_VERSION("0.1");

static unsigned short dport;
module_param(dport, ushort, 0644);
MODULE_PARM_DESC(dport, "Destination TCP port filter. 0 means log all ports");

static struct nf_hook_ops tcp_logger_ops;

static unsigned int tcp_logger_hook(void *priv, struct sk_buff *skb, const struct nf_hook_state *state)
{
    struct iphdr *iph;
    struct tcphdr *tcph;
    unsigned int nhoff;
    unsigned int ihl;
    unsigned short sport;
    unsigned short dst_port;

    if (!skb)
        return NF_ACCEPT;

    nhoff = skb_network_offset(skb);

    if (!pskb_may_pull(skb, nhoff + sizeof(*iph)))
        return NF_ACCEPT;

    iph = ip_hdr(skb);
    if (!iph || iph->version != 4 || iph->protocol != IPPROTO_TCP)
        return NF_ACCEPT;

    ihl = iph->ihl * 4;
    if (ihl < sizeof(*iph))
        return NF_ACCEPT;

    if (!pskb_may_pull(skb, nhoff + ihl + sizeof(*tcph)))
        return NF_ACCEPT;

    iph = ip_hdr(skb);
    tcph = (struct tcphdr *)((unsigned char *)iph + ihl);

    if (!tcph->syn || tcph->ack)
        return NF_ACCEPT;

    sport = ntohs(tcph->source);
    dst_port = ntohs(tcph->dest);

    if (dport != 0 && dst_port != dport)
        return NF_ACCEPT;

    pr_info("tcp_logger: %pI4:%hu -> %pI4:%hu\n", &iph->saddr, sport, &iph->daddr, dst_port);

    return NF_ACCEPT;
}

static int __init tcp_logger_init(void)
{
    int ret;

    tcp_logger_ops.hook = tcp_logger_hook;
    tcp_logger_ops.pf = NFPROTO_IPV4;
    tcp_logger_ops.hooknum = NF_INET_LOCAL_OUT;
    tcp_logger_ops.priority = NF_IP_PRI_FIRST;

    ret = nf_register_net_hook(&init_net, &tcp_logger_ops);
    if (ret)
        return ret;

    pr_info("tcp_logger: loaded, dport=%hu\n", dport);
    return 0;
}

static void __exit tcp_logger_exit(void)
{
    nf_unregister_net_hook(&init_net, &tcp_logger_ops);
    pr_info("tcp_logger: unloaded\n");
}

module_init(tcp_logger_init);
module_exit(tcp_logger_exit);
