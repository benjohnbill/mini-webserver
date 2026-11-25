#!/usr/bin/env python3
"""Monte Carlo check of tools/lbsim.py's theory (contract-3 section 5, contract-4 section 6).

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


def check_sd(item, theory_sd, samples, rel=0.10):
    """Sample sd vs theory sd, within rel (relative): 4% for the large uniform runs, 10% for hashed."""
    check(item, theory_sd, sd(samples), rel * theory_sd)


def check_concentration(item, model, conc, tops):
    """P(>=80%) within 3 binomial SE of the MC trials, top within 0.03 (contract-3 sections 6 and 9)."""
    p = model["p_concentrated"]
    check(f"{item}: P(>=80%)", p, mean(conc), 3 * math.sqrt(max(p * (1 - p), 1e-12) / len(conc)))
    check(f"{item}: top share", model["top_share"], mean(tops), 0.03)


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
        check_sd(f"{method} uniform arc sd b{i + 1}", theory_sd_beta(pts[i], total), shares[i], 0.04)
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
        # Contract-3 section 9: the exact formula for every m_r, so P within 3 SE and top within 0.03.
        m = pts[r]
        if m == 1:                   # one arc: everything goes to one server, always
            check(f"{method} remove b{r + 1} (m=1): P(>=80%)", tr["p_concentrated"], mean(conc), 0, True)
            check(f"{method} remove b{r + 1} (m=1): top share", tr["top_share"], mean(tops), 0, True)
            check(f"{method} remove b{r + 1}: MC itself is 1", 1.0, mean(conc), 0.0, True)
        else:
            check_concentration(f"{method} remove b{r + 1} (m={m})", tr, conc, tops)


def ring_remove_mc(rng, method, weights, points_per_weight, r, reps):
    """Concentration and top of removing r, on uniform placements of an arbitrary weight list."""
    n = len(weights)
    pts = [points_per_weight * w for w in weights]
    conc, tops = [], []
    for _ in range(reps):
        pos, own = uniform_placement(rng, pts)
        frac = removed_arcs(pos, own, n, r)
        top = max(frac[j] for j in range(n) if j != r)
        tops.append(top)
        conc.append(1.0 if top >= 0.8 else 0.0)
    tr = lbsim.theory(method, list(weights), 254, remove=r)
    check_concentration(f"{method} {','.join(map(str, weights))} remove b{r + 1} (m={pts[r]})", tr, conc, tops)


def ring_add_mc(rng, method, points_per_weight, reps):
    """--add by simulation: a new weight-1 backend's arc share is the moved fraction."""
    pts = [points_per_weight * w for w in WEIGHTS] + [points_per_weight]
    moved = []
    for _ in range(reps):
        pos, own = uniform_placement(rng, pts)
        moved.append(arc_shares(pos, own, len(pts))[-1])
    check_mean(f"{method} add (uniform placements): moved", lbsim.theory(method, list(WEIGHTS), 254, add=True)["moved"],
               moved)


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
    prefix = hosts[0]
    for host in hosts[1:]:
        while not host.startswith(prefix):
            prefix = prefix[:-1]
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


def local_hosts(count):
    return [f"127.0.0.1:{8001 + i}" for i in range(count)]


