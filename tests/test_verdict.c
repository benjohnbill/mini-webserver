#include <stdio.h>

#include "check.h"
#include "verdict.h"

/* Only the rules that SPEC 11 and GLOSSARY.md fix are here. The owner
   adds the rest of the Verdict table with its tests (SPEC 11 row 3). */

static struct verdict decide(const char *user_agent, int retry_after)
{
    struct verdict_signals signals = {"", user_agent, "/", retry_after};

    return verdict_decide(&signals);
}

static void verification_clients_within_the_limit_pass(void)
{
    static const char *agents[] = {"curl/8.18.0", "ApacheBench/2.3", "wrk",
                                   "hey/0.0.1", NULL};
    for (size_t i = 0; i < sizeof(agents) / sizeof(agents[0]); i++) {
        CHECK_EQ_INT(decide(agents[i], 0).kind, VERDICT_PASS);
    }
}

static void a_request_over_the_rate_limit_is_slowed_with_its_retry_after(void)
{
    struct verdict verdict = decide("curl/8.18.0", 7);
    CHECK_EQ_INT(verdict.kind, VERDICT_SLOW);
    CHECK_EQ_INT(verdict.retry_after, 7);
}

int main(void)
{
    verification_clients_within_the_limit_pass();
    a_request_over_the_rate_limit_is_slowed_with_its_retry_after();
    printf("test_verdict: ok\n");

    return 0;
}
