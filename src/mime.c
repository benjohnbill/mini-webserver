#include "mime.h"

#include <stddef.h>
#include <string.h>

#define MIME_DEFAULT_TYPE "application/octet-stream"

struct mime_entry {
    const char *extension;
    const char *type;
};

static const struct mime_entry mime_entries[] = {
    {"html", "text/html"},     {"css", "text/css"},
    {"js", "text/javascript"}, {"json", "application/json"},
    {"txt", "text/plain"},     {"png", "image/png"},
    {"jpg", "image/jpeg"},     {"jpeg", "image/jpeg"},
    {"gif", "image/gif"},      {"svg", "image/svg+xml"},
    {"ico", "image/x-icon"},
};

const char *mime_find_type(const char *path)
{
    const char *dot = strrchr(path, '.');
    const char *slash = strrchr(path, '/');
    if (dot == NULL || (slash != NULL && dot < slash)) {
        return MIME_DEFAULT_TYPE;
    }

    for (size_t i = 0; i < sizeof(mime_entries) / sizeof(mime_entries[0]);
         i++) {
        if (strcmp(dot + 1, mime_entries[i].extension) == 0) {
            return mime_entries[i].type;
        }
    }
    return MIME_DEFAULT_TYPE;
}
