#include <dev/net/loopback.k.h>
#include <dev/net/net.k.h>
#include <fs/devtmpfs.k.h>
#include <fs/vfs/vfs.k.h>
#include <ipc/socket.k.h>
#include <ipc/socket/tcp.k.h>
#include <ipc/socket/udp.k.h>
#include <lib/bitmap.k.h>
#include <lib/errno.k.h>
#include <lib/event.k.h>
#include <lib/lock.k.h>
#include <lib/print.k.h>
#include <lyre/sockios.h>
#include <net/if.h>
#include <netinet/in.h>
#include <printf/printf.h>
#include <sched/sched.k.h>
#include <time/time.k.h>

static int net_ethcount = 0;
static VECTOR_TYPE(struct net_adapter *) net_adapters = VECTOR_INIT;
static uint8_t *net_portbitmap; // use a bitmap to keep track of port allocations
static spinlock_t net_portbitmaplock = SPINLOCK_INIT;

be_uint16_t net_checksum(void *data, size_t length) {
    uint16_t *p = (uint16_t *)data;
    uint32_t csum = 0;

    for (size_t i = length; i >= 2; i -= 2) {
        csum += *p++;
    }

    csum = (csum & 0xFFFF) + (csum >> 16);
    if (csum > UINT16_MAX) {
        csum += 1;
    }

    be_uint16_t ret;
    ret = ~csum;
    return ret;
}

struct net_adapter *net_findadapterbyip(struct net_inetaddr addr) {
    VECTOR_FOR_EACH(&net_adapters, it,
        struct net_adapter *adapter = *it;
        if (adapter->ip.value == addr.value) {
            return adapter;
        }
    );

    return NULL;
}

static bool net_grabcache(struct net_adapter *adapter, struct net_inetaddr ip, struct net_macaddr *mac) {
    spinlock_acquire(&adapter->addrcachelock);
    VECTOR_FOR_EACH(&adapter->addrcache, it,
        struct net_inethwpair *pair = *it;
        if (pair->inet.value == ip.value) {
            *mac = pair->hw;
            spinlock_release(&adapter->addrcachelock);
            return true; // found us a cached record
        }
    );
    spinlock_release(&adapter->addrcachelock);
    return false;
}

static bool net_findcache(struct net_adapter *adapter, struct net_inetaddr ip) {
    spinlock_acquire(&adapter->addrcachelock);
    VECTOR_FOR_EACH(&adapter->addrcache, it,
        struct net_inethwpair *pair = *it;
        if (pair->inet.value == ip.value) {
            spinlock_release(&adapter->addrcachelock);
            return true; // found us a cached record
        }
    );
    spinlock_release(&adapter->addrcachelock);
    return false;
}

uint16_t net_allocport(void) {
    spinlock_acquire(&net_portbitmaplock);
    for (size_t i = 0; i < NET_PORTRANGEEND - NET_PORTRANGESTART; i++) {
        if (net_portbitmap[i] == 0xff) {
            continue;
        }

        for (size_t bit = 0; bit < 8; bit++) {
            if (bitmap_test(net_portbitmap, bit + (i * 8)) == false) {
                bitmap_set(net_portbitmap, bit + (i * 8));
                spinlock_release(&net_portbitmaplock);
                return bit + (i * 8) + NET_PORTRANGESTART;
            }
        }
    }
    spinlock_release(&net_portbitmaplock);

    debug_print(0, "net: Could not allocate port\n");
    return 0;
}

void net_releaseport(uint16_t port) {
    bitmap_reset(net_portbitmap, port);
}

void net_bindsocket(struct net_adapter *adapter, struct socket *sock) {
    spinlock_acquire(&adapter->socklock);

    ((struct inetsocket *)sock)->adapter = adapter;
    VECTOR_PUSH_BACK(&adapter->boundsocks, sock);

    spinlock_release(&adapter->socklock);
}

void net_unbindsocket(struct net_adapter *adapter, struct socket *sock) {
    spinlock_acquire(&adapter->socklock);

    VECTOR_REMOVE_BY_VALUE(&adapter->boundsocks, sock);
    ((struct inetsocket *)sock)->adapter = NULL;

    spinlock_release(&adapter->socklock);
}