def keyed_concentration_checks(rng, key_hashes, trials=3000, hrw_trials=2000, nkeys=254):
    """Concentration and top of mod / hrw / random when r leaves (contract-3 section 9.3).
    mod: exact over hashed keys. hrw / random: trials of K keys, conditioned on r having keys."""
    for weights, r in (((3, 1, 1), 1), ((1, 1, 4, 1, 1), 0)):
        n = len(weights)
        rest = [j for j in range(n) if j != r]
        what = f"{','.join(map(str, weights))} remove b{r + 1}"
        # mod over 200k hashed keys: where r's keys go is fixed by the slot lists, so top is a fraction.
        slots = [i for i, w in enumerate(weights) for _ in range(w)]
        after = [i for i, w in enumerate(weights) if i != r for _ in range(w)]
        dest = [after[h % len(after)] for h in key_hashes if slots[h % len(slots)] == r]
        top = max(dest.count(j) for j in rest) / len(dest)
        th = lbsim.theory("mod", list(weights), nkeys, remove=r)
        check(f"mod {what}: top share (200k hashed keys)", th["top_share"], top, 0.01)
        check(f"mod {what}: P(>=80%) (0 or 1)", th["p_concentrated"], float(top >= 0.8), 0, True)

        def trial_stats(pick_dests, count=trials):
            conc, tops = [], []
            for _ in range(count):
                d = pick_dests()
                if not d:            # r had no key in this sample: excluded (conditional on n > 0)
                    continue
                t = max(d.count(j) for j in rest) / len(d)
                tops.append(t)
                conc.append(1.0 if t >= 0.8 else 0.0)
            return conc, tops

        # hrw, pure model: every key draws u ~ U(0,1) per backend and scores -w / ln u, with fresh draws in
        # every trial. This checks the formula alone; that the real hash behaves like U(0,1) is checked
        # separately on 200k hashed keys (hrw share / moved / redistribution below). Resampling a fixed
        # pool of hashed keys is not an independent sample: the pool's own error in where r's keys go is
        # amplified ~5x in P(>=80%) (it gave 0.247 against the exact 0.220 for 3,1,1).
        rnd = rng.random

        def hrw_dests():
            out = []
            for _ in range(nkeys):
                score = [-w / math.log(1.0 - rnd()) for w in weights]
                if max(range(n), key=score.__getitem__) == r:
                    out.append(max(rest, key=score.__getitem__))
            return out
        conc, tops = trial_stats(hrw_dests, hrw_trials)
        check_concentration(f"hrw {what} (K={nkeys}, {len(conc)} trials)", lbsim.theory("hrw", list(weights), nkeys, remove=r),
                            conc, tops)
        # random: each key draws by weight; r's keys draw again among the rest by weight.
        rest_w = [weights[j] for j in rest]

        def random_dests():
            hit = rng.choices(range(n), weights, k=nkeys).count(r)
            return rng.choices(rest, rest_w, k=hit) if hit else []
        conc, tops = trial_stats(random_dests)
        check_concentration(f"random {what} (K={nkeys}, {len(conc)} trials)",
                            lbsim.theory("random", list(weights), nkeys, remove=r), conc, tops)


def even_top_checks(rng, nkeys=254):
    """hrw / random top when q is even (1x8, remove b1). The theory's top is the independent-binomial
    approximation (≈), known to come out low; check the direction and the size of that bias."""
    weights, r = [1] * 8, 0
    n = len(weights)
    rest = list(range(1, n))
    rnd = rng.random

    def hrw_dests():                 # pure model, as in keyed_concentration_checks
        out = []
        for _ in range(nkeys):
            score = [-w / math.log(1.0 - rnd()) for w in weights]
            if max(range(n), key=score.__getitem__) == r:
                out.append(max(rest, key=score.__getitem__))
        return out

    def random_dests():
        hit = rng.choices(range(n), weights, k=nkeys).count(r)
        return rng.choices(rest, k=hit) if hit else []

    for method, dests, trials in (("hrw", hrw_dests, 300), ("random", random_dests, 1500)):
        tops = []
        for _ in range(trials):
            d = dests()
            if d:                    # conditional on r having keys
                tops.append(max(d.count(j) for j in rest) / len(d))
        approx = lbsim.theory(method, weights, nkeys, remove=r)["top_share"]
        mc = mean(tops)
        what = f"{method} 1x8 remove b1 (K={nkeys}, {len(tops)} trials): top"
        check(f"{what}: approximation <= MC (biased low)", 1, int(approx <= mc), 0, True)
        check(f"{what}: MC - approximation <= 0.05", approx, mc, 0.05)


