#!/usr/bin/env python3
"""Monte Carlo check of tools/lbsim.py's theory (contract-3 section 5).

Every expected value here comes from simulating the method itself (random ring placements, hashed rings,
hashed keys, random draws); lbsim's formulas are only the thing being checked. Fixed seeds, standard
library only, about 20 s. Run: python3 -B tests/lbsim_mc.py  (rc 0 = every check passed).
"""
from bisect import bisect_right
from fractions import Fraction
from itertools import accumulate
import math
import os
import random
import sys
import time

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "tools"))
import lbsim  # noqa: E402

WEIGHTS = (1, 3, 1, 1, 2)
HOSTS = [f"127.0.0.1:{8001 + i}" for i in range(len(WEIGHTS))]
RESULTS = []          # (item, theory, mc, err, limit, ok)
INFO = []             # reported, not checked


def mean(xs):
    return sum(xs) / len(xs)


def sd(xs):
    m = mean(xs)
    return math.sqrt(sum((x - m) ** 2 for x in xs) / (len(xs) - 1))


def check(item, theory, mc, limit, exact=False):
    err = abs(theory - mc)
    ok = err == 0 if exact else err <= limit
    RESULTS.append((item, theory, mc, err, 0.0 if exact else limit, ok))


def check_mean(item, theory, samples):
    """Mean of samples vs theory, within 3 standard errors."""
    se = sd(samples) / math.sqrt(len(samples)) if len(samples) > 1 else 0.0
    check(item, theory, mean(samples), max(3 * se, 1e-12))


def check_sd(item, theory_sd, samples):
    """Sample sd vs theory sd, within 10% (relative)."""
    check(item, theory_sd, sd(samples), 0.10 * theory_sd)


def check_rate(item, target, hits, n, tol):
    check(item, target, hits / n, tol)


# --- ring placements: points uniform on the circle [0, 1) -------------------------------------

def uniform_placement(rng, points):
    """One ring with points[i] points for backend i: (sorted positions, owner of each)."""
    rnd = rng.random
    ring = sorted([(rnd(), i) for i, m in enumerate(points) for _ in range(m)])
    return [p for p, _ in ring], [o for _, o in ring]


def arc_shares(pos, own, n):
    """Each point owns (previous point, this point]; the first one wraps from the last."""
    share = [0.0] * n
    for k, p in enumerate(pos):
        share[own[k]] += (p - pos[k - 1]) % 1.0 if len(pos) > 1 else 1.0
    return share


def removed_arcs(pos, own, n, r):
    """Where r's arcs go when r leaves: each one to the owner of the next point clockwise that is
    not r's. Returns the fraction of r's total that each backend receives."""
    got = [0.0] * n
    total = 0.0
    size = len(pos)
    nxt = None
    # Walk backwards twice round, so every r point knows the next non-r owner after it.
    for step in range(2 * size - 1, -1, -1):
        k = step % size
        if own[k] != r:
            nxt = own[k]
        elif step < size:
            arc = (pos[k] - pos[k - 1]) % 1.0
            got[nxt] += arc
            total += arc
    return [g / total for g in got]


def theory_sd_beta(m, total):
    """sd of Beta(m, total - m): the arc share of m out of total uniform points."""
    return math.sqrt(m * (total - m) / (total * total * (total + 1)))