void net_unbindall(struct net_adapter *adapter) {
    spinlock_acquire(&adapter->socklock);

    while (adapter->boundsocks.length) {
        struct inetsocket *sock = (struct inetsocket *)VECTOR_ITEM(&adapter->boundsocks, adapter->boundsocks.length - 1);
        sock->adapter = NULL;
        VECTOR_REMOVE(&adapter->boundsocks, adapter->boundsocks.length - 1);
    }

    spinlock_release(&adapter->socklock);
}

static void net_fragment(struct net_adapter *adapter, void *buffer, size_t length) {
    struct net_inetheader *originalinetheader = (struct net_inetheader *)(buffer + NET_LINKLAYERFRAMESIZE(adapter));
    struct net_inetheader *inetheader = originalinetheader;
    size_t mtu = adapter->mtu;

    uint16_t tmp = __builtin_bswap16(inetheader->fragoff);
    uint16_t fragoff = tmp & NET_IPOFFMASK;
    uint16_t mf = tmp & NET_IPFLAGMF;
    uint16_t left = length - sizeof(struct net_inetheader) - NET_LINKLAYERFRAMESIZE(adapter);
    bool last = false;
    uint16_t poff = NET_LINKLAYERFRAMESIZE(adapter) + sizeof(struct net_inetheader);
    uint16_t nfb = (mtu - sizeof(struct net_inetheader) - NET_LINKLAYERFRAMESIZE(adapter)) / 8;

    while (left) {
        last = (left <= mtu - sizeof(struct net_inetheader) - NET_LINKLAYERFRAMESIZE(adapter));

        tmp = mf | (NET_IPOFFMASK & (fragoff));
        if (!last) {
            tmp = tmp | NET_IPFLAGMF;
        }

        uint16_t cop = last ? left : nfb * 8; // if it's the last fragment, simply dump the rest of the data, otherwise dump the mtu size

        uint8_t *buf = alloc(NET_LINKLAYERFRAMESIZE(adapter) + sizeof(struct net_inetheader) + cop);
        memcpy(buf, buffer, NET_LINKLAYERFRAMESIZE(adapter)); // copy link layer
        memcpy(buf + NET_LINKLAYERFRAMESIZE(adapter), inetheader, sizeof(struct net_inetheader)); // copy ip header to next part in buffer
        inetheader = (struct net_inetheader *)(buf + NET_LINKLAYERFRAMESIZE(adapter));
        memcpy(inetheader->data, buffer + poff, cop);
        poff += cop; // since we're only using poff to figure out what point in the input data buffer we'll grab this data, it's preferrable to do this

        inetheader->fragoff = __builtin_bswap16(tmp);
        inetheader->len = __builtin_bswap16(cop + sizeof(struct net_inetheader));
        inetheader->csum = 0;
        inetheader->csum = net_checksum(inetheader, sizeof(struct net_inetheader));

        adapter->txpacket(adapter, buf, NET_LINKLAYERFRAMESIZE(adapter) + sizeof(struct net_inetheader) + cop);
        free(buf);

        left -= cop;
        fragoff += nfb;
    }

}

