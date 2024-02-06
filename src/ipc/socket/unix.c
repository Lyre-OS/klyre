#include <stdint.h>
#include <stddef.h>
#include <fs/vfs/vfs.k.h>
#include <ipc/socket.k.h>
#include <ipc/socket/unix.k.h>
#include <lib/alloc.k.h>
#include <lib/errno.k.h>
#include <lib/print.k.h>
#include <sched/sched.k.h>
#include <sys/un.h>
#include <poll.h>
#include <sys/stat.h>

#define SOCK_BUFFER_SIZE 0x4000

struct unix_socket {
    struct socket;

    void *data;
    size_t capacity;
    size_t read_ptr;
    size_t write_ptr;
    size_t used;
};

static ssize_t unix_read(struct resource *_this, struct f_description *description, void *buf, off_t offset, size_t count) {
    (void)offset;

    struct unix_socket *this = (struct unix_socket *)_this;

    ssize_t ret = -1;
    spinlock_acquire(&this->lock);

    while (this->used == 0) {
        // XXX uncomment this to properly return EOF
        // if (this->refcount < 2) {
        //     ret = 0;
        //     goto cleanup;
        // }

        if ((description->flags & O_NONBLOCK) != 0) {
            errno = EWOULDBLOCK;
            goto cleanup;
        }

        spinlock_release(&this->lock);

        struct event *events[] = {&this->event};
        if (event_await(events, 1, true) < 0) {
            errno = EINTR;
            goto cleanup;
        }

        spinlock_acquire(&this->lock);
    }

    if (this->used < count) {
        count = this->used;
    }

    size_t before_wrap = 0, after_wrap = 0, new_ptr = 0;
    if (this->read_ptr + count > this->capacity) {
        before_wrap = this->capacity - this->read_ptr;
        after_wrap = count - before_wrap;
        new_ptr = after_wrap;
    } else {
        before_wrap = count;
        after_wrap = 0;
        new_ptr = this->read_ptr + count;

        if (new_ptr == this->capacity) {
            new_ptr = 0;
        }
    }

    memcpy(buf, this->data + this->read_ptr, before_wrap);
    if (after_wrap != 0) {
        memcpy(buf + before_wrap, this->data, after_wrap);
    }

    this->read_ptr = new_ptr;
    this->used -= count;
    this->peer->status |= POLLOUT;

    event_trigger(&this->peer->event, false);

    this->status &= ~POLLIN;
    ret = count;

cleanup:
    spinlock_release(&this->lock);
    return ret;
}

static ssize_t unix_write(struct resource *_this, struct f_description *description, const void *buf, off_t offset, size_t count) {
    (void)description;
    (void)offset;

    struct unix_socket *this = (struct unix_socket *)_this;
    struct unix_socket *peer = (struct unix_socket *)this->peer;

    ssize_t ret = -1;
    spinlock_acquire(&peer->lock);

    while (peer->used == peer->capacity) {
        if ((description->flags & O_NONBLOCK) != 0) {
            errno = EWOULDBLOCK;
            goto cleanup;
        }

        spinlock_release(&peer->lock);

        struct event *events[] = {&peer->event};
        if (event_await(events, 1, true) < 0) {
            errno = EINTR;
            goto cleanup;
        }

        spinlock_acquire(&peer->lock);
    }

    if (peer->used + count > peer->capacity) {
        count = peer->capacity - peer->used;
    }

    size_t before_wrap = 0, after_wrap = 0, new_ptr = 0;
    if (peer->write_ptr + count > peer->capacity) {
        before_wrap = peer->capacity - peer->write_ptr;
        after_wrap = count - before_wrap;
        new_ptr = after_wrap;
    } else {
        before_wrap = count;
        after_wrap = 0;
        new_ptr = peer->write_ptr + count;

        if (new_ptr == peer->capacity) {
            new_ptr = 0;
        }
    }

    memcpy(peer->data + peer->write_ptr, buf, before_wrap);
    if (after_wrap != 0) {
        memcpy(peer->data, buf + before_wrap, after_wrap);
    }

    peer->write_ptr = new_ptr;
    peer->used += count;
    peer->status |= POLLIN;

    event_trigger(&peer->event, false);
    ret = count;

cleanup:
    spinlock_release(&peer->lock);
    return ret;
}

