#!/usr/bin/env python3
"""Compare key distributions before and after a load-balancer membership change."""
import argparse
from bisect import bisect_left
from collections import Counter
from dataclasses import dataclass
from functools import lru_cache
from ipaddress import IPv6Address
import math
from random import Random
import re


UINT32 = 1 << 32
MASK = UINT32 - 1
METHODS = ("mod", "rr", "random", "ring1", "ring10", "ketama", "hrw")
POINTS = {"ring1": 1, "ring10": 10, "ketama": 160}
MAX_BACKENDS = 8


@dataclass(frozen=True)
class Backend:
    name: str
    host: str
    port: int
    weight: int = 1

    @property
    def endpoint(self):
        return f"{self.host}:{self.port}"

    @property
    def address(self):
        return f"[{self.host}]:{self.port}" if ":" in self.host else self.endpoint


def ring_hashstr(s: str) -> int:
    h = 2166136261
    for byte in s.encode("utf-8"):
        h = ((h ^ byte) * 16777619) & MASK
    h = ((h ^ (h >> 16)) * 0x85EBCA6B) & MASK
    h = ((h ^ (h >> 13)) * 0xC2B2AE35) & MASK
    return h ^ (h >> 16)


class Ring:
    def __init__(self, ring, members):
        self.ring = ring
        self.hashes = [point for point, _ in ring]
        self.members = members

    def lookup(self, key):
        slot = bisect_left(self.hashes, ring_hashstr(key)) % len(self.ring)
        return self.ring[slot][1]

    def arc_shares(self):
        if self.members == 1:
            return [1.0]
        owned = [0] * self.members
        for i, (point, index) in enumerate(self.ring):
            owned[index] += (point - self.ring[i - 1][0]) & MASK
        return [fraction / UINT32 for fraction in owned]


def build_ring(hosts: list[str], weights: list[int], points: int) -> Ring:
    entries = []
    for index, (address, weight) in enumerate(zip(hosts, weights)):
        host, _, port = address.rpartition(":")
        endpoint = f"{host.removeprefix('[').removesuffix(']')}:{int(port)}"
        entries.extend((ring_hashstr(f"{endpoint}-{k}"), index) for k in range(points * weight))
    return Ring(sorted(entries), len(hosts))


def weighted_slots(weights):
    return [index for index, weight in enumerate(weights) for _ in range(weight)]


def distribution(method, backends, seed=1):
    if method in ("mod", "random"):
        slots = [backends[i].name for i in weighted_slots([b.weight for b in backends])]
        if method == "random":
            rng = Random(seed)
            return lambda _key: rng.choice(slots), {}
        return lambda key: slots[ring_hashstr(key) % len(slots)], {}
    if method == "hrw":
        def pick(key):
            def score(backend):
                value = (ring_hashstr(f"{key}|{backend.endpoint}") + 0.5) / UINT32
                return -backend.weight / math.log(value)

            return max(backends, key=score).name

        return pick, {}
    ring = build_ring([b.address for b in backends], [b.weight for b in backends], POINTS[method])
    return (lambda key: backends[ring.lookup(key)].name,
            {b.name: share for b, share in zip(backends, ring.arc_shares())})


def log_binomial_mass(n, p, k):
    return (math.lgamma(n + 1) - math.lgamma(k + 1) - math.lgamma(n - k + 1)
            + k * math.log(p) + (n - k) * math.log1p(-p))


def upper_binomial_sum(n, p, k):
    log_mass = log_binomial_mass(n, p, k)
    odds = p / (1 - p)
    # Sum relative to the first term in this decreasing tail.
    total = term = 1.0
    while k < n and term > total * 1e-16:
        term *= (n - k) / (k + 1) * odds
        total += term
        k += 1
    return math.exp(log_mass + math.log(total))


