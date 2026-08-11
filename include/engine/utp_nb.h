#ifndef UTP_NB_H
#define UTP_NB_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// Non-blocking µTP transport for the streaming netloop.
//
// One UDP socket is shared by all µTP sessions; the caller polls it and calls
// utp_nb_service() whenever it becomes readable. Outgoing data is buffered and
// pushed to libutp via UTP_Write; incoming data is delivered through the
// per-session read buffer.

typedef struct utp_nb_sess utp_nb_sess;

// Create/destroy the shared UDP socket and libutp context. Safe to call
// init() multiple times (subsequent calls are no-ops).
int  utp_nb_init(void);
void utp_nb_exit(void);

// The UDP socket to add to poll(). Returns -1 if not initialized.
int  utp_nb_fd(void);

// Feed incoming UDP packets to libutp and tick its timeouts. Call after the
// fd returned by utp_nb_fd() is readable, and also periodically on timeout.
void utp_nb_service(void);

// Start an outgoing µTP connection. Returns NULL on immediate failure.
utp_nb_sess *utp_nb_connect(uint32_t ip_net, uint16_t port_host);

// Close a connection. The handle must not be used afterwards.
void utp_nb_close(utp_nb_sess *s);

// Read up to len bytes from the session buffer. Returns bytes read, 0 if no
// data is available yet, or -1 on error/EOF.
int utp_nb_read(utp_nb_sess *s, void *buf, int len);

// Write data. All bytes are queued; returns len, or -1 on error.
int utp_nb_write(utp_nb_sess *s, const void *buf, int len);

// 1 = connected, 0 = still connecting, -1 = error/closed.
int utp_nb_state(const utp_nb_sess *s);

#ifdef __cplusplus
}
#endif

#endif
