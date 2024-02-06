#ifndef _IPC__SOCKET_K_H
#define _IPC__SOCKET_K_H

#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include <lib/event.k.h>
#include <lib/resource.k.h>
#include <sys/socket.h>
#include <dev/net/net.k.h>

enum socket_state {
    SOCKET_CREATED,
    SOCKET_BOUND,
    SOCKET_LISTENING,
    SOCKET_CONNECTED,
    SOCKET_CLOSED,
};

struct socket {
    struct resource;
    struct sockaddr_storage localaddr;
    struct sockaddr_storage peeraddr;

    struct socket **backlog;
    size_t backlog_max;
    size_t backlog_i;

    struct event connect_event;
    enum socket_state state;
    struct socket *peer;
    bool bound;

    int family;
    int type;
    int protocol;

    bool (*bind)(struct socket *this, struct f_description *description, void *addr, socklen_t len);
    bool (*connect)(struct socket *this, struct f_description *description, void *addr, socklen_t len);
    bool (*getpeername)(struct socket *this, struct f_description *description, void *addr, socklen_t *len);
    bool (*getsockname)(struct socket *this, struct f_description *description, void *addr, socklen_t *len);
    bool (*listen)(struct socket *this, struct f_description *description, int backlog);
    struct socket *(*accept)(struct socket *this, struct f_description *description, struct socket *other, void *addr, socklen_t *len);
    ssize_t (*recvmsg)(struct socket *this, struct f_description *description, struct msghdr *msg, int flags);
    ssize_t (*sendmsg)(struct socket *this, struct f_description *description, const struct msghdr *msg, int flags);
    ssize_t (*getsockopt)(struct socket *this, struct f_description *description, int level, int optname, void *optval, socklen_t *optlen);
    ssize_t (*setsockopt)(struct socket *this, struct f_description *description, int level, int optname, const void *optval, socklen_t optlen);
};

struct inetsocket {
    struct socket;
    struct net_adapter *adapter;

    be_uint16_t port; // redundancy
    be_uint16_t destport; // redundancy
    bool canbroadcast;
    bool canroute;
};

void *socket_create(int family, int type, int protocol, int size);
bool socket_add_to_backlog(struct socket *sock, struct socket *other);

#endif