def binomial_tail(n, p, k):
    if k <= 0:
        return 1.0
    if k > n or p == 0:
        return 0.0
    if p == 1:
        return 1.0
    if k > n * p:
        return min(1.0, upper_binomial_sum(n, p, k))
    return max(0.0, 1 - upper_binomial_sum(n, 1 - p, n - k + 1))


@lru_cache(maxsize=256)
def beta_quantile(a: int, b: int, q: float) -> float:
    if not a or not b:
        return float(a > 0)
    if q == 0 or q == 1:
        return q
    lo, hi = 0.0, 1.0
    for _ in range(44):
        mid = (lo + hi) / 2
        if binomial_tail(a + b - 1, mid, a) < q:
            lo = mid
        else:
            hi = mid
    return (lo + hi) / 2


def top_expected(n, probabilities):
    # Hoeffding bounds the omitted tails of the maximum below float precision.
    highest = max(probabilities)
    bound = math.log(1e17 * len(probabilities))
    gap = math.sqrt(2 * bound / n)
    # Destinations far below the largest mean cannot overtake it to this precision.
    groups = Counter(p for p in probabilities if highest - p <= gap)
    if sum(groups.values()) == 1:
        return highest
    mean = n * highest
    radius = math.sqrt(n * bound / 2)
    lo = max(0, math.floor(mean - radius))
    hi = min(n - 1, math.ceil(mean + radius))
    cdf = [1 - binomial_tail(n, p, lo) for p in groups]
    log_mass = [log_binomial_mass(n, p, lo) for p in groups]
    log_odds = [math.log(p) - math.log1p(-p) for p in groups]
    multiplicities = list(groups.values())
    total = lo
    for k in range(lo, hi + 1):
        step = math.log((n - k) / (k + 1))
        combined = 1.0
        for i, odds in enumerate(log_odds):
            cdf[i] = min(1.0, cdf[i] + math.exp(log_mass[i]))
            log_mass[i] += step + odds
            combined *= cdf[i] ** multiplicities[i]
        total += 1 - combined
    return total / n


@lru_cache(maxsize=256)
def keyed_concentration(keys, removed_share, probabilities):
    lo, hi = 1, keys
    if keys > 512:
        mean = keys * removed_share
        radius = 8 * math.sqrt(mean * (1 - removed_share))
        lo = max(lo, math.floor(mean - radius))
        hi = min(hi, math.ceil(mean + radius))
    log_mass = log_binomial_mass(keys, removed_share, lo)
    log_odds = math.log(removed_share) - math.log1p(-removed_share)
    groups = Counter(probabilities)
    mass = concentrated = top = 0.0
    for n in range(lo, hi + 1):
        weight = math.exp(log_mass)
        threshold = (4 * n + 4) // 5
        conditional = sum(copies * binomial_tail(n, p, threshold) for p, copies in groups.items())
        mass += weight
        concentrated += weight * conditional
        top += weight * top_expected(n, probabilities)
        if n < hi:
            log_mass += math.log((keys - n) / (n + 1)) + log_odds
    # Normalize after excluding zero removed keys and truncating the outer tails.
    return min(1.0, concentrated / mass), top / mass


def ring_concentration(removed, remaining_points):
    if len(remaining_points) == 1:
        return 1.0, 1.0

    remaining = sum(remaining_points)
    compositions = math.comb(removed + remaining - 1, removed)
    # The beta-binomial marginal counts weak compositions by destination.
    mass = [
        sum(math.comb(c + points - 1, c)
            * math.comb(removed - c + remaining - points - 1, removed - c)
            for points in remaining_points) / compositions
        for c in range(1, removed + 1)
    ]

    def sum_tails(t):
        # Beta(c, removed - c) survival is Bin(removed - 1, t) CDF at c - 1.
        log_failure = math.log1p(-t)
        log_term = (removed - 1) * log_failure
        log_odds = math.log(t) - log_failure
        cdf = 0.0
        total = mass[-1]  # c == removed has all the arc mass.
        for c, probability in enumerate(mass[:-1], 1):
            cdf += math.exp(log_term)
            total += probability * cdf
            log_term += math.log((removed - c) / c) + log_odds
        return min(1.0, total)

    top = math.fsum(sum_tails((i + 0.5) / 100) for i in range(100)) / 100
    return sum_tails(0.8), top


