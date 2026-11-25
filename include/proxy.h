#ifndef PROXY_H
#define PROXY_H

#include <stddef.h>
#include "http.h"
#include "route.h"

/* Open a TCP connection with a connect timeout (non-blocking connect +
 * poll). Also applies timeout_ms as SO_RCVTIMEO/SO_SNDTIMEO.
 * Returns fd or -1. */
int tcp_connect_timeout(const char *host, const char *port, int timeout_ms);

/* Split "http://host[:port]/path?q" into parts. port defaults to "80",
 * path defaults to "/". Only the http scheme is accepted.
 * Returns 0 on success, -1 on malformed / unsupported URI. */
int parse_absolute_uri(const char *uri, char *host, size_t host_size,
                       char *port, size_t port_size,
                       char *path, size_t path_size);

/* Build the upstream request into out (method, target, HTTP/1.1, headers
 * minus hop-by-hop ones, Host, Connection: close, X-Forwarded-For when
 * client_ip != NULL, Content-Length, then body). Returns bytes written or
 * -1 if it does not fit. Pure function: unit-testable. */
long proxy_build_request(const http_request *req, const char *target,
                         const char *host_header, const char *client_ip,
                         char *out, size_t out_size);

/* Relay req to backend b and stream the backend's response back to
 * client_fd unchanged. The caller closes the client connection afterwards.
 * Returns 0 if a response was relayed; otherwise the status the caller
 * should send: 502 (connect/IO failure) or 504 (timeout before any byte). */
int proxy_reverse(int client_fd, const http_request *req, const backend *b,
                  int timeout_ms, const char *client_ip);

/* Same as proxy_reverse but on an already connected upstream fd, which
 * it always closes. Lets the caller retry other backends on connect
 * failure without ever resending a request. */
int proxy_reverse_fd(int client_fd, int upstream, const http_request *req,
                     const backend *b, const char *client_ip);

/* Forward proxy: req->uri is absolute ("http://..."). Same return
 * convention as proxy_reverse; 400 for an unparsable URI. */
int proxy_forward(int client_fd, const http_request *req, int timeout_ms,
                  const char *client_ip);

#endif