def keyed_add_checks(rng, nkeys=60_000):
    """--add by simulation for hrw (a new host joins the race). The random row is a tautology on purpose:
    lbsim defines random --add as "the new server takes each key with probability 1/(W+1)", so drawing
    that only re-checks the definition against theory's number, not an independent property."""
    hosts = [f"h:{i + 1}" for i in range(len(WEIGHTS) + 1)]
    weights = list(WEIGHTS) + [1]
    moved = 0
    for i in range(nkeys):
        score = [hrw_score(x, w) for x, w in zip(hrw_hashes(f"a{i}", hosts), weights)]
        moved += max(range(len(weights)), key=score.__getitem__) == len(WEIGHTS)
    p = lbsim.theory("hrw", list(WEIGHTS), 254, add=True)["moved"]
    check(f"hrw add ({nkeys // 1000}k hashed keys): moved", p, moved / nkeys, 3 * math.sqrt(p * (1 - p) / nkeys))
    total = sum(WEIGHTS) + 1
    moved = sum(rng.randrange(total) == 0 for _ in range(nkeys))
    p = lbsim.theory("random", list(WEIGHTS), 254, add=True)["moved"]
    check(f"random add ({nkeys // 1000}k draws of its own 1/(W+1) rule, definition check): moved", p,
          moved / nkeys, 3 * math.sqrt(p * (1 - p) / nkeys))


# --- contract 4: extra moved and moved 95% ---------------------------------------------------------

def mod_extra_fraction(weights, remove=None):
    """Own enumeration over one period: keys whose server changes although they were not on the removed
    server (remove) or did not go to the new server (add)."""
    before = [i for i, w in enumerate(weights) for _ in range(w)]
    after = ([i for i, w in enumerate(weights) if i != remove for _ in range(w)] if remove is not None
             else before + [len(weights)])
    t = remove if remove is not None else len(weights)
    period = math.lcm(len(before), len(after))
    extra = 0
    for x in range(period):
        old, new = before[x % len(before)], after[x % len(after)]
        extra += old != new and (old != t if remove is not None else new != t)
    return Fraction(extra, period)


def mod_extra95_checks():
    """mod extra95 at small K: Binomial(K, own enumerated extra) range by exact Fractions, compared exactly."""
    for keys in (7, 20):
        for weights in ((1, 3, 1), (1, 1, 1), (3, 1, 1), (1, 3, 1, 1, 2), (1, 2)):
            for remove in (*range(len(weights)), None):
                p = mod_extra_fraction(weights, remove)
                want = binomial_range(keys, p)
                th = lbsim.theory("mod", list(weights), keys, remove=remove, add=remove is None)
                what = f"K={keys} mod {','.join(map(str, weights))} " + (
                    f"remove b{remove + 1}" if remove is not None else "add")
                check(f"{what}: extra95 = Binomial range of own extra {p} {want}", 1,
                      int(th["extra95"] is not None and tuple(th["extra95"]) == want), 0, True)


def mod_extra_checks(key_hashes):
    """theory("mod")["extra"] and mod_exact()["extra"] against the own enumeration, then against hashed keys."""
    cases = [((1, 1, 1), r) for r in range(3)] + [((1, 3, 1, 1, 2), r) for r in range(5)]
    cases += [((1, 2), 1), ((2, 1, 1), 0), ((3, 1, 1), 1), ((1, 1, 4, 1, 1), 0)]
    cases += [(w, None) for w in ((1, 1, 1), (1, 3, 1, 1, 2), (2, 1, 1), (1, 2))]
    for weights, remove in cases:
        what = f"mod {','.join(map(str, weights))} " + (f"remove b{remove + 1}" if remove is not None else "add")
        want = float(mod_extra_fraction(weights, remove))
        th = lbsim.theory("mod", list(weights), 254, remove=remove, add=remove is None)
        check(f"{what}: extra = own enumeration", want, th["extra"], 1e-12)
        check(f"{what}: mod_exact extra = own enumeration", want,
              lbsim.mod_exact(list(weights), remove=remove, add=remove is None)["extra"], 1e-12)
    check("mod 1,1,1 remove b2: extra = hand count 1/3", 1 / 3, float(mod_extra_fraction((1, 1, 1), 1)), 1e-12)
    check("mod 1,1,1 add: extra = hand count 1/2", 1 / 2, float(mod_extra_fraction((1, 1, 1))), 1e-12)
    for weights, remove in (((1, 3, 1, 1, 2), 2), ((1, 3, 1, 1, 2), None), ((3, 1, 1), 1)):
        slots = [i for i, w in enumerate(weights) for _ in range(w)]
        after = ([i for i, w in enumerate(weights) if i != remove for _ in range(w)] if remove is not None
                 else slots + [len(weights)])
        t = remove if remove is not None else len(weights)
        extra = 0
        for h in key_hashes:
            old, new = slots[h % len(slots)], after[h % len(after)]
            extra += old != new and (old != t if remove is not None else new != t)
        p = lbsim.theory("mod", list(weights), 254, remove=remove, add=remove is None)["extra"]
        what = f"mod {','.join(map(str, weights))} " + (f"remove b{remove + 1}" if remove is not None else "add")
        check(f"{what}: extra ({len(key_hashes) // 1000}k hashed keys)", p, extra / len(key_hashes),
              3 * math.sqrt(max(p * (1 - p), 1e-12) / len(key_hashes)))


