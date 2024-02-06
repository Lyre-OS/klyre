#include <poll.h>
#include <dev/net/net.k.h>
#include <ipc/socket.k.h>
#include <ipc/socket/tcp.k.h>
#include <lib/errno.k.h>
#include <lib/print.k.h>
#include <lib/random.k.h>
#include <lyre/sockios.h>
#include <netinet/in.h>
#include <sched/sched.k.h>
// #include <sys/ioctl.h>
#include <time/time.k.h>

// XXX: Poll events may not be correct

enum {
    TCP_STATECLOSED,
    TCP_STATELISTEN,
    TCP_STATESYNSENT,
    TCP_STATESYNRECV,
    TCP_STATEESTABLISHED,
    TCP_STATECLOSEWAIT,
    TCP_STATEFINWAIT1,
    TCP_STATECLOSING,
    TCP_STATELASTACK,
    TCP_STATEFINWAIT2,
    TCP_STATETIMEWAIT
};

enum {
    TCP_EVENTSTATEDATA,
    TCP_EVENTSTATERESET,
    TCP_EVENTSTATECLOSED
};

enum {
    TCP_FLAGTS = (1 << 0), // timestamp is enabled
};

struct tcp_flags {
    union {
        be_uint16_t flags;
        struct { // since we need the data offset we may as well include these flags
            uint8_t ns : 1;
            uint8_t rsvd : 3;
            uint8_t doff : 4;
            uint8_t fin : 1;
            uint8_t syn : 1;
            uint8_t rst : 1;
            uint8_t psh : 1;
            uint8_t ack : 1;
            uint8_t urg : 1;
            uint8_t ece : 1;
            uint8_t cwr : 1;
        };
    };
};

struct tcp_header {
    be_uint16_t srcport;
    be_uint16_t destport;
    be_uint32_t sequence;
    be_uint32_t acknumber;
    struct tcp_flags;
    be_uint16_t winsize;
    be_uint16_t csum;
    be_uint16_t urgent;
} __attribute__((packed));

struct tcp_packet {
    struct tcp_header header;
    uint32_t len;
    uint8_t *data;
};

struct tcp_stream {
    spinlock_t lock;
    size_t size;
    size_t pos;
    uint8_t *buf;
};

static int64_t tcp_streamread(struct tcp_stream *stream, void *buf, size_t len) {
    if (len > stream->pos) {
        len = stream->pos;
    }

    if (!len) {
        goto cleanup;
    }

    memcpy(buf, stream->buf, len);
    memcpy(stream->buf, stream->buf + len, stream->pos - len);
    stream->pos -= len;

cleanup:
    return len;
}

static int64_t tcp_streamwrite(struct tcp_stream *stream, void *buf, size_t len) {
    if (stream->pos + len > stream->size) {
        kernel_print("net: TCP stream write exceeds stream size (this should never happen) of len: %u, position: %u and size: %u\n", len, stream->pos, stream->size);
        return -1;
    }

    memcpy(stream->buf + stream->pos, buf, len);
    stream->pos += len;

    return len;
}

// 64KiB buffer sizes for send and receive
#define TCP_BUFFERSIZE 0xffff

struct tcp_connection {
    struct net_inetaddr local;
    struct net_inetaddr remote;
    be_uint16_t localport;
    be_uint16_t remoteport;
};

struct tcp_retransmitentry {
    struct timespec first;
    struct timespec last;
    size_t rto;
    uint32_t seq;
    struct tcp_flags flags;
    size_t len;
    uint8_t data[];
};

struct tcp_socket {
    struct inetsocket;

    struct tcp_connection conn;
    struct tcp_socket *parent;

    int tcpstate;
    spinlock_t statelock;

    spinlock_t busyon;

    struct timespec timewaittimer;
    struct timespec connecttimeout;
    VECTOR_TYPE(struct tcp_retransmitentry *) retransmitqueue;
    spinlock_t retransmitlock;

    uint8_t eventstate; // state for what is happening on an event
    uint16_t maxseg;
    uint16_t flags;

    // RFC783
    uint32_t snduna;
    uint32_t sndnxt;
    uint32_t sndwl1;
    uint32_t sndwl2;
    uint32_t sndis; // send initial sequence
    uint32_t sndwnd;
    uint32_t rcvwnd;
    uint32_t rcvnxt;
    uint32_t rcvis; // receive initial sequence

    uint32_t recenttimestamp; // echo data
    uint32_t lastack; // last ack number
    uint32_t lastackts; // last ack number sent

    struct tcp_stream rcvbuf;
    // no need for a send buffer as we just spit them out as soon as we're told to send them (no ACK delay algorithm)
};

static spinlock_t tcp_socketslock = SPINLOCK_INIT;
static VECTOR_TYPE(struct tcp_socket *) tcp_sockets;

// TCP Options (SYN or SYN/ACK)
#define TCP_OPTEOL 0
#define TCP_OPTNOP 1
#define TCP_OPTMSS 2
#define TCP_OPTWINDOWSCALE 3
#define TCP_OPTSACKPERM 4
#define TCP_OPTSACK 5
#define TCP_OPTECHO 6
#define TCP_OPTECHOREPLY 7
#define TCP_OPTTIMESTAMPS 8

// should we do checksum on receive?
// #define TCP_DOCSUM

static bool tcp_grabsocket(struct tcp_connection conn, struct tcp_socket **socket) {
    (void)socket;

    spinlock_acquire(&tcp_socketslock);
    VECTOR_FOR_EACH(&tcp_sockets, it,
        struct tcp_socket *itsocket = *it;
        if (itsocket->conn.localport == conn.localport && itsocket->conn.local.value == conn.local.value && itsocket->conn.remoteport == conn.remoteport && itsocket->conn.remote.value == conn.remote.value) {
            *socket = itsocket;
            spinlock_release(&tcp_socketslock);
            return true;
        }
    );
    spinlock_release(&tcp_socketslock);
    return false;
}

static struct tcp_socket *tcp_tryfindsocket(struct tcp_connection conn) {
    struct tcp_socket *socket = NULL;

    // check for established connections (first pass)
    if (tcp_grabsocket(conn, &socket)) {
        return socket;
    }

    conn.remote.value = INADDR_ANY;
    conn.remoteport = INADDR_ANY;

    // check for listening connections (second pass)
    if (tcp_grabsocket(conn, &socket)) {
        return socket;
    }

    conn.local.value = INADDR_ANY;

    // check for listening connections on any address (third pass)
    if (tcp_grabsocket(conn, &socket)) {
        return socket;
    }

    return socket;
}

static void tcp_setstate(struct tcp_socket *this, int state) {
    spinlock_acquire(&this->statelock);

    this->tcpstate = state;

    spinlock_release(&this->statelock);
}

static int tcp_getstate(struct tcp_socket *this) {
    spinlock_acquire(&this->statelock);

    int state = this->tcpstate;

    spinlock_release(&this->statelock);
    return state;
}

static bool tcp_acquireport(struct tcp_socket *sock, uint16_t port) {
    if (!port) {
        errno = EINVAL;
        return false;
    }

    // default unacquired port
    struct tcp_connection conn;
    conn.local = NET_IPSTRUCT(((struct sockaddr_in *)&sock->localaddr)->sin_addr.s_addr);
    conn.remote = NET_IPSTRUCT(INADDR_ANY);
    conn.localport = __builtin_bswap16(port);
    conn.remoteport = INADDR_ANY;

    sock->conn = conn;

    spinlock_acquire(&tcp_socketslock);
    VECTOR_PUSH_BACK(&tcp_sockets, sock);
    spinlock_release(&tcp_socketslock);
    return true;
}

