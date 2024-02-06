#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include <ipc/socket.k.h>
#include <ipc/socket/tcp.k.h>
#include <ipc/socket/udp.k.h>
#include <ipc/socket/unix.k.h>
#include <lib/alloc.k.h>
#include <lib/errno.k.h>
#include <lib/panic.k.h>
#include <lib/print.k.h>
#include <lib/resource.k.h>
#include <lib/debug.k.h>
#include <sched/sched.k.h>
#include <sys/stat.h>
#include <poll.h>
#include <netinet/in.h>

static bool stub_bind(struct socket *this, struct f_description *description, void *addr, socklen_t len) {
    (void)this;
    (void)description;
    (void)addr;
    (void)len;
    errno = ENOSYS;
    return false;
}

static bool stub_connect(struct socket *this, struct f_description *description, void *addr, socklen_t len) {
    (void)this;
    (void)description;
    (void)addr;
    (void)len;
    errno = ENOSYS;
    return false;
}

static bool stub_getpeername(struct socket *this, struct f_description *description, void *addr, socklen_t *len) {
    (void)this;
    (void)description;
    (void)addr;
    (void)len;
    errno = ENOSYS;
    return false;
}

static bool stub_getsockname(struct socket *this, struct f_description *description, void *addr, socklen_t *len) {
    (void)this;
    (void)description;
    (void)addr;
    (void)len;
    errno = ENOSYS;
    return false;
}

static bool stub_listen(struct socket *this, struct f_description *description, int backlog) {
    (void)this;
    (void)description;
    (void)backlog;
    errno = ENOSYS;
    return false;
}

static struct socket *stub_accept(struct socket *this, struct f_description *description, struct socket *other, void *addr, socklen_t *len) {
    (void)this;
    (void)description;
    (void)other;
    (void)addr;
    (void)len;
    errno = ENOSYS;
    return false;
}

static ssize_t stub_recvmsg(struct socket *this, struct f_description *description, struct msghdr *msg, int flags) {
    (void)this;
    (void)description;
    (void)msg;
    (void)flags;
    errno = ENOSYS;
    return -1;
}

static ssize_t stub_sendmsg(struct socket *this, struct f_description *description, const struct msghdr *msg, int flags) {
    (void)this;
    (void)description;
    (void)msg;
    (void)flags;
    errno = ENOSYS;
    return -1;
}

static ssize_t stub_getsockopt(struct socket *this, struct f_description *description, int level, int optname, void *optval, socklen_t *optlen) {
    (void)this;
    (void)description;
    (void)level;
    (void)optname;
    (void)optval;
    (void)optlen;
    errno = ENOSYS;
    return -1;
}

static ssize_t stub_setsockopt(struct socket *this, struct f_description *description, int level, int optname, const void *optval, socklen_t optlen) {
    (void)this;
    (void)description;
    (void)level;
    (void)optname;
    (void)optval;
    (void)optlen;
    errno = ENOSYS;
    return -1;
}

void *socket_create(int family, int type, int protocol, int size) {
    struct socket *sock = resource_create(size);
    if (sock == NULL) {
        goto cleanup;
    }

    sock->stat.st_mode = S_IFSOCK;

    // XXX the following line should be uncommented, or even
    // better, it should be moved to resource_create... why is it not
    // like that already? we need to go over that one day and fix all
    // oversights related to refcounting resources

    // sock->refcount = 1;
    sock->state = SOCKET_CREATED;
    sock->family = family;
    sock->type = type;
    sock->protocol = protocol;

    sock->bind = stub_bind;
    sock->connect = stub_connect;
    sock->getpeername = stub_getpeername;
    sock->getsockname = stub_getsockname;
    sock->listen = stub_listen;
    sock->accept = stub_accept;
    sock->recvmsg = stub_recvmsg;
    sock->sendmsg = stub_sendmsg;
    sock->getsockopt = stub_getsockopt;
    sock->setsockopt = stub_setsockopt;

    return (struct socket *)sock;

cleanup:
    if (sock != NULL) {
        free(sock);
    }
    return NULL;
}

