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
    if (session->ssl == NULL || SSL_set_fd(session->ssl, conn_fd) != 1 ||
        SSL_accept(session->ssl) != 1) {
        /* A failed handshake is the client's fault: drop the errors. */
        ERR_clear_error();
        SSL_free(session->ssl);
        free(session);
        return NULL;
    }

    return session;
}

void tls_session_destroy(struct tls_session *session)
{
    if (session == NULL) {
        return;
    }
    SSL_shutdown(session->ssl);
    ERR_clear_error();
    SSL_free(session->ssl);
    free(session);
}

size_t tls_session_read(struct tls_session *session, char *buffer, size_t size)
{
    size_t count = 0;
    if (SSL_read_ex(session->ssl, buffer, size, &count) != 1) {
        ERR_clear_error();
        return 0;
    }

    return count;
}

bool tls_session_send(struct tls_session *session, const char *bytes,
                      size_t len)
{
    for (size_t sent = 0; sent < len;) {
        size_t written = 0;
        if (SSL_write_ex(session->ssl, bytes + sent, len - sent, &written) !=
            1) {
            ERR_clear_error();
            return false;
        }
        sent += written;
    }

    return true;
}