static be_uint16_t tcp_checksum(struct net_inetaddr src, struct net_inetaddr dest, void *data, uint16_t length) {
    struct {
        uint32_t src;
        uint32_t dest;
        uint8_t zero;
        uint8_t protocol;
        be_uint16_t length;
    } header;
    uint32_t csum = 0;

    header.src = src.value;
    header.dest = dest.value;
    header.zero = 0;
    header.protocol = IPPROTO_TCP;
    header.length = __builtin_bswap16(length);

    uint16_t *ptr = (uint16_t *)&header;
    size_t i = sizeof(header);
    int reit = 0;
update:
    for (; i >= 2; i -= 2) {
        csum += *ptr++;
    }

    if (i) {
        csum += (uint16_t)*((uint8_t *)ptr);
    }

    csum = (csum & 0xffff) + (csum >> 16);

    ptr = (uint16_t *)data;
    i = length;
    reit++;
    if (reit == 1) {
        goto update;
    }

    be_uint16_t ret;
    ret = ~csum;
    return ret;
}

static void tcp_queueforretransmit(struct tcp_socket *this, uint32_t seq, struct tcp_flags flags, uint8_t *data, size_t len) {
    struct tcp_retransmitentry *entry = alloc(sizeof(struct tcp_retransmitentry) + len);
    entry->rto = 200000; // 200ms
    entry->seq = seq;
    entry->flags = flags;
    entry->len = len;
    memcpy(entry->data, data, entry->len);
    entry->first = time_monotonic;
    entry->last = entry->first;
    VECTOR_PUSH_BACK(&this->retransmitqueue, entry);
}

static void tcp_queuecleanup(struct tcp_socket *this) {
    spinlock_acquire(&this->retransmitlock);
    VECTOR_FOR_EACH(&this->retransmitqueue, it,
        struct tcp_retransmitentry *entry = *it;
        if (entry->seq >= this->snduna) {
            break;
        }

        VECTOR_REMOVE(&this->retransmitqueue, VECTOR_FIND(&this->retransmitqueue, entry));
        free(entry);
    );
    spinlock_release(&this->retransmitlock);
    free(this->retransmitqueue.data);
}

static void tcp_settimewait(struct tcp_socket *this) {
    this->timewaittimer = timespec_add(time_monotonic, (struct timespec) { .tv_sec = 12 });
}

// XXX: Segments should be dumped into a vector and sent at our disgression (in order, delayed one after another to prevent out-of-sequence issues on remotes with no implementation for such)
static ssize_t tcp_sendsegment(struct net_adapter *adapter, uint32_t seq, uint32_t ack, struct tcp_flags flags, uint16_t window, uint8_t *data, size_t len, struct tcp_connection conn, struct tcp_socket *sock) {
    size_t optlen = 0;
    if (sock != NULL) {
        if (sock->flags & TCP_FLAGTS) {
            optlen = 12;
        }
    }

    if (flags.syn) {
        optlen += 4;
    }

    struct tcp_header *header = (struct tcp_header *)alloc(sizeof(struct tcp_header) + optlen + len); // allocate for header + data
    header->srcport = conn.localport;
    header->destport = conn.remoteport;
    header->sequence = __builtin_bswap32(seq);
    header->acknumber = __builtin_bswap32(ack);
    header->flags = flags.flags;
    header->doff = (sizeof(struct tcp_header) + optlen) >> 2; // use equivalent shift instead of division for extra speed points
    header->winsize = __builtin_bswap16(window);

    uint32_t *opts = (uint32_t *)((uint64_t)header + sizeof(struct tcp_header));

    if (sock != NULL) {
        if (sock->flags & TCP_FLAGTS) {
            opts[0] = __builtin_bswap32(0x0101080a); // NOP, NOP, Timestamp Option, 10
            opts[1] = __builtin_bswap32(time_monotonic.tv_sec);
            opts[2] = __builtin_bswap32(sock->recenttimestamp);
            opts += 3;
        }
    }

    if (flags.syn) {
        uint8_t *optb = (uint8_t *)opts;
        *optb++ = TCP_OPTMSS;
        *optb++ = 4;
        uint16_t *optw = (uint16_t *)optb;
        *optw++ = __builtin_bswap16(adapter->mtu - (NET_LINKLAYERFRAMESIZE(adapter) + (sizeof(struct net_inetheader) + sizeof(struct tcp_header) + 40)));
        opts += 1;
    }

    memcpy((void *)((uint64_t)header + sizeof(struct tcp_header) + optlen), data, len); // implicitly the next bit of data (since the pointer is working in sizes of the header)

    header->csum = tcp_checksum(adapter->ip, conn.remote, header, sizeof(struct tcp_header) + optlen + len); // following data will be immediately after the header data

    ssize_t ret = net_sendinet(adapter, adapter->ip, conn.remote, IPPROTO_TCP, header, sizeof(struct tcp_header) + optlen + len);
    free(header);
    if (ret == -1) {
        return ret;
    }

    return len;
}

static ssize_t tcp_send(struct tcp_socket *this, struct tcp_flags flags, uint8_t *data, size_t len);

static bool tcp_close(struct tcp_socket *this) {
    spinlock_acquire(&this->busyon);
    switch (tcp_getstate(this)) {
        case TCP_STATECLOSED:
            break; // connection already unreferenced
        case TCP_STATELISTEN:
        case TCP_STATESYNSENT:
            tcp_setstate(this, TCP_STATECLOSED);
            break;
        case TCP_STATEESTABLISHED:
        case TCP_STATESYNRECV: {
            struct tcp_flags flags = { 0 };
            flags.fin = 1;
            flags.ack = 1;
            tcp_send(this, flags, NULL, 0);
            this->sndnxt++;
            tcp_setstate(this, TCP_STATEFINWAIT1);
            break;
        }
        case TCP_STATEFINWAIT1:
        case TCP_STATEFINWAIT2:
        case TCP_STATECLOSING:
        case TCP_STATELASTACK:
        case TCP_STATETIMEWAIT:
            goto retfalse; // connection closing
        case TCP_STATECLOSEWAIT: {
            struct tcp_flags flags = { 0 };
            flags.fin = 1;
            flags.ack = 1;
            tcp_send(this, flags, NULL, 0);
            this->sndnxt++;
            tcp_setstate(this, TCP_STATELASTACK);
            break;
        }
        default:
            goto retfalse; // unknown state, this should never happen
    }
    spinlock_release(&this->busyon);

    if (tcp_getstate(this) == TCP_STATECLOSED) { // socket connection has closed properly (we should free all resources and give up on the port allocation if we have any)
        if (this->adapter) {
            spinlock_acquire(&this->lock);
            net_unbindsocket(this->adapter, (struct socket *)this);
            spinlock_release(&this->lock);
        }

        tcp_queuecleanup(this);

        spinlock_acquire(&tcp_socketslock);
        if (VECTOR_FIND(&tcp_sockets, this) != VECTOR_INVALID_INDEX) {
            VECTOR_REMOVE_BY_VALUE(&tcp_sockets, this);
        }
        spinlock_release(&tcp_socketslock);

        free(this->rcvbuf.buf);

        if (!this->parent) {
            net_releaseport(__builtin_bswap16(this->port)); // only free port if we're the owner of the bound socket port
        }

        resource_free((struct resource *)this);
    }

    return true;

retfalse:
    spinlock_release(&this->busyon);
    return false;
}

