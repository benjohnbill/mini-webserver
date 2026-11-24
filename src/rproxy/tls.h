#ifndef TLS_H
#define TLS_H

#include <stddef.h>

/* I/O layer of the Reverse Proxy: OpenSSL behind two opaque structs, so
   that no other file includes an OpenSSL header. */

/* The server context: the certificate, the key, and TLS 1.2 at least. */
struct tls_context;
/* One TLS connection over a non-blocking socket. */
struct tls_session;

/* The result of one step. A step that wants read or write did not finish:
   call it again when the socket is ready for that direction. */
enum tls_status {
    TLS_DONE,
    TLS_WANT_READ,
    TLS_WANT_WRITE,
    TLS_CLOSED /* the peer closed, or an error */
};

/* Returns NULL and prints the OpenSSL errors if a file cannot be loaded. */
struct tls_context *tls_context_create(const char *cert_file,
                                       const char *key_file);
void tls_context_destroy(struct tls_context *context);

/* Makes a server session on conn_fd. It does no I/O: the handshake comes
   with tls_session_handshake. Returns NULL if memory cannot be set up. */
struct tls_session *tls_session_create(struct tls_context *context,
                                       int conn_fd);
/* Sends close_notify once, with no wait, and frees the session. It does
   not close conn_fd. */
void tls_session_destroy(struct tls_session *session);

/* One step of the server side of the handshake. */
enum tls_status tls_session_handshake(struct tls_session *session);

/* Reads up to size bytes. On TLS_DONE, *count is at least 1. */
enum tls_status tls_session_read(struct tls_session *session, char *buffer,
                                 size_t size, size_t *count);
/* Writes up to len bytes. On TLS_DONE, *count is at least 1 and can be
   less than len. After a want, call it again with the same bytes. */
enum tls_status tls_session_send(struct tls_session *session, const char *bytes,
                                 size_t len, size_t *count);

#endif
