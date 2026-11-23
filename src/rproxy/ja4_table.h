#ifndef JA4_TABLE_H
#define JA4_TABLE_H

#include <stddef.h>

#include "ja4.h"

/* Decision module of the Reverse Proxy: the JA4 table, a JA4 to a label
   (for example "curl" or "browser"). No file I/O: main.c reads the file
   and gives the text. */

#define JA4_TABLE_ENTRY_MAX 256
#define JA4_TABLE_LABEL_MAX 32

struct ja4_table_entry {
    char ja4[JA4_LEN + 1];
    char label[JA4_TABLE_LABEL_MAX + 1];
};

struct ja4_table {
    struct ja4_table_entry entries[JA4_TABLE_ENTRY_MAX];
    size_t count;
};

/* Reads text into fingerprints. One entry on each line: "JA4 LABEL", separated by
   spaces or tabs. The JA4 has exactly JA4_LEN characters. The label has at
   most JA4_TABLE_LABEL_MAX characters. "#" starts a comment to the end of
   the line. Blank lines are skipped. Returns 0, or the number (from 1) of
   the first bad line. A line after JA4_TABLE_ENTRY_MAX entries is bad. */
size_t ja4_table_parse(const char *text, struct ja4_table *fingerprints);

/* Returns the label of ja4, or NULL if fingerprints does not have it. If
   fingerprints has ja4 two times, the first line wins. */
const char *ja4_table_find(const struct ja4_table *fingerprints,
                           const char *ja4);

#endif