def mod_exact(weights: list[int], remove: int | None = None, add: bool = False) -> dict:
    result = dict(moved=None, extra=None, redistribution=None, p_concentrated=None, top_share=None)
    if remove is None and not add:
        return result
    before = weighted_slots(weights)
    after_weights = list(weights)
    if remove is not None:
        after_weights[remove] = 0
    else:
        after_weights.append(1)
    after = weighted_slots(after_weights)
    period = math.lcm(len(before), len(after))
    target = remove if remove is not None else len(weights)
    moved, extra, arrivals = 0, 0, Counter()
    # Treat residues as uniform over the period, ignoring the incomplete uint32 cycle.
    for x in range(period):
        old, new = before[x % len(before)], after[x % len(after)]
        if old != new:
            moved += 1
            extra += (old if remove is not None else new) != target
        if old == remove:
            arrivals[new] += 1
    result.update(moved=moved / period, extra=extra / period)
    if remove is not None:
        count = sum(arrivals.values())
        top = max(arrivals.values()) / count
        result.update(redistribution=[None if i == remove else arrivals[i] / count
                                      for i in range(len(weights))],
                      p_concentrated=float(top >= 0.8), top_share=top)
    return result


def discrete_interval(keys, cdf):
    def quantile(q):
        lo, hi = 0, keys
        while lo < hi:
            mid = (lo + hi) // 2
            if cdf(mid) >= q - 1e-12:  # Include ties despite rounding in the CDF.
                hi = mid
            else:
                lo = mid + 1
        return lo / keys

    return quantile(0.025), quantile(0.975)


@lru_cache(maxsize=256)
def binomial_interval(keys, probability):
    return discrete_interval(keys, lambda k: 1 - binomial_tail(keys, probability, k + 1))


def hypergeometric_tail(population, marked, draws, threshold):
    lower = max(0, draws - (population - marked))
    upper = min(draws, marked)
    if threshold <= lower:
        return 1.0
    if threshold > upper:
        return 0.0
    mode = (draws + 1) * (marked + 1) // (population + 2)
    unmarked = population - marked
    # Normalize relative to the modal mass so neither tail needs large binomial coefficients.
    below = float(mode < threshold)
    above = 1.0 - below
    term = 1.0
    for x in range(mode, upper):
        term *= (marked - x) * (draws - x) / ((x + 1) * (unmarked - draws + x + 1))
        if term < 1e-18:
            break
        if x + 1 < threshold:
            below += term
        else:
            above += term
    term = 1.0
    for x in range(mode, lower, -1):
        term *= x * (unmarked - draws + x) / ((marked - x + 1) * (draws - x + 1))
        if term < 1e-18:
            break
        if x - 1 < threshold:
            below += term
        else:
            above += term
    return above / (below + above)


@lru_cache(maxsize=256)
def beta_binomial_interval(keys, a, b):
    if b == 0:
        return 1.0, 1.0
    if a == 0:
        return 0.0, 0.0
    # In a uniform subset of size a+b-1, the a-th selected position minus a
    # is beta-binomial. Its CDF is the chance of at least a selections by k+a.
    draws = a + b - 1
    return discrete_interval(keys, lambda k: hypergeometric_tail(keys + draws, k + a, draws, a))


