#ifndef _DEV__NET__NET_K_H
#define _DEV__NET__NET_K_H

#include <lib/lock.k.h>
#include <lib/resource.k.h>
#include <lib/vector.k.h>
#include <net/if.h>
#include <stdint.h>
#include <sys/socket.h>

#define NET_PORTRANGESTART 49152
#define NET_PORTRANGEEND UINT16_MAX

#define NET_ETHPROTOIPV4 0x800
#define NET_ETHPROTOARP 0x806

typedef uint16_t be_uint16_t;
typedef uint32_t be_uint32_t;

struct net_inetaddr {
    union {
        struct {
            uint8_t data[4]; // encapsulate so union shares all this memory instead of individual elements
        };
        uint32_t value;
    };
};

struct net_icmpheader {
    uint8_t type;
    uint8_t code;
    be_uint16_t csum;
    uint8_t data[];
} __attribute__((packed));

#define NET_IPFLAGMF 0x2000
#define NET_IPFLAGDF 0x4000
#define NET_IPFLAGRF 0x8000
#define NET_IPOFFMASK 0x1fff

struct net_inetheader {
    uint8_t ihl : 4;
    uint8_t version : 4;
    uint8_t ecn : 2;
    uint8_t dscp : 6;
    be_uint16_t len;
    be_uint16_t id;
    be_uint16_t fragoff;
    uint8_t ttl;
    uint8_t protocol;
    be_uint16_t csum;
    struct net_inetaddr src;
    struct net_inetaddr dest;
    uint8_t data[];
} __attribute__((packed));

struct net_macaddr {
    uint8_t mac[6];
} __attribute__((packed));

struct net_etherframe {
    struct net_macaddr dest;
    struct net_macaddr src;
    be_uint16_t type;
    uint8_t data[]; // data needs to be in a dynamic representation
} __attribute__((packed));

struct net_arpheader {
    be_uint16_t hwtype;
    be_uint16_t prtype;
    uint8_t hwlen;
    uint8_t plen;
    be_uint16_t opcode;
    struct net_macaddr srchw;
    struct net_inetaddr srcpr;
    struct net_macaddr desthw;
    struct net_inetaddr destpr;
} __attribute__((packed));

struct net_packet { // high-level kernel interface for packets
    size_t len;
    uint8_t *data;
};

struct net_inethwpair {
    struct net_inetaddr inet;
    struct net_macaddr hw;
};

struct net_adapter {
    struct resource;

    struct net_macaddr mac;
    struct net_macaddr permmac;
    struct net_inetaddr ip;
    struct net_inetaddr gateway;
    struct net_inetaddr subnetmask;
    uint16_t ipframe;
    uint16_t flags;
    int index;
    size_t hwmtu; // hardware driver MTU
    size_t mtu;
    VECTOR_TYPE(struct net_inethwpair *) addrcache; // keep a record (per adapter as they may be connected to different networks) of IP-to-MAC records (from the ARP cache)
    spinlock_t addrcachelock;
    VECTOR_TYPE(struct net_packet *) cache;
    spinlock_t cachelock; // not the same as the char device lock
    char ifname[IFNAMSIZ];
    uint8_t type;
    struct event packetevent; // signal for packet arrival

    spinlock_t socklock; // lock on socket
    VECTOR_TYPE(struct socket *) boundsocks;

    void (*txpacket)(struct net_adapter *adapter, const void *data, size_t length);
    void (*updateflags)(struct net_adapter *adapter, uint16_t old);
};

#define NET_LINKLAYERFRAMESIZE(a) ({ \
    __auto_type LINKLAYERFRAMESIZE_ret = 0; \
    if (a->type & NET_ADAPTERETH) { \
        LINKLAYERFRAMESIZE_ret = sizeof(struct net_etherframe); \
    } \
    LINKLAYERFRAMESIZE_ret; \
})

#define NET_IP(a, b, c, d) (((uint32_t)d << 24) | ((uint32_t)c << 16) | ((uint32_t)b << 8) | ((uint32_t)a << 0))
#define NET_IPSTRUCT(a) ({ (struct net_inetaddr) { .value = (a) }; })
#define NET_PRINTIP(a) (a).data[0], (a).data[1], (a).data[2], (a).data[3]
#define NET_MACSTRUCT(a, b, c, d, e, f) ({ (struct net_macaddr) { .mac = { a, b, c, d, e, f } }; })
#define NET_PRINTMAC(a) (a).mac[0], (a).mac[1], (a).mac[2], (a).mac[3], (a).mac[4], (a).mac[5]

enum {
    NET_ADAPTERETH = (1 << 0),
    NET_ADAPTERLO = (1 << 1)
};

int net_ifioctl(struct resource *_this, struct f_description *description, uint64_t request, uint64_t arg);
ssize_t net_getsockopt(struct socket *_this, struct f_description *description, int level, int optname, void *optval, socklen_t *optlen);
ssize_t net_setsockopt(struct socket *_this, struct f_description *description, int level, int optname, const void *optval, socklen_t optlen);

be_uint16_t net_checksum(void *data, size_t length);
ssize_t net_sendinet(struct net_adapter *adapter, struct net_inetaddr src, struct net_inetaddr dest, uint8_t protocol, void *data, size_t length);
void net_bindsocket(struct net_adapter *adapter, struct socket *sock);
void net_unbindsocket(struct net_adapter *adapter, struct socket *sock);
void net_unbindall(struct net_adapter *adapter);
uint16_t net_allocport(void);
void net_releaseport(uint16_t port);
struct net_adapter *net_findadapterbyip(struct net_inetaddr addr);
ssize_t net_lookup(struct net_adapter *adapter, struct net_inetaddr ip, struct net_macaddr *mac);
ssize_t net_route(struct net_adapter **adapter, struct net_inetaddr local, struct net_inetaddr remote, struct net_macaddr *mac);
void net_register(struct net_adapter *adapter);
void net_init(void);

#endif