static void tcp_queueemit(struct tcp_socket *this, struct tcp_retransmitentry *entry) {
    struct net_adapter *rxtadapter = NULL;
    if(tcp_getstate(this) == TCP_STATECLOSED) { // don't retransmit on closed sockets
        return;
    }

    if (!this->adapter) {
        struct net_macaddr mac;
        struct net_adapter *adapter = NULL;
        struct net_inetaddr thisaddr = (struct net_inetaddr) { .value = INADDR_ANY };

        // grab an adapter for this
        int status = net_route(&adapter, thisaddr, this->conn.remote, &mac);
        if (status != 0) {
            return;
        }

        rxtadapter = adapter;
    } else {
        rxtadapter = this->adapter;
    }

    struct timespec now = time_monotonic;

    struct timespec diff = timespec_sub(now, entry->first);
    if (diff.tv_sec >= 5) {
        tcp_setstate(this, TCP_STATECLOSED); // give up on retransmission if it takes too long
        return;
    }

    struct timespec timeout = entry->last;
    timeout = timespec_add(timeout, (struct timespec) { .tv_sec = 0, .tv_nsec = entry->rto });
    if ((now.tv_sec == timeout.tv_sec ? now.tv_nsec > timeout.tv_nsec : now.tv_sec > timeout.tv_sec)) {
        tcp_sendsegment(rxtadapter, entry->seq, this->rcvnxt, entry->flags, this->rcvwnd, entry->data, entry->len, this->conn, this);
        entry->last = now;
        entry->rto *= 2;
    }
}

static void tcp_retransmitall(struct tcp_socket *this) {
    if (tcp_getstate(this) == TCP_STATECLOSED) {
        return;
    }

    spinlock_acquire(&this->retransmitlock);
    VECTOR_FOR_EACH(&this->retransmitqueue, it2,
        struct tcp_retransmitentry *entry = *it2;
        tcp_queueemit(this, entry);
    );
    spinlock_release(&this->retransmitlock);
}

static void tcp_timer(void *unused) {
    (void)unused;

    for (;;) {
        time_nsleep(100 * 1000000);
        struct timespec now = time_monotonic;
        spinlock_acquire(&tcp_socketslock);
        VECTOR_FOR_EACH(&tcp_sockets, it,
            struct tcp_socket *sock = *it;
            if (tcp_getstate(sock) == TCP_STATETIMEWAIT) {
                if ((now.tv_sec == sock->timewaittimer.tv_sec ? now.tv_nsec > sock->timewaittimer.tv_nsec : now.tv_sec > sock->timewaittimer.tv_sec)) {
                    spinlock_release(&tcp_socketslock);
                    tcp_setstate(sock, TCP_STATECLOSED);
                    tcp_close(sock);
                    spinlock_acquire(&tcp_socketslock);
                    continue;
                }
            }

            if (tcp_getstate(sock) == TCP_STATESYNSENT) { // check for connection timeout
                if ((now.tv_sec == sock->connecttimeout.tv_sec ? now.tv_nsec > sock->connecttimeout.tv_nsec : now.tv_sec > sock->connecttimeout.tv_sec)) {
                    event_trigger(&sock->connect_event, false);
                    continue;
                }
            }

            tcp_retransmitall(sock);
        );
        spinlock_release(&tcp_socketslock);
    }
}

static ssize_t tcp_send(struct tcp_socket *this, struct tcp_flags flags, uint8_t *data, size_t len) {
    uint32_t seq = this->sndnxt; // next send sequence number

    if (flags.syn) {
        seq = this->sndis; // use initial sequence instead as it's a SYN packet
    }
    if ((flags.syn || flags.fin) || len) {
        tcp_queueforretransmit(this, seq, flags, data, len);
    }

    return tcp_sendsegment(this->adapter, seq, this->rcvnxt, flags, this->rcvwnd, data, len, this->conn, this);
}

static bool tcp_unref(struct resource *_this, struct f_description *description) {
    (void)description;

    struct tcp_socket *this = (struct tcp_socket *)_this;

    this->refcount--;
    if (this->refcount == 0) {
        return tcp_close(this);
    }

    return true;
}

static void tcp_parseoptions(struct tcp_socket *this, struct tcp_packet *packet) {
    uint8_t *opts = (uint8_t *)&packet->header + sizeof(struct tcp_header); // offset to options start

    if ((uint8_t)(packet->header.doff << 2) > sizeof(struct tcp_header)) { // we actually have options
        size_t max = (packet->header.doff << 2) - sizeof(struct tcp_header);
        for (size_t i = 0; i < max;) {
            uint8_t opt = opts[i];
            switch (opt) {
                case TCP_OPTEOL: // EOL (Used for delimiting the options list)
                    return;
                case TCP_OPTNOP: // NOP (Used for padding)
                    i++;
                    break;
                case TCP_OPTMSS: // MSS
                    if (opts[i + 1] != 4 || i + 4 > max) { // ensure length is correct, *and* MSS does not go out of bounds for the options list
                        return;
                    }
                    this->maxseg = (opts[i + 2] << 8) | opts[i + 3]; // implicit big endian to little endian
                    i += 4;
                    break;
                case TCP_OPTTIMESTAMPS: // TIMESTAMPS
                    if (opts[i + 1] != 10 || i + 10 > max) { // ditto
                        return;
                    }
                    uint32_t ts = opts[i + 2] | (opts[i + 3] << 8) | (opts[i + 4] << 16) | (opts[i + 5] << 24);
                    if (packet->header.syn) {
                        this->recenttimestamp = __builtin_bswap32(ts); // swap to little endian
                        this->flags |= TCP_FLAGTS;
                    } else if ((int32_t)(this->lastack - __builtin_bswap32(packet->header.sequence)) >= 0 &&
                               (int32_t)(this->lastack - __builtin_bswap32(packet->header.sequence) + packet->len) <= 0) {
                        this->recenttimestamp = __builtin_bswap32(ts);
                    }
                    i += 10;
                    break;
                default: // unimplemented option
                    if (opts[i + 1] == 0) {
                        return; // bad length on unknown field
                    }

                    i += opts[i + 1]; // increment based on length
                    break;
            }
        }
    }
}

