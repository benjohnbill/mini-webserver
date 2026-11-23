#include <stdio.h>
#include <string.h>

#include "check.h"
#include "ja4_table.h"

#define CURL_JA4 "t13d3112h2_e8f1e7e78f70_b26ce05bbdd6"
#define CHROME_JA4 "t13d1516h2_8daaf6152771_e5627efa2ab1"

static struct ja4_table fingerprints;

static void a_listed_ja4_gives_its_label(void)
{
    CHECK_EQ_INT(ja4_table_parse(CURL_JA4 " curl\n" CHROME_JA4 " browser\n",
                                 &fingerprints),
                 0);
    CHECK(strcmp(ja4_table_find(&fingerprints, CURL_JA4), "curl") == 0);
    CHECK(strcmp(ja4_table_find(&fingerprints, CHROME_JA4), "browser") == 0);
}

static void an_unlisted_ja4_gives_null(void)
{
    CHECK_EQ_INT(ja4_table_parse(CURL_JA4 " curl\n", &fingerprints), 0);
    CHECK(ja4_table_find(&fingerprints, CHROME_JA4) == NULL);
}

static void the_empty_ja4_gives_null(void)
{
    CHECK_EQ_INT(ja4_table_parse(CURL_JA4 " curl\n", &fingerprints), 0);
    CHECK(ja4_table_find(&fingerprints, "") == NULL);
}

static void comments_blank_lines_and_tabs_are_accepted(void)
{
    const char *text = "# TLS libraries\n"
                       "\n" CURL_JA4 "\tcurl   # curl 8 with OpenSSL\r\n";
    CHECK_EQ_INT(ja4_table_parse(text, &fingerprints), 0);
    CHECK_EQ_INT(fingerprints.count, 1);
    CHECK(strcmp(ja4_table_find(&fingerprints, CURL_JA4), "curl") == 0);
}

static void the_first_line_wins_for_a_ja4_that_is_there_two_times(void)
{
    CHECK_EQ_INT(
        ja4_table_parse(CURL_JA4 " curl\n" CURL_JA4 " python\n", &fingerprints),
        0);
    CHECK(strcmp(ja4_table_find(&fingerprints, CURL_JA4), "curl") == 0);
}

static void a_line_with_no_label_gives_its_line_number(void)
{
    CHECK_EQ_INT(
        ja4_table_parse(CURL_JA4 " curl\n" CHROME_JA4 "\n", &fingerprints), 2);
}

static void a_ja4_of_the_wrong_length_gives_its_line_number(void)
{
    CHECK_EQ_INT(
        ja4_table_parse("t13d3112h2_e8f1e7e78f70 curl\n", &fingerprints), 1);
}

static void a_third_word_gives_its_line_number(void)
{
    CHECK_EQ_INT(ja4_table_parse(CURL_JA4 " curl extra\n", &fingerprints), 1);
}

static void a_label_over_the_limit_gives_its_line_number(void)
{
    CHECK_EQ_INT(ja4_table_parse(CURL_JA4
                                 " abcdefghijklmnopqrstuvwxyz0123456\n",
                                 &fingerprints),
                 1);
    CHECK_EQ_INT(ja4_table_parse(CURL_JA4 " abcdefghijklmnopqrstuvwxyz012345\n",
                                 &fingerprints),
                 0);
}

static void a_line_after_the_last_free_entry_gives_its_line_number(void)
{
    static char text[(JA4_TABLE_ENTRY_MAX + 1) * (JA4_LEN + 8)];
    size_t used = 0;
    for (int i = 0; i <= JA4_TABLE_ENTRY_MAX; i++) {
        used +=
            (size_t)snprintf(text + used, sizeof(text) - used,
                             "t13d3112h2_e8f1e7e78f70_b26ce05bb%03d x\n", i);
    }
    CHECK_EQ_INT(ja4_table_parse(text, &fingerprints), 257);
}

int main(void)
{
    a_listed_ja4_gives_its_label();
    an_unlisted_ja4_gives_null();
    the_empty_ja4_gives_null();
    comments_blank_lines_and_tabs_are_accepted();
    the_first_line_wins_for_a_ja4_that_is_there_two_times();
    a_line_with_no_label_gives_its_line_number();
    a_ja4_of_the_wrong_length_gives_its_line_number();
    a_third_word_gives_its_line_number();
    a_label_over_the_limit_gives_its_line_number();
    a_line_after_the_last_free_entry_gives_its_line_number();
    printf("test_ja4_table: ok\n");

    return 0;
}
