#ifndef IPC__SOCKET__UNIX_K_H_
#define IPC__SOCKET__UNIX_K_H_

#include <ipc/socket.k.h>

struct socket *socket_create_unix(int type, int protocol);

#endif
