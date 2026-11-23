#include "handlers.h"

#include <time.h>

#include "serve.h"

#define TIME_TEXT_SIZE 64

int handlers_echo(const struct request *request, struct response_plan *plan)
{
    const char *content_type = request->content_type != NULL
                                   ? request->content_type
                                   : "application/octet-stream";
    if (response_plan_init(plan, 200, content_type, (off_t)request->body_len,
                           !serve_keeps_open(request, 200), true)) {
        plan->body = request->body;
        plan->body_len = request->body_len;
    }

    return 0;
}

int handlers_tell_time(const struct request *request,
                       struct response_plan *plan)
{
    char text[TIME_TEXT_SIZE];
    struct tm utc;
    time_t now = time(NULL);
    if (gmtime_r(&now, &utc) == NULL) {
        return 500;
    }
    strftime(text, sizeof(text), "{\"now\":\"%Y-%m-%dT%H:%M:%SZ\"}", &utc);
    response_plan_init_text(plan, 200, "application/json", text,
                            !serve_keeps_open(request, 200), true);

    return 0;
}