static void tcp_netpacket(struct tcp_socket *this, struct net_inetaddr src, struct net_inetaddr dest, void *buf, size_t length) {
    spinlock_acquire(&this->busyon);
    struct tcp_packet *packet = alloc(sizeof(struct tcp_packet));
    memcpy(&packet->header, buf, sizeof(struct tcp_header));
    size_t optlen = (packet->header.doff << 2) - sizeof(struct tcp_header);
    packet->len = length - optlen;
    size_t seglen = packet->len;
    if (packet->header.syn) {
        seglen++;
    }
    if (packet->header.fin) {
        seglen++;
    }

    packet->data = packet->len ? alloc(packet->len) : NULL; // only allocate pointer if we have to (prevents allocation errors)
    memcpy(packet->data, buf + (packet->header.doff << 2), packet->len); // memcpy will already ignore copy if there is no data
    struct net_adapter *connadapter = NULL;
    struct tcp_connection conn = (struct tcp_connection) { .local = dest, .localport = packet->header.destport, .remote = src, .remoteport = packet->header.srcport };

    // get us an adapter to work with now we're listening on this packet
    if (!this->adapter) {
        struct net_macaddr mac;
        struct net_adapter *adapter = NULL;
        struct net_inetaddr thisaddr = (struct net_inetaddr) { .value = INADDR_ANY };

        // grab an adapter for this
        int status = net_route(&adapter, thisaddr, src, &mac);
        if (status != 0) {
            goto dropsegment;
        }

        connadapter = adapter; // get a temporary address allocation for an adapter
    } else {
        connadapter = this->adapter; // give it the adapter that is assigned to this socket
    }

    bool acceptable = false;

    tcp_parseoptions(this, packet);

    if (tcp_getstate(this) == TCP_STATELISTEN) {
        if (packet->header.rst) {
            goto dropsegment;
        }

        if (packet->header.ack) {
            struct tcp_flags flags = { 0 };
            flags.rst = 1;
            tcp_sendsegment(connadapter, __builtin_bswap32(packet->header.acknumber), 0, flags, 0, NULL, 0, conn, NULL);
            goto dropsegment;
        }

        if (packet->header.syn) {
            struct tcp_socket *socket = (struct tcp_socket *)socket_create_tcp(SOCK_STREAM, IPPROTO_TCP);
            socket->parent = this;
            net_bindsocket(connadapter, (struct socket *)socket);
            socket->conn = conn;
            socket->rcvwnd = socket->rcvbuf.size;
            socket->rcvnxt = __builtin_bswap32(packet->header.sequence) + 1;
            socket->rcvis = __builtin_bswap32(packet->header.sequence);
            socket->sndis = random_generate();

            spinlock_acquire(&tcp_socketslock);
            VECTOR_PUSH_BACK(&tcp_sockets, socket);
            spinlock_release(&tcp_socketslock);

            tcp_parseoptions(socket, packet);

            struct tcp_flags flags = { 0 };
            flags.syn = 1;
            flags.ack = 1;
            tcp_send(socket, flags, NULL, 0);

            socket->sndnxt = socket->sndis + 1;
            socket->snduna = socket->sndis;
            tcp_setstate(socket, TCP_STATESYNRECV);
            goto dropsegment;
        }

        goto dropsegment;
    } else if (tcp_getstate(this) == TCP_STATESYNSENT) {
        if (packet->header.ack) {
            if (__builtin_bswap32(packet->header.acknumber) <= this->sndis || __builtin_bswap32(packet->header.acknumber) > this->sndnxt) {
                struct tcp_flags flags = { 0 };
                flags.rst = 1;
                tcp_sendsegment(connadapter, __builtin_bswap32(packet->header.acknumber), 0, flags, 0, NULL, 0, conn, NULL);
                goto dropsegment;
            }

            if (this->snduna <= __builtin_bswap32(packet->header.acknumber) && __builtin_bswap32(packet->header.acknumber) <= this->sndnxt) {
                acceptable = true;
            }
        }

        if (packet->header.rst) {
            if (acceptable) {
                this->eventstate = TCP_STATECLOSED;
                tcp_setstate(this, TCP_STATECLOSED);
                this->status |= POLLHUP;
                spinlock_release(&this->busyon);
                tcp_close(this);
                spinlock_acquire(&this->busyon);
                event_trigger(&this->connect_event, false);
            }
            goto dropsegment;
        }

        if (packet->header.syn && packet->header.ack) {
            this->rcvnxt = __builtin_bswap32(packet->header.sequence) + 1;
            this->rcvis = __builtin_bswap32(packet->header.sequence);
            if (acceptable) {
                this->snduna = __builtin_bswap32(packet->header.acknumber);
                // cleanup retransmit
                tcp_queuecleanup(this);
            }
            if (this->snduna > this->sndis) {
                tcp_parseoptions(this, packet);
                tcp_setstate(this, TCP_STATEESTABLISHED); // YES! we got a connection!
                this->state = SOCKET_CONNECTED;
                this->status |= POLLIN;
                struct tcp_flags flags = { 0 };
                flags.ack = 1;
                tcp_send(this, flags, NULL, 0);
                this->sndwnd = __builtin_bswap32(packet->header.winsize); // update our send window with how much the remote can feasibly handle
                this->sndwl1 = __builtin_bswap32(packet->header.sequence);
                this->sndwl2 = __builtin_bswap32(packet->header.acknumber);
                event_trigger(&this->connect_event, false);
                goto dropsegment;
            } else {
                tcp_setstate(this, TCP_STATESYNRECV);
                struct tcp_flags flags = { 0 };
                flags.syn = 1;
                flags.ack = 1;
                tcp_send(this, flags, NULL, 0); // swap to pretending to be a server
                goto dropsegment;
            }
        } else if (packet->header.ack) { // potential half-open connection (close it)
            struct tcp_flags flags = { 0 };
            flags.rst = 1;
            tcp_sendsegment(connadapter, __builtin_bswap32(packet->header.acknumber), 0, flags, 0, NULL, 0, conn, NULL);
        }

        goto dropsegment;
    }

    switch (tcp_getstate(this)) {
        case TCP_STATESYNRECV:
        case TCP_STATEESTABLISHED:
        case TCP_STATEFINWAIT1:
        case TCP_STATEFINWAIT2:
        case TCP_STATECLOSEWAIT:
        case TCP_STATECLOSING:
        case TCP_STATELASTACK:
        case TCP_STATETIMEWAIT:
            if (!seglen) {
                if (!this->rcvwnd) { // we're allowed to have a zero window if the segment has no length
                    if (__builtin_bswap32(packet->header.sequence) == this->rcvnxt) {
                        acceptable = true;
                    }
                } else {
                    if (this->rcvnxt <= __builtin_bswap32(packet->header.sequence) && __builtin_bswap32(packet->header.sequence) < this->rcvnxt + this->rcvwnd) {
                        acceptable = true;
                    }
                }
            } else {
                if (!this->rcvwnd) { // we cannot have zero windows
                    acceptable = false;
                } else {
                    if ((this->rcvnxt <= __builtin_bswap32(packet->header.sequence) && __builtin_bswap32(packet->header.sequence) < this->rcvnxt + this->rcvwnd) ||
                        (this->rcvnxt <= __builtin_bswap32(packet->header.sequence) + seglen - 1 &&
                         __builtin_bswap32(packet->header.sequence) + seglen - 1 < this->rcvnxt + this->rcvwnd)) {
                        acceptable = true;
                    }
                }
            }

            if (!acceptable) {
                if (!packet->header.rst) {
                    struct tcp_flags flags = { 0 };
                    flags.ack = 1;
                    tcp_send(this, flags, NULL, 0);
                }
                goto dropsegment;
            }
    }

    switch (tcp_getstate(this)) {
        case TCP_STATESYNRECV:
            if (packet->header.rst) {
                tcp_setstate(this, TCP_STATECLOSED);
                this->status |= POLLHUP;
                spinlock_release(&this->busyon);
                tcp_close(this);
                spinlock_acquire(&this->busyon);
                goto dropsegment;
            }
            break;
        case TCP_STATEESTABLISHED:
        case TCP_STATEFINWAIT1:
        case TCP_STATEFINWAIT2:
        case TCP_STATECLOSEWAIT:
            if (packet->header.rst) {
                tcp_setstate(this, TCP_STATECLOSED);
                this->eventstate = TCP_EVENTSTATERESET;
                this->status |= POLLHUP;
                event_trigger(&this->event, false);
                spinlock_release(&this->busyon);
                tcp_close(this);
                spinlock_acquire(&this->busyon);
                goto dropsegment;
            }
            break;
        case TCP_STATECLOSING:
        case TCP_STATELASTACK:
        case TCP_STATETIMEWAIT:
            if (packet->header.rst) {
                tcp_setstate(this, TCP_STATECLOSED);
                this->eventstate = TCP_EVENTSTATERESET;
                event_trigger(&this->event, false);
                this->status |= POLLHUP;
                spinlock_release(&this->busyon);
                tcp_close(this);
                spinlock_acquire(&this->busyon);
                goto dropsegment;
            }
            break;
    }

    switch (tcp_getstate(this)) {
        case TCP_STATESYNRECV:
        case TCP_STATEESTABLISHED:
        case TCP_STATEFINWAIT1:
        case TCP_STATEFINWAIT2:
        case TCP_STATECLOSEWAIT:
        case TCP_STATECLOSING:
        case TCP_STATELASTACK:
        case TCP_STATETIMEWAIT:
            if (packet->header.syn) { // remote should not be trying to initiate a connection whilst this is happening (XXX: Should probably retransmit SYN+ACK)
                struct tcp_flags flags = { 0 };
                flags.rst = 1;
                tcp_send(this, flags, NULL, 0);
                this->status |= POLLHUP;
                tcp_setstate(this, TCP_STATECLOSED);
                this->eventstate = TCP_EVENTSTATERESET;
                event_trigger(&this->event, false);
                spinlock_release(&this->busyon);
                tcp_close(this);
                spinlock_acquire(&this->busyon);
                goto dropsegment;
            }
            break;
    }

    if (!packet->header.ack) {
        goto dropsegment;
    }

    switch (tcp_getstate(this)) {
        case TCP_STATESYNRECV:
            if (this->snduna <= __builtin_bswap32(packet->header.acknumber) && __builtin_bswap32(packet->header.acknumber) <= this->sndnxt) {
                tcp_setstate(this, TCP_STATEESTABLISHED);
                this->state = SOCKET_CONNECTED;
                this->status |= POLLIN;
                socket_add_to_backlog((struct socket *)this->parent, (struct socket *)this);
                event_trigger(&this->parent->connect_event, false); // alert accept() that there is a new connection
            } else {
                // invalid ack, sending reset
                struct tcp_flags flags = { 0 };
                flags.rst = 1;
                tcp_sendsegment(connadapter, __builtin_bswap32(packet->header.ack), 0, flags, 0, NULL, 0, this->conn, NULL);
                goto dropsegment;
            }
        case TCP_STATEESTABLISHED:
        case TCP_STATEFINWAIT1:
        case TCP_STATEFINWAIT2:
        case TCP_STATECLOSEWAIT:
        case TCP_STATECLOSING:
            if (this->snduna < __builtin_bswap32(packet->header.acknumber) && __builtin_bswap32(packet->header.acknumber) <= this->sndnxt) {
                this->snduna = __builtin_bswap32(packet->header.acknumber);
                // cleanup retransmit
                tcp_queuecleanup(this);
                if (this->sndwl1 < __builtin_bswap32(packet->header.sequence) || (this->sndwl1 == __builtin_bswap32(packet->header.sequence) && this->sndwl2 <= __builtin_bswap32(packet->header.acknumber))) {
                    this->sndwnd = __builtin_bswap32(packet->header.winsize);
                    this->sndwl1 = __builtin_bswap32(packet->header.sequence);
                    this->sndwl2 = __builtin_bswap32(packet->header.acknumber);
                }
            } else if (__builtin_bswap32(packet->header.acknumber) > this->sndnxt) {
                struct tcp_flags flags = { 0 };
                flags.ack = 1;
                tcp_send(this, flags, NULL, 0);
                goto dropsegment;
            }

            switch (tcp_getstate(this)) {
                case TCP_STATEFINWAIT1:
                    if (__builtin_bswap32(packet->header.acknumber) == this->sndnxt) {
                        tcp_setstate(this, TCP_STATEFINWAIT2);
                    }
                    break;
                case TCP_STATEFINWAIT2:
                case TCP_STATECLOSEWAIT:
                    break;
                case TCP_STATECLOSING:
                    if (__builtin_bswap32(packet->header.acknumber) == this->sndnxt) {
                        tcp_setstate(this, TCP_STATETIMEWAIT);
                        tcp_settimewait(this);
                        event_trigger(&this->event, false);
                    }
                    break;
            }
            break;
        case TCP_STATELASTACK:
            if (__builtin_bswap32(packet->header.acknumber) == this->sndnxt) {
                tcp_setstate(this, TCP_STATECLOSED);
                spinlock_release(&this->busyon);
                tcp_close(this);
                spinlock_acquire(&this->busyon);
            }
            goto dropsegment;
        case TCP_STATETIMEWAIT:
            if (packet->header.fin) {
                tcp_settimewait(this);
            }
            break;
    }

    switch (tcp_getstate(this)) {
        case TCP_STATEESTABLISHED:
        case TCP_STATEFINWAIT1:
        case TCP_STATEFINWAIT2:
            if (packet->len) {
                spinlock_acquire(&this->rcvbuf.lock);
                if (tcp_streamwrite(&this->rcvbuf, packet->data, packet->len) == -1) {
                    spinlock_release(&this->rcvbuf.lock);
                    goto dropsegment;
                }
                spinlock_release(&this->rcvbuf.lock);
                this->rcvnxt = __builtin_bswap32(packet->header.sequence) + seglen;
                this->rcvwnd -= packet->len;
                struct tcp_flags flags = { 0 };
                flags.ack = 1;
                tcp_send(this, flags, NULL, 0);
                this->eventstate = TCP_EVENTSTATEDATA;
                this->status |= POLLIN;
                event_trigger(&this->event, false);
            }
            break;
    }

    if (packet->header.fin) {
        switch (tcp_getstate(this)) {
            case TCP_STATECLOSED:
            case TCP_STATELISTEN:
            case TCP_STATESYNSENT:
                goto dropsegment;
        }

        switch (tcp_getstate(this)) {
            case TCP_STATESYNRECV:
            case TCP_STATEESTABLISHED:
                tcp_setstate(this, TCP_STATECLOSEWAIT);
                if (!this->rcvbuf.pos) {
                    this->status &= ~(POLLIN | POLLOUT);
                } else {
                    this->status |= POLLIN; // there is still data
                }
                event_trigger(&this->event, false); // TODO: Set event state?
                break;
            case TCP_STATEFINWAIT1:
                if (__builtin_bswap32(packet->header.acknumber) == this->sndnxt) {
                    tcp_setstate(this, TCP_STATETIMEWAIT);
                    tcp_settimewait(this);
                } else {
                    tcp_setstate(this, TCP_STATECLOSING);
                }
                break;
            case TCP_STATEFINWAIT2:
                tcp_setstate(this, TCP_STATETIMEWAIT);
                tcp_settimewait(this);
                break;
            case TCP_STATECLOSEWAIT:
            case TCP_STATECLOSING:
            case TCP_STATELASTACK:
                break;
            case TCP_STATETIMEWAIT:
                tcp_settimewait(this);
                break;
        }

        this->rcvnxt = __builtin_bswap32(packet->header.sequence) + 1;
        struct tcp_flags flags = { 0 };
        flags.ack = 1;
        // sending ack
        tcp_send(this, flags, NULL, 0);
    }
dropsegment:
    free(packet->data);
    free(packet);
    spinlock_release(&this->busyon);
}

