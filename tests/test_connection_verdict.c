#include <stdio.h>

#include "check.h"
#include "connection_verdict.h"

static void a_client_with_no_signal_passes(void)
{
    struct connection_signals signals = {false, false, NULL};
    CHECK_EQ_INT(connection_decide(&signals), CONN_PASS);
}

static void a_client_ip_in_the_block_list_is_dropped(void)
{
    struct connection_signals signals = {true, false, NULL};
    CHECK_EQ_INT(connection_decide(&signals), CONN_DROP);
}

static void a_key_in_the_block_table_is_dropped(void)
{
    struct connection_signals signals = {false, true, NULL};
    CHECK_EQ_INT(connection_decide(&signals), CONN_DROP);
}

static void a_ja4_label_alone_does_not_drop(void)
{
    struct connection_signals signals = {false, false, "curl"};
    CHECK_EQ_INT(connection_decide(&signals), CONN_PASS);
}

static void a_listed_client_with_a_ja4_label_is_dropped(void)
{
    struct connection_signals signals = {true, true, "browser"};
    CHECK_EQ_INT(connection_decide(&signals), CONN_DROP);
}

int main(void)
{
    a_client_with_no_signal_passes();
    a_client_ip_in_the_block_list_is_dropped();
    a_key_in_the_block_table_is_dropped();
    a_ja4_label_alone_does_not_drop();
    a_listed_client_with_a_ja4_label_is_dropped();
    printf("test_connection_verdict: ok\n");

    return 0;
}