def ring_checks(rng, method, points_per_weight, reps):
    n = len(WEIGHTS)
    pts = [points_per_weight * w for w in WEIGHTS]
    total = sum(pts)
    nkeys = 254
    th = lbsim.theory(method, list(WEIGHTS), nkeys)
    shares = [[] for _ in range(n)]
    inside = [0] * n
    keys_inside = [0] * n
    rnd = rng.random
    redis = {r: ([[] for _ in range(n)], [], []) for r in (0, 1)}   # r -> (per-j fractions, top, conc)
    for _ in range(reps):
        pos, own = uniform_placement(rng, pts)
        s = arc_shares(pos, own, n)
        # K keys land uniformly on the circle, so backend i gets each with probability s[i].
        cum = list(accumulate(s))
        counts = [0] * n
        for _ in range(nkeys):
            counts[min(bisect_right(cum, rnd() * cum[-1]), n - 1)] += 1
        for i in range(n):
            shares[i].append(s[i])
            lo, hi = th["arc95"][i]
            inside[i] += lo <= s[i] <= hi
            lo, hi = th["share95"][i]
            keys_inside[i] += lo <= counts[i] / nkeys <= hi
        for r in redis:
            frac = removed_arcs(pos, own, n, r)
            per_j, tops, conc = redis[r]
            for j in range(n):
                if j != r:
                    per_j[j].append(frac[j])
            top = max(frac[j] for j in range(n) if j != r)
            tops.append(top)
            conc.append(1.0 if top >= 0.8 else 0.0)
    for i in range(n):
        check_mean(f"{method} uniform arc mean b{i + 1}", th["share"][i], shares[i])
        check_sd(f"{method} uniform arc sd b{i + 1}", theory_sd_beta(pts[i], total), shares[i])
        check_rate(f"{method} arc95 coverage b{i + 1}", 0.95, inside[i], reps, 0.02)
        # Per member: an exact discrete range covers a bit more than 95% (contract section 7).
        check_rate(f"{method} keys({nkeys}) share95 coverage b{i + 1} (0.93..0.995)", 0.9625,
                   keys_inside[i], reps, 0.0325)
    for r, (per_j, tops, conc) in redis.items():
        tr = lbsim.theory(method, list(WEIGHTS), 254, remove=r)
        for j in range(n):
            if j != r:
                check_mean(f"{method} remove b{r + 1} (m={pts[r]}): redistribution to b{j + 1}",
                           tr["redistribution"][j], per_j[j])
        # Limits from contract-3 section 6: m_r <= 64 has the exact formula for P (3 binomial SE over
        # the placements) and a near-exact top (0.03); m_r > 64 keeps the binomial approximation (0.10).
        m = pts[r]
        exact = m == 1               # one arc: everything goes to one server, always
        p = tr["p_concentrated"]
        p_limit = 3 * math.sqrt(max(p * (1 - p), 1e-12) / reps) if m <= 64 else 0.10
        check(f"{method} remove b{r + 1} (m={m}): P(>=80%)", p, mean(conc), p_limit, exact)
        check(f"{method} remove b{r + 1} (m={m}): top share", tr["top_share"], mean(tops),
              0.03 if m <= 64 else 0.10, exact)
        if exact:
            check(f"{method} remove b{r + 1}: MC itself is 1", 1.0, mean(conc), 0.0, True)


# --- hashed rings: lbsim.build_ring with host names that change per seed ------------------------

def hashed_ring_checks(rng, method, points_per_weight, reps, nkeys=254):
    n = len(WEIGHTS)
    pts = [points_per_weight * w for w in WEIGHTS]
    total = sum(pts)
    th = lbsim.theory(method, list(WEIGHTS), nkeys)
    arcs = [[] for _ in range(n)]
    keys = [[] for _ in range(n)]
    inside = [0] * n
    for seed in range(reps):
        hosts = [f"10.{seed >> 8}.{seed & 255}.{i}:80" for i in range(n)]
        ring = lbsim.build_ring(hosts, list(WEIGHTS), points_per_weight)
        a = ring.arc_shares()
        counts = [0] * n
        for _ in range(nkeys):     # random keys, so the K-key sampling error is in the spread
            counts[ring.lookup(f"{rng.randrange(256)}.{rng.randrange(256)}.{rng.randrange(256)}."
                               f"{rng.randrange(256)}")] += 1
        for i in range(n):
            arcs[i].append(a[i])
            keys[i].append(counts[i] / nkeys)
            lo, hi = th["share95"][i]
            inside[i] += lo <= counts[i] / nkeys <= hi
    for i in range(n):
        check_mean(f"{method} hashed arc mean b{i + 1}", th["share"][i], arcs[i])
        var_s = theory_sd_beta(pts[i], total) ** 2
        check_sd(f"{method} hashed arc sd b{i + 1}", math.sqrt(var_s), arcs[i])
        # (b): placement spread + K-key sampling error, by the law of total variance.
        mu = th["share"][i]
        check_sd(f"{method} keys({nkeys}) sd b{i + 1}", math.sqrt(var_s + (mu * (1 - mu) - var_s) / nkeys), keys[i])
        check_mean(f"{method} keys({nkeys}) mean b{i + 1}", mu, keys[i])
    # share95 (b) is an exact discrete range (contract section 7): a bit above 95%. Per member it is checked
    # on the uniform placements (R >= 2000) and by the exact small-K ranges; here R is small (250 for
    # ketama, sd of a member's rate ~0.012), so members are reported and only the pooled rate is checked.
    check_rate(f"{method} hashed keys({nkeys}) share95 coverage, pooled (0.93..0.995)", 0.9625,
               sum(inside), reps * n, 0.0325)
    INFO.append(f"{method} hashed keys({nkeys}) share95 coverage per member (R={reps}): "
                + "  ".join(f"b{i + 1} {inside[i] / reps:.3f}" for i in range(n)))