static bool unix_bind(struct socket *this, struct f_description *description, void *addr_, socklen_t len) {
    (void)description;
    (void)len;

    struct sockaddr_un *addr = (struct sockaddr_un *)addr_;
    if (addr->sun_family != AF_UNIX) {
        errno = EINVAL;
        return false;
    }

    struct thread *thread = sched_current_thread();
    struct process *proc = thread->process;
    struct vfs_node *node = vfs_create(proc->cwd, addr->sun_path, S_IFSOCK);
    if (node == NULL) {
        return false;
    }

    this->stat = node->resource->stat;
    node->resource = (struct resource *)this;
    memcpy(&this->localaddr, addr, sizeof(struct sockaddr_un));
    this->bound = true;
    return true;
}

static bool unix_connect(struct socket *_this, struct f_description *description, void *_addr, socklen_t len) {
    (void)description;
    (void)len;

    struct sockaddr_un *addr = (struct sockaddr_un *)_addr;
    if (addr->sun_family != AF_UNIX) {
        errno = EINVAL;
        return false;
    }

    struct thread *thread = sched_current_thread();
    struct process *proc = thread->process;
    struct vfs_node *node = vfs_get_node(proc->cwd, addr->sun_path, true);
    if (node == NULL) {
        return false;
    }

    memcpy(&_this->localaddr, addr, sizeof(struct sockaddr_un));

    struct socket *sock;
    if (S_ISSOCK(node->resource->stat.st_mode)) {
        sock = (struct socket *)node->resource;

        if (sock->family != AF_UNIX) {
            errno = EINVAL;
            return false;
        }
    } else {
        errno = ENOTSOCK;
        return false;
    }

    if (sock->state != SOCKET_LISTENING) {
        errno = ECONNREFUSED;
        return false;
    }

    spinlock_acquire(&sock->lock);
    if (!socket_add_to_backlog(sock, _this)) {
        spinlock_release(&sock->lock);
        return false;
    }

    event_trigger(&sock->event, false);
    spinlock_release(&sock->lock);

    struct event *conn_event = &_this->connect_event;
    ssize_t index = event_await(&conn_event, 1, true);
    if (index == -1) {
        errno = EINTR;
        return false;
    }

    event_trigger(&sock->connect_event, false);
    _this->status |= POLLOUT;
    event_trigger(&_this->event, false);

    return true;
}

static bool unix_getpeername(struct socket *this, struct f_description *description, void *addr, socklen_t *len) {
    (void)description;

    size_t actual_len = *len;
    if (actual_len < sizeof(struct sockaddr_un)) {
        actual_len = sizeof(struct sockaddr_un);
    }

    memcpy(addr, &this->peeraddr, actual_len);
    *len = actual_len;
    return true;
}

static bool unix_getsockname(struct socket *this, struct f_description *description, void *addr, socklen_t *len) {
    (void)description;

    if (!this->bound) {
        return true;
    }

    size_t actual_len = *len;
    if (actual_len < sizeof(struct sockaddr_un)) {
        actual_len = sizeof(struct sockaddr_un);
    }

    memcpy(addr, &this->localaddr, actual_len);
    *len = actual_len;
    return true;
}

static bool unix_listen(struct socket *this_, struct f_description *description, int backlog) {
    (void)this_;
    (void)description;
    (void)backlog;
    return true;
}

static struct socket *unix_accept(struct socket *_this, struct f_description *description, struct socket *other, void *_addr, socklen_t *len) {
    (void)description;

    struct unix_socket *sock = (struct unix_socket *)socket_create_unix(_this->type, _this->protocol);
    if (sock == NULL) {
        return NULL;
    }

    struct sockaddr_un *addr = (struct sockaddr_un *)_addr;
    *addr = *(struct sockaddr_un *)&other->localaddr;
    *len = sizeof(struct sockaddr_un);

    sock->peer = other;
    sock->state = SOCKET_CONNECTED;
    sock->peeraddr = other->localaddr;
    return (struct socket *)sock;
}