def consistent_extra_checks(rng, reps=12, nkeys=254):
    """Consistent hashing: a key not on the changed server keeps its server, so the measured extra moved
    is 0 in every trial. Real hashed rings / hrw with host names and keys that change per trial."""
    n = len(WEIGHTS)
    for method in ("ring1", "ring10", "ketama", "hrw", "random"):
        for change in ("remove", "add"):
            for r in ([1, 2] if change == "remove" else [None]):
                th = lbsim.theory(method, list(WEIGHTS), nkeys, remove=r, add=r is None)
                what = f"{method} {change}" + (f" b{r + 1}" if r is not None else "")
                check(f"{what}: theory extra = 0", 0.0, th["extra"], 0, True)
                check(f"{what}: theory extra95 = (0, 0)", 1, int(tuple(th["extra95"]) == (0.0, 0.0)), 0, True)
        worst = 0
        for trial in range(reps):
            hosts = [f"10.{trial}.{rng.randrange(256)}.{i}:80" for i in range(n + 1)]
            weights = list(WEIGHTS) + [1]
            keys = [f"x{rng.getrandbits(40)}" for _ in range(nkeys)]
            for r in (trial % n, None):            # remove one member, then add a weight-1 member
                old_idx = list(range(n))
                new_idx = [i for i in range(n) if i != r] if r is not None else list(range(n + 1))
                if method in ("ring1", "ring10", "ketama"):
                    ppw = {"ring1": 1, "ring10": 10, "ketama": 160}[method]
                    ring_old = lbsim.build_ring([hosts[i] for i in old_idx], [weights[i] for i in old_idx], ppw)
                    ring_new = lbsim.build_ring([hosts[i] for i in new_idx], [weights[i] for i in new_idx], ppw)
                    pairs = [(old_idx[ring_old.lookup(k)], new_idx[ring_new.lookup(k)]) for k in keys]
                elif method == "hrw":
                    pairs = []
                    for k in keys:
                        score = [hrw_score(x, w) for x, w in zip(hrw_hashes(k, hosts), weights)]
                        pairs.append((max(old_idx, key=score.__getitem__), max(new_idx, key=score.__getitem__)))
                else:                                  # random as lbsim defines it (contract-3 section 3)
                    olds = rng.choices(old_idx, [weights[i] for i in old_idx], k=nkeys)
                    if r is not None:
                        rest = [i for i in old_idx if i != r]
                        pairs = [(o, rng.choices(rest, [weights[i] for i in rest])[0] if o == r else o)
                                 for o in olds]
                    else:
                        pairs = [(o, n if rng.randrange(sum(WEIGHTS) + 1) == 0 else o) for o in olds]
                t = r if r is not None else n
                extra = sum(o != m and (o != t if r is not None else m != t) for o, m in pairs)
                worst = max(worst, extra)
        check(f"{method}: measured extra moved = 0 in all {2 * reps} hashed remove/add trials (max count)",
              0, worst, 0, True)