static bool tcp_connect(struct socket *_this, struct f_description *description, void *_addr, socklen_t len) {
    (void)description;
    (void)len;

    struct tcp_socket *this = (struct tcp_socket *)_this;

    if (this->state == SOCKET_CONNECTED && tcp_getstate(this) == TCP_STATEESTABLISHED) {
        errno = EISCONN; // already connected
        return false;
    } else if (tcp_getstate(this) == TCP_STATESYNSENT) {
        errno = EALREADY; // already waiting for connection
        return false;
    }

    struct sockaddr_in *addr = (struct sockaddr_in *)_addr;

    memcpy(&this->peeraddr, addr, sizeof(struct sockaddr_in));

    struct net_macaddr mac;
    struct net_inetaddr thisaddr = NET_IPSTRUCT(INADDR_ANY);

    // grab an adapter for this
    int status = net_route(&this->adapter, thisaddr, NET_IPSTRUCT(addr->sin_addr.s_addr), &mac);
    if (status != 0) {
        return false;
    }

    if (!this->port) {
        this->port = __builtin_bswap16(net_allocport());

        if (!this->port) {
            errno = EINTR;
            return false;
        }
        tcp_acquireport(this, __builtin_bswap16(this->port));
    }

    this->rcvwnd = this->rcvbuf.size;
    this->sndis = random_generate();
    struct tcp_flags flags = { 0 };
    flags.syn = 1;
    net_bindsocket(this->adapter, (struct socket *)this);
    spinlock_acquire(&tcp_socketslock);
    VECTOR_PUSH_BACK(&tcp_sockets, this);
    spinlock_release(&tcp_socketslock);
    this->conn = (struct tcp_connection) { .local = this->adapter->ip, .localport = this->port, .remote = NET_IPSTRUCT(addr->sin_addr.s_addr), .remoteport = addr->sin_port };
    tcp_setstate(this, TCP_STATESYNSENT);
    this->connecttimeout = timespec_add(time_monotonic, (struct timespec) { .tv_sec = 5 });
    if (tcp_send(this, flags, NULL, 0) == -1) {
        tcp_setstate(this, TCP_STATECLOSED);
        tcp_close(this);
        errno = ECONNRESET;
        return false;
    }

    this->snduna = this->sndis;
    this->sndnxt = this->sndis + 1;

    // XXX: Implement non-blocking connect with errno EINPROGRESS

again:
    struct event *events[] = { &this->connect_event };
    event_await(events, 1, true);

    if (tcp_getstate(this) != TCP_STATEESTABLISHED) {
        if (tcp_getstate(this) == TCP_STATESYNRECV) {
            goto again;
        }

        tcp_setstate(this, TCP_STATECLOSED);
        tcp_close(this);
        errno = ETIMEDOUT;
        return false;
    }

    this->status |= POLLOUT | POLLIN;
    event_trigger(&this->event, false);

    return true;
}