static ssize_t unix_recvmsg(struct socket *_this, struct f_description *description, struct msghdr *msg, int flags) {
    ASSERT_MSG(flags == 0, "Unix domain sockets don't support flags");

    struct unix_socket *this = (struct unix_socket *)_this;

    ssize_t ret = -1;
    spinlock_acquire(&this->lock);

    size_t count = 0;
    for (size_t i = 0; i < msg->msg_iovlen; i++) {
        count += msg->msg_iov[i].iov_len;
    }

    while (this->used == 0) {
        // XXX uncomment this to properly return EOF
        // if (this->refcount < 2) {
        //     ret = 0;
        //     goto cleanup;
        // }

        this->peer->status |= POLLOUT;
        event_trigger(&this->peer->event, false);

        if ((description->flags & O_NONBLOCK) != 0) {
            errno = EWOULDBLOCK;
            goto cleanup;
        }

        spinlock_release(&this->lock);

        struct event *events[] = {&this->event};
        if (event_await(events, 1, true) < 0) {
            errno = EINTR;
            goto cleanup;
        }

        spinlock_acquire(&this->lock);
    }

    if (this->used < count) {
        count = this->used;
    }

    size_t before_wrap = 0, after_wrap = 0, new_ptr = 0;
    if (this->read_ptr + count > this->capacity) {
        before_wrap = this->capacity - this->read_ptr;
        after_wrap = count - before_wrap;
        new_ptr = after_wrap;
    } else {
        before_wrap = count;
        after_wrap = 0;
        new_ptr = this->read_ptr + count;

        if (new_ptr == this->capacity) {
            new_ptr = 0;
        }
    }

    void *tmp_buffer = alloc(before_wrap + after_wrap);
    if (tmp_buffer == NULL) {
        goto cleanup;
    }

    memcpy(tmp_buffer, this->data + this->read_ptr, before_wrap);
    if (after_wrap != 0) {
        memcpy(tmp_buffer + before_wrap, this->data, after_wrap);
    }

    size_t transferred = 0;
    size_t remaining = before_wrap + after_wrap;
    for (size_t i = 0; i < msg->msg_iovlen; i++) {
        size_t transfer_count = MIN(msg->msg_iov[i].iov_len, remaining);
        memcpy(msg->msg_iov[i].iov_base, tmp_buffer + transferred, transfer_count);
        transferred += transfer_count;
        remaining -= transfer_count;
    }

    free(tmp_buffer);

    this->read_ptr = new_ptr;
    this->used -= transferred;
    this->peer->status |= POLLOUT;
    event_trigger(&this->peer->event, false);

    if (msg->msg_name != NULL && this->state == SOCKET_CONNECTED) {
        socklen_t actual_size = msg->msg_namelen;
        if (actual_size < sizeof(struct sockaddr_un)) {
            actual_size = sizeof(struct sockaddr_un);
        }

        memcpy(msg->msg_name, &this->peer->localaddr, actual_size);
        msg->msg_namelen = actual_size;
    }

    this->status &= ~POLLIN;
    ret = transferred;

cleanup:
    spinlock_release(&this->lock);
    return ret;
}

struct socket *socket_create_unix(int type, int protocol) {
    if (type != SOCK_STREAM) {
        errno = EINVAL;
        return NULL;
    }

    struct unix_socket *sock = socket_create(AF_UNIX, type, protocol, sizeof(struct unix_socket));
    if (sock == NULL) {
        goto cleanup;
    }

    sock->state = SOCKET_CREATED;
    sock->family = AF_UNIX;
    sock->type = type;
    sock->protocol = protocol;
    sock->data = alloc(SOCK_BUFFER_SIZE);
    sock->capacity = SOCK_BUFFER_SIZE;

    sock->stat.st_mode = S_IFSOCK;

    sock->read = unix_read;
    sock->write = unix_write;

    sock->bind = unix_bind;
    sock->connect = unix_connect;
    sock->getpeername = unix_getpeername;
    sock->getsockname = unix_getsockname;
	sock->listen = unix_listen;
    sock->accept = unix_accept;
    sock->recvmsg = unix_recvmsg;

    return (struct socket *)sock;

cleanup:
    if (sock != NULL) {
        resource_free((struct resource *)sock);
    }
    return NULL;
}

bool socket_add_to_backlog(struct socket *sock, struct socket *other) {
    if (sock->backlog_i == sock->backlog_max) {
        errno = EAGAIN; // XXX should this be EAGAIN?
        return false;
    }

    sock->status |= POLLIN;
    sock->backlog[sock->backlog_i++] = other;
    return true;
}
