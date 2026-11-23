#include "verdict.h"

struct verdict verdict_decide(const struct verdict_signals *signals)
{
    /* SLOT(owner): SPEC §11 "Code the owner types" row 3. The slot lets
       every request through. */
    (void)signals;

    return (struct verdict){VERDICT_PASS, 0};
}