static ssize_t tcp_read(struct resource *_this, struct f_description *description, void *buf, off_t offset, size_t count) {
    (void)offset;

    struct tcp_socket *this = (struct tcp_socket *)_this;

    ssize_t ret = -1;

    if (this->state != SOCKET_CONNECTED) {
        errno = ENOTCONN;
        goto cleanup;
    }

retry:
    spinlock_acquire(&this->busyon);
    switch (tcp_getstate(this)) {
        case TCP_STATECLOSED:
            errno = ENOTCONN;
            goto cleanup;
        case TCP_STATELISTEN:
        case TCP_STATESYNSENT:
        case TCP_STATESYNRECV:
            errno = ENOBUFS;
            goto cleanup;
        case TCP_STATEESTABLISHED:
        case TCP_STATEFINWAIT1:
        case TCP_STATEFINWAIT2:
            if (this->rcvbuf.pos == 0) {
                if ((description->flags & O_NONBLOCK) != 0) {
                    errno = EWOULDBLOCK;
                    goto cleanup;
                }
                struct event *events[] = { &this->event };
                spinlock_release(&this->busyon);
                event_await(events, 1, true);
                goto retry;
            }
            break;
        case TCP_STATECLOSEWAIT:
            if (this->rcvbuf.pos) {
                break; // escape (we have data left)
            }
            // otherwise, fall through to a connection close (we should not expect any more data from the remote)
            ret = 0;
            goto cleanup;
        case TCP_STATECLOSING:
        case TCP_STATELASTACK:
        case TCP_STATETIMEWAIT:
            ret = 0;
            goto cleanup;
        default:
            errno = EINVAL;
            goto cleanup; // should never happen
    }

    switch (this->eventstate) {
        case TCP_EVENTSTATEDATA:
            break;
        case TCP_EVENTSTATERESET:
        case TCP_EVENTSTATECLOSED:
            errno = ECONNRESET; // remote forcibly closed/reset by peer
            goto cleanup;
    }

    if (this->rcvbuf.pos < count) {
        count = this->rcvbuf.pos;
    }

    spinlock_acquire(&this->lock);
    this->rcvwnd += count;

    spinlock_acquire(&this->rcvbuf.lock);
    tcp_streamread(&this->rcvbuf, buf, count);
    spinlock_release(&this->rcvbuf.lock);

    if (this->rcvbuf.pos == 0) {
        this->status &= ~POLLIN;
    }
    ret = count;

cleanup:
    spinlock_release(&this->lock);
    spinlock_release(&this->busyon);
    return ret;
}

static ssize_t tcp_recvmsg(struct socket *_this, struct f_description *description, struct msghdr *msg, int flags) {
    struct tcp_socket *this = (struct tcp_socket *)_this;

    ssize_t ret = -1;
    if (this->state != SOCKET_CONNECTED || msg->msg_name != NULL) {
        errno = ENOTCONN;
        goto cleanup;
    }

retry:
    spinlock_acquire(&this->busyon);
    switch (tcp_getstate(this)) {
        case TCP_STATECLOSED:
            errno = ENOTCONN;
            goto cleanup;
        case TCP_STATELISTEN:
        case TCP_STATESYNSENT:
        case TCP_STATESYNRECV:
            errno = ENOBUFS;
            goto cleanup;
        case TCP_STATEESTABLISHED:
        case TCP_STATEFINWAIT1:
        case TCP_STATEFINWAIT2:
            if (this->rcvbuf.pos == 0) {
                if (flags & MSG_DONTWAIT) {
                    errno = EAGAIN;
                    goto cleanup;
                } else if ((description->flags & O_NONBLOCK) != 0) {
                    errno = EWOULDBLOCK;
                    goto cleanup;
                }
                struct event *events[] = { &this->event };
                spinlock_release(&this->busyon);
                event_await(events, 1, true);
                goto retry;
            }
            break;
        case TCP_STATECLOSEWAIT:
            if (this->rcvbuf.pos) {
                break; // escape (we have data left)
            }
            // otherwise, fall through to a connection close (we should not expect any more data from the remote)
            ret = 0;
            goto cleanup;
        case TCP_STATECLOSING:
        case TCP_STATELASTACK:
        case TCP_STATETIMEWAIT:
            ret = 0;
            goto cleanup;
        default:
            errno = EINVAL;
            goto cleanup; // should never happen

    }

    switch (this->eventstate) {
        case TCP_EVENTSTATEDATA:
            break;
        case TCP_EVENTSTATERESET:
        case TCP_EVENTSTATECLOSED:
            errno = ECONNRESET; // remote forcibly closed/reset by peer
            goto cleanup;
    }

    size_t count = 0;
    for (size_t i = 0; i < msg->msg_iovlen; i++) {
        count += msg->msg_iov[i].iov_len;
    }

    if (this->rcvbuf.pos < count) {
        count = this->rcvbuf.pos;
    }

    spinlock_acquire(&this->lock);
    this->rcvwnd += count;

    uint8_t *buf = alloc(count);
    spinlock_acquire(&this->rcvbuf.lock);
    tcp_streamread(&this->rcvbuf, buf, count);
    spinlock_release(&this->rcvbuf.lock);

    size_t transferred = 0;
    size_t remaining = count;
    for (size_t i = 0; i < msg->msg_iovlen; i++) {
        size_t transfercount = MIN(msg->msg_iov[i].iov_len, remaining);
        memcpy(msg->msg_iov[i].iov_base, buf + transferred, transfercount);
        transferred += transfercount;
        remaining -= transfercount;
    }

    if (this->rcvbuf.pos == 0) {
        this->status &= ~POLLIN; // XXX: How do polling events *actually* work? (and how could they be improved from what lyre already does)
    }
    ret = transferred;
    free(buf);

cleanup:
    spinlock_release(&this->lock);
    spinlock_release(&this->busyon);
    return ret;
}