# --- hashed keys for mod / hrw / random ---------------------------------------------------------

def mod_checks(key_hashes):
    for weights, remove, add, hand in (((1, 1, 1), 1, False, 2 / 3), ((1, 3, 1, 1, 2), 2, False, None),
                                       ((1, 2), 0, False, None), ((2, 1, 1), None, True, None),
                                       ((1, 3, 1, 1, 2), 1, False, None)):
        slots = [i for i, w in enumerate(weights) for _ in range(w)]
        after = ([i for i, w in enumerate(weights) if i != remove for _ in range(w)] if remove is not None
                 else slots + [len(weights)])
        old = [slots[h % len(slots)] for h in key_hashes]
        new = [after[h % len(after)] for h in key_hashes]
        moved = sum(a != b for a, b in zip(old, new)) / len(key_hashes)
        exact = lbsim.mod_exact(list(weights), remove=remove, add=add)
        what = f"mod {','.join(map(str, weights))} " + (f"remove b{remove + 1}" if remove is not None else "add")
        check(f"{what}: moved (200k hashed keys)", exact["moved"], moved, 0.01)
        if hand is not None:
            check(f"{what}: moved = hand count {hand:.4f}", hand, exact["moved"], 1e-12)
        if remove is not None:
            from_r = [b for a, b in zip(old, new) if a == remove]
            for j in range(len(weights)):
                if j != remove:
                    check(f"{what}: redistribution to b{j + 1}", exact["redistribution"][j],
                          from_r.count(j) / len(from_r), 0.01)


def hrw_score(key_host_hash, w):
    u = (key_host_hash + 0.5) / 2 ** 32
    return -w / math.log(u)


def hrw_hashes(key, hosts):
    """ring_hashstr(f"{key}|{host}") for every host, sharing the FNV-1a pass over the common prefix
    (the hash is defined in contract-3 section 5; lbsim.ring_hashstr is checked against it below)."""
    prefix = os.path.commonprefix(hosts)
    h = 2166136261
    for byte in f"{key}|{prefix}".encode():
        h = ((h ^ byte) * 16777619) & 0xFFFFFFFF
    out = []
    for host in hosts:
        x = h
        for byte in host[len(prefix):].encode():
            x = ((x ^ byte) * 16777619) & 0xFFFFFFFF
        x = ((x ^ (x >> 16)) * 0x85EBCA6B) & 0xFFFFFFFF
        x = ((x ^ (x >> 13)) * 0xC2B2AE35) & 0xFFFFFFFF
        out.append(x ^ (x >> 16))
    return out


# --- exact small-K ranges: share95 / moved95 against independently computed quantiles -------------

def exact_range(pmf, keys):
    """Contract section 7: lo = smallest k with CDF >= 0.025, hi = smallest k with CDF >= 0.975."""
    cdf, lo, hi = Fraction(0), None, None
    for k, p in enumerate(pmf):
        cdf += p
        if lo is None and cdf >= Fraction(1, 40):
            lo = k
        if hi is None and cdf >= Fraction(39, 40):
            hi = k
    return lo / keys, hi / keys


