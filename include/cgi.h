#ifndef CGI_H
#define CGI_H

#include <stddef.h>
#include "http.h"
#include "server.h"

/* Internal: CGI/1.1 execution (RFC 3875) for requests under /cgi-bin/. */

#define CGI_TIMEOUT_MS 5000           /* script wall-clock limit -> 504 */
#define CGI_MAX_OUTPUT (4u << 20)     /* stdout cap -> 500 */

typedef struct {
    int status;                       /* from "Status:" or 200 / 302 */
    char content_type[256];
    char headers[2048];               /* extra "Name: value\r\n" lines */
    char *output;                     /* raw script stdout (malloc'd) */
    const char *body;                 /* points into output */
    size_t body_len;
} cgi_result;

/* Run the script named by req->path ("/cgi-bin/<name>[/path-info]") inside
 * cfg->cgi_dir. Returns 0 and fills *res (free with cgi_result_free), or an
 * HTTP error status for the caller to send: 403, 404, 500 or 504. */
int cgi_run(const http_request *req, const server_config *cfg,
            const char *client_ip, cgi_result *res);

void cgi_result_free(cgi_result *res);

#endif