static ssize_t tcp_write(struct resource *_this, struct f_description *description, const void *buf, off_t offset, size_t count) {
    (void)description;
    (void)offset;

    struct tcp_socket *this = (struct tcp_socket *)_this;

    ssize_t ret = -1;

    if (this->state != SOCKET_CONNECTED) {
        errno = ENOTCONN;
        goto cleanup;
    }

retry:
    spinlock_acquire(&this->busyon);
    switch (tcp_getstate(this)) {
        case TCP_STATECLOSED:
            errno = ENOTCONN;
            goto cleanup;
        case TCP_STATELISTEN:
        case TCP_STATESYNSENT:
        case TCP_STATESYNRECV:
            errno = ENOBUFS;
            goto cleanup;
        case TCP_STATEESTABLISHED:
        case TCP_STATECLOSEWAIT: {
            size_t progress = 0;
            size_t mss = this->maxseg ? this->maxseg : this->adapter->mtu - (NET_LINKLAYERFRAMESIZE(this->adapter) + sizeof(struct net_inetheader) + sizeof(struct tcp_header) + 40); // calculate maximum segment from size of header and estimated theoretical maximum options length
            while (progress < count) {
                size_t cap = this->sndwnd - (this->sndnxt - this->snduna);
                size_t mssmin = MIN(mss, count - progress);
                if (!cap) { // no space
                    struct event *events[] = { &this->event };
                    spinlock_release(&this->busyon);
                    event_await(events, 1, true); // wait until something new happens (XXX: Perhaps not ideal?)
                    goto retry;
                }
                size_t seglen = MIN(mssmin, cap); // length of segment (must be under maximum segment size *and* window)
                struct tcp_flags flags = { 0 };
                flags.psh = 1;
                flags.ack = 1;
                if (tcp_send(this, flags, (void *)(buf + progress), seglen) == -1) {
                    tcp_setstate(this, TCP_STATECLOSED);
                    tcp_close(this);
                    errno = ECONNRESET;
                    goto cleanup;
                }
                this->sndnxt += seglen;
                progress += seglen;
            }
            ret = count;
            goto cleanup;
        }
        case TCP_STATECLOSING:
        case TCP_STATELASTACK:
        case TCP_STATETIMEWAIT:
            ret = 0;
            goto cleanup;
        default:
            errno = EINVAL;
            goto cleanup; // should never happen
    }

    this->status |= POLLOUT;
cleanup:
    spinlock_release(&this->busyon);
    return ret;
}

static ssize_t tcp_sendmsg(struct socket *_this, struct f_description *description, const struct msghdr *msg, int flags) {
    (void)description;
    (void)flags;

    struct tcp_socket *this = (struct tcp_socket *)_this;

    ssize_t ret = -1;
    size_t count = 0;
    for (size_t i = 0; i < msg->msg_iovlen; i++) {
        count += msg->msg_iov[i].iov_len;
    }

    uint8_t *buf = alloc(count);

    if (this->state != SOCKET_CONNECTED || msg->msg_name != NULL) {
        errno = ENOTCONN;
        goto cleanup;
    }

    size_t transferred = 0;
    size_t remaining = count;
    for (size_t i = 0; i < msg->msg_iovlen; i++) {
        size_t transfercount = MIN(msg->msg_iov[i].iov_len, remaining);
        memcpy(buf + transferred, msg->msg_iov[i].iov_base, transfercount);
        transferred += transfercount;
        remaining -= transfercount;
    }

retry:
    spinlock_acquire(&this->busyon);
    switch (tcp_getstate(this)) {
        case TCP_STATECLOSED:
            errno = ENOTCONN;
            goto cleanup;
        case TCP_STATELISTEN:
        case TCP_STATESYNSENT:
        case TCP_STATESYNRECV:
            errno = ENOBUFS;
            goto cleanup;
        case TCP_STATEESTABLISHED:
        case TCP_STATECLOSEWAIT: {
            size_t progress = 0;
            size_t mss = this->maxseg ? this->maxseg : this->adapter->mtu - (NET_LINKLAYERFRAMESIZE(this->adapter) + sizeof(struct net_inetheader) + sizeof(struct tcp_header) + 40); // calculate maximum segment from size of header and estimated theoretical maximum options length
            while (progress < count) {
                size_t cap = this->sndwnd - (this->sndnxt - this->snduna);
                size_t mssmin = MIN(mss, count - progress);
                if (!cap) { // no space
                    struct event *events[] = { &this->event };
                    spinlock_release(&this->busyon);
                    event_await(events, 1, true); // wait until something new happens (XXX: Perhaps not ideal?)
                    goto retry;
                }
                size_t seglen = MIN(mssmin, cap); // length of segment (must be under maximum segment size *and* window)
                struct tcp_flags sendflags = { 0 };
                sendflags.psh = 1;
                sendflags.ack = 1;
                if (tcp_send(this, sendflags, (void *)(buf + progress), seglen) < 0) {
                    tcp_setstate(this, TCP_STATECLOSED);
                    this->unref((struct resource *)this, NULL);
                    errno = ECONNRESET;
                    goto cleanup;
                }
                this->sndnxt += seglen;
                progress += seglen;
            }
            ret = count;
            goto cleanup;
        }
        case TCP_STATECLOSING:
        case TCP_STATELASTACK:
        case TCP_STATETIMEWAIT:
            ret = 0;
            goto cleanup;
        default:
            errno = EINVAL;
            goto cleanup; // should never happen
    }

    this->status |= POLLOUT;
cleanup:
    free(buf);
    spinlock_release(&this->busyon);
    return ret;
}

static struct socket *tcp_accept(struct socket *_this, struct f_description *description, struct socket *other, void *_addr, socklen_t *len) {
    (void)description;
    (void)other; // we don't rely on the syscall to get our peer

    while (_this->backlog_i == 0) {
        if ((description->flags & O_NONBLOCK)) {
            errno = EWOULDBLOCK;
            return NULL;
        }

        struct event *events[] = { &_this->connect_event }; // await for connection to socket
        event_await(events, 1, true);
    }

    struct tcp_socket *connection = (struct tcp_socket *)_this->backlog[0];
    for (size_t i = 1; i < _this->backlog_i; i++) {
        _this->backlog[i - 1] = _this->backlog[i];
    }
    _this->backlog_i--;

    struct sockaddr_in *addr = (struct sockaddr_in *)_addr;
    *addr = *(&(struct sockaddr_in) { .sin_addr = { .s_addr = connection->conn.remote.value }, .sin_family = AF_INET, .sin_zero = { 0 }, .sin_port = connection->conn.remoteport });
    *len = sizeof(struct sockaddr_in); // implict truncate

    return (struct socket *)connection;
}