int syscall_socket(void *_, int family, int type, int protocol) {
    (void)_;

    DEBUG_SYSCALL_ENTER("socket(%d, %d, %d)", family, type, protocol);

    int ret = -1;

    struct thread *thread = sched_current_thread();
    struct process *proc = thread->process;

    size_t socktype = type;
    int flags = 0;
    if (type & SOCK_CLOEXEC) {
        flags |= O_CLOEXEC;
        socktype -= SOCK_CLOEXEC;
    }
    if (type & SOCK_NONBLOCK) {
        flags |= O_NONBLOCK;
        socktype -= SOCK_NONBLOCK;
    }

    struct socket *sock;
    switch (family) {
        case AF_UNIX:
            sock = socket_create_unix(socktype, protocol);
            break;
        case AF_INET:
            switch (socktype) {
                case SOCK_STREAM:
                    if (protocol == 0) {
                        protocol = IPPROTO_TCP;
                    }
                    sock = socket_create_tcp(socktype, protocol);
                    break;
                case SOCK_DGRAM:
                    if (protocol == 0) {
                        protocol = IPPROTO_UDP;
                    }
                    sock = socket_create_udp(socktype, protocol);
                    break;
                default:
                    errno = EINVAL;
                    goto cleanup;
            }
            break;
        default:
            errno = EINVAL;
            goto cleanup;
    }

    if (sock == NULL) {
        goto cleanup;
    } 

    ret = fdnum_create_from_resource(proc, (struct resource *)sock, flags, 0, false);
    if (ret == -1) {
        resource_free((struct resource *)sock);
        goto cleanup;
    }

cleanup:
    DEBUG_SYSCALL_LEAVE("%d", ret);
    return ret;
}

int syscall_setsockopt(void *_, int fdnum, int level, int optname, const void *optval, socklen_t optlen) {
    (void)_;

    DEBUG_SYSCALL_ENTER("setsockopt(%d, %d, %d, %lx, %lu)", fdnum, level, optname, optval, optlen);

    int ret = -1;

    struct thread *thread = sched_current_thread();
    struct process *proc = thread->process;

    struct f_descriptor *fd = fd_from_fdnum(proc, fdnum);
    if (fd == NULL) {
        goto cleanup;
    }

    struct f_description *desc = fd->description;
    if (S_ISSOCK(desc->res->stat.st_mode)) {
        struct socket *sock = (struct socket *)desc->res;

        if (sock->state != SOCKET_CREATED) {
            errno = EINVAL;
        } else {
            ret = sock->setsockopt(sock, desc, level, optname, optval, optlen);
        }
    } else {
        errno = ENOTSOCK;
    }

    desc->refcount--;

cleanup:
    DEBUG_SYSCALL_LEAVE("%d", ret);
    return ret;
}

int syscall_getsockopt(void *_, int fdnum, int level, int optname, void *optval, socklen_t *optlen) {
    (void)_;

    DEBUG_SYSCALL_ENTER("getsockopt(%d, %d, %d, %lx, %lx)", fdnum, level, optname, optval, optlen);

    int ret = -1;

    struct thread *thread = sched_current_thread();
    struct process *proc = thread->process;

    struct f_descriptor *fd = fd_from_fdnum(proc, fdnum);
    if (fd == NULL) {
        goto cleanup;
    }

    struct f_description *desc = fd->description;
    if (S_ISSOCK(desc->res->stat.st_mode)) {
        struct socket *sock = (struct socket *)desc->res;

        if (sock->state != SOCKET_CREATED) {
            errno = EINVAL;
        } else {
            ret = sock->getsockopt(sock, desc, level, optname, optval, optlen);
        }
    } else {
        errno = ENOTSOCK;
    }

    desc->refcount--;

cleanup:
    DEBUG_SYSCALL_LEAVE("%d", ret);
    return ret;
}

