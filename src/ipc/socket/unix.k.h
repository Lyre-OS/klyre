#ifndef _IPC__SOCKET__UNIX_K_H
#define _IPC__SOCKET__UNIX_K_H

#include <ipc/socket.k.h>

struct socket *socket_create_unix(int type, int protocol);

#endif
