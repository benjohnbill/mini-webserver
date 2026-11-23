#include "client_hello.h"

enum client_hello_status client_hello_parse(const unsigned char *bytes,
                                            size_t len,
                                            struct client_hello *hello)
{
    (void)bytes;
    (void)len;
    (void)hello;

    return CLIENT_HELLO_INVALID;
}