ssize_t net_sendinet(struct net_adapter *adapter, struct net_inetaddr src, struct net_inetaddr dest, uint8_t protocol, void *data, size_t length) {
    uint8_t *buffer = alloc(NET_LINKLAYERFRAMESIZE(adapter) + sizeof(struct net_inetheader) + length); // since ICMP is variable size
    struct net_inetheader *ipheader = NULL;
    if (adapter->type & NET_ADAPTERETH) {
        struct net_etherframe *frame = (struct net_etherframe *)buffer;
        frame->type = __builtin_bswap16(NET_ETHPROTOIPV4); // IPv4
        frame->src = adapter->mac;

        if (dest.value == INADDR_BROADCAST) {
            frame->dest = NET_MACSTRUCT(0xff, 0xff, 0xff, 0xff, 0xff, 0xff);
        } else {
            ssize_t status = net_route(&adapter, src, dest, &frame->dest);
            if (status != 0) {
                free(buffer);
                return status;
            }
        }
        ipheader = (struct net_inetheader *)frame->data;
    }

    ipheader->ihl = 5;
    ipheader->version = 4; // IPv4
    ipheader->len = __builtin_bswap16(sizeof(struct net_inetheader) + length);
    ipheader->fragoff = 0;
    ipheader->ttl = 64;
    ipheader->id = __builtin_bswap16(adapter->ipframe++);
    ipheader->protocol = protocol;
    ipheader->csum = 0;
    ipheader->dest = dest; // reply destination
    ipheader->src = adapter->ip;

    ipheader->csum = net_checksum(ipheader, sizeof(struct net_inetheader));

    memcpy(ipheader->data, data, length);

    if (net_findadapterbyip(dest) == NULL) { // not an interface we have
        if (adapter->mtu && (NET_LINKLAYERFRAMESIZE(adapter) + sizeof(struct net_inetheader) + length > adapter->mtu)) {
            net_fragment(adapter, buffer, NET_LINKLAYERFRAMESIZE(adapter) + sizeof(struct net_inetheader) + length);
            free(buffer);
            return 0;
        }

        adapter->txpacket(adapter, buffer, NET_LINKLAYERFRAMESIZE(adapter) + sizeof(struct net_inetheader) + length);
        free(buffer);
    } else { // if it's an interface we have access to, just feed the packet right into it as if it were a loopback device (we also do not have to care about MTU)
        struct net_adapter *a = net_findadapterbyip(dest);
        struct net_packet *packet = alloc(sizeof(struct net_packet));
        packet->len = NET_LINKLAYERFRAMESIZE(adapter) + sizeof(struct net_inetheader) + length;
        packet->data = alloc(packet->len);
        memcpy(packet->data, buffer, packet->len);
        free(buffer);

        // feed right back into this adapter
        spinlock_acquire(&a->cachelock);
        VECTOR_PUSH_BACK(&a->cache, packet);
        spinlock_release(&a->cachelock);
        event_trigger(&a->packetevent, false);
    }
    return 0;
}

ssize_t net_lookup(struct net_adapter *adapter, struct net_inetaddr ip, struct net_macaddr *mac) {
    struct net_adapter *a = NULL;
    if ((a = net_findadapterbyip(ip))) { // check if it's an address already assigned to one of our adapters, in which case we can skip this step entirely
        *mac = a->mac;
        return 0;
    }

    if (net_grabcache(adapter, ip, mac)) { // already have an ARP cache for this
        return 0;
    }

    uint8_t *buffer = alloc(NET_LINKLAYERFRAMESIZE(adapter) + sizeof(struct net_arpheader));
    struct net_arpheader *arp = NULL;
    if (adapter->type & NET_ADAPTERETH) {
        struct net_etherframe *frame = (struct net_etherframe *)buffer;
        frame->type = __builtin_bswap16(NET_ETHPROTOARP);
        frame->src = adapter->mac;
        frame->dest = NET_MACSTRUCT(0xff, 0xff, 0xff, 0xff, 0xff, 0xff); // broadcast request

        arp = (struct net_arpheader *)frame->data;
        arp->desthw = frame->dest;
        arp->srchw = frame->src;
    }

    arp->hwlen = 6;
    arp->hwtype = __builtin_bswap16(adapter->type);

    arp->destpr = ip;
    arp->srcpr = adapter->ip;
    arp->plen = 4;
    arp->prtype = __builtin_bswap16(NET_ETHPROTOIPV4); // IPv4

    arp->opcode = __builtin_bswap16(1); // request

    adapter->txpacket(adapter, buffer, NET_LINKLAYERFRAMESIZE(adapter) + sizeof(struct net_arpheader));
    free(buffer);
    if (net_findcache(adapter, ip)) {
        goto skipcase; // in case we (impossibly) manage to get an instant response handle it
    }

    // XXX: Think of a better timeout delay
    size_t timeout = 500;
    while (timeout > 0) {
        if (net_findcache(adapter, ip)) {
            break;
        }
        timeout--;
        time_nsleep(10 * 1000000);
    }

    if (!net_findcache(adapter, ip)) {
        debug_print(0, "net: Timeout on ARP response\n");
        errno = ENETUNREACH;
        return -1;
    }
skipcase:
    ASSERT_MSG(net_grabcache(adapter, ip, mac), "net: ARP reply broken");
    return 0;
}