def theory(method: str, weights: list[int], keys: int,
           remove: int | None = None, add: bool = False) -> dict:
    total = sum(weights)
    points = POINTS.get(method, 0)
    share = [weight / total for weight in weights]
    arc95 = None
    if points:
        share95 = [beta_binomial_interval(keys, points * w, points * (total - w)) for w in weights]
        arc95 = [(beta_quantile(points * w, points * (total - w), 0.025),
                  beta_quantile(points * w, points * (total - w), 0.975)) for w in weights]
    elif method == "rr":
        share95 = [(p, p) for p in share]
    else:
        share95 = [binomial_interval(keys, p) for p in share]
    result = dict(share=share, share95=share95, arc95=arc95, moved=None, moved95=None,
                  extra=None, extra95=None,
                  redistribution=None, p_concentrated=None, top_share=None, approx=set())
    if method == "mod":
        result.update(mod_exact(weights, remove, add))
        if result["moved"] is not None:
            result["moved95"] = binomial_interval(keys, result["moved"])
            result["extra95"] = binomial_interval(keys, result["extra"])
    elif remove is not None:
        remaining = total - weights[remove]
        ratios = [None if i == remove else w / remaining for i, w in enumerate(weights)]
        result["redistribution"] = ratios
        probabilities = [p for p in ratios if p is not None]
        if method == "rr":
            top = max(probabilities)
            result.update(top_share=top, p_concentrated=float(top >= 0.8))
            return result
        result.update(moved=share[remove], moved95=share95[remove])
        if len(probabilities) == 1 or points * weights[remove] == 1:
            result.update(top_share=1.0, p_concentrated=1.0)
        else:
            if points:
                remaining_points = [points * w for i, w in enumerate(weights) if i != remove]
                concentration, top = ring_concentration(points * weights[remove], remaining_points)
            else:
                concentration, top = keyed_concentration(keys, share[remove], tuple(sorted(probabilities)))
            result.update(p_concentrated=concentration, top_share=top)
            result["approx"].add("top_share")
    elif add and method != "rr":
        moved = 1 / (total + 1)
        moved95 = (beta_binomial_interval(keys, points, points * total) if points
                   else binomial_interval(keys, moved))
        result.update(moved=moved, moved95=moved95)
    if method != "mod" and result["moved"] is not None:
        result.update(extra=0.0, extra95=(0.0, 0.0))
    return result


def client_keys(count):
    if count <= 254:
        return (f"127.0.0.{i + 2}" for i in range(count))
    return (f"10.{i >> 16}.{(i >> 8) & 255}.{i & 255}" for i in range(count))


def local_backend(number, weight=1):
    return Backend(f"b{number}", "127.0.0.1", 8000 + number, weight)


def parse_address(text):
    match = re.fullmatch(r"(?:\[([^%\]]+)\]|([^\s:\[\]#,*]+)):([0-9]{1,7})", text)
    if match:
        v6, host, port = match.groups()
        if v6:
            IPv6Address(v6)
        host, port = v6 or host, int(port)
        if len(host.encode("utf-8")) <= 255 and 1 <= port <= 65535:
            return host, port
    raise ValueError(f"invalid backend address: {text!r}")


def config(args):
    hosts = args.hosts.split(",") if args.hosts is not None else None
    count = len(hosts) if hosts is not None else (3 if args.backends is None else args.backends)
    if not 1 <= count <= MAX_BACKENDS:
        raise ValueError(f"backends must be 1..{MAX_BACKENDS}")
    weights = args.weights.split(",") if args.weights is not None else ["1"] * count
    if len(weights) != count or any(weight not in ("1", "2", "3", "4") for weight in weights):
        raise ValueError("provide one weight in 1..4 for each backend")
    backends = [
        local_backend(i, int(weight)) if hosts is None
        else Backend(f"b{i}", *parse_address(hosts[i - 1]), int(weight))
        for i, weight in enumerate(weights, 1)
    ]
    if len({backend.endpoint for backend in backends}) != count:
        raise ValueError("backend addresses must be distinct")
    methods = args.methods.split(",")
    if any(method not in METHODS for method in methods) or len(set(methods)) != len(methods):
        raise ValueError("methods must be a comma-separated list of distinct known methods")
    if not 1 <= args.keys <= 1 << 24:
        raise ValueError("keys must be 1..16777216")
    return backends, methods


