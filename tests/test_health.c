/* Unit tests for the /_lb/status table: columns stay aligned when a value
 * is wider than the default column. */
#include "health.h"
#include "test.h"

static server_config cfg;             /* route_table is too big for the stack */

/* Offset where `word` starts in line, or -1. */
static long col(const char *line, const char *word)
{
    const char *s = strstr(line, word);
    return s ? (long)(s - line) : -1;
}

int main(void)
{
    char out[8192];
    route_table_init(&cfg.routes);
    cfg.port = 8080;
    cfg.threads = 4;
    cfg.queue_size = 256;
    cfg.backend_cap = 2;
    CHECK(route_table_add(&cfg.routes,
                          "/echo=[::ffff:127.0.0.1]:19256,127.0.0.1:19257") == 0);
    backend *b = &cfg.routes.routes[0].backends[1];
    b->inflight = 2;                  /* "2/2 FULL" */
    b->req_ok = 123456789012UL;       /* wider than the OK column */

    CHECK(lb_status_render(out, sizeof out, &cfg, 1, 0, 0, 0) > 0);
    printf("%s", out);

    char *ln[8] = { 0 };
    int count = 0;
    for (char *s = strtok(out, "\n"); s && count < 8; s = strtok(NULL, "\n"))
        ln[count++] = s;
    CHECK(count == 5);                /* front, route, header, 2 rows */
    if (count == 5) {
        CHECK(col(ln[2], "STATE") > 0);
        CHECK(col(ln[3], "UP") == col(ln[2], "STATE"));
        CHECK(col(ln[4], "UP") == col(ln[2], "STATE"));
        CHECK(col(ln[4], "2/2 FULL") == col(ln[2], "INFLIGHT"));
        CHECK(col(ln[4], "123456789012") == col(ln[2], "OK"));
        CHECK(col(ln[3], "0s ago") == col(ln[2], "CHANGED"));
        CHECK(col(ln[4], "0s ago") == col(ln[2], "CHANGED"));
    }
    TEST_MAIN_END();
}