ssize_t net_route(struct net_adapter **adapter, struct net_inetaddr local, struct net_inetaddr remote, struct net_macaddr *mac) {
    bool islocal = false;

    if (*adapter) {
        if (local.value != INADDR_ANY && local.value != (*adapter)->ip.value) { // INADDR_ANY will reference all interfaces, but if it's not that we have to be aligned with what the adapter is assigned with
            errno = ENETUNREACH;
            return -1;
        }

        if ((remote.value & (*adapter)->subnetmask.value) == ((*adapter)->ip.value & (*adapter)->subnetmask.value)) {
            islocal = true; // this is us!!!
        }
    } else {
        VECTOR_FOR_EACH(&net_adapters, it,
            struct net_adapter *itadapter = *it;

            if (local.value != INADDR_ANY && itadapter->ip.value != local.value) {
                continue; // is not us
            }

            if ((remote.value & itadapter->subnetmask.value) == (itadapter->ip.value & itadapter->subnetmask.value)) {
                islocal = true; // this is us!!!
                *adapter = itadapter; // set the adapter (to receive based on route)
            } else if (!islocal && itadapter->gateway.value != 0) {
                *adapter = itadapter;
            }
        );
    }

    if (!(*adapter)) {
        errno = ENETUNREACH;
        return -1;
    }

    if (islocal) {
        int status = net_lookup(*adapter, remote, mac);
        if (status != 0) {
            return status;
        }
    } else {
        int status = net_lookup(*adapter, (*adapter)->gateway, mac); // since we set the MAC to the gateway all packets will be directed there, but on the IPv4 layer they will be forwarded by the router
        if (status != 0) {
            return status;
        }
    }

    return 0;
}

static void net_onicmp(struct net_adapter *adapter, struct net_inetheader *inetheader, size_t length) {
    if (length < sizeof(struct net_icmpheader)) {
        debug_print(0, "net: Discarded [too] short ICMP packet (len: %d)\n", length);
        return;
    }

    struct net_icmpheader *header = (struct net_icmpheader *)inetheader->data;
    // XXX: Checksum validation
    if (header->type == 8) { // echo request
        struct net_icmpheader *icmpreply = alloc(length);
        icmpreply->type = 0; // echo reply
        icmpreply->code = 0;
        icmpreply->csum = 0;
        memcpy(icmpreply->data, header->data, length - sizeof(struct net_icmpheader));
        icmpreply->csum = net_checksum(icmpreply, length);

        net_sendinet(adapter, adapter->ip, inetheader->src, IPPROTO_ICMP, icmpreply, length);
        free(icmpreply);
    }
}


struct net_reassmetadata {
    // used to identify specifically against a fragment
    uint8_t timer; // timer until reassembly failure
    void *data; // data thus far (received data we've gotten)
    uint16_t len; // length of data
    struct net_inetheader header;
};

static VECTOR_TYPE(struct net_reassmetadata *) net_reassemblemeta = VECTOR_INIT;

static struct net_inetheader *net_reassemble(struct net_inetheader *header) {
    if (header->ihl * 4 > sizeof(struct net_inetheader)) { // unimplemented for options
        return NULL;
    }

    struct net_reassmetadata *metadata = NULL;
    VECTOR_FOR_EACH(&net_reassemblemeta, it,
        struct net_reassmetadata *m = *it;
        if (m->header.id == header->id &&
            m->header.src.value == header->src.value &&
            m->header.dest.value == header->dest.value) {
            metadata = m;
        }
    );

    if (metadata == NULL) { // we have not already started working on this fragment
        if ((__builtin_bswap16(header->fragoff) & NET_IPOFFMASK) * 8 != 0) { // ensure this is the first fragment
            return NULL; // do not accept any new fragments that are not the initial fragment for this packet
        }
        metadata = alloc(sizeof(struct net_reassmetadata));
        metadata->data = alloc(__builtin_bswap16(header->len)); // allocate initial data
        memcpy(&metadata->header, header, sizeof(struct net_inetheader));
        metadata->timer = 3; // maximum age
        VECTOR_PUSH_BACK(&net_reassemblemeta, metadata);
    }