def changed_members(backends, remove, add):
    if remove:
        if len(backends) == 1 or not any(backend.name == remove for backend in backends):
            raise ValueError("remove must name an existing backend and leave at least one")
        return [backend for backend in backends if backend.name != remove]
    if add:
        if len(backends) == MAX_BACKENDS:
            raise ValueError(f"cannot add a backend beyond {MAX_BACKENDS}")
        new = local_backend(len(backends) + 1)
        if any(backend.endpoint == new.endpoint for backend in backends):
            raise ValueError(f"new backend address conflicts with {new.address}")
        return backends + [new]
    return None


def interval_text(interval, measured=None):
    lo, hi = interval
    mark = " !" if measured is not None and not lo <= measured <= hi else ""
    return f"[{100 * lo:.1f}-{100 * hi:.1f}]{mark}"


def print_distribution(label, backends, counts, arcs, nkeys, model):
    print(f"  {label}")
    for i, backend in enumerate(backends):
        name = backend.name
        expected = model["share"][i]
        fraction = counts[name] / nkeys if counts is not None else expected
        bar = "#" * round(fraction * 40)
        arc = f"  arc {arcs[name]:.1%}" if arcs else ""
        if counts is None:
            sample = f"exact {fraction:>6.1%}"
        else:
            sample = f"keys {counts[name]:>5} ({fraction:>6.1%})"
        expected_text = f"E {expected:.1%} {interval_text(model['share95'][i], fraction)}"
        if arcs:
            expected_text += f"  arc {interval_text(model['arc95'][i], arcs[name])}"
        print(f"    {name:>3}  weight {backend.weight}  {sample}  {bar:<40}{arc}  {expected_text}")


def theory_value(model, field):
    value = model[field]
    if value is None:
        return "n/a"
    mark = "≈" if field in model["approx"] else ""
    return f"{mark}{value:.1%}"


def compare(method, before, after, nkeys, remove, seed):
    weights = [b.weight for b in before]
    removed_index = next((i for i, b in enumerate(before) if b.name == remove), None)
    adding = after is not None and removed_index is None
    target = after[-1].name if adding else remove
    model = theory(method, weights, nkeys, remove=removed_index, add=adding)
    counts, counts_after, arrivals = Counter(), Counter(), Counter()
    moved = extra = 0
    arcs, arcs_after = {}, {}
    if method != "rr":
        pick, arcs = distribution(method, before, seed)
        if after is not None:
            pick_after, arcs_after = distribution(method, after, seed + 1)
        add_rng = Random(seed + 1) if adding and method == "random" else None
        add_slots = sum(weights) + 1
        for key in client_keys(nkeys):
            old_name = pick(key)
            counts[old_name] += 1
            if after is not None:
                if add_rng is not None:
                    new_name = after[-1].name if add_rng.randrange(add_slots) == 0 else old_name
                elif method == "random" and old_name != remove:
                    new_name = old_name
                else:
                    new_name = pick_after(key)
                counts_after[new_name] += 1
                if old_name != new_name:
                    moved += 1
                    extra += (new_name if adding else old_name) != target
                if old_name == remove:
                    arrivals[new_name] += 1
    else:
        counts = counts_after = None

    print(method)
    print_distribution("before" if after is not None else "distribution", before, counts, arcs,
                       nkeys, model)
    if after is not None:
        after_model = theory(method, [b.weight for b in after], nkeys)
        print_distribution(f"after removing {remove}" if remove else "after adding a backend",
                           after, counts_after, arcs_after, nkeys, after_model)
        if method == "rr":
            print("  moved n/a (no key affinity)")
        else:
            fraction = moved / nkeys
            print(f"  moved {moved}/{nkeys} ({fraction:.1%})  E {model['moved']:.1%} "
                  f"{interval_text(model['moved95'], fraction)}")
            fraction = extra / nkeys
            print(f"  extra moved {extra}/{nkeys} ({fraction:.1%})  E {model['extra']:.1%} "
                  f"{interval_text(model['extra95'], fraction)}")
        if remove:
            redistribution = model["redistribution"]
            if method == "rr":
                print("  requests redistributed (no key affinity)")
            else:
                removed = counts[remove]
                print(f"  keys from {remove} ({removed})")
            for backend, expected in zip(before, redistribution):
                if expected is None:
                    continue
                if method == "rr":
                    print(f"    {backend.name}  exact {expected:.1%}")
                else:
                    received = arrivals[backend.name]
                    fraction = received / removed if removed else 0
                    mark = " <- concentrated" if fraction >= 0.8 else ""
                    print(f"    {backend.name}  {received} ({fraction:.1%}){mark}  E {expected:.1%}")
            condition = " | removed has keys" if method in ("hrw", "random") else ""
            print(f"  E top {theory_value(model, 'top_share')}  "
                  f"P(≥80%{condition}) {theory_value(model, 'p_concentrated')}")
    print()
    return model


