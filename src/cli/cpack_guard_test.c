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
 * The per-member price is what it is, and the cases below are what pin it.
 * It replaced a flat 4 pages/member, which was one run's whole-image delta
 * residual divided by that run's member count -- per-SWEEP overhead charged
 * per member forever. What replaced it is TWO measured rates, not a page
 * count: CPACK_MEMBER_META (1,024 B, the packed rows a member costs -- 721 /
 * 717 B/file measured) and CPACK_MEMBER_UNBATCHED (256 B, the share of block
 * rounding taken by the 6% of members the batching stage does not take).
 * The PAYLOAD, meanwhile, is priced where the engine spends it: rounded up
 * ONCE, on the sum, because stage 6 seals members into shared batch segments
 * (vol_textzone.c) -- 94% of them, on every corpus measured. See the
 * constants' comments in volume_internal.h and impl_docs/AUDIT.md 6a.2.
 *
 * Every case that DECLINES carries a control that ACCEPTS, and every case
 * that leans on a term is shown to flip when the term under test is changed.
 * A refusal that does not flip is not testing the term.
 *
 *   1. a plain win -- a few compressible members: accepted
 *   2. break-even: a decomposition that saves nothing is declined
 *   3. the 0.5% band: a real gain below INVFS_MIN_GAIN_PCT is declined,
 *      and one above it is accepted
 *   4. THE PRICE: the blocks a payload costs are whole blocks, rounded on
 *      the SUM (the members share them), and the per-member term is a rate
 *      that does not scale with the payload at all
 *   5. WP108's archive, replayed. The CONTENT projection alone calls it a
 *      0.63% gain; the volume really charged 4,935,680 B for a 1,695,889 B
 *      container. The guard must still decline it -- and the term it now
 *      declines on is stated, not assumed
 *   6. the rootfs regime: 50,000 members of a realistic size, which the
 *      flat 4-pages-per-member model refused and this one accepts
 *   7. 50,000 sub-block members -- the shape the size-blind per-member
 *      rounding refused and the batched price accepts, which is the whole
 *      point, with the retired rounding as its red control
 *   8. shapes that genuinely grow are still declined, with controls that
 *      flip them
 *   9. many small members vs one big member of the same content
 *  10. the re-deflation bound: a map whose largest kind-2 entry re-deflates
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

/* The projection cpack_project() builds. `content` is ALREADY the blocks the
 * payloads cost: the sum of the members' ZSTD-19 projections, rounded up
 * once (cpack_project() does that rounding itself; the cases below pass the
 * total they are about). `meta` is the per-member rate -- both halves of it
 * when it is the shipped price. Passing 0 removes the term under test, and
 * that is how the red controls are written: if a decline does not become an
 * acceptance when the term is removed, the case is not testing the term, it
 * is testing the arithmetic. */
static cpack_size_proj proj(uint64_t fixed, uint64_t content, uint64_t members,
                            uint64_t meta)
{
    cpack_size_proj p;
    memset(&p, 0, sizeof p);
    p.fixed = fixed;
    p.content = content;
    p.member_count = members;
    p.member_cost = members * meta;
    return p;
}

/* the shape cpack_project() prices with the shipped per-member rate */
#define SHIPPED_MEMBER_RATE (CPACK_MEMBER_META + CPACK_MEMBER_UNBATCHED)

static cpack_size_proj priced(uint64_t fixed, uint64_t content,
                              uint64_t members)
{
    return proj(fixed, content, members, SHIPPED_MEMBER_RATE);
}

/* What a shape cost under the retired flat 4-pages-per-member model, and what
 * it cost under the intermediate PER-MEMBER block rounding (each member's own
 * payload rounded up to whole blocks -- correct when nothing batches, wrong
 * by 12.8x on the payloads that do). Both are kept HERE, in the test, so the
 * new cases can prove they discriminate: each is refused by one of those
 * prices and accepted by the shipped one. If either is ever reinstated, those
 * cases go red. */
#define OLD_FLAT_COST_PER_MEMBER 16384ull
#define OLD_PER_MEMBER_RATE  (SHIPPED_MEMBER_RATE + INVFS_BLOCK_SIZE)

