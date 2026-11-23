#ifndef CACHE_OPTIONS_H
#define CACHE_OPTIONS_H

#include <stdbool.h>

#include "config.h"

/* The --cache options of the Reverse Proxy. */

/* Sets the defaults: the cache is off. */
void cache_options_init(struct cache_options *options);

/* Returns false if name is not a cache option or value is not valid. */
bool cache_options_parse(struct cache_options *options, const char *name,
                         const char *value);

#endif