static bool tcp_listen(struct socket *_this, struct f_description *description, int backlog) {
    (void)description;
    (void)backlog;

    if (_this->state == SOCKET_CONNECTED) {
        errno = EISCONN; // TCP socket cannot be both client and server
        return false;
    }

    if (_this->state != SOCKET_BOUND) { // to listen, the socket must have an address bound to it
        errno = EDESTADDRREQ;
        return false;
    }

    tcp_setstate((struct tcp_socket *)_this, TCP_STATELISTEN);

    return true;
}

static bool tcp_bind(struct socket *_this, struct f_description *description, void *addr_, socklen_t len) {
    (void)description;
    (void)len;

    struct tcp_socket *this = (struct tcp_socket *)_this;

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
        errno = EADDRNOTAVAIL;
        return false;
    }

    memcpy(&this->localaddr, addr, sizeof(struct sockaddr_in));

    bool ret = tcp_acquireport(this, __builtin_bswap16(this->port)); // final state success depends on the ability to attain access to this port
    if (!ret) {
        if (this->adapter) {
            net_unbindsocket(this->adapter, (struct socket *)this);
        }
    } else {
        this->bound = true;
    }
    return ret;
}

void tcp_ontcp(struct net_adapter *adapter, struct net_inetheader *inetheader, size_t length) {
    (void)adapter;

    struct tcp_header *header = (struct tcp_header *)inetheader->data;

#ifdef TCP_DOCSUM
    if (tcp_checksum(inetheader->src, inetheader->dest, inetheader->data + sizeof(struct tcp_header), length) != header->csum) {
        return;
    }
#endif

    debug_print(0, "net: Received TCP packet from %d.%d.%d.%d:%d to %d.%d.%d.%d:%d\n", NET_PRINTIP(inetheader->src), __builtin_bswap16(header->srcport), NET_PRINTIP(inetheader->dest), __builtin_bswap16(header->destport));

    if (header->doff * 4 < sizeof(struct tcp_header)) { // not large enough to contain the full TCP header
        return;
    }

    struct tcp_connection conn = (struct tcp_connection) { .local = inetheader->dest, .localport = header->destport, .remote = inetheader->src, .remoteport = header->srcport };
    struct tcp_socket *sock = tcp_tryfindsocket(conn);
    if (!sock || tcp_getstate(sock) == TCP_STATECLOSED) {
        if (header->rst) { // We don't care if the remote side is trying to reset connection
            return;
        } else if (!header->ack) { // RST, ACK (Standard port unreachable)
            struct tcp_flags flags = { 0 };
            flags.rst = 1;
            flags.ack = 1;
            tcp_sendsegment(adapter, 0, __builtin_bswap32(header->sequence) + 1, flags, 0, NULL, 0, conn, NULL);
        } else { // RST (Packet already ACKs)
            struct tcp_flags flags = { 0 };
            flags.rst = 1;
            tcp_sendsegment(adapter,__builtin_bswap32(header->acknumber), 0, flags, 0, NULL, 0, conn, NULL);
        }
        return; // no sockets listening for this sort of data
    }

    // pass it off to the socket in question
    tcp_netpacket((struct tcp_socket *)sock, inetheader->src, inetheader->dest, inetheader->data, length - sizeof(struct tcp_header));
}

ssize_t tcp_getsockopt(struct socket *_this, struct f_description *description, int level, int optname, void *optval, socklen_t *optlen) {
    struct tcp_socket *this = (struct tcp_socket *)_this;

    switch (level) {
        case SOL_SOCKET:
            net_getsockopt(_this, description, level, optname, optval, optlen);
            break;
        case IPPROTO_TCP:
            switch (optname) {
                // case TCP_MAXSEG:
                //     if (*optlen < sizeof(int)) {
                //         errno = EINVAL;
                //         return -1;
                //     }
                //     *((int *)optval) = this->maxseg;
                //     break;
                default:
                    errno = EINVAL;
                    return -1;
            }
            break;
        default:
            errno = EINVAL;
            return -1;
    }
    return 0;
}

ssize_t tcp_setsockopt(struct socket *_this, struct f_description *description, int level, int optname, const void *optval, socklen_t optlen) {
    struct tcp_socket *this = (struct tcp_socket *)_this;

    switch (level) {
        case SOL_SOCKET:
            net_setsockopt(_this, description, level, optname, optval, optlen);
            break;
        case IPPROTO_TCP:
            switch (optname) {
                // case TCP_MAXSEG:
                //     if (optlen < sizeof(int)) {
                //         errno = EINVAL;
                //         return -1;
                //     }
                //     this->maxseg = *((int *)optval);
                //     break;
                default:
                    errno = EINVAL;
                    return -1;
            }
            break;
        default:
            errno = EINVAL;
            return -1;
    }
    return 0;
}

int tcp_sockioctl(struct resource *_this, struct f_description *description, uint64_t request, uint64_t arg) {
    struct tcp_socket *this = (struct tcp_socket *)_this;

    switch (request) {
        case SIOCINQ:
            if (this->state == SOCKET_LISTENING) {
                errno = EINVAL;
                return -1;
            }

            *((int *)arg) = this->rcvbuf.pos;
            return 0;
        // case SIOCGSTAMP:
            // struct timeval *val = (struct timeval *)arg;
            // val->tv_sec = this->recenttimestamp;
            // val->tv_usec = 0;
            // return 0;
    }

    return net_ifioctl(_this, description, request, arg);
}

static bool tcp_getpeername(struct socket *_this, struct f_description *description, void *addr, socklen_t *len) {
    (void)description;

    struct tcp_socket *this = (struct tcp_socket *)_this;

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

static bool tcp_getsockname(struct socket *_this, struct f_description *description, void *addr, socklen_t *len) {
    (void)description;

    struct tcp_socket *this = (struct tcp_socket *)_this;

    if (!this->bound && tcp_getstate(this) < TCP_STATEESTABLISHED && tcp_getstate(this) != TCP_STATELISTEN) {
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

static spinlock_t tcp_firstlock = SPINLOCK_INIT;
static bool tcp_firstsocket = true;

struct socket *socket_create_tcp(int type, int protocol) {
    if (protocol != IPPROTO_TCP) {
        errno = EPROTOTYPE;
        return NULL;
    }

    struct tcp_socket *sock = socket_create(AF_INET, type, protocol, sizeof(struct tcp_socket));
    if (sock == NULL) {
        goto cleanup;
    }

    spinlock_acquire(&tcp_firstlock);
    if (tcp_firstsocket) {
        tcp_firstsocket = false;
        sched_new_kernel_thread(tcp_timer, NULL, true);
    }
    spinlock_release(&tcp_firstlock);

    sock->state = SOCKET_CREATED;
    sock->family = AF_INET;
    sock->type = type;
    sock->protocol = protocol;
    sock->stat.st_mode = S_IFSOCK;

    sock->bind = tcp_bind;
    sock->listen = tcp_listen;
    sock->accept = tcp_accept;
    sock->connect = tcp_connect;
    sock->read = tcp_read;
    sock->write = tcp_write;
    sock->recvmsg = tcp_recvmsg;
    sock->sendmsg = tcp_sendmsg;
    sock->getsockopt = tcp_getsockopt;
    sock->setsockopt = tcp_setsockopt;
    sock->getpeername = tcp_getpeername;
    sock->getsockname = tcp_getsockname;

    sock->unref = tcp_unref;
    sock->ioctl = net_ifioctl;

    sock->rcvbuf.buf = alloc(TCP_BUFFERSIZE);
    sock->rcvbuf.size = TCP_BUFFERSIZE;
    return (struct socket *)sock;
cleanup:
    if (sock != NULL) {
        resource_free((struct resource *)sock);
    }
    return NULL;
}
