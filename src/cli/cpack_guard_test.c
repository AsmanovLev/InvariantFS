/* cpack_guard_test.c — WP119: the containerpack sweep's size guard.
 *
 * The guard is the one that decides whether vol_containerpack_sweep() may
 * commit a decomposition. It is the only thing between a container pack
 * that hands back a BIGGER shape than the container it came from and a
 * volume that quietly grows, so it is tested the way the ZIP pack's own
 * guard was validated (registry codecpacks/zip/1.0.0/zip.c, WP108): the
 * historical regression is replayed here as a case, and the guard has to
 * refuse it.
 *
 * The cases below are the arithmetic the sweep feeds the guard, so they are
 * the contract, not an approximation of it:
 *
 *   1. a plain win -- a few compressible members: accepted
 *   2. break-even: a decomposition that saves nothing is declined
 *   3. the 0.5% band: a real gain below INVFS_MIN_GAIN_PCT is declined,
 *      and one above it is accepted
 *   4. WP108's archive, replayed: the CONTENT projection alone calls it a
 *      0.63% gain; charging the engine's per-member bookkeeping calls it a
 *      2.9x loss. This is the case the guard exists for.
 *   5. many small members vs one big member of the same content (the
 *      tools/test-containerpack.sh fixture): the member COUNT decides,
 *      and no codec is involved in that verdict
 *   6. the re-deflation bound: a map whose largest kind-2 entry re-deflates
 *      more than CPACK_REPRO_MAX is declined even when its content is a
 *      large win -- compressed content is not a safety bound, because a
 *      kind-2 entry stores nothing and pays on every read
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include "volume_internal.h"

static int checks = 0;
static int failures = 0;

static void ok(int cond, const char *what)
{
    checks++;
    if (!cond) {
        failures++;
        fprintf(stderr, "  FAIL  %s\n", what);
    } else {
        printf("  OK    %s\n", what);
    }
}

static cpack_size_proj proj(uint64_t fixed, uint64_t content, uint64_t members)
{
    cpack_size_proj p;
    memset(&p, 0, sizeof p);
    p.fixed = fixed;
    p.content = content;
    p.member_count = members;
    p.member_cost = members * CPACK_MEMBER_COST;
    return p;
}

int main(void)
{
    cpack_size_proj p;
    const char *why = NULL;

    printf("cpack_guard_test: the containerpack sweep size guard (WP119)\n");
    printf("  MEM_COST=%llu B/member  ZGAIN=%llu/1000  REPRO_MAX=%llu B\n",
           (unsigned long long)CPACK_MEMBER_COST,
           (unsigned long long)CPACK_ZGAIN_MILLE,
           (unsigned long long)CPACK_REPRO_MAX);

    /* 1. a plain win: 3 members that compress to a third of themselves */
    p = proj(4096, 1000000, 3);
    ok(cpack_size_guard(3000000, &p, &why) == 1,
       "compressible members: decomposition accepted");
    ok(why == NULL, "an accepted decomposition reports no reason");

    /* 2. break-even: the same container stored whole costs nothing extra */
    p = proj(4096, 1000000, 3);
    ok(cpack_size_guard(1000000 + 4096 + 3 * CPACK_MEMBER_COST, &p, &why) == 0,
       "break-even: decomposition declined");
    ok(why != NULL, "a declined decomposition says why");

    /* 3. the 0.5% band. projected = orig*995/1000 exactly is declined
     *    (the comparison is projected*1000 < orig*995); one byte less is
     *    inside the band and accepted. */
    p = proj(0, 995000, 0);
    ok(cpack_size_guard(1000000, &p, &why) == 0,
       "exactly at the gain threshold: declined");
    p = proj(0, 994999, 0);
    ok(cpack_size_guard(1000000, &p, &why) == 1,
       "one byte past the gain threshold: accepted");

    /* 4. WP108's 201-member archive, replayed.
     *      content projection (zstd-19 alone)  1,685,140 B
     *      the container actually stored        1,695,889 B
     *      what the volume really charged        4,935,680 B
     *    The content projection calls that a 0.63% GAIN, which is how the
     *    pack passed its own guard and shipped a 3 MB regression. */
    p = proj(0, 1685140, 201);
    ok(1685140 * 1000ull < 1695889 * 995ull,
       "WP108 replay: the content projection alone sees a gain");
    ok(cpack_size_guard(1695889, &p, &why) == 0,
       "WP108 replay: charged the engine's per-member cost, declined");
    /* the guard prices that shape at 4,978,324 B against the 4,935,680 B the
     * volume really charged: MEM_COST rounds the measured 16,167 B/member UP,
     * so the guard is 42,644 B pessimistic on this corpus. Under-charging is
     * what turns a conservative guard into a hopeful one. */
    ok(1685140 + 201 * CPACK_MEMBER_COST == 4978324ull &&
       4978324ull > 4935680ull,
       "WP108 replay: the guard over-charges the measured 4.9 MB shape");

    /* 5. many small members: the per-member bookkeeping, not the codec.
     *    Same content, same compressed size, 200 members vs one: the count
     *    is what the guard is really deciding here. */
    p = proj(1612, 400000, 200);
    ok(cpack_size_guard(3278612, &p, &why) == 0,
       "400 KB of content as 200 members: declined (3.2 MB of bookkeeping)");
    p = proj(1612, 400000, 1);
    ok(cpack_size_guard(3278612, &p, &why) == 1,
       "the same content as ONE member: accepted");

    /* 6. the re-deflation bound. A kind-2 entry stores nothing, so a
     *    content projection cannot see what it costs: it re-deflates its
     *    whole raw_len on every read that touches it. */
    p = proj(0, 1000, 1);
    p.repro_max = CPACK_REPRO_MAX;
    p.repro_bytes = CPACK_REPRO_MAX;
    ok(cpack_size_guard(1000000, &p, &why) == 1,
       "kind-2 re-deflation exactly at the bound: accepted");
    p.repro_max = CPACK_REPRO_MAX + 1;
    ok(cpack_size_guard(1000000, &p, &why) == 0,
       "kind-2 re-deflation over the bound: declined despite a 1000x win");
    ok(why && strstr(why, "kind-2") != NULL,
       "the re-deflation refusal names the kind-2 re-deflation");

    /* 7. degenerate inputs: an empty container is never a decomposition,
     *    and a missing projection is never a gain. */
    p = proj(0, 0, 0);
    ok(cpack_size_guard(0, &p, &why) == 0, "empty container: declined");
    ok(cpack_size_guard(1024, NULL, &why) == 0,
       "no projection: declined");

    /* 8. overflow cannot masquerade as a gain */
    p = proj(UINT64_MAX - 8, UINT64_MAX - 8, 4);
    ok(cpack_size_guard(UINT64_MAX, &p, &why) == 0,
       "a projection that overflows is declined, not wrapped");

    printf("\ncpack_guard_test summary: %d checks, %d failures\n",
           checks, failures);
    return failures ? 1 : 0;
}
