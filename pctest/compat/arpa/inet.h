#ifndef PC_ARPA_INET_COMPAT_H
#define PC_ARPA_INET_COMPAT_H

#include "../sys/socket.h"

// htonl/htons/ntohl/ntohs, inet_addr, inet_ntoa, inet_pton, inet_ntop and
// getaddrinfo/freeaddrinfo all come from winsock2.h/ws2tcpip.h via
// sys/socket.h above.

#endif
