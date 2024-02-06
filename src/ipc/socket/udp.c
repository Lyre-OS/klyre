#include <poll.h>
#include <dev/net/net.k.h>
#include <lib/errno.k.h>
#include <lib/print.k.h>
#include <lyre/sockios.h>
#include <ipc/socket.k.h>
#include <netinet/in.h>
// #include <sys/ioctl.h>
#include <time/time.k.h>

// XXX: Poll events may not be correct

struct udp_header {
    be_uint16_t srcport;
    be_uint16_t destport;
    be_uint16_t length;
    be_uint16_t csum;
    uint8_t data[];
} __attribute__((packed));

struct udp_packet {
    struct net_inetaddr srcip;
    be_uint16_t srcport;
    size_t len;
    uint8_t *data;
};

struct udp_socket {
    struct inetsocket;

    uint32_t recenttimestamp;
    VECTOR_TYPE(struct udp_packet *) packets; // udp packets
};

// should we validate checksums on UDP?
// #define UDP_DOCSUM

static spinlock_t udp_socketslock = SPINLOCK_INIT;
static VECTOR_TYPE(struct udp_socket *) udp_sockets; // keep a reference of all UDP sockets

static bool udp_grabsocket(be_uint16_t port, struct udp_socket **socket) {
    (void)socket; // we are going to use it though

    spinlock_acquire(&udp_socketslock);
    VECTOR_FOR_EACH(&udp_sockets, it,
        struct udp_socket *itsocket = *it;
        if (itsocket->port == port) {
            *socket = itsocket;
            spinlock_release(&udp_socketslock);
            return true;
        }
    );
    spinlock_release(&udp_socketslock);
    return false;
}

static bool udp_acquireport(struct udp_socket *sock, uint16_t port) {
    if (!port) {
        errno = EINVAL; // invalid port
        return false;
    }

    spinlock_acquire(&udp_socketslock);
    VECTOR_PUSH_BACK(&udp_sockets, sock);
    spinlock_release(&udp_socketslock);
    return true;
}

static ssize_t udp_read(struct resource *_this, struct f_description *description, void *buf, off_t offset, size_t count) {
    (void)offset;

    struct udp_socket *this = (struct udp_socket *)_this;

    ssize_t ret = -1;

    spinlock_acquire(&this->lock);
    if (this->packets.length <= 0) {
        spinlock_release(&this->lock);
        if ((description->flags & O_NONBLOCK) != 0) {
            errno = EWOULDBLOCK;
            goto cleanup;
        }
reawait:
        struct event *events[] = { &this->event };
        event_await(events, 1, true);
        if (this->packets.length <= 0) {
            goto reawait;
        }
    }

    spinlock_acquire(&this->lock);

    struct udp_packet *packet = VECTOR_ITEM(&this->packets, 0);

    VECTOR_REMOVE(&this->packets, 0);

    if (packet->len < count) {
        count = packet->len;
    }

    memcpy(buf, packet->data, count);

    if (this->packets.length == 0) {
        this->status &= ~POLLIN;
    }
    ret = count;

cleanup:
    spinlock_release(&this->lock);
    return ret;
}

static ssize_t udp_sendpacket(struct net_adapter *adapter, struct net_inetaddr src, struct net_inetaddr dest, be_uint16_t srcport, be_uint16_t destport, const void *buf, size_t len) {

    uint8_t *buffer = alloc(sizeof(struct udp_header) + len);

    struct udp_header *header = (struct udp_header *)buffer;

    header->destport = destport;
    header->srcport = srcport;
    header->length = __builtin_bswap16(sizeof(struct udp_header) + len);
    header->csum = 0; // XXX: Checksum
    memcpy(header->data, buf, len);

    ssize_t ret = net_sendinet(adapter, src, dest, IPPROTO_UDP, header, __builtin_bswap16(header->length));
    free(header);
    return ret;
}