def print_summary(models, weights, remove):
    print("summary (theory)")
    print("  method  max share dev(95%)  moved 95%     extra moved  P(concentrated)  note")
    for method, model in models:
        deviation = max(max(p - lo, hi - p)
                        for p, (lo, hi) in zip(model["share"], model["share95"]))
        note = ""
        if method == "rr":
            note = "no key affinity"
        elif method == "mod":
            note = "exact (enumerated)" if model["moved"] is not None else ""
        elif method == "random":
            change = 1 - sum(p * p for p in model["share"])
            note = f"per-request draw: no key affinity; same key changes 1 - sum(p²) = {change:.1%}"
        elif method == "ring1" and remove is not None and weights[remove] == 1:
            note = "one arc → always one server"
        moved_range = "n/a"
        if model["moved95"] is not None:
            lo, hi = model["moved95"]
            moved_range = f"{100 * lo:.1f}-{hi:.1%}"
        print(f"  {method:<6}  {deviation:>18.1%}  {moved_range:<12}  "
              f"{theory_value(model, 'extra'):>11}  "
              f"{theory_value(model, 'p_concentrated'):>15}  {note}".rstrip())


def self_test():
    for key, expected in (
        ("", 0xAB3E7C0B),
        ("127.0.0.2", 0x6C083465),
        ("127.0.0.1:8001-0", 0xF084B321),
        ("127.0.0.1:8002-479", 0x4420CABA),
    ):
        assert ring_hashstr(key) == expected

    backends = [local_backend(i) for i in (1, 2, 3)]
    remaining = [backends[0], backends[2]]
    for method in ("mod", "ring1", "ketama"):
        before, shares = distribution(method, backends)
        after, _ = distribution(method, remaining)
        pairs = [(before(key), after(key)) for key in client_keys(254)]
        moved = [(old, new) for old, new in pairs if old != new]
        if method == "mod":
            assert len(moved) > len(pairs) / 2
        else:
            assert moved and all(old == "b2" for old, _ in moved)
            assert sum(shares.values()) == 1
            if method == "ring1":
                assert len({new for _, new in moved}) == 1
    ring = build_ring(["127.0.0.1:8001", "127.0.0.1:8002"], [3, 1], 160)
    assert len(ring.ring) == 640 and sum(ring.arc_shares()) == 1
    assert parse_address("[::1]:08001") == ("::1", 8001)
    assert list(client_keys(255))[-1] == "10.0.0.254"
    assert mod_exact([1, 1, 1], remove=1)["moved"] == 2 / 3
    assert mod_exact([1, 1, 1], remove=1)["extra"] == 1 / 3
    assert mod_exact([1, 1, 1], add=True)["extra"] == 1 / 2
    assert mod_exact([1, 1, 1])["extra"] is None
    for method in (*POINTS, "hrw", "random"):
        for change in ({"remove": 1}, {"add": True}):
            model = theory(method, [1, 1, 1], 7, **change)
            assert model["extra"] == 0.0 and model["extra95"] == (0.0, 0.0)
    rr = theory("rr", [1, 1, 1], 7, remove=1)
    assert rr["extra"] is None and rr["extra95"] is None
    assert theory("ring1", [1, 1, 1], 254, remove=1)["p_concentrated"] == 1
    assert theory("rr", [1, 3], 254, remove=1)["share95"] == [(0.25, 0.25), (0.75, 0.75)]
    assert abs(beta_quantile(2, 2, 0.5) - 0.5) < 1e-12
    assert abs(beta_quantile(1, 3, 0.025) + beta_quantile(3, 1, 0.975) - 1) < 1e-12
    assert abs(top_expected(2, [0.5, 0.5]) - 0.6875) < 1e-12
    assert binomial_interval(1, 0.025) == (0.0, 0.0)
    assert beta_binomial_interval(39, 1, 1) == (0.0, 38 / 39)
    assert beta_binomial_interval(254, 1, 0) == (1.0, 1.0)
    concentration, top = ring_concentration(2, [1, 1])
    assert math.isclose(concentration, 0.8) and math.isclose(top, 11 / 12)
    assert theory("ring10", [3, 1, 2], 254, remove=0)["approx"] == {"top_share"}
    assert theory("ketama", [3, 1, 2], 254, remove=0)["approx"] == {"top_share"}
    concentration, top = keyed_concentration(1, 0.2, (0.5, 0.5))
    assert math.isclose(concentration, 1) and math.isclose(top, 0.75)
    for method in POINTS:
        model = theory(method, [1, 2], 254, remove=1)
        assert model["top_share"] == model["p_concentrated"] == 1
        assert not model["approx"]
    print("lbsim self-test passed")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    members = parser.add_mutually_exclusive_group()
    members.add_argument("--backends", type=int)
    members.add_argument("--hosts", help="comma-separated host:port addresses")
    parser.add_argument("--weights")
    parser.add_argument("--keys", type=int, default=254)
    changes = parser.add_mutually_exclusive_group()
    changes.add_argument("--remove", metavar="BACKEND")
    changes.add_argument("--add", action="store_true")
    parser.add_argument("--methods", default=",".join(METHODS))
    parser.add_argument("--seed", type=int, default=1, help="seed for the random method")
    output = parser.add_mutually_exclusive_group()
    output.add_argument("--map", action="store_true",
                        help="print the current Ketama key mapping (addresses with --hosts)")
    output.add_argument("--share", action="store_true", help="print the current Ketama arc shares")
    output.add_argument("--self-test", action="store_true")
    args = parser.parse_args()
    if args.self_test:
        self_test()
        return
    if (args.map or args.share) and (args.remove is not None or args.add):
        parser.error("--map and --share cannot be combined with --remove or --add")
    try:
        backends, methods = config(args)
        after = changed_members(backends, args.remove, args.add)
    except ValueError as exc:
        parser.error(str(exc))

    if args.map or args.share:
        pick, shares = distribution("ketama", backends)
        if args.map:
            labels = {backend.name: backend.address if args.hosts is not None else backend.name
                      for backend in backends}
            for key in client_keys(args.keys):
                print(key, labels[pick(key)])
        else:
            for backend in backends:
                print(backend.address, f"{shares[backend.name]:.1%}")
        return
    print("Assume independent uniform hashes; E = expectation; 95% arc (a) = placement only; "
          "95% keys/moved (b) = exact BetaBinomial/Binomial quantiles; "
          "! = outside interval; ≈ = approximation.")
    models = [(method, compare(method, backends, after, args.keys, args.remove, args.seed))
              for method in methods]
    weights = [backend.weight for backend in backends]
    remove = next((i for i, b in enumerate(backends) if b.name == args.remove), None)
    print_summary(models, weights, remove)


if __name__ == "__main__":
    main()