int syscall_socketpair(void *_, int domain, int type, int protocol, int *fds) {
    (void)_;

    DEBUG_SYSCALL_ENTER("socketpair(%d, %d, %d, %lx)", domain, type, protocol, fds);

    int ret = -1;

    struct thread *thread = sched_current_thread();
    struct process *proc = thread->process;

    int flags = 0;
    if (type & SOCK_CLOEXEC) {
        flags |= O_CLOEXEC;
    }
    if (type & SOCK_NONBLOCK) {
        flags |= O_NONBLOCK;
    }

    struct socket *sock0 = NULL;
    struct socket *sock1 = NULL;

    switch (domain) {
        case AF_UNIX: {
            sock0 = socket_create_unix(type, protocol);
            sock1 = socket_create_unix(type, protocol);
            sock0->peer = sock1;
            sock1->peer = sock0;
            sock0->state = SOCKET_CONNECTED;
            sock1->state = SOCKET_CONNECTED;

            sock0->status |= POLLOUT;
            sock1->status |= POLLOUT;
            break;
        }    
        default:
            errno = EINVAL;
            goto cleanup;
    }

    fds[0] = fdnum_create_from_resource(proc, (struct resource *)sock0, flags, 0, false);
    fds[1] = fdnum_create_from_resource(proc, (struct resource *)sock1, flags, 0, false);

    ret = 0;

cleanup:
    DEBUG_SYSCALL_LEAVE("%d", ret);
    return ret;
}

int syscall_bind(void *_, int fdnum, void *addr, socklen_t len) {
    (void)_;

    DEBUG_SYSCALL_ENTER("bind(%d, %lx, %lu)", fdnum, addr, len);

    int ret = -1;

    struct thread *thread = sched_current_thread();
    struct process *proc = thread->process;

    struct f_descriptor *fd = fd_from_fdnum(proc, fdnum);
    if (fd == NULL) {
        goto cleanup;
    }

    struct f_description *desc = fd->description;
    if (S_ISSOCK(desc->res->stat.st_mode)) {
        struct socket *sock = (struct socket *)desc->res;

        if (sock->state != SOCKET_CREATED) {
            errno = EINVAL;
        } else if (sock->bind(sock, desc, addr, len)) {
            sock->state = SOCKET_BOUND;
            ret = 0;
        }
    } else {
        errno = ENOTSOCK;
    }

    desc->refcount--;

cleanup:
    DEBUG_SYSCALL_LEAVE("%d", ret);
    return ret;
}

int syscall_connect(void *_, int fdnum, void *addr, socklen_t len) {
    (void)_;

    DEBUG_SYSCALL_ENTER("connect(%d, %lx, %lu)", fdnum, addr, len);

    int ret = -1;

    struct thread *thread = sched_current_thread();
    struct process *proc = thread->process;

    struct f_descriptor *fd = fd_from_fdnum(proc, fdnum);
    if (fd == NULL) {
        goto cleanup;
    }

    struct f_description *desc = fd->description;
    if (S_ISSOCK(desc->res->stat.st_mode)) {
        struct socket *sock = (struct socket *)desc->res;

        if (sock->state == SOCKET_CONNECTED) {
            errno = EISCONN;
        } else if (sock->state != SOCKET_CREATED) {
            errno = EINVAL;
        } else if (sock->connect(sock, desc, addr, len)) {
            sock->state = SOCKET_CONNECTED;
            ret = 0;
        }
    } else {
        errno = ENOTSOCK;
    }

    desc->refcount--;

cleanup:
    DEBUG_SYSCALL_LEAVE("%d", ret);
    return ret;
}