static ssize_t udp_write(struct resource *_this, struct f_description *description, const void *buf, off_t offset, size_t count) {
    (void)description;
    (void)offset;

    struct udp_socket *this = (struct udp_socket *)_this;

    ssize_t ret = -1;

    if (this->state != SOCKET_CONNECTED) { // raw socket writes require a peer connection
        errno = ENOTCONN;
        goto cleanup;
    }

    struct sockaddr_in *addr = (struct sockaddr_in *)&this->peeraddr;

    if (addr->sin_family != AF_INET) {
        errno = EINVAL;
        goto cleanup;
    }

    if (addr->sin_addr.s_addr == INADDR_BROADCAST && !this->canbroadcast) {
        errno = EOPNOTSUPP;
        goto cleanup;
    }

    struct net_inetaddr destaddr;
    destaddr.value = addr->sin_addr.s_addr;
    be_uint16_t destport;
    destport = addr->sin_port;

    if (!this->port) {
        this->port = __builtin_bswap16(net_allocport());

        if (!this->port) {
            errno = EINTR;
            goto cleanup;
        }
        if (!udp_acquireport(this, __builtin_bswap16(this->port))) {
            errno = EADDRINUSE;
            goto cleanup;
        }
    }

    // XXX: Check for ICMP packet from remote (Destination Unreachable, etc.)
    if (!this->adapter) {
        struct net_macaddr mac;
        struct net_adapter *adapter = NULL;
        struct net_inetaddr thisaddr = (struct net_inetaddr) { .value = INADDR_ANY };

        // grab an adapter for this
        int status = net_route(&adapter, thisaddr, destaddr, &mac);
        if (status != 0) {
            return status;
        }

        status = udp_sendpacket(adapter, thisaddr, destaddr, this->port, destport, buf, count);
        if (status != 0) {
            return status;
        }
    } else {
        struct net_inetaddr thisaddr;
        thisaddr.value = ((struct sockaddr_in *)&this->localaddr)->sin_addr.s_addr;
        int status = udp_sendpacket(this->adapter, thisaddr, destaddr, this->port, destport, buf, count);
        if (status != 0) {
            return status;
        }
    }

    this->status |= POLLOUT;
cleanup:
    return ret;
}

static ssize_t udp_sendmsg(struct socket *_this, struct f_description *description, const struct msghdr *msg, int flags) {
    (void)description;
    (void)flags;

    struct udp_socket *this = (struct udp_socket *)_this;

    ssize_t ret = -1;

    spinlock_acquire(&this->lock);

    if (this->state != SOCKET_CONNECTED && msg->msg_name == NULL) {
        errno = EDESTADDRREQ;
        goto cleanup;
    }

    struct sockaddr_in *addr = this->state == SOCKET_CONNECTED ? ((struct sockaddr_in *)&this->peeraddr) : (struct sockaddr_in *)msg->msg_name;

    if (addr->sin_family != AF_INET) {
        errno = EINVAL;
        goto cleanup;
    }

    if (addr->sin_addr.s_addr == INADDR_BROADCAST && !this->canbroadcast) {
        errno = EOPNOTSUPP;
        goto cleanup;
    }

    size_t count = 0;
    for (size_t i = 0; i < msg->msg_iovlen; i++) {
        count += msg->msg_iov[i].iov_len;
    }

    uint8_t *buf = alloc(count);

    size_t transferred = 0;
    size_t remaining = count;
    for (size_t i = 0; i < msg->msg_iovlen; i++) {
        size_t transfercount = MIN(msg->msg_iov[i].iov_len, remaining);
        memcpy(buf + transferred, msg->msg_iov[i].iov_base, transfercount);
        transferred += transfercount;
        remaining -= transfercount;
    }

    struct net_inetaddr destaddr;
    destaddr.value = addr->sin_addr.s_addr;
    be_uint16_t destport;
    destport = addr->sin_port;

    if (!this->port) {
        this->port = __builtin_bswap16(net_allocport());

        if (!this->port) {
            errno = EINTR;
            goto cleanup;
        }
        if (!udp_acquireport(this, __builtin_bswap16(this->port))) {
            errno = EADDRINUSE;
            goto cleanup;
        }
    }

    // XXX: Check for ICMP packet from remote (Destination Unreachable, etc.)
    if (!this->adapter) {
        struct net_macaddr mac;
        struct net_adapter *adapter = NULL;
        struct net_inetaddr thisaddr = (struct net_inetaddr) { .value = INADDR_ANY };

        // grab an adapter for this
        int status = net_route(&adapter, thisaddr, destaddr, &mac);
        if (status != 0) {
            return status;
        }

        status = udp_sendpacket(adapter, thisaddr, destaddr, this->port, destport, buf, count);
        if (status != 0) {
            return status;
        }
    } else {

        struct net_inetaddr thisaddr;
        thisaddr.value = ((struct sockaddr_in *)&this->localaddr)->sin_addr.s_addr;
        int status = udp_sendpacket(this->adapter, thisaddr, destaddr, this->port, destport, buf, count);
        if (status != 0) {
            return status;
        }
    }

    this->status |= POLLOUT;
    ret = transferred;

cleanup:
    spinlock_release(&this->lock);
    return ret;
}

