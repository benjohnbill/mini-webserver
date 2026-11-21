/* File extension -> Content-Type. Text types carry an explicit charset so
 * browsers do not have to guess the encoding. */
#include "server.h"

#include <string.h>
#include <strings.h>

static const struct {
    const char *ext;
    const char *type;
} mime_table[] = {
    { "html", "text/html; charset=utf-8" },
    { "htm",  "text/html; charset=utf-8" },
    { "css",  "text/css; charset=utf-8" },
    { "js",   "text/javascript; charset=utf-8" },
    { "mjs",  "text/javascript; charset=utf-8" },
    { "json", "application/json; charset=utf-8" },
    { "txt",  "text/plain; charset=utf-8" },
    { "xml",  "application/xml; charset=utf-8" },
    { "svg",  "image/svg+xml; charset=utf-8" },
    { "png",  "image/png" },
    { "jpg",  "image/jpeg" },
    { "jpeg", "image/jpeg" },
    { "gif",  "image/gif" },
    { "ico",  "image/x-icon" },
    { "webp", "image/webp" },
    { "pdf",  "application/pdf" },
    { "wasm", "application/wasm" },
};

const char *mime_type_for_path(const char *path)
{
    if (path == NULL)
        return "application/octet-stream";
    /* Only look at the last path segment so "/a.b/file" has no extension. */
    const char *base = strrchr(path, '/');
    base = base ? base + 1 : path;
    const char *dot = strrchr(base, '.');
    if (dot == NULL || dot == base || dot[1] == '\0')
        return "application/octet-stream"; /* none, dotfile, trailing dot */

    for (size_t i = 0; i < sizeof mime_table / sizeof mime_table[0]; i++)
        if (strcasecmp(dot + 1, mime_table[i].ext) == 0)
            return mime_table[i].type;
    return "application/octet-stream";
}