def moved95_coverage(rng, reps=400, nkeys=254, r=2):
    """moved/K inside theory's moved95, with placement and keys drawn fresh every trial (0.93..0.995)."""
    n = len(WEIGHTS)
    total = sum(WEIGHTS)
    rnd = rng.random
    for method in ("ring1", "ring10", "ketama", "hrw", "random", "mod"):
        for change in ("remove", "add"):
            rem = r if change == "remove" else None
            lo, hi = lbsim.theory(method, list(WEIGHTS), nkeys, remove=rem, add=rem is None)["moved95"]
            t = r if rem is not None else n             # removal: keys of r move; add: keys won by the new one
            weights = list(WEIGHTS) if rem is not None else list(WEIGHTS) + [1]
            count = reps if method != "ketama" else reps // 2
            hits = 0
            for _ in range(count):
                if method in ("ring1", "ring10", "ketama"):
                    ppw = {"ring1": 1, "ring10": 10, "ketama": 160}[method]
                    pos, own = uniform_placement(rng, [ppw * w for w in weights])
                    size = len(pos)
                    moved = sum(own[bisect_right(pos, rnd()) % size] == t for _ in range(nkeys))
                elif method == "hrw":           # pure model: u ~ U(0,1) per key and backend
                    moved = 0
                    for _ in range(nkeys):
                        score = [-w / math.log(1.0 - rnd()) for w in weights]
                        moved += max(range(len(weights)), key=score.__getitem__) == t
                elif method == "random":
                    moved = (rng.choices(range(n), WEIGHTS, k=nkeys).count(r) if rem is not None
                             else sum(rng.randrange(total + 1) == 0 for _ in range(nkeys)))
                else:                           # mod: fresh uniform 32-bit key hashes, fixed slot order
                    before = [i for i, w in enumerate(WEIGHTS) for _ in range(w)]
                    after = ([i for i in before if i != r] if rem is not None else before + [n])
                    moved = 0
                    for _ in range(nkeys):
                        h = rng.getrandbits(32)
                        moved += before[h % len(before)] != after[h % len(after)]
                hits += lo <= moved / nkeys <= hi
            check_rate(f"{method} {change}{f' b{r + 1}' if rem is not None else ''}: moved95 coverage "
                       f"({count} trials, 0.93..0.995)", 0.9625, hits, count, 0.0325)


def output_checks():
    """New summary header and an extra moved line per keyed method, no '!' on consistent methods."""
    import subprocess
    script = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "tools", "lbsim.py")
    for change in (["--remove", "b3"], ["--add"]):
        proc = subprocess.run([sys.executable, "-B", script, "--backends", "5", "--weights", "1,3,1,1,2", *change],
                              capture_output=True, text=True, check=False)
        out = proc.stdout.splitlines()
        what = " ".join(change)
        check(f"output {what}: exit code 0", 0, proc.returncode, 0, True)
        # summary rows, split on whitespace (no column positions): method, dev, moved 95%, extra moved, P, note...
        rows = {}
        if "summary (theory)" in out:
            for line in out[out.index("summary (theory)") + 1:]:
                tok = line.split()
                if tok and tok[0] in lbsim.METHODS:
                    rows[tok[0]] = tok
        check(f"output {what}: summary has a row for every method", 1, int(set(rows) == set(lbsim.METHODS)), 0, True)
        rem = 2 if change[0] == "--remove" else None
        tm = lbsim.theory("mod", [1, 3, 1, 1, 2], 254, remove=rem, add=rem is None)
        lo, hi = tm["moved95"]
        want = [f"{100 * lo:.1f}-{hi:.1%}", f"{tm['extra']:.1%}"]
        check(f"output {what}: summary mod moved 95% / extra = theory {want}", 1,
              int(rows.get("mod", [None] * 4)[2:4] == want), 0, True)
        check(f"output {what}: summary rr moved 95% / extra = n/a n/a", 1,
              int(rows.get("rr", [None] * 4)[2:4] == ["n/a", "n/a"]), 0, True)
        for m in ("random", "ring1", "ring10", "ketama", "hrw"):
            tr = lbsim.theory(m, [1, 3, 1, 1, 2], 254, remove=rem, add=rem is None)
            lo, hi = tr["moved95"]
            want = [f"{100 * lo:.1f}-{hi:.1%}", "0.0%"]
            check(f"output {what}: summary {m} moved 95% = theory, extra 0.0%", 1,
                  int(rows.get(m, [None] * 4)[2:4] == want), 0, True)
        header = [line for line in out if "max share dev(95%)" in line]
        check(f"output {what}: summary header has moved 95% and extra moved, no moved E", 1,
              int(len(header) == 1 and "moved 95%" in header[0] and "extra moved" in header[0]
                  and "moved E" not in header[0]), 0, True)
        method, extra = None, {}
        for line in out:
            if line in lbsim.METHODS:
                method = line
            elif line.startswith("summary"):
                method = None
            elif method and line.startswith("  extra moved "):
                extra[method] = line
        for m in lbsim.METHODS:
            if m == "rr":
                check(f"output {what}: rr has no extra moved line", 1, int(m not in extra), 0, True)
            elif m == "mod":
                check(f"output {what}: mod has an extra moved line", 1, int(m in extra), 0, True)
            else:
                check(f"output {what}: {m} extra moved line present, 0, no '!'", 1,
                      int(m in extra and extra[m].startswith("  extra moved 0/") and "!" not in extra[m]), 0, True)


