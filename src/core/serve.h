#ifndef SERVE_H
#define SERVE_H

#include <stdbool.h>

#include "config.h"
#include "http_parse.h"
#include "response.h"

/* Opens files (stat, open). */
void serve_request(const struct request *request, const struct config *config,
                   struct response_plan *plan);
bool serve_keeps_open(const struct request *request, int status);

#endif
