#ifndef _IPC__SOCKET__TCP_K_H
#define _IPC__SOCKET__TCP_K_H

#include <dev/net/net.k.h>

struct socket *socket_create_tcp(int type, int protocol);
void tcp_ontcp(struct net_adapter *adapter, struct net_inetheader *inetheader, size_t length);

#endif