int syscall_listen(void *_, int fdnum, int backlog) {
    (void)_;

    DEBUG_SYSCALL_ENTER("listen(%d, %d)", fdnum, backlog);

    int ret = -1;

    struct thread *thread = sched_current_thread();
    struct process *proc = thread->process;

    struct f_descriptor *fd = fd_from_fdnum(proc, fdnum);
    if (fd == NULL) {
        goto cleanup;
    }

    struct f_description *desc = fd->description;
    if (S_ISSOCK(desc->res->stat.st_mode)) {
        struct socket *sock = (struct socket *)desc->res;

        if (sock->state != SOCKET_BOUND) {
            errno = EINVAL;
        } else if (sock->listen(sock, desc, backlog)) {
            sock->backlog_max = backlog;
            sock->backlog = alloc(backlog * sizeof(struct socket *));
            sock->state = SOCKET_LISTENING;
            ret = 0;
        }
    } else {
        errno = ENOTSOCK;
    }

    desc->refcount--;

cleanup:
    DEBUG_SYSCALL_LEAVE("%d", ret);
    return ret;
}

int syscall_accept(void *_, int fdnum, void *addr, socklen_t *len) {
    (void)_;

    DEBUG_SYSCALL_ENTER("accept(%d, %lx, %lx)", fdnum, addr, len);

    int ret = -1;

    struct thread *thread = sched_current_thread();
    struct process *proc = thread->process;

    struct f_descriptor *fd = fd_from_fdnum(proc, fdnum);
    if (fd == NULL) {
        goto cleanup;
    }

    struct f_description *desc = fd->description;
    if (S_ISSOCK(desc->res->stat.st_mode)) {
        struct socket *sock = (struct socket *)desc->res;

        if (sock->state != SOCKET_LISTENING) {
            errno = EINVAL;
        } else if (sock->family == AF_UNIX) { // UNIX domain sockets act differently (we know and can modify sockets on both sides, rather than just one end)
            spinlock_acquire(&sock->lock);

            while (sock->backlog_i == 0) {
                sock->status &= ~POLLIN;
                if ((desc->flags & O_NONBLOCK) != 0) {      
                    errno = EWOULDBLOCK;
                    goto cleanup1;
                }

                spinlock_release(&sock->lock);

                struct event *sock_event = &sock->event;
                ssize_t index = event_await(&sock_event, 1, true);
                if (index == -1) {
                    errno = EINTR;
                    goto cleanup1;
                }

                spinlock_acquire(&sock->lock);
            }

            struct socket *peer = sock->backlog[0];
            for (size_t i = 1; i < sock->backlog_i; i++) {
                sock->backlog[i - 1] = sock->backlog[i];
            }
            sock->backlog_i--;

            struct socket *connection_socket = sock->accept(sock, desc, peer, addr, len);
            if (connection_socket == NULL) {
                goto cleanup1;
            }

            peer->refcount++;
            peer->peer = connection_socket;
            peer->state = SOCKET_CONNECTED;
            if (sock->backlog_i == 0) {
                sock->status &= ~POLLIN;
            }

            event_trigger(&peer->connect_event, false);

            struct event *conn_event = &sock->connect_event;
            ssize_t index = event_await(&conn_event, 1, true);
            if (index == -1) {
                errno = EINTR;
                goto cleanup1;
            }

            ret = fdnum_create_from_resource(proc, (struct resource *)connection_socket, 0, 0, false);

cleanup1:
            spinlock_release(&sock->lock);
        } else if (sock->family == AF_INET) { // INET sockets
            spinlock_acquire(&sock->lock);

            // rely on the socket implementation to await connections and such
            struct socket *connection_socket = sock->accept(sock, desc, NULL, addr, len);
            if (connection_socket == NULL) {
                goto cleanup2;
            }

            ret = fdnum_create_from_resource(proc, (struct resource *)connection_socket, 0, 0, false);
cleanup2:
            spinlock_release(&sock->lock);
        }
    } else {
        errno = ENOTSOCK;
    }

    desc->refcount--;

cleanup:
    DEBUG_SYSCALL_LEAVE("%d", ret);
    return ret;
}