def binomial_range(keys, p):
    return exact_range([math.comb(keys, k) * p ** k * (1 - p) ** (keys - k) for k in range(keys + 1)], keys)


def beta_binomial_range(keys, a, b):
    def beta(x, y):        # B(x, y) for positive integers
        return Fraction(math.factorial(x - 1) * math.factorial(y - 1), math.factorial(x + y - 1))
    return exact_range([math.comb(keys, k) * beta(k + a, keys - k + b) / beta(a, b) for k in range(keys + 1)],
                       keys)


def mod_moved_fraction(weights, remove=None):
    """Exact moved fraction of mod by counting residues over one full period (own enumeration)."""
    before = [i for i, w in enumerate(weights) for _ in range(w)]
    after = ([i for i, w in enumerate(weights) if i != remove for _ in range(w)] if remove is not None
             else before + [len(weights)])
    period = math.lcm(len(before), len(after))
    return Fraction(sum(before[x % len(before)] != after[x % len(after)] for x in range(period)), period)


def exact_range_checks():
    weights = [1, 3, 1]
    total = sum(weights)
    for keys in (7, 20):
        for method in ("mod", "hrw", "random"):
            th = lbsim.theory(method, weights, keys)
            for i, w in enumerate(weights):
                want = binomial_range(keys, Fraction(w, total))
                check(f"K={keys} {method} share95 b{i + 1} = Binomial range {want}", 1,
                      int(tuple(th["share95"][i]) == want), 0, True)
            for remove, add in ((1, False), (None, True)):
                tr = lbsim.theory(method, weights, keys, remove=remove, add=add)
                if method == "mod":
                    p = mod_moved_fraction(weights, remove)
                else:
                    p = Fraction(weights[remove], total) if remove is not None else Fraction(1, total + 1)
                want = binomial_range(keys, p)
                what = f"remove b{remove + 1}" if remove is not None else "add"
                check(f"K={keys} {method} {what}: moved95 = Binomial range {want}", 1,
                      int(tr["moved95"] is not None and tuple(tr["moved95"]) == want), 0, True)
        for method, points in (("ring1", 1), ("ring10", 10), ("ketama", 160)):
            th = lbsim.theory(method, weights, keys)
            for i, w in enumerate(weights):
                want = beta_binomial_range(keys, points * w, points * (total - w))
                check(f"K={keys} {method} share95 b{i + 1} = BetaBinomial range {want}", 1,
                      int(tuple(th["share95"][i]) == want), 0, True)
            tr = lbsim.theory(method, weights, keys, remove=1)
            check(f"K={keys} {method} remove b2: moved95 = b2's share95", 1,
                  int(tr["moved95"] is not None and tuple(tr["moved95"]) == tuple(th["share95"][1])), 0, True)
            ta = lbsim.theory(method, weights, keys, add=True)
            want = beta_binomial_range(keys, points, points * total)
            check(f"K={keys} {method} add: moved95 = BetaBinomial range {want}", 1,
                  int(ta["moved95"] is not None and tuple(ta["moved95"]) == want), 0, True)


def keyed_checks(name, pick_before, pick_after, nkeys, r):
    """Shares, moved and redistribution of a per-key method against lbsim.theory."""
    n = len(WEIGHTS)
    th = lbsim.theory(name, list(WEIGHTS), nkeys, remove=r)
    before = [pick_before(k) for k in range(nkeys)]
    after = [pick_after(k, before[k]) for k in range(nkeys)]
    for i in range(n):
        p = th["share"][i]
        check(f"{name} share b{i + 1}", p, before.count(i) / nkeys, 3 * math.sqrt(p * (1 - p) / nkeys))
    pr = WEIGHTS[r] / sum(WEIGHTS)
    moved = sum(a != b for a, b in zip(before, after)) / nkeys
    check(f"{name} remove b{r + 1}: moved", th["moved"], moved, 3 * math.sqrt(pr * (1 - pr) / nkeys))
    check(f"{name} remove b{r + 1}: only b{r + 1}'s keys move", 0,
          sum(a != b and a != r for a, b in zip(before, after)), 0, True)
    from_r = [b for a, b in zip(before, after) if a == r]
    for j in range(n):
        if j != r:
            q = th["redistribution"][j]
            check(f"{name} remove b{r + 1}: redistribution to b{j + 1}", q, from_r.count(j) / len(from_r),
                  3 * math.sqrt(q * (1 - q) / len(from_r)))


