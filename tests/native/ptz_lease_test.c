/* Host test of src/features/ptz/ptz_lease.c with a FAKE CLOCK: arm, identical repeat (refresh), expiry boundary, clear, a different lease length, a clock that steps back,
 * and the lease sequence of a client that holds a move with 1 s repeats and a 2 s Timeout. Prints "ptz_lease_test: all passed". */
#include <stdio.h>

#include "ptz_lease.h"

static int fails;
#define CHECK(c, msg) do { if (!(c)) { printf("FAIL: %s (line %d)\n", msg, __LINE__); fails++; } } while (0)

int main(void)
{
    ptzl_t l;
    int64_t t = 1000000; /* the fake monotonic clock, ms */

    ptzl_clear(&l);
    CHECK(!ptzl_live(&l, t) && !ptzl_expired(&l, t) && ptzl_remaining_ms(&l, t) == 0, "a cleared lease is neither live nor expired (nothing to stop)");

    ptzl_arm(&l, t, 450);
    CHECK(ptzl_live(&l, t) && !ptzl_expired(&l, t) && ptzl_remaining_ms(&l, t) == 450, "armed: live, 450 ms left");
    CHECK(ptzl_live(&l, t + 449) && ptzl_remaining_ms(&l, t + 449) == 1, "449 ms later: live, 1 ms left");
    CHECK(ptzl_live(&l, t + 450) && !ptzl_expired(&l, t + 450) && ptzl_remaining_ms(&l, t + 450) == 0, "exactly at the lease: still live (the boundary is inclusive)");
    CHECK(!ptzl_live(&l, t + 451) && ptzl_expired(&l, t + 451), "one ms past the lease: expired (the dead-man fires)");
    CHECK(ptzl_expired(&l, t + 100000) && ptzl_remaining_ms(&l, t + 100000) == 0, "and stays expired");

    /* an identical repeat restarts the lease */
    ptzl_arm(&l, t, 450);
    ptzl_arm(&l, t + 300, 450);
    CHECK(ptzl_live(&l, t + 700) && ptzl_remaining_ms(&l, t + 700) == 50, "a repeat at +300 ms keeps it live until +750 ms");
    CHECK(ptzl_expired(&l, t + 751), "... and it expires 451 ms after the repeat");

    /* a repeat with another length replaces the length */
    ptzl_arm(&l, t, 150);
    ptzl_arm(&l, t + 100, 2000);
    CHECK(ptzl_live(&l, t + 2100) && ptzl_expired(&l, t + 2101), "a repeat with a 2 s lease replaces the 150 ms one");

    /* a non positive length clears */
    ptzl_arm(&l, t, 450);
    ptzl_arm(&l, t + 10, 0);
    CHECK(!ptzl_live(&l, t + 11) && !ptzl_expired(&l, t + 11), "arming with 0 clears it");
    ptzl_arm(&l, t, 450);
    ptzl_clear(&l);
    CHECK(!ptzl_live(&l, t + 1) && !ptzl_expired(&l, t + 100000), "clear (Stop) ends the hold: not live, nothing left to expire");

    /* the clock steps backwards: never a spurious expiry, the remaining time is bounded by the lease */
    ptzl_arm(&l, t, 450);
    CHECK(ptzl_live(&l, t - 5000) && !ptzl_expired(&l, t - 5000) && ptzl_remaining_ms(&l, t - 5000) == 450, "a clock behind the arming instant is live, remaining capped at the lease");

    /* a client holding a move: repeats every 1000 ms with Timeout 2 s never lets it expire; the last repeat's lease ends the move 2 s later */
    ptzl_arm(&l, t, 2000);
    for (int k = 1; k <= 5; k++) {
        int64_t now = t + 1000L * k;
        CHECK(ptzl_live(&l, now), "the previous repeat is still live when the next one arrives");
        ptzl_arm(&l, now, 2000);
    }
    CHECK(ptzl_live(&l, t + 5000 + 2000) && ptzl_expired(&l, t + 5000 + 2001), "after the client goes away the move ends 2 s after the last repeat");
    /* a 1 s repeat period with the default 450 ms lease expires in between (the head stops, the next repeat restarts it): clients should send Timeout for their repeat period */
    ptzl_arm(&l, t, 450);
    CHECK(ptzl_expired(&l, t + 1000), "a 450 ms lease repeated every 1 s expires in between");

    if (fails) {
        printf("ptz_lease_test: %d FAILED\n", fails);
        return 1;
    }
    printf("ptz_lease_test: all passed\n");
    return 0;
}
