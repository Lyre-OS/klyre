#ifndef IPC__SOCKET__UDP_K_H_
#define IPC__SOCKET__UDP_K_H_

#include <dev/net/net.k.h>

struct socket *socket_create_udp(int type, int protocol);
void udp_onudp(struct net_adapter *adapter, struct net_inetheader *inetheader, size_t length);

#endif
