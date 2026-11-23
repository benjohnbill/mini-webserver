#include "ja4_table.h"

size_t ja4_table_parse(const char *text, struct ja4_table *fingerprints)
{
    (void)text;
    fingerprints->count = 0;

    return 1;
}

const char *ja4_table_find(const struct ja4_table *fingerprints,
                           const char *ja4)
{
    (void)fingerprints;
    (void)ja4;

    return NULL;
}
