#ifndef CACHE_OPTIONS_H
#define CACHE_OPTIONS_H

#include <stdbool.h>

#include "config.h"

/* The --cache options, shared by the server and the Reverse Proxy, so that
   both have the same names, defaults, and meanings. */

/* Sets the defaults: the cache is off. */
void cache_options_init(struct cache_options *options);

/* Returns false if name is not a cache option or value is not valid. */
bool cache_options_parse(struct cache_options *options, const char *name,
                         const char *value);

#endif