    uint16_t fragoff = (__builtin_bswap16(header->fragoff) & NET_IPOFFMASK) * 8;
    uint16_t len = __builtin_bswap16(header->len) - sizeof(struct net_inetheader);
    metadata->len += len;

    metadata->data = realloc(metadata->data, metadata->len);
    memcpy(metadata->data + fragoff, header->data, len);

    if ((header->fragoff & __builtin_bswap16(NET_IPFLAGMF)) == 0) { // this is the final fragment
        struct net_inetheader *fraghdr = alloc(sizeof(struct net_inetheader) + metadata->len);
        memcpy(fraghdr, &metadata->header, sizeof(struct net_inetheader));
        fraghdr->fragoff = 0; // clear flags on header
        fraghdr->len = __builtin_bswap16(sizeof(struct net_inetheader) + metadata->len);
        fraghdr->csum = 0;
        fraghdr->csum = net_checksum(fraghdr, sizeof(struct net_inetheader));
        memcpy(fraghdr->data, metadata->data, metadata->len);
        VECTOR_REMOVE_BY_VALUE(&net_reassemblemeta, metadata);
        free(metadata->data);
        free(metadata);
        return fraghdr;
    }

    return NULL; // not ready yet
}

static void net_fraghandler(void *unused) {
    (void)unused;

    for (;;) {
        time_nsleep(1000 * 1000000); // sleep 1000ms

        VECTOR_FOR_EACH(&net_reassemblemeta, it,
            struct net_reassmetadata *meta = *it;

            meta->timer--;
            if (meta->timer == 0) {
                VECTOR_REMOVE_BY_VALUE(&net_reassemblemeta, meta);
                debug_print(0, "net: Timed out on fragment reassembly (packet id: %d)\n", __builtin_bswap16(meta->header.id));
                free(meta->data);
                free(meta);
            }
        );
    }
}

static void net_oninet(struct net_adapter *adapter, void *data, size_t length) {
    if (length < sizeof(struct net_inetheader)) {
        debug_print(0, "net: Discarded [too] short IPv4 packet (len: %d)\n", length);
        return;
    }

    struct net_inetheader *header = (struct net_inetheader *)data;

    if (header->version != 4) {
        debug_print(0, "net: Invalid version on IPv4 packet (ver: %d)\n", header->version);
        return;
    }

    be_uint16_t csum = header->csum;

    header->csum = 0; // exclude checksum from calculation
    if (csum != net_checksum(data, sizeof(struct net_inetheader))) {
        debug_print(0, "net: Invalid checksum on IPv4 packet\n");
        return;
    }

    if (header->fragoff & __builtin_bswap16(NET_IPFLAGMF | NET_IPOFFMASK)) { // if a packet has the more fragements flag set and/or a fragment offset, assume it's part of a fragmented packet
        header = net_reassemble(header);
        if (header == NULL) {
            return; // reassembly failed/not complete, drop packet from here (reassembly works by grouping the packets as we get them, combining them together until they form a full packet)
        }
    }

    length = __builtin_bswap16(header->len); // update length to reflect actual length of this packet (some hardware drivers will not reflect the true length of the packet)

    switch (header->protocol) {
        case IPPROTO_ICMP: // ICMP
            net_onicmp(adapter, header, length - sizeof(struct net_inetheader));
            break;
        case IPPROTO_TCP: // TCP
            tcp_ontcp(adapter, header, length - sizeof(struct net_inetheader));
            break;
        case IPPROTO_UDP: // UDP
            udp_onudp(adapter, header, length - sizeof(struct net_inetheader));
            break;
        default:
            struct net_icmpheader *icmpreply = alloc(sizeof(struct net_icmpheader) + sizeof(struct net_inetheader) + __builtin_bswap16(header->len));
            icmpreply->type = 3; // unreachable
            icmpreply->code = 2; // protocol
            icmpreply->csum = 0;
            memcpy(icmpreply->data, header, sizeof(struct net_inetheader));
            memcpy(icmpreply->data + sizeof(struct net_inetheader), header->data, __builtin_bswap16(header->len));
            icmpreply->csum = net_checksum(icmpreply, sizeof(struct net_icmpheader) + sizeof(struct net_inetheader) + __builtin_bswap16(header->len));

            net_sendinet(adapter, adapter->ip, header->src, IPPROTO_ICMP, icmpreply, length);
            free(icmpreply);
            break;
    }
}