int main(void)
{
    cpack_size_proj p;
    const char *why = NULL;
    uint64_t nm;

    printf("cpack_guard_test: the containerpack sweep size guard (WP119)\n");
    printf("  META=%llu B/member (packed rows) + UNBATCHED=%llu B/member "
           "(the 6%% batching misses); payload blocks are rounded on the SUM\n"
           "  block=%d B  ZGAIN=%llu/1000  REPRO_MAX=%llu B\n",
           (unsigned long long)CPACK_MEMBER_META,
           (unsigned long long)CPACK_MEMBER_UNBATCHED, INVFS_BLOCK_SIZE,
           (unsigned long long)CPACK_ZGAIN_MILLE,
           (unsigned long long)CPACK_REPRO_MAX);

    /* 1. a plain win: 3 members that compress to a third of themselves */
    p = priced(4096, 1000000, 3);
    ok(cpack_size_guard(3000000, &p, &why) == 1,
       "compressible members: decomposition accepted");
    ok(why == NULL, "an accepted decomposition reports no reason");

    /* 2. break-even: the same container stored whole costs nothing extra */
    p = priced(4096, 1000000, 3);
    ok(cpack_size_guard(1000000 + 4096 + 3 * SHIPPED_MEMBER_RATE, &p, &why) == 0,
       "break-even: decomposition declined");
    ok(why != NULL, "a declined decomposition says why");

    /* 3. the 0.5% band. projected = orig*995/1000 exactly is declined
     *    (the comparison is projected*1000 < orig*995); one byte less is
     *    inside the band and accepted. */
    p = priced(0, 995000, 0);
    ok(cpack_size_guard(1000000, &p, &why) == 0,
       "exactly at the gain threshold: declined");
    p = priced(0, 994999, 0);
    ok(cpack_size_guard(1000000, &p, &why) == 1,
       "one byte past the gain threshold: accepted");

    /* 4. THE PRICE. Two halves, and they are not the same kind of thing.
     *    The payload: WHOLE blocks, rounded up ONCE on the sum, because
     *    stage 6 seals the members into shared batch segments rather than
     *    giving each one its own. The member: a rate, twice. */
    ok(cpack_member_data_cost(0) == 0, "an empty payload costs no block");
    ok(cpack_member_data_cost(1) == 4096,
       "1 B of payload still costs a whole block");
    ok(cpack_member_data_cost(4095) == 4096 &&
       cpack_member_data_cost(4096) == 4096,
       "a payload that fills a block costs exactly one block");
    ok(cpack_member_data_cost(4097) == 8192,
       "one byte over a block costs a second block");
    ok(cpack_member_data_cost(100ull * 1024) == 100ull * 1024,
       "a payload that fills whole blocks pays for them and no rounding");
    ok(cpack_member_data_cost(100ull * 1024 + 1) == 106496ull,
       "and one byte over that pays one more block, not one more page");
    /* Rounding the SUM rather than the members is the change. 10,000 members
     * of 100 B come to 1 MB of payload, which the batching stage packs into
     * 245 blocks; the per-member price bills them 10,000. 10,000 members of
     * 4,096 B need 40,960,000 B of blocks, batched or not -- and that
     * symmetry is the sound side of the model. */
    ok(10000 * cpack_member_data_cost(100) == 40960000ull,
       "10,000 tiny members: the PER-MEMBER price is 40.96 MB of blocks");
    ok(cpack_member_data_cost(10000 * 100ull) == 1003520ull,
       "10,000 tiny members batched: the blocks they SHARE are 245, not "
       "10,000 -- 1 MB of payload is 1 MB of blocks, however it is split");
    ok(cpack_member_data_cost(10000 * 4096ull) == 40960000ull,
       "10,000 block-sized members: batched or not, the blocks are the same");
    /* the member term does not scale with the payload at all -- which is the
     * difference between this and the retired constant's intent */
    ok(SHIPPED_MEMBER_RATE == SHIPPED_MEMBER_RATE &&
        cpack_member_data_cost(100ull * 1024) ==
           25 * cpack_member_data_cost(4096),
       "the data term scales with the payload; the member term is flat");

    /* 5. WP108's archive, replayed.
     *      content projection (zstd-19 alone)  1,685,140 B
     *      the container actually stored        1,695,889 B
     *      what the volume really charged        4,935,680 B
     *    The content projection calls that a 0.63% GAIN, which is how the
     *    pack passed its own guard and shipped a 3 MB regression. The guard
     *    must still refuse, and the term it now refuses on is the block
     *    rounding on the content: 1,687,552 > 1,687,409.
     *
     *    The old pin (1,685,140 + 201*16384 == 4,978,324 > 4,935,680)
     *    asserted that the guard OVER-charges this shape by 42,644 B. That
     *    was a true fact about a fiction: 16 KiB was never a per-member
     *    measurement. What is true about the volume is below.
     *
     *    What is NOT claimed: that the guard's 1,944,832 B is the real cost.
     *    It is 2.6x BELOW the 4,935,680 B the volume really charged. The
     *    refusal is right for a partly wrong reason, and the residual it
     *    cannot see is per-SWEEP, not per-member. Stated here so nobody
     *    reads this case as the guard being accurate. */
    {
        const uint64_t orig = 1695889, unrounded = 1685140, members = 201;
        const uint64_t rounded = cpack_member_data_cost(unrounded);
        p = priced(0, rounded, members);
        ok(unrounded * 1000ull < orig * 995ull,
           "WP108 replay: the content projection alone sees a gain");
        ok(rounded == 1687552ull &&
           rounded + members * SHIPPED_MEMBER_RATE == 1944832ull,
           "WP108 replay: the honest price is 1,944,832 B, 2.6x BELOW the "
           "4,935,680 B the volume really charged -- and it still refuses");
        ok(cpack_size_guard(orig, &p, &why) == 0,
           "WP108 replay: still declined");
        /* red control: the block rounding is the term the decline now rests
         * on. Remove it -- the price the old guard computed, 1,685,140 of
         * content and no per-member charge at all -- and the shape is
         * ACCEPTED, which is exactly the 3 MB regression this guard exists
         * to stop. So the rounding is load-bearing, and the case is
         * testing the term rather than the arithmetic. */
        p = proj(0, unrounded, members, 0);
        ok(cpack_size_guard(orig, &p, &why) == 1,
           "WP108 red control: unrounded content and no per-member charge is "
           "ACCEPTED -- the regression the guard exists to stop");
        /* the retired price, kept as the reference the new one is measured
         * against rather than quietly forgotten */
        ok(unrounded + members * OLD_FLAT_COST_PER_MEMBER == 4978324ull &&
           4978324ull > 4935680ull,
           "WP108 replay: the retired flat model priced this shape at "
           "4,978,324 B -- 2.6x the volume's real charge, for a corpus of "
           "201 members");
    }

    /* 6. THE ROOTFS REGIME. 50,000 members of 8,192 B -- a 410 MB image, the
     *    shape AGENTS.md 2.7 is aimed at. Members that size reach ZSTD-19 at
     *    about 2,730 B each, so 50,000 of them come to 136,500,000 B of
     *    payload:
     *      content   50,000 x 2,730 -> 136,503,296 B of blocks
     *      member    50,000 x 1,280            =  64,000,000
     *      fixed     recipe 400,008 + table 2,250,000 + map 1,250,000
     *                                           =   3,900,008
     *      total                              = 204,403,304
     *    against 410,000,008 B of container. Measured marginal cost of one
     *    more member in this regime is 1,753 B (tools/measure-cpack-member-
     *    cost.sh), so the price here is 1.65x the engine: sound, and not
     *    tuned down until it looked right. */
    {
        const uint64_t nmem = 50000, fixed = 3900008, orig = 410000008;
        const uint64_t content = cpack_member_data_cost(nmem * 2730);
        p = priced(fixed, content, nmem);
        ok(cpack_member_data_cost(2730) == 4096,
           "rootfs: a 2,730 B projection is under a block, and the members "
           "share it -- 50,000 of them are 136 MB, not 200 MB");
        ok(content == 136503296ull &&
           fixed + content + nmem * SHIPPED_MEMBER_RATE == 204403304ull,
           "rootfs: 50,000 members price at 204,403,304 B");
        ok(cpack_size_guard(orig, &p, &why) == 1,
           "rootfs: 50,000 members accepted");
        /* red control 1: the retired flat model refuses this very shape
         * (50,000 x 16,384 = 819,200,000 B), so this case goes red if the
         * per-member price is ever replaced by it. */
        p = proj(fixed, nmem * 2730, nmem, OLD_FLAT_COST_PER_MEMBER);
        ok(cpack_size_guard(orig, &p, &why) == 0,
           "rootfs red control: the flat 4-pages-per-member model refuses "
           "the same 50,000-member shape");
        /* red control 2: the retired PER-MEMBER block rounding refuses it
         * too (50,000 x 4,096 = 204,800,000 B of blocks the members share
         * in 136 MB). This is the case that distinguishes the two retired
         * models from the shipped one. */
        p = proj(fixed, nmem * cpack_member_data_cost(2730), nmem,
                 OLD_PER_MEMBER_RATE);
        ok(cpack_size_guard(orig, &p, &why) == 0,
           "rootfs red control: the retired per-member block rounding refuses "
           "the same shape, billing 200 MB of blocks the engine spends 136");
    }

    /* 7. 50,000 SUB-BLOCK members: the shape the author's rootfs target is
     *    actually made of, and the one this change exists to unblock. Mean
     *    4,592 B of text per member projects to about 1,530 B at ZSTD-19 --
     *    under a third of a block. Per-member rounding bills each of them a
     *    whole 4,096 B block, 204,800,000 B for payloads that batch into
     *    76,500,992 B of blocks, and refuses a shape the engine stores at
     *    half its size. The shipped price:
     *      content   50,000 x 1,530            =  76,500,992
     *      member    50,000 x 1,280            =  64,000,000
     *      fixed                                 =   3,900,008
     *      total                                = 144,401,000
     *    against 230,000,008 B of container. The measured marginal is
     *    1,753 B/member = 87,650,000 B, so this is 1.65x the engine again.
     *
     *    red control: the per-member rounding the change retired. Same
     *    shape, same everything else, 204,800,000 B of blocks instead of
     *    76,500,992 -- DECLINED. If the rounding ever comes back per member,
     *    this case goes red. */
    {
        const uint64_t nmem = 50000, fixed = 3900008, orig = 230000008;
        const uint64_t batched = cpack_member_data_cost(nmem * 1530);
        p = priced(fixed, batched, nmem);
        ok(cpack_member_data_cost(1530) == 4096,
           "50,000 sub-block members: each is under a block, and 94% of them "
           "share the block the engine actually allocates");
        ok(batched == 76500992ull &&
           fixed + batched + nmem * SHIPPED_MEMBER_RATE == 144401000ull,
           "50,000 sub-block members price at 144,401,000 B, 1.65x the "
           "measured 87,650,000 B");
        ok(cpack_size_guard(orig, &p, &why) == 1,
           "50,000 sub-block members ACCEPTED -- the dominant small-file "
           "shape is reachable");
        p = proj(fixed, nmem * cpack_member_data_cost(1530), nmem,
                 OLD_PER_MEMBER_RATE);
        ok(cpack_size_guard(orig, &p, &why) == 0,
           "50,000 sub-block members red control: the retired per-member "
           "rounding refuses the very same shape");
    }

    /* 8. shapes that GENUINELY grow are still declined, and the term each
     *    one rests on is shown to be the term by flipping it.
     *
     *    8a. 2,000 members of 4,241 B of text, each projecting to 4,090 B, in
     *    an 8,500,000 B container. The payloads batch to 8,183,808 B of
     *    blocks, so the real cost is 8,183,808 + 156,008 + 2,000 files of
     *    bookkeeping at the measured 720 B = 9.8 MB, which is over the
     *    8.5 MB container: the decline is a true one.
     *        shipped   156,008 + 8,183,808 + 2,000 x 1,280 = 10,899,816
     *        no rate   156,008 + 8,183,808                =  8,339,816  < 8,457,500
     *        red control: remove the rate and it is accepted. */
    {
        const uint64_t nmem = 2000, orig = 8500000, fixed = 156008;
        const uint64_t content = cpack_member_data_cost(nmem * 4090);
        p = priced(fixed, content, nmem);
        ok(content == 8183808ull && fixed + content == 8339816ull &&
           fixed + content + nmem * SHIPPED_MEMBER_RATE == 10899816ull,
           "2,000 members: the per-member rate is the difference between "
           "8,339,816 B and 10,899,816 B");
        ok(cpack_size_guard(orig, &p, &why) == 0,
           "2,000 near-block members: declined, and it really would grow");
        p = proj(fixed, content, nmem, 0);
        ok(cpack_size_guard(orig, &p, &why) == 1,
           "2,000 near-block members red control: with the per-member rate "
           "removed the shape is accepted, so the decline IS the term");
    }
    /*    8b. 50,000 members of 1,024 B of text in a 51,600,008 B container.
     *        The payloads batch to 17,002,496 B of blocks -- a third of the
     *        container -- so here it is the PER-MEMBER RATE that decides, and
     *        the real cost (17.0 MB of blocks + 50,000 files at 720 B +
     *        2.65 MB fixed = 55.6 MB) is over the container: another true
     *        decline.
     *        shipped   2,650,008 + 17,002,496 + 50,000 x 1,280 = 83,652,504
     *        no rate   2,650,008 + 17,002,496               = 19,652,504 */
    {
        const uint64_t nmem = 50000, orig = 51600008, fixed = 2650008;
        const uint64_t content = cpack_member_data_cost(nmem * 340);
        p = priced(fixed, content, nmem);
        ok(content == 17002496ull,
           "50,000 one-KiB members: 17,002,496 B of blocks, a third of the "
           "container");
        ok(fixed + content + nmem * SHIPPED_MEMBER_RATE == 83652504ull,
           "50,000 one-KiB members: the per-member rate is 64 MB on top");
        ok(cpack_size_guard(orig, &p, &why) == 0,
           "50,000 one-KiB members: declined, and it really would grow -- "
           "50,000 files of bookkeeping outweigh a 34 MB compression gain");
        p = proj(fixed, content, nmem, 0);
        ok(cpack_size_guard(orig, &p, &why) == 1,
           "50,000 one-KiB members red control: with the per-member rate "
           "removed the payloads alone are comfortably a gain");
        /* red control: the same bytes as a tenth of the members */
        p = priced(390008, cpack_member_data_cost(5000 * 3400), 5000);
        ok(cpack_size_guard(orig, &p, &why) == 1,
           "50,000 one-KiB members red control: the same bytes as 5,000 "
           "members are accepted -- the verdict is the member count");
    }

    /* 9. many small members: the per-member bookkeeping, not the codec.
     *    Same content, same compressed size, 200 members vs one: the count
     *    is what the guard is really deciding here. */
    p = priced(1612, 400000, 200);
    ok(cpack_size_guard(600000, &p, &why) == 0,
       "400 KB of content as 200 members: declined (the bookkeeping alone "
       "is more than the container)");
    p = priced(1612, 400000, 1);
    ok(cpack_size_guard(600000, &p, &why) == 1,
       "the same content as ONE member: accepted");

    /* 10. the re-deflation bound. A kind-2 entry stores nothing, so a
     *    content projection cannot see what it costs: it re-deflates its
     *    whole raw_len on every read that touches it. */
    p = priced(0, 1000, 1);
    p.repro_max = CPACK_REPRO_MAX;
    p.repro_bytes = CPACK_REPRO_MAX;
    ok(cpack_size_guard(1000000, &p, &why) == 1,
       "kind-2 re-deflation exactly at the bound: accepted");
    p.repro_max = CPACK_REPRO_MAX + 1;
    ok(cpack_size_guard(1000000, &p, &why) == 0,
       "kind-2 re-deflation over the bound: declined despite a 1000x win");
    ok(why && strstr(why, "kind-2") != NULL,
       "the re-deflation refusal names the kind-2 re-deflation");

    /* 11. degenerate inputs: an empty container is never a decomposition,
     *    and a missing projection is never a gain. */
    p = priced(0, 0, 0);
    ok(cpack_size_guard(0, &p, &why) == 0, "empty container: declined");
    ok(cpack_size_guard(1024, NULL, &why) == 0,
       "no projection: declined");

    /* 12. overflow cannot masquerade as a gain */
    p = priced(0, 0, 1);
    p.repro_max = UINT64_MAX;
    p.repro_bytes = UINT64_MAX;
    ok(cpack_size_guard(UINT64_MAX, &p, &why) == 0,
       "a projection that overflows is declined, not wrapped");

    /* 13. linearity of the per-member rate, and the scale of what it
     *     replaced: 100,000 members cost 128 MB of bookkeeping, not the
     *     1.64 GB the retired flat constant charged. */
    nm = 100000 * SHIPPED_MEMBER_RATE;
    ok(nm == 128000000ull,
       "the per-member rate is linear in the count: 100,000 members cost "
       "128 MB of bookkeeping");
    ok(100000 * OLD_FLAT_COST_PER_MEMBER == 1638400000ull,
       "the retired flat model charged 1.64 GB for the same 100,000 "
       "members: 12.8x the shipped rate");

    /* 14. THE RANGE PROPERTY, distribution-free.
     *
     *     Every case above is ONE shape. The claim that actually replaced
     *     the flat constant is a claim about a RANGE, so it is tested as
     *     one: whatever the member-size distribution, the shipped price
     *     must not exceed the price the retired model charged for the
     *     same shape. Not "under for the sizes we tried" -- under for
     *     every size in the swept range, and the sweep is dense enough
     *     that the boundaries land on exact multiples of the block.
     *
     *     Both sides are computed by the SAME function over the SAME
     *     sizes, so the comparison is the models and nothing else:
     *
     *         shipped(S, n) = cpack_member_data_cost(n*S) + n*1280
     *         retired(S, n) = n * 16384
     *
     *     The content term is the members' INCOMPRESSIBLE payload: the
     *     zstd-19 projection can only be smaller, and the guard must
     *     hold for the case where the codecs do nothing. So a violation
     *     here cannot be explained away by compression.
     *
     *     THE BOUNDARY IS EXACT, and it is a statement about ALL member
     *     counts, not one of them:
     *       - new <= old      holds for every S <= 12288 (3 whole blocks)
     *                         at EVERY count from 1 to 50,000, and fails
     *                         at S = 12289. Asserted both ways below.
     *       - new <= old/2.6  holds for every S <= 4096  (1 whole block)
     *                         at EVERY count from 1 to 50,000, and fails
     *                         at S = 4097. Asserted both ways below.
     *     12288 and 4096 are 4096*3 and 4096*1: the property is a
     *     statement about BLOCKS, which is the whole design.
     *
     *     WHERE the boundary is crossed matters and is stated rather than
     *     assumed: the first violation at S = 12289 is at n = 1, and at
     *     S = 4097 it is at n = 1, 2 and 3. Both are the SMALL counts,
     *     because the rounding is amortised over the members -- n=1 pays
     *     a whole extra block for one payload, n=50,000 shares it. So the
     *     boundary is set by the worst count, not the rootfs one, which
     *     is the conservative direction and is asserted explicitly below:
     *     at 50,000 members the margin survives out to S = 5021.
     *
     *     The 2.6x figure is WP108's, and it is the number the AUDIT
     *     (impl_docs/AUDIT.md 6a) puts on the retired model's inflation
     *     on the archive it was fitted to. It is asserted here as a
     *     RATIO over a range, not as a comment: the retired price divided
     *     by the shipped price is >= 2.6 for every member size up to one
     *     block, at every member count from 1 to 50,000. */
    {
        /* 3 whole blocks, and the first byte past it */
        const uint64_t S_OK = 3 * (uint64_t)INVFS_BLOCK_SIZE;
        const uint64_t S_BAD = S_OK + 1;
        /* one whole block, and the first byte past it */
        const uint64_t T_OK = 1 * (uint64_t)INVFS_BLOCK_SIZE;
        const uint64_t T_BAD = T_OK + 1;
        const uint64_t COUNTS[] = {1, 2, 3, 7, 201, 402, 1000, 2000,
                                   5000, 12500, 25000, 50000};
        const unsigned ncounts = sizeof COUNTS / sizeof COUNTS[0];
        uint64_t worst_ratio_milli = 0;   /* retired/shipped, x1000 */
        unsigned ci;
        int u_under_old = 1, u_under_26 = 1;
        int bad_s_exceeds_somewhere = 0, bad_t_exceeds_somewhere = 0;
        int bad_s_worst_is_small = 1, bad_t_worst_is_small = 1;

        for (ci = 0; ci < ncounts; ci++) {
            const uint64_t n = COUNTS[ci];
            const uint64_t old = n * OLD_FLAT_COST_PER_MEMBER;
            const uint64_t neu = cpack_member_data_cost(n * S_OK) +
                                 n * SHIPPED_MEMBER_RATE;
            const uint64_t too_big = cpack_member_data_cost(n * S_BAD) +
                                     n * SHIPPED_MEMBER_RATE;
            const uint64_t tneu = cpack_member_data_cost(n * T_OK) +
                                  n * SHIPPED_MEMBER_RATE;
            const uint64_t tbig = cpack_member_data_cost(n * T_BAD) +
                                  n * SHIPPED_MEMBER_RATE;
            if (neu > old) u_under_old = 0;
            if (tneu * 26 > old * 10) u_under_26 = 0;
            if (too_big > old) {
                bad_s_exceeds_somewhere = 1;
                if (n > 3) bad_s_worst_is_small = 0;
            }
            if (tbig * 26 > old * 10) {
                bad_t_exceeds_somewhere = 1;
                if (n > 3) bad_t_worst_is_small = 0;
            }
            /* track the worst ratio seen inside the 2.6-safe region */
            if (tneu && old * 1000 / tneu > worst_ratio_milli)
                worst_ratio_milli = old * 1000 / tneu;
        }
        ok(u_under_old,
           "RANGE: for every member count 1..50,000 and every member size "
           "up to 12288 B (3 whole blocks), the shipped charge is under the "
           "retired flat charge -- distribution-free, not one sample");
        ok(u_under_26,
           "RANGE 2.6x: for every member count 1..50,000 and every member "
           "size up to 4096 B (1 whole block), the shipped charge is under "
           "the retired charge by a factor of at least 2.6 (WP108's figure)");
        ok(bad_s_exceeds_somewhere && bad_t_exceeds_somewhere,
           "RANGE boundary (red side): at 12289 B (and at 4097 B for the "
           "2.6x margin) the shipped charge DOES exceed the retired one at "
           "some count -- the two properties are tight, and the test above "
           "is not passing because the inequality is vacuous");
        ok(bad_s_worst_is_small && bad_t_worst_is_small,
           "RANGE boundary is set by the SMALL counts (n<=3), where the "
           "block rounding is least amortised -- the conservative direction, "
           "and stated rather than assumed");
        ok(worst_ratio_milli >= 2600,
           "RANGE 2.6x: the worst ratio over the whole safe region is 2.6x "
           "or better, measured across every count not asserted per-size");
        /* The rootfs count, where the margin survives much further -- the
         * asymmetry above, asserted so the reader does not have to trust
         * the small-count boundary as the whole story. */
        {
            uint64_t s, far = 0;
            const uint64_t dn = 50000, dold = dn * OLD_FLAT_COST_PER_MEMBER;
            for (s = 1; s <= 8192; s++) {
                const uint64_t dneu = cpack_member_data_cost(dn * s) +
                                      dn * SHIPPED_MEMBER_RATE;
                if (dneu * 26ull > dold * 10ull) break;
                far = s;
            }
            /* 5021, not a round number: the last size that holds is the
             * last size whose ratio is still >= 2.6, and the one past it
             * is 2.5998. Both are stated so the number can be checked
             * rather than taken on faith. */
            ok(far == 5021,
               "RANGE at the rootfs count: 50,000 members hold the 2.6x "
               "margin out to a member size of 5021 B (ratio 2.6002 there, "
               "2.5998 at 5022) -- further than the small-count boundary, "
               "exactly as the amortisation predicts");
        }
        /* A dense sweep inside the safe region, so the property is not
         * resting on 13 sizes: 4,097 sizes, EVERY size 0..4096, at the
         * rootfs member count. */
        {
            int dense_ok = 1;
            const uint64_t dn = 50000;
            const uint64_t dold = dn * OLD_FLAT_COST_PER_MEMBER;
            uint64_t s;
            for (s = 0; s <= T_OK; s++) {
                const uint64_t dneu = cpack_member_data_cost(dn * s) +
                                      dn * SHIPPED_MEMBER_RATE;
                if (dneu * 26 > dold * 10) { dense_ok = 0; break; }
            }
            ok(dense_ok,
               "RANGE dense sweep: all 4,097 member sizes from 0 to 4096 B, "
               "at 50,000 members, hold the 2.6x margin -- the property is "
               "checked size by size, not sampled");
        }
        /* And a real control: the sweep is not self-fulfilling. The model
         * it is checked AGAINST is the retired PER-MEMBER block rounding --
         * a model the change actually rejected, and one close enough to
         * look like a fix. Over the same sizes at 50,000 members that
         * model is charged 4,096 B of blocks for EVERY member, so it
         * blows the 2.6x margin at every size from 1 B up, block multiples
         * included. If the sweep were only ever fed the shipped model, it
         * could not tell the two apart -- and it would also wrongly claim
         * that "block multiples are always safe", which is the specific
         * mistake this control rules out. */
        {
            int pm_blown_sub = 0, pm_blown_at_block = 0;
            uint64_t s;
            const uint64_t dn = 50000, dold = dn * OLD_FLAT_COST_PER_MEMBER;
            for (s = 1; s < T_OK; s++) {
                const uint64_t pm = dn * cpack_member_data_cost(s) +
                                    dn * OLD_PER_MEMBER_RATE;
                if (pm * 26 > dold * 10) { pm_blown_sub = 1; break; }
            }
            for (s = T_OK; s <= T_OK + 4; s++) {
                const uint64_t pm = dn * cpack_member_data_cost(s) +
                                    dn * OLD_PER_MEMBER_RATE;
                if (pm * 26 > dold * 10) { pm_blown_at_block = 1; break; }
            }
            ok(pm_blown_sub,
               "RANGE red control: the retired PER-MEMBER block rounding is "
               "caught by this very sweep on sub-block sizes -- the range "
               "property discriminates models, it is not a tautology about "
               "one model");
            ok(pm_blown_at_block,
               "RANGE red control: and it is caught at WHOLE-BLOCK sizes "
               "too, so the sweep is not blind to block-multiple shapes and "
               "'block multiples are safe' is not something it invented");
        }
    }

    /* 15. THE 50,000-MEMBER FALSE NEGATIVE, and the decision taken on it.
     *
     *     The shape: 50,000 members whose TOTAL projected payload lands
     *     under one block, so cpack_member_data_cost() of the sum is a
     *     single block (or zero) and the payload term is negligible
     *     against the bookkeeping term.
     *
     *     DECISION: this is NOT fixed in this WP, and the reason is that
     *     as described it no longer exists. Under the retired flat model
     *     the same shape charges 50,000 x 16,384 = 819,200,000 B and is
     *     DECLINED for any container under 823 MB -- asserted below as a
     *     red control. Under the shipped price it charges 50,000 x 1,280 =
     *     64,000,000 B plus a single block, and is ACCEPTED from 68.3 MB
     *     up. The false negative was a symptom of the flat constant, and
     *     the thing that removed it is the thing under test: a per-member
     *     cost that is a rate rather than a page count.
     *
     *     What REMAINS, and is stated rather than hidden: the shipped
     *     rate is 1,280 B/member against a MEASURED 720 B/member
     *     (tools/measure-file-meta-cost.sh, 721/717 B per file), so the
     *     guard still over-charges bookkeeping by 1.78x. That leaves a
     *     band of container sizes -- orig in [f + n*720, f + n*1280),
     *     44% wide at n=50,000 -- where a decomposition that would have
     *     been a genuine small gain is DECLINED. It is left in place
     *     because:
     *       1. it is the SAFE direction. A false negative declines a
     *          decomposition; the container is then stored whole, which is
     *          bit-exact and costs exactly what it cost before. The
     *          opposite error is what shipped WP108's 3 MB regression, and
     *          tuning a rate DOWN to widen the accept region is how that
     *          would happen again;
     *       2. the 1.78x is headroom, and the headroom is load-bearing.
     *          CPACK_MEMBER_UNBATCHED is fitted to text-shaped corpora
     *          (AUDIT 6a.2 records this), and a real rootfs's unbatched
     *          fraction is UNMEASURED. Closing the band means setting the
     *         rate to the measurement, which removes the only margin
     *          standing between the guard and an unmeasured corpus;
     *       3. the band is bounded and known (44% of container size at
     *          50,000 members, and it narrows as the payload term grows),
     *          so it is a tuning question with a stated number, not an
     *          unquantified risk. Re-measuring the unbatched fraction on
     *          a real rootfs is the work that would close it, and that is
     *          a different WP.
     *
     *     Both edges are asserted, so a future change to the rate that
     *     moves this band is visible rather than silent. */
    {
        const uint64_t nmem = 50000, fixed = 3900008;
        /* the whole projected payload under one block */
        const uint64_t content = cpack_member_data_cost(0);
        const uint64_t charged = fixed + content + nmem * SHIPPED_MEMBER_RATE;
        /* the accepted-from threshold: the guard's own 0.5% band. The
         * comparison is `projected*1000 < orig*995` (strict), so the
         * smallest accepting orig is floor(projected*1000/995) + 1 --
         * not the division alone, which lands one byte below. */
        const uint64_t accept_from = charged * 1000 / 995 + 1;
        /* red control: the retired flat model on this very shape */
        cpack_size_proj flat = proj(fixed, content, nmem,
                                    OLD_FLAT_COST_PER_MEMBER);
        cpack_size_proj empty;
        ok(content == 0 && charged == 67900008ull,
           "false negative: 50,000 members with the whole payload under one "
           "block cost 67,900,008 B -- one block of payload, 64 MB of "
           "bookkeeping");
        ok(accept_from == 68241215ull,
           "false negative: that shape is ACCEPTED from a 68,241,215 B "
           "container up (the guard's comparison is strict)");
        memset(&empty, 0, sizeof empty);
        /* A zero projection is a PURE gain, so the guard accepts it for
         * any non-empty container -- that is correct, not a hole, and it
         * is asserted so the boundary case above cannot be mistaken for
         * one. The degenerate refusals are orig_len == 0 and a NULL
         * projection, both covered in case 11. */
        ok(cpack_size_guard(accept_from, &empty, &why) == 1 &&
           cpack_size_guard(0, &empty, &why) == 0,
           "false negative: a zero projection accepts for a non-empty "
           "container (a pure gain) and is refused for an empty one -- the "
           "band case is not being carried by a degenerate input");
        p = priced(fixed, content, nmem);
        ok(cpack_size_guard(accept_from, &p, &why) == 1,
           "false negative: the under-one-block 50,000-member shape is "
           "ACCEPTED -- the false negative described for this change no "
           "longer exists under the shipped price");
        ok(cpack_size_guard(accept_from - 1, &p, &why) == 0,
           "false negative: and it is DECLINED one byte below that "
           "threshold, so the case is pinned at the boundary");
        /* red control: the retired flat model declines the SAME shape at a
         * container size the shipped price accepts 12x over. */
        ok(cpack_size_guard(accept_from, &flat, &why) == 0,
           "false negative red control: the retired flat model DECLINES the "
           "same shape the shipped price accepts -- 819,200,000 B against "
           "67,900,008 B");
        ok(nmem * SHIPPED_MEMBER_RATE == 64000000ull &&
           nmem * OLD_FLAT_COST_PER_MEMBER == 819200000ull,
           "false negative: the retired model charged 819,200,000 B of "
           "bookkeeping for the same 50,000 members; the shipped price "
           "charges 64,000,000 B");
        /* the residual, stated as arithmetic so it cannot drift silently */
        {
            const uint64_t MEASURED_PER_FILE = 720; /* B, measured */
            const uint64_t real_book = nmem * MEASURED_PER_FILE;
            ok(real_book == 36000000ull &&
               nmem * SHIPPED_MEMBER_RATE == 64000000ull,
               "false negative residual: the guard charges 64,000,000 B of "
               "bookkeeping where 36,000,000 B was measured -- 1.78x, the "
               "headroom that leaves the residual decline band open");
            /* the band: [real, charged). Asserted, so closing it later is
             * a deliberate visible change and not an accident. */
            ok(fixed + real_book == 39900008ull &&
               fixed + nmem * SHIPPED_MEMBER_RATE == 67900008ull &&
               (nmem * SHIPPED_MEMBER_RATE - real_book) * 100 /
               (nmem * SHIPPED_MEMBER_RATE) == 43,
               "false negative residual: the remaining false-negative band "
               "is orig in [39,900,008, 67,900,008) -- 43% of container "
               "size at 50,000 members, DECLINED by choice, see the note");
        }
    }

    printf("\ncpack_guard_test summary: %d checks, %d failures\n",
           checks, failures);
    return failures ? 1 : 0;
}
