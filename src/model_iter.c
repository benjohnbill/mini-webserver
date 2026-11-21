#include "model.h"

int model_iter_run(int listen_fd, const struct config *config)
{
    /* SLOT(owner): SPEC §8 row 5 */
    (void)listen_fd;
    (void)config;
    return -1;
}