static void net_onarp(struct net_adapter *adapter, void *data, size_t length) {
    if (length < sizeof(struct net_arpheader)) {
        debug_print(0, "net: Discarded [too] short ARP packet (len: %d)\n", length);
        return;
    }

    struct net_arpheader *header = (struct net_arpheader *)data;

    if (__builtin_bswap16(header->opcode) == 0x01) { // request
        struct net_adapter *arpadapter = net_findadapterbyip(header->destpr);

        if (arpadapter) {
            uint8_t *buffer = alloc(NET_LINKLAYERFRAMESIZE(adapter) + sizeof(struct net_arpheader));
            struct net_arpheader *reply = NULL;
            if (adapter->type & NET_ADAPTERETH) {
                struct net_etherframe *ethframe = (struct net_etherframe *)buffer;
                ethframe->type = __builtin_bswap16(NET_ETHPROTOARP); // ARP
                ethframe->src = arpadapter->mac;
                ethframe->dest = header->srchw;

                reply = (struct net_arpheader *)ethframe->data;
            }

            reply->desthw = header->srchw;
            reply->srchw = arpadapter->mac;
            reply->hwlen = 6;
            reply->hwtype = __builtin_bswap16(arpadapter->type);

            reply->destpr = header->srcpr;
            reply->srcpr = arpadapter->ip;
            reply->plen = 4;
            reply->prtype = __builtin_bswap16(NET_ETHPROTOIPV4);

            reply->opcode = __builtin_bswap16(2); // reply

            arpadapter->txpacket(arpadapter, buffer, NET_LINKLAYERFRAMESIZE(adapter) + sizeof(struct net_arpheader)); // reply should come from the adapter in question rather than the one that picked up the packet
            free(buffer);
        }
    }

    struct net_inethwpair *pair = alloc(sizeof(struct net_inethwpair));
    pair->inet = header->srcpr;
    pair->hw = header->srchw;
    spinlock_acquire(&adapter->addrcachelock);
    VECTOR_PUSH_BACK(&adapter->addrcache, pair); // cache the ip-hw correlation from this ARP packet for later use (reduces the need for ip lookups)
    spinlock_release(&adapter->addrcachelock);
}

static noreturn void net_ifhandler(struct net_adapter *adapter) {
    debug_print(0, "net: Interface thread initialised on %s\n", adapter->ifname);

    for (;;) {
        while (adapter->cache.length == 0) {
            struct event *events[] = { &adapter->packetevent };
            event_await(events, 1, true);
        }

        spinlock_acquire(&adapter->cachelock);
        struct net_packet *packet = VECTOR_ITEM(&adapter->cache, 0); // grab latest

        struct net_etherframe *ethframe = NULL; // ethernet frame (in case of ethernet)
        if (adapter->type & NET_ADAPTERETH) { // ethernet
            ethframe = (struct net_etherframe *)packet->data;
        }
        VECTOR_REMOVE(&adapter->cache, 0); // remove from cache to give us more space
        spinlock_release(&adapter->cachelock);

        if (adapter->type & NET_ADAPTERETH) { // ethernet
            switch (__builtin_bswap16(ethframe->type)) {
                case NET_ETHPROTOIPV4: // IPv4
                    net_oninet(adapter, ethframe->data, packet->len - sizeof(struct net_etherframe));
                    free(packet);
                    free(packet->data);
                    break;
                case NET_ETHPROTOARP: // ARP
                    net_onarp(adapter, ethframe->data, packet->len - sizeof(struct net_etherframe));
                    free(packet);
                    free(packet->data);
                    break;
                default:
                    free(packet);
                    free(packet->data);
                    break;
            }
        }
    }
}

