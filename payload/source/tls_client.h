#ifndef TLS_CLIENT_H
#define TLS_CLIENT_H

#include <stddef.h>
#include <sys/types.h>

/* Outbound TCP connection, plain or TLS (BearSSL), with one API for both.
 *
 * Not thread safe: the trust anchors are loaded once into a process-wide cache without a
 * lock (hub_fetch is single threaded). hub_daemon builds it with -DNET_NO_TLS: plain TCP only,
 * TLS goes through hub_fetch, so BearSSL is on flash once (libbearssl.so.0). */

#define TLS_DEFAULT_CA_BUNDLE "/data/harmony/cacert.pem"

enum {
    NET_TLS_VERIFY = 0,   /* chain to a CA in the bundle + host name check */
    NET_TLS_PIN = 1,      /* the server key must match the pin (empty pin: any key, reported) */
    NET_TLS_INSECURE = 2  /* encryption only, any certificate */
};

/* SHA-256 of the server public key, as 64 hex characters */
#define NET_PIN_HEX_LEN 64

typedef struct net_conn net_conn;

typedef struct {
    int tls;                 /* 0 = plain TCP */
    int verify;              /* NET_TLS_* (TLS only) */
    const char *pin;         /* NET_TLS_PIN: expected key hash (hex), NULL or "" = accept any */
    const char *ca_bundle;   /* NET_TLS_VERIFY: PEM file, NULL = TLS_DEFAULT_CA_BUNDLE */
    int timeout_ms;          /* connect timeout, then the timeout of each read and write */
    const char *connect_addrs; /* NULL: resolve host. Else numeric addresses, comma separated,
                                * tried in order without a lookup (the caller resolved and
                                * checked them); host still names the server: SNI, certificate */
} net_conn_opts;

/* Connects (and completes the TLS handshake). NULL on failure with a message in err.
 * pin_out (NET_PIN_HEX_LEN + 1 bytes, may be NULL) receives the server key hash on TLS. */
net_conn *net_conn_open(const char *host, int port, const net_conn_opts *opts,
                        char *pin_out, char *err, size_t err_sz);
/* Writes everything (and flushes TLS). 0 or -1. */
int net_conn_write(net_conn *c, const void *buf, size_t len);
/* Up to len bytes: > 0, 0 at the end of the stream, -1 on error or timeout. */
ssize_t net_conn_read(net_conn *c, void *buf, size_t len);
void net_conn_close(net_conn *c);
/* Message of the last error on c (TLS alert, timeout ...). */
const char *net_conn_error(const net_conn *c);

#endif