def main():
    t0 = time.monotonic()
    rng = random.Random(20261125)
    for method, ppw, reps in (("ring1", 1, 4000), ("ring10", 10, 3000), ("ketama", 160, 2000)):
        ring_checks(rng, method, ppw, reps)
    for method, ppw, reps in (("ring1", 1, 1000), ("ring10", 10, 600), ("ketama", 160, 250)):
        hashed_ring_checks(rng, method, ppw, reps)
    ring_remove_mc(rng, "ketama", [1] * 8, 160, 0, 600)        # m_r = 160: the old approximation's range
    for method, ppw, reps in (("ring1", 1, 3000), ("ring10", 10, 2000), ("ketama", 160, 800)):
        ring_add_mc(rng, method, ppw, reps)

    nkeys = 200_000
    keys = [f"k{i}" for i in range(nkeys)]
    key_hashes = [lbsim.ring_hashstr(k) for k in keys]
    mod_checks(key_hashes)
    keyed_concentration_checks(rng, key_hashes)
    keyed_add_checks(rng)
    even_top_checks(rng)

    exact_range_checks()

    # contract 4: extra moved and moved 95%
    mod_extra_checks(key_hashes)
    mod_extra95_checks()
    consistent_extra_checks(rng)
    moved95_coverage(rng)
    output_checks()

    # hrw: the method as defined (score -w / ln u per key and backend), r = b2 (weight 3)
    r = 1
    sample = [hrw_hashes(k, HOSTS) == [lbsim.ring_hashstr(f"{k}|{h}") for h in HOSTS] for k in keys[:2000]]
    check("hrw: shared-prefix hash = lbsim.ring_hashstr (2000 keys)", 1, int(all(sample)), 0, True)
    # Host names only need to differ; short ones make the 200k x 5 hashes cheaper (same hash, same rule).
    short = [f"h:{i + 1}" for i in range(len(WEIGHTS))]
    scores = [[hrw_score(x, w) for x, w in zip(hrw_hashes(k, short), WEIGHTS)] for k in keys]
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
    for kw in ({"remove": r}, {"add": True}):
        t = lbsim.theory("rr", list(WEIGHTS), 254, **kw)
        check(f"rr {next(iter(kw))}: extra and extra95 are None", 1,
              int(t["extra"] is None and t["extra95"] is None), 0, True)
    t = lbsim.theory("ketama", list(WEIGHTS), 254)
    check("no change: extra and extra95 are None", 1, int(t["extra"] is None and t["extra95"] is None), 0, True)

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