// Direct ioctl() on adapter (from something like the device or via the socket)
int net_ifioctl(struct resource *_this, struct f_description *description, uint64_t request, uint64_t arg) {
    struct ifreq *req = (struct ifreq *)arg;
    if (req->ifr_ifru.ifru_ivalue) {
        if (request == SIOCGIFNAME) { // this one relies on the index instead of the name
            struct net_adapter *this = NULL; // adapter in question
            VECTOR_FOR_EACH(&net_adapters, it,
                struct net_adapter *a = *it;
                if (a->index == req->ifr_ifru.ifru_ivalue) {
                    this = a;
                }
            );

            if (this == NULL) {
                // XXX: Should there be an errno?
                errno = ENODEV;
                return -1;
            }

            strncpy(req->ifr_ifrn.ifrn_name, this->ifname, IFNAMSIZ);
            return 0;
        }
    }

    struct net_adapter *this = NULL; // adapter in question
    switch (request) {
        default: {
            VECTOR_FOR_EACH(&net_adapters, it,
                struct net_adapter *a = *it;
                if (!strncmp(a->ifname, req->ifr_ifrn.ifrn_name, IFNAMSIZ)) {
                    this = a;
                }
            );

            if (this == NULL) {
                VECTOR_FOR_EACH(&net_adapters, it, // if there is no matching interface name, try match based on interface index
                    struct net_adapter *a = *it;
                    if (a->index == req->ifr_ifru.ifru_ivalue) {
                        this = a;
                    }
                );

                if (this == NULL) {
                    // XXX: Should there be an errno?
                    errno = ENODEV;
                    return -1;
                }
            }

        }
    }

    switch (request) {
        case SIOCGIFGATEWAY: { // XXX: Make a gateway ioctl
            struct sockaddr_in *inaddr = (struct sockaddr_in *)&req->ifr_addr;
            inaddr->sin_family = AF_INET;
            inaddr->sin_addr.s_addr = this->gateway.value;
            return 0;
        }
        case SIOCSIFGATEWAY: { // XXX: Make a gateway ioctl
            struct sockaddr_in *inaddr = (struct sockaddr_in *)&req->ifr_addr;
            if (inaddr->sin_family != AF_INET) {
                errno = EPROTONOSUPPORT;
                return -1;
            }

            this->gateway.value = inaddr->sin_addr.s_addr;

            return 0;
        }
        case SIOCGIFFLAGS: {
            req->ifr_ifru.ifru_flags = this->flags;
            return 0;
        }
        case SIOCSIFFLAGS: {
            uint16_t old = this->flags;
            this->flags = req->ifr_ifru.ifru_flags;
            this->updateflags(this, old); // update for flags
            return 0;
        }
        case SIOCSIFNAME: {
            char devpath[32];
            snprintf(devpath, 32, "/dev/%s", this->ifname);
            vfs_unlink(vfs_root, devpath); // unlink original
            strncpy(this->ifname, req->ifr_newname, IFNAMSIZ);
            devtmpfs_add_device((struct resource *)this, this->ifname); // add new one
            return 0;
        }
        case SIOCGIFMTU: {
            req->ifr_ifru.ifru_mtu = this->mtu;
            return 0;
        }
        case SIOCSIFMTU: {
            if (this->hwmtu) {
                if (this->mtu > this->hwmtu) {
                    errno = EINVAL;
                    return -1; // MTU is over the maximum the hardware can handle
                }

                if (req->ifr_ifru.ifru_mtu) {
                    this->mtu = req->ifr_ifru.ifru_mtu; // will only set to the requested MTU if it's not zero
                } else {
                    errno = EINVAL;
                    return -1;
                }
            } else {
                this->mtu = req->ifr_ifru.ifru_mtu; // will set to whatever the dynamic MTU adapter can handle
            }
            return 0;
        }
        case SIOCGIFADDR: {
            struct sockaddr_in *inaddr = (struct sockaddr_in *)&req->ifr_addr;
            inaddr->sin_family = AF_INET;
            inaddr->sin_addr.s_addr = this->ip.value;
            return 0;
        }
        case SIOCSIFADDR: {
            struct sockaddr_in *inaddr = (struct sockaddr_in *)&req->ifr_addr;
            if (inaddr->sin_family != AF_INET) {
                errno = EPROTONOSUPPORT;
                return -1;
            }

            this->ip.value = inaddr->sin_addr.s_addr;
            return 0;
        }
        case SIOCGIFNETMASK: {
            struct sockaddr_in *inaddr = (struct sockaddr_in *)&req->ifr_netmask;
            inaddr->sin_family = AF_INET;
            inaddr->sin_addr.s_addr = this->subnetmask.value;
            return 0;
        }
        case SIOCSIFNETMASK: {
            struct sockaddr_in *inaddr = (struct sockaddr_in *)&req->ifr_netmask;

            if (inaddr->sin_family != AF_INET) {
                errno = EPROTONOSUPPORT;
                return -1;
            }

            this->subnetmask.value = inaddr->sin_addr.s_addr;
            return 0;
        }
        case SIOCGIFHWADDR: {
            memcpy(req->ifr_hwaddr.sa_data, this->mac.mac, sizeof(struct net_macaddr));
            return 0;
        }
        case SIOCGIFINDEX: {
            req->ifr_ifru.ifru_ivalue = this->index;
            return 0;
        }
    }

    return resource_default_ioctl(_this, description, request, arg);
}

