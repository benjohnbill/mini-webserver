#include "verdict.h"

#include <stddef.h>
#include <string.h>

/* Source: Cloudflare "AI Scrapers and Crawlers" rule, ai.robots.txt, JA4. */

/* The rule table: the first rule that matches decides. */
struct verdict_rule {
    bool (*matches)(const struct verdict_signals *signals);
    enum verdict_kind kind;
};

/* User-Agent tokens of AI and SEO crawlers that name themselves. */
static const char *const crawler_names[] = {
    "GPTBot", "CCBot", "ClaudeBot", "Bytespider", "PerplexityBot", "AhrefsBot",
};

static bool agent_is_crawler(const struct verdict_signals *signals);
static bool agent_lies_about_chrome(const struct verdict_signals *signals);
static bool request_is_over_limit(const struct verdict_signals *signals);

static const struct verdict_rule rules[] = {
    {agent_is_crawler, VERDICT_DENY},
    {agent_lies_about_chrome, VERDICT_TARPIT},
    {request_is_over_limit, VERDICT_SLOW},
};

struct verdict verdict_decide(const struct verdict_signals *signals)
{
    for (size_t i = 0; i < sizeof(rules) / sizeof(rules[0]); i++) {
        if (rules[i].matches(signals)) {
            int retry_after =
                rules[i].kind == VERDICT_SLOW ? signals->retry_after : 0;
            return (struct verdict){.kind = rules[i].kind,
                                    .retry_after = retry_after};
        }
    }

    return (struct verdict){.kind = VERDICT_PASS};
}

static bool agent_is_crawler(const struct verdict_signals *signals)
{
    if (signals->user_agent == NULL) {
        return false;
    }
    for (size_t i = 0; i < sizeof(crawler_names) / sizeof(crawler_names[0]);
         i++) {
        if (strstr(signals->user_agent, crawler_names[i]) != NULL) {
            return true;
        }
    }

    return false;
}

/* Slot: the JA4 label rule comes next. */
static bool agent_lies_about_chrome(const struct verdict_signals *signals)
{
    (void)signals;

    return false;
}

static bool request_is_over_limit(const struct verdict_signals *signals)
{
    return signals->retry_after > 0;
}
