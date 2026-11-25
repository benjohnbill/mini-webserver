/* Unit tests for -B / -W parsing. */
#include "server.h"
#include "test.h"

static server_config cfg;             /* route_table is too big for the stack */

static int live;                     /* cfg.live holds a table from the last good parse */

static int parse(const char *a, const char *b)
{
    char *argv[] = { "webserver", "-r", ".", "-c", ".", (char *)a, (char *)b, NULL };
    if (live)
        route_live_destroy(&cfg.live);
    int rc = config_parse_args(&cfg, a ? 7 : 5, argv);
    live = rc == 0;
    return rc;
}

int main(void)
{
    CHECK(parse(NULL, NULL) == 0);
    CHECK(cfg.backend_cap == 4);      /* default threads 8 / 2 */
    CHECK(cfg.queue_wait_max_ms == 2000);

    CHECK(parse("-B", "0") == 0);
    CHECK(cfg.backend_cap == 0);
    CHECK(parse("-B", "1024") == 0);
    CHECK(cfg.backend_cap == 1024);
    CHECK(parse("-B", "1025") == -1);
    CHECK(parse("-B", "-1") == -1);

    CHECK(parse("-W", "0") == 0);
    CHECK(cfg.queue_wait_max_ms == 0);
    CHECK(parse("-W", "600000") == 0);
    CHECK(cfg.queue_wait_max_ms == 600000);
    CHECK(parse("-W", "600001") == -1);
    CHECK(parse("-W", "5x") == -1);
    if (live)
        route_live_destroy(&cfg.live);
    TEST_MAIN_END();
}