static bool udp_connect(struct socket *_this, struct f_description *description, void *_addr, socklen_t len) {
    (void)description;
    (void)len;

    struct udp_socket *this = (struct udp_socket *)_this;

    struct sockaddr_in *addr = (struct sockaddr_in *)_addr;

    spinlock_acquire(&this->lock);
    // mark address and socket state
    memcpy(&this->peeraddr, addr, sizeof(struct sockaddr_in));

    this->state = SOCKET_CONNECTED;
    this->status |= POLLIN | POLLOUT;
    spinlock_release(&this->lock);

    event_trigger(&this->event, false);

    return true;
}

static ssize_t udp_recvmsg(struct socket *_this, struct f_description *description, struct msghdr *msg, int flags) {
    struct udp_socket *this = (struct udp_socket *)_this;

    ssize_t ret = -1;

    spinlock_acquire(&this->lock);
    if (this->packets.length <= 0) {
        spinlock_release(&this->lock);
        if (flags & MSG_DONTWAIT) {
            errno = EAGAIN;
            return ret;
        } else {
            if ((description->flags & O_NONBLOCK) != 0) {
                errno = EWOULDBLOCK;
                goto cleanup;
            }
reawait:
            struct event *events[] = { &this->event };
            event_await(events, 1, true);
            if (this->packets.length <= 0) {
                goto reawait;
            }
            spinlock_acquire(&this->lock);
        }
    }

    struct udp_packet *packet = VECTOR_ITEM(&this->packets, 0);

    VECTOR_REMOVE(&this->packets, 0);

    size_t count = 0;
    for (size_t i = 0; i < msg->msg_iovlen; i++) {
        count += msg->msg_iov[i].iov_len;
    }

    if (packet->len < count) {
        count = packet->len;
    }

    size_t transferred = 0;
    size_t remaining = count;
    for (size_t i = 0; i < msg->msg_iovlen; i++) {
        size_t transfercount = MIN(msg->msg_iov[i].iov_len, remaining);
        memcpy(msg->msg_iov[i].iov_base, packet->data + transferred, transfercount);
        transferred += transfercount;
        remaining -= transfercount;
    }

    if (msg->msg_name != NULL) {
        socklen_t actual_size = msg->msg_namelen;
        actual_size = sizeof(struct sockaddr_in); // implicit truncate

        struct sockaddr_in addr = (struct sockaddr_in) { .sin_family = AF_INET, .sin_port = packet->srcport, .sin_addr.s_addr = packet->srcip.value };
        memcpy(msg->msg_name, &addr, actual_size);
        msg->msg_namelen = actual_size;
    }

    if (this->packets.length == 0) {
        this->status &= ~POLLIN;
    }
    ret = transferred;

cleanup:
    spinlock_release(&this->lock);
    return ret;
}