ssize_t net_getsockopt(struct socket *_this, struct f_description *description, int level, int optname, void *optval, socklen_t *optlen) {
    (void)description;
    (void)level;

    if (_this->family != AF_INET) {
        errno = EINVAL;
        return -1;
    }

    switch (optname) {
        case SO_ACCEPTCONN:
            if (_this->protocol != IPPROTO_TCP) {
                break;
            }
            *((bool *)optval) = _this->state == SOCKET_LISTENING;
            *optlen = sizeof(bool);
            return 0;
        case SO_BINDTODEVICE:
            size_t len = *optlen;
            strncpy(optval, ((struct inetsocket *)_this)->adapter->ifname, len);
            return 0;
        case SO_BROADCAST:
            *((bool *)optval) = ((struct inetsocket *)_this)->canbroadcast;
            return 0;
    }

    errno = EINVAL;
    return -1;
}

ssize_t net_setsockopt(struct socket *_this, struct f_description *description, int level, int optname, const void *optval, socklen_t optlen) {
    (void)description;
    (void)level;

    if (_this->family != AF_INET) {
        errno = EINVAL;
        return -1;
    }

    switch (optname) {
        case SO_BROADCAST: {
            ((struct inetsocket *)_this)->canbroadcast = *((bool *)optval);
            break;
        }
        case SO_BINDTODEVICE: {
            const char *ifname = (const char *)optval;
            VECTOR_FOR_EACH(&net_adapters, it,
                struct net_adapter *adapter = *it;
                if (!strncmp(adapter->ifname, ifname, optlen)) {
                    net_bindsocket(adapter, _this);
                    return 0;
                }
            );
            break;
        }
        case SO_DONTROUTE:
            // XXX: Prevent routing to gateway on IP level
            ((struct inetsocket *)_this)->canroute = *((bool *)optval);
            return 0;
    }
    errno = EINVAL;
    return 0;
}

void net_register(struct net_adapter *adapter) {
    if (adapter->type & NET_ADAPTERLO) {
        adapter->mtu = 0; // loopback interfaces do not care about MTU
        strcpy(adapter->ifname, "lo");
    } else if (adapter->type & NET_ADAPTERETH) {
        adapter->mtu = 1500; // this can be changed!
        snprintf(adapter->ifname, IFNAMSIZ, "eth%d", net_ethcount++);
    }

    VECTOR_PUSH_BACK(&net_adapters, adapter);
    adapter->index = net_adapters.length;

    adapter->addrcache = (typeof(adapter->addrcache))VECTOR_INIT;
    adapter->cache = (typeof(adapter->cache))VECTOR_INIT;
    adapter->cachelock = (spinlock_t)SPINLOCK_INIT;
    adapter->addrcachelock = (spinlock_t)SPINLOCK_INIT;

    sched_new_kernel_thread(net_ifhandler, adapter, true);
}

void net_init(void) {
    net_portbitmap = alloc(NET_PORTRANGEEND - NET_PORTRANGESTART);
    loopback_init();
    sched_new_kernel_thread(net_fraghandler, NULL, true);
}