def main():
    t0 = time.monotonic()
    rng = random.Random(20261125)
    for method, ppw, reps in (("ring1", 1, 4000), ("ring10", 10, 3000), ("ketama", 160, 2000)):
        ring_checks(rng, method, ppw, reps)
    for method, ppw, reps in (("ring1", 1, 1500), ("ring10", 10, 800), ("ketama", 160, 250)):
        hashed_ring_checks(rng, method, ppw, reps)

    nkeys = 200_000
    keys = [f"k{i}" for i in range(nkeys)]
    mod_checks([lbsim.ring_hashstr(k) for k in keys])

    exact_range_checks()

    # hrw: the method as defined (score -w / ln u per key and backend), r = b2 (weight 3)
    r = 1
    sample = [hrw_hashes(k, HOSTS) == [lbsim.ring_hashstr(f"{k}|{h}") for h in HOSTS] for k in keys[:2000]]
    check("hrw: shared-prefix hash = lbsim.ring_hashstr (2000 keys)", 1, int(all(sample)), 0, True)
    scores = [[hrw_score(x, w) for x, w in zip(hrw_hashes(k, HOSTS), WEIGHTS)] for k in keys]
    keyed_checks("hrw", lambda k: max(range(len(WEIGHTS)), key=scores[k].__getitem__),
                 lambda k, old: max((j for j in range(len(WEIGHTS)) if j != r), key=scores[k].__getitem__),
                 nkeys, r)
    # random: one weighted draw per key, kept; r's keys draw again among the rest
    drng = random.Random(7)
    rest = [j for j in range(len(WEIGHTS)) if j != r]
    keyed_checks("random", lambda k: drng.choices(range(len(WEIGHTS)), WEIGHTS)[0],
                 lambda k, old: drng.choices(rest, [WEIGHTS[j] for j in rest])[0] if old == r else old,
                 nkeys, r)

    # Contract section 8: on a ring the keys range (b) adds sampling error to the arc range (a), so it
    # must (nearly) contain it: hi_keys >= hi_arc - 1/K and lo_keys <= lo_arc + 1/K. Checked for the
    # weights before and after removing b2 (1,1,1,2: the ring1 b1 case with hi_keys 60.6% vs hi_arc 60.2%).
    for weights in (list(WEIGHTS), [1, 1, 1, 2]):
        for method in ("ring1", "ring10", "ketama"):
            t = lbsim.theory(method, weights, 254)
            worst = min(min(t["share95"][i][1] - t["arc95"][i][1] + 1 / 254,
                            t["arc95"][i][0] + 1 / 254 - t["share95"][i][0]) for i in range(len(weights)))
            check(f"{method} {','.join(map(str, weights))}: keys 95% contains arc 95% (worst margin {worst:+.4f})",
                  1, int(worst >= 0), 0, True)

    # rr: exact shares, no key affinity
    th = lbsim.theory("rr", list(WEIGHTS), 254, remove=r)
    for i, w in enumerate(WEIGHTS):
        check(f"rr share b{i + 1} = w/W", w / sum(WEIGHTS), th["share"][i], 1e-12)
    check("rr remove: moved is None (no key affinity)", 1, int(th["moved"] is None), 0, True)

    width = max(len(item) for item, *_ in RESULTS)
    print(f"{'item':<{width}}  {'theory':>8}  {'MC':>8}  {'err':>8}  {'limit':>8}")
    for item, theory, mc, err, limit, ok in RESULTS:
        print(f"{item:<{width}}  {theory:8.4f}  {mc:8.4f}  {err:8.4f}  {limit:8.4f}  {'ok' if ok else 'FAIL'}")
    for line in INFO:
        print(f"info: {line}")
    bad = [row for row in RESULTS if not row[5]]
    print(f"lbsim_mc: {len(RESULTS)} checks, {len(bad)} failed, {time.monotonic() - t0:.1f}s")
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main())