static bool udp_bind(struct socket *_this, struct f_description *description, void *addr_, socklen_t len) {
    (void)description;
    (void)len;

    struct udp_socket *this = (struct udp_socket *)_this;

    struct sockaddr_in *addr = (struct sockaddr_in *)addr_;
    if (addr->sin_family != AF_INET) {
        errno = EINVAL;
        return false;
    }

    struct net_adapter *a = NULL;
    if (addr->sin_addr.s_addr == INADDR_ANY) { // on all interfaces
        if (this->adapter) {
            net_unbindsocket(this->adapter, (struct socket *)this);
        }

        this->adapter = NULL; // since we want all interfaces
    } else if ((a = net_findadapterbyip((struct net_inetaddr) { .value = addr->sin_addr.s_addr }))) {
        net_bindsocket(a, (struct socket *)this);
    } else {
        errno = -EADDRNOTAVAIL;
        return false;
    }

    memcpy(&this->localaddr, addr, sizeof(struct sockaddr_in));

    this->port = addr->sin_port; // assign listening port
    if (!this->port) { // 0 port (bind to any port)
        this->port = __builtin_bswap16(net_allocport());
    }

    bool ret = udp_acquireport(this, __builtin_bswap16(this->port)); // final state success depends on the ability to attain access to this port
    if (!ret) {
        if (this->adapter) {
            net_unbindsocket(this->adapter, (struct socket *)this);
        }
    } else {
        this->bound = true;
    }
    return ret;
}

static void udp_netpacket(struct udp_socket *_this, struct net_inetaddr src, be_uint16_t srcport, void *buf, size_t length) {
    struct udp_packet *packet = alloc(sizeof(struct udp_packet));
    packet->srcip = src;
    packet->srcport = srcport;
    packet->len = length;
    packet->data = alloc(length);
    memcpy(packet->data, buf, length);

    spinlock_acquire(&_this->lock);
    VECTOR_PUSH_BACK(&_this->packets, packet);
    spinlock_release(&_this->lock);

    _this->status |= POLLIN;
    event_trigger(&_this->event, false);

    _this->recenttimestamp = time_monotonic.tv_sec;
}

void udp_onudp(struct net_adapter *adapter, struct net_inetheader *inetheader, size_t length) {
    (void)adapter;

    if (length < sizeof(struct udp_header)) {
        debug_print(0, "net: Discarded [too] short UDP packet (len: %d)\n", length);
        return;
    }

    struct udp_header *header = (struct udp_header *)inetheader->data;
    if (__builtin_bswap16(header->length) > length) {
        debug_print(0, "net: Discarded [too] long UDP packet (len: %d (should be %d))\n", length, __builtin_bswap16(header->length));
        return;
    }

    debug_print(0, "net: Received UDP packet from %d.%d.%d.%d:%d to %d.%d.%d.%d:%d\n", NET_PRINTIP(inetheader->src), __builtin_bswap16(header->srcport), NET_PRINTIP(inetheader->dest), __builtin_bswap16(header->destport));

#ifdef UDP_DOCSUM
    // XXX: Checksum validation
#endif

    struct udp_socket *socket = NULL;
    if (udp_grabsocket(header->destport, &socket)) {
        udp_netpacket(socket, inetheader->src, header->srcport, header->data, __builtin_bswap16(header->length));
    } else {
        // reply with ICMP port unreachable
        struct net_icmpheader *icmpreply = alloc(length + sizeof(struct net_inetheader));
        icmpreply->type = 3;
        icmpreply->code = 3;
        icmpreply->csum = 0;
        memcpy(icmpreply->data, &inetheader, sizeof(struct net_inetheader));
        memcpy(icmpreply->data + sizeof(struct net_inetheader), inetheader->data, __builtin_bswap16(inetheader->len));
        icmpreply->csum = net_checksum(icmpreply, sizeof(struct net_icmpheader) + sizeof(struct net_inetheader) + __builtin_bswap16(inetheader->len));
        net_sendinet(adapter, adapter->ip, inetheader->src, IPPROTO_ICMP, icmpreply, length);
        free(icmpreply);
    }
}

