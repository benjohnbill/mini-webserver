#include "ip_list.h"

size_t ip_list_parse(const char *text, struct ip_list *block_list)
{
    (void)text;
    block_list->count = 0;

    return 1;
}

bool ip_list_has_address(const struct ip_list *block_list, const char *address)
{
    (void)block_list;
    (void)address;

    return false;
}
