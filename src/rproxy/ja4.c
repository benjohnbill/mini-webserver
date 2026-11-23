#include "ja4.h"

void ja4_format(const struct client_hello *hello, char *ja4)
{
    /* SLOT(owner): SPEC §11 "Code the owner types" row 1 */
    (void)hello;
    ja4[0] = '\0';
}
