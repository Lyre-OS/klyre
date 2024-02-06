#ifndef _IPC__SOCKET__UDP_K_H
#define _IPC__SOCKET__UDP_K_H

#include <dev/net/net.k.h>

struct socket *socket_create_udp(int type, int protocol);
void udp_onudp(struct net_adapter *adapter, struct net_inetheader *inetheader, size_t length);

#endif
