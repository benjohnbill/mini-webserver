#include "model.h"

int model_fork_run(int listen_fd, const struct config *config)
{
    /* SLOT(owner): SPEC §8 row 3 */
    (void)listen_fd;
    (void)config;
    return -1;
}
