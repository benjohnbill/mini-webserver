#include "model.h"

int model_pool_run(int listen_fd, const struct config *config)
{
    /* SLOT(owner): SPEC §8 row 1 */
    (void)listen_fd;
    (void)config;
    return -1;
}