static bool udp_unref(struct resource *_this, struct f_description *description) {
    (void)description;

    struct udp_socket *this = (struct udp_socket *)_this;

    this->refcount--;
    if (this->refcount == 0) {
        if (this->adapter) {
            spinlock_acquire(&this->lock);
            net_unbindsocket(this->adapter, (struct socket *)this);
            spinlock_release(&this->lock);
        }

        spinlock_acquire(&udp_socketslock);
        if (VECTOR_FIND(&udp_sockets, this) != VECTOR_INVALID_INDEX) {
            VECTOR_REMOVE_BY_VALUE(&udp_sockets, this);
        }
        spinlock_release(&udp_socketslock);

        net_releaseport(__builtin_bswap16(this->port));
    }

    return true;
}

ssize_t udp_getsockopt(struct socket *_this, struct f_description *description, int level, int optname, void *optval, socklen_t *optlen) {
    switch (level) {
        case SOL_SOCKET:
            net_getsockopt(_this, description, level, optname, optval, optlen);
            break;
        default:
            errno = EINVAL;
            return -1;
    }
    return 0;
}

ssize_t udp_setsockopt(struct socket *_this, struct f_description *description, int level, int optname, const void *optval, socklen_t optlen) {
    switch (level) {
        case SOL_SOCKET:
            net_setsockopt(_this, description, level, optname, optval, optlen);
            break;
        default:
            errno = EINVAL;
            return -1;
    }
    return 0;
}

int udp_sockioctl(struct resource *_this, struct f_description *description, uint64_t request, uint64_t arg) {
    struct udp_socket *this = (struct udp_socket *)_this;

    switch (request) {
        case SIOCINQ:
            if (this->state == SOCKET_LISTENING) {
                errno = EINVAL;
                return -1;
            }

            *((int *)arg) = this->packets.length;
            return 0;
        // case SIOCGSTAMP:
            // struct timeval *val = (struct timeval *)arg;
            // val->tv_sec = this->recenttimestamp;
            // val->tv_usec = 0;
            // return 0;
    }

    return net_ifioctl(_this, description, request, arg);
}

static bool udp_getpeername(struct socket *this, struct f_description *description, void *addr, socklen_t *len) {
    (void)description;

    if (this->state != SOCKET_CONNECTED) {
        errno = ENOTCONN;
        return false;
    }

    size_t actual_len = *len;
    if (actual_len < sizeof(struct sockaddr_in)) {
        actual_len = sizeof(struct sockaddr_in);
    }

    memcpy(addr, &this->peeraddr, actual_len);
    *len = actual_len;
    return true;
}

static bool udp_getsockname(struct socket *this, struct f_description *description, void *addr, socklen_t *len) {
    (void)description;

    if (!this->bound) {
        return true;
    }

    size_t actual_len = *len;
    if (actual_len < sizeof(struct sockaddr_in)) {
        actual_len = sizeof(struct sockaddr_in);
    }

    memcpy(addr, &this->localaddr, actual_len);
    *len = actual_len;
    return true;
}

struct socket *socket_create_udp(int type, int protocol) {
    if (protocol != IPPROTO_UDP) {
        errno = EPROTOTYPE;
        return NULL;
    }

    struct udp_socket *sock = socket_create(AF_INET, type, protocol, sizeof(struct udp_socket));
    if (sock == NULL) {
        goto cleanup;
    }

    sock->status = SOCKET_CREATED;
    sock->family = AF_INET;
    sock->type = type;
    sock->protocol = protocol;
    sock->packets = (typeof(sock->packets))VECTOR_INIT;
    sock->stat.st_mode = S_IFSOCK;

    sock->read = udp_read;
    sock->write = udp_write;
    sock->unref = udp_unref;
    sock->ioctl = net_ifioctl;

    sock->bind = udp_bind;
    sock->connect = udp_connect; // connect to socket (will now become default address)
    sock->getpeername = udp_getpeername;
    sock->getsockname = udp_getsockname;
    sock->recvmsg = udp_recvmsg;
    sock->sendmsg = udp_sendmsg;
    sock->getsockopt = udp_getsockopt;
    sock->setsockopt = udp_setsockopt;

    return (struct socket *)sock;

cleanup:
    if (sock != NULL) {
        resource_free((struct resource *)sock);
    }
    return NULL;
}
