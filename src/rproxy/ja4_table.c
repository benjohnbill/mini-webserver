#include "ja4_table.h"

#include <stdbool.h>
#include <string.h>

/* The longest line that can hold an entry, with room for spaces and a
   comment. Longer lines are bad. */
#define LINE_MAX_LEN 255
#define BLANKS " \t\r"

static size_t line_copy(const char *start, char *line);
static char *line_trim(char *line);
static bool entry_parse(char *text, struct ja4_table_entry *entry);
static bool word_copy(const char *word, size_t len, char *out, size_t max);

size_t ja4_table_parse(const char *text, struct ja4_table *fingerprints)
{
    char line[LINE_MAX_LEN + 2];
    size_t line_number = 0;
    fingerprints->count = 0;
    while (*text != '\0') {
        line_number++;
        size_t len = line_copy(text, line);
        text += len;
        if (len > LINE_MAX_LEN + 1) {
            return line_number;
        }
        char *entry_text = line_trim(line);
        if (*entry_text == '\0') {
            continue;
        }
        if (fingerprints->count == JA4_TABLE_ENTRY_MAX ||
            !entry_parse(entry_text,
                         &fingerprints->entries[fingerprints->count])) {
            return line_number;
        }
        fingerprints->count++;
    }

    return 0;
}

const char *ja4_table_find(const struct ja4_table *fingerprints,
                           const char *ja4)
{
    for (size_t i = 0; i < fingerprints->count; i++) {
        if (strcmp(fingerprints->entries[i].ja4, ja4) == 0) {
            return fingerprints->entries[i].label;
        }
    }

    return NULL;
}

/* Copies the line that starts at start into line, which has
   LINE_MAX_LEN + 2 bytes, and cuts it there if it is longer. Returns the
   bytes of the line in the text, with its '\n'. */
static size_t line_copy(const char *start, char *line)
{
    const char *end = strchr(start, '\n');
    size_t len = end != NULL ? (size_t)(end - start) : strlen(start);
    size_t kept = len <= LINE_MAX_LEN ? len : LINE_MAX_LEN + 1;
    memcpy(line, start, kept);
    line[kept] = '\0';

    return end != NULL ? len + 1 : len;
}

/* Cuts the comment and the blanks at the two ends. */
static char *line_trim(char *line)
{
    char *comment = strchr(line, '#');
    if (comment != NULL) {
        *comment = '\0';
    }
    line += strspn(line, BLANKS);
    size_t len = strlen(line);
    while (len > 0 && strchr(BLANKS, line[len - 1]) != NULL) {
        len--;
    }
    line[len] = '\0';

    return line;
}

/* "JA4 LABEL": two words and blanks between them. The line has no blank
   at its ends. */
static bool entry_parse(char *text, struct ja4_table_entry *entry)
{
    size_t ja4_len = strcspn(text, BLANKS);
    const char *label = text + ja4_len + strspn(text + ja4_len, BLANKS);
    size_t label_len = strlen(label);

    return ja4_len == JA4_LEN && label_len > 0 &&
           strcspn(label, BLANKS) == label_len &&
           word_copy(text, ja4_len, entry->ja4, JA4_LEN) &&
           word_copy(label, label_len, entry->label, JA4_TABLE_LABEL_MAX);
}

static bool word_copy(const char *word, size_t len, char *out, size_t max)
{
    if (len > max) {
        return false;
    }
    memcpy(out, word, len);
    out[len] = '\0';

    return true;
}
