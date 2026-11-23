#ifndef TLS_H
#define TLS_H

#include <stdbool.h>
#include <stddef.h>
#include <sys/types.h>

/* I/O layer of the Reverse Proxy: OpenSSL behind two opaque structs, so
   that no other file includes an OpenSSL header. */

/* The server context: the certificate, the key, and TLS 1.2 at least. */
struct tls_context;
/* One TLS connection over a blocking socket. */
struct tls_session;

/* Returns NULL and prints the OpenSSL errors if a file cannot be loaded. */
struct tls_context *tls_context_create(const char *cert_file,
                                       const char *key_file);
void tls_context_destroy(struct tls_context *context);

/* Does the server side of the handshake on conn_fd. The timeouts of
   conn_fd limit it. Returns NULL if the handshake fails. */
struct tls_session *tls_session_create(struct tls_context *context,
                                       int conn_fd);
/* Sends close_notify and frees the session. It does not close conn_fd. */
void tls_session_destroy(struct tls_session *session);

/* Reads up to size bytes. Returns the count, or 0 at the end or on an
   error (a timeout included). */
size_t tls_session_read(struct tls_session *session, char *buffer, size_t size);
/* Writes all len bytes. Returns false on an error. */
bool tls_session_send(struct tls_session *session, const char *bytes,
                      size_t len);

#endif
