#ifndef JA4_H
#define JA4_H

#include "client_hello.h"

/* Decision module of the Reverse Proxy. The rules are the FoxIO JA4
   document, technical_details/JA4.md. The transport is always TCP ("t"). */

/* "t13d1516h2_8daaf6152771_e5627efa2ab1": 10 + 1 + 12 + 1 + 12. */
#define JA4_LEN 36

/* Writes the JA4 of hello and a terminating NUL into ja4, which has
   JA4_LEN + 1 bytes. */
void ja4_format(const struct client_hello *hello, char *ja4);

#endif
