#include "cache_options.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>

#define names_count(names) ((int)(sizeof(names) / sizeof((names)[0])))

static int name_find(const char *value, const char *const *names, int count);
static bool number_parse(const char *text, long min, long max, int *number);

void cache_options_init(struct cache_options *options)
{
    *options = (struct cache_options){.is_on = false,
                                      .admit = CACHE_ADMIT_ALL,
                                      .evict = CACHE_EVICT_FIFO,
                                      .lock = CACHE_LOCK_GLOBAL,
                                      .bytes = DEFAULT_CACHE_BYTES,
                                      .object_max = DEFAULT_CACHE_OBJECT_MAX,
                                      .ttl = 0};
}

/* Each names table is in the order of its enum in config.h. */
bool cache_options_parse(struct cache_options *options, const char *name,
                         const char *value)
{
    static const char *const on_names[] = {"off", "on"};
    static const char *const admit_names[] = {"all", "size", "second"};
    static const char *const evict_names[] = {"fifo", "lru"};
    static const char *const lock_names[] = {"global", "striped"};
    int index = -1;
    if (strcmp(name, "--cache") == 0) {
        index = name_find(value, on_names, names_count(on_names));
        options->is_on = index == 1;
    } else if (strcmp(name, "--cache-admit") == 0) {
        index = name_find(value, admit_names, names_count(admit_names));
        options->admit = (enum cache_admit)index;
    } else if (strcmp(name, "--cache-evict") == 0) {
        index = name_find(value, evict_names, names_count(evict_names));
        options->evict = (enum cache_evict)index;
    } else if (strcmp(name, "--cache-lock") == 0) {
        index = name_find(value, lock_names, names_count(lock_names));
        options->lock = (enum cache_lock)index;
    } else if (strcmp(name, "--cache-bytes") == 0) {
        return number_parse(value, 1, CACHE_BYTES_MAX, &options->bytes);
    } else if (strcmp(name, "--cache-object-max") == 0) {
        return number_parse(value, 1, CACHE_BYTES_MAX, &options->object_max);
    } else if (strcmp(name, "--cache-ttl") == 0) {
        return number_parse(value, 0, CACHE_TTL_MAX, &options->ttl);
    }

    return index >= 0;
}

/* Returns the index of value in names, or -1. */
static int name_find(const char *value, const char *const *names, int count)
{
    for (int i = 0; i < count; i++) {
        if (strcmp(value, names[i]) == 0) {
            return i;
        }
    }

    return -1;
}

static bool number_parse(const char *text, long min, long max, int *number)
{
    char *end = NULL;
    errno = 0;
    long value = strtol(text, &end, 10);
    if (errno != 0 || end == text || *end != '\0' || value < min ||
        value > max) {
        return false;
    }
    *number = (int)value;

    return true;
}
