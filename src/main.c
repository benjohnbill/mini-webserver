#include <signal.h>
#include <stdlib.h>

#include "server.h"
#include "server_run.h"

int main(int argc, char **argv)
{
    /* Keep startup SIGHUP pending until server_run can handle it. */
    sigset_t block;
    sigemptyset(&block);
    sigaddset(&block, SIGHUP);
    sigprocmask(SIG_BLOCK, &block, NULL);

    /* static: the config embeds the route table and two PATH_MAX buffers */
    static server_config cfg;

    if (config_parse_args(&cfg, argc, argv) != 0)
        return 2;
    return server_run(&cfg) == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