int syscall_getpeername(void *_, int fdnum, void *addr, socklen_t *len) {
    (void)_;

    DEBUG_SYSCALL_ENTER("getpeername(%d, %lx, %lx)", fdnum, addr, len);

    ssize_t ret = -1;

    struct thread *thread = sched_current_thread();
    struct process *proc = thread->process;

    struct f_descriptor *fd = fd_from_fdnum(proc, fdnum);
    if (fd == NULL) {
        goto cleanup;
    }

    struct f_description *desc = fd->description;
    if (S_ISSOCK(desc->res->stat.st_mode)) {
        struct socket *sock = (struct socket *)desc->res;

        if (sock->state != SOCKET_CONNECTED) {
            errno = ENOTCONN;
        } else if (sock->getpeername(sock, desc, addr, len)) {
            ret = 0;
        }
    } else {
        errno = ENOTSOCK;
    }

    desc->refcount--;

cleanup:
    DEBUG_SYSCALL_LEAVE("%d", ret);
    return ret;
}

int syscall_getsockname(void *_, int fdnum, void *addr, socklen_t *len) {
    (void)_;

    DEBUG_SYSCALL_ENTER("getsockname(%d, %lx, %lx)", fdnum, addr, len);

    ssize_t ret = -1;

    struct thread *thread = sched_current_thread();
    struct process *proc = thread->process;

    struct f_descriptor *fd = fd_from_fdnum(proc, fdnum);
    if (fd == NULL) {
        goto cleanup;
    }

    struct f_description *desc = fd->description;
    if (S_ISSOCK(desc->res->stat.st_mode)) {
        struct socket *sock = (struct socket *)desc->res;

        if (sock->getsockname(sock, desc, addr, len)) {
            ret = 0;
        }
    } else {
        errno = ENOTSOCK;
    }

    desc->refcount--;

cleanup:
    DEBUG_SYSCALL_LEAVE("%d", ret);
    return ret;
}

ssize_t syscall_sendmsg(void *_, int fdnum, const struct msghdr *msg, int flags) {
    (void)_;

    DEBUG_SYSCALL_ENTER("sendmsg(%d, %lx, %d)", fdnum, msg, flags);

    ssize_t ret = -1;

    struct thread *thread = sched_current_thread();
    struct process *proc = thread->process;

    struct f_descriptor *fd = fd_from_fdnum(proc, fdnum);
    if (fd == NULL) {
        goto cleanup;
    }

    struct f_description *desc = fd->description;
    if (S_ISSOCK(desc->res->stat.st_mode)) {
        struct socket *sock = (struct socket *)desc->res;

        if (sock->state != SOCKET_CONNECTED && sock->type != SOCK_DGRAM) {
            errno = ENOTCONN;
        } else {
            ret = sock->sendmsg(sock, desc, msg, flags);
        }
    } else {
        errno = ENOTSOCK;
    }

    desc->refcount--;
cleanup:
    DEBUG_SYSCALL_LEAVE("%d", ret);
    return ret;
}

ssize_t syscall_recvmsg(void *_, int fdnum, struct msghdr *msg, int flags) {
    (void)_;

    DEBUG_SYSCALL_ENTER("recvmsg(%d, %lx, %d)", fdnum, msg, flags);

    ssize_t ret = -1;

    struct thread *thread = sched_current_thread();
    struct process *proc = thread->process;

    struct f_descriptor *fd = fd_from_fdnum(proc, fdnum);
    if (fd == NULL) {
        goto cleanup;
    }

    struct f_description *desc = fd->description;
    if (S_ISSOCK(desc->res->stat.st_mode)) {
        struct socket *sock = (struct socket *)desc->res;

        if (sock->state != SOCKET_CONNECTED && sock->type != SOCK_DGRAM) { // if not connected *and* a type that supports connections
            errno = ENOTCONN;
        } else {
            ret = sock->recvmsg(sock, desc, msg, flags);
        }
    } else {
        errno = ENOTSOCK;
    }

    desc->refcount--;

cleanup:
    DEBUG_SYSCALL_LEAVE("%d", ret);
    return ret;
}
