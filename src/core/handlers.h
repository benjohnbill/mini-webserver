#ifndef HANDLERS_H
#define HANDLERS_H

#include "http_parse.h"
#include "response.h"

/* Each returns 0, or a status code when it made no plan. */
int handlers_echo(const struct request *request, struct response_plan *plan);
int handlers_tell_time(const struct request *request,
                       struct response_plan *plan);

#endif
