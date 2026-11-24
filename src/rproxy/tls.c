#include "tls.h"

#include <openssl/err.h>
#include <openssl/ssl.h>
#include <stdlib.h>

struct tls_context {
    SSL_CTX *ssl_context;
};

struct tls_session {
    SSL *ssl;
};

static enum tls_status session_get_status(struct tls_session *session,
                                          int result);

struct tls_context *tls_context_create(const char *cert_file,
                                       const char *key_file)
{
    struct tls_context *context = malloc(sizeof(*context));
    if (context == NULL) {
        return NULL;
    }
    context->ssl_context = SSL_CTX_new(TLS_server_method());
    if (context->ssl_context == NULL ||
        SSL_CTX_set_min_proto_version(context->ssl_context, TLS1_2_VERSION) !=
            1 ||
        SSL_CTX_use_certificate_chain_file(context->ssl_context, cert_file) !=
            1 ||
        SSL_CTX_use_PrivateKey_file(context->ssl_context, key_file,
                                    SSL_FILETYPE_PEM) != 1 ||
        SSL_CTX_check_private_key(context->ssl_context) != 1) {
        ERR_print_errors_fp(stderr);
        tls_context_destroy(context);
        return NULL;
    }
    /* A send can finish part of the bytes, and the caller can send the
       rest from a buffer that moved (SSL_CTX_set_mode(3)). */
    SSL_CTX_set_mode(context->ssl_context,
                     SSL_MODE_ENABLE_PARTIAL_WRITE |
                         SSL_MODE_ACCEPT_MOVING_WRITE_BUFFER);

    return context;
}

void tls_context_destroy(struct tls_context *context)
{
    if (context == NULL) {
        return;
    }
    SSL_CTX_free(context->ssl_context);
    free(context);
}

struct tls_session *tls_session_create(struct tls_context *context, int conn_fd)
{
    struct tls_session *session = malloc(sizeof(*session));
    if (session == NULL) {
        return NULL;
    }
    session->ssl = SSL_new(context->ssl_context);
    if (session->ssl == NULL || SSL_set_fd(session->ssl, conn_fd) != 1) {
        ERR_clear_error();
        SSL_free(session->ssl);
        free(session);
        return NULL;
    }
    SSL_set_accept_state(session->ssl);

    return session;
}

/* One SSL_shutdown only: a second call would wait for the close_notify
   of the client, and rproxy closes the socket after this. */
void tls_session_destroy(struct tls_session *session)
{
    if (session == NULL) {
        return;
    }
    if (SSL_is_init_finished(session->ssl)) {
        SSL_shutdown(session->ssl);
    }
    ERR_clear_error();
    SSL_free(session->ssl);
    free(session);
}

enum tls_status tls_session_handshake(struct tls_session *session)
{
    return session_get_status(session, SSL_do_handshake(session->ssl));
}

enum tls_status tls_session_read(struct tls_session *session, char *buffer,
                                 size_t size, size_t *count)
{
    *count = 0;

    return session_get_status(session,
                              SSL_read_ex(session->ssl, buffer, size, count));
}

enum tls_status tls_session_send(struct tls_session *session, const char *bytes,
                                 size_t len, size_t *count)
{
    *count = 0;

    return session_get_status(session,
                              SSL_write_ex(session->ssl, bytes, len, count));
}

/* result is the return value of an SSL call: 1 is success. A failed
   step is the fault of the client: drop the errors. */
static enum tls_status session_get_status(struct tls_session *session,
                                          int result)
{
    if (result == 1) {
        return TLS_DONE;
    }
    switch (SSL_get_error(session->ssl, result)) {
    case SSL_ERROR_WANT_READ:
        return TLS_WANT_READ;
    case SSL_ERROR_WANT_WRITE:
        return TLS_WANT_WRITE;
    default:
        ERR_clear_error();
        return TLS_CLOSED;
    }
}
