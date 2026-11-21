#ifndef CHECK_H
#define CHECK_H

#include <stdio.h>
#include <stdlib.h>

#define CHECK(cond)                                                            \
    do {                                                                       \
        if (!(cond)) {                                                         \
            fprintf(stderr, "%s:%d: %s: CHECK(%s) failed\n", __FILE__,         \
                    __LINE__, __func__, #cond);                                \
            exit(1);                                                           \
        }                                                                      \
    } while (0)

#define CHECK_EQ_INT(actual, expected)                                         \
    do {                                                                       \
        long long check_actual = (actual);                                     \
        long long check_expected = (expected);                                 \
        if (check_actual != check_expected) {                                  \
            fprintf(stderr, "%s:%d: %s: %s is %lld, expected %lld\n",          \
                    __FILE__, __LINE__, __func__, #actual, check_actual,       \
                    check_expected);                                           \
            exit(1);                                                           \
        }                                                                      \
    } while (0)

#endif
