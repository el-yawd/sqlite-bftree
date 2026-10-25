#!/usr/bin/env python3
"""
costmodel.py -- predict device reads per point read, before measuring.

WHY.  A benchmark result is only explained once it can be predicted.  Point
reads under direct I/O are a pure caching problem: every lookup that misses
the cache reads exactly one leaf page from the device (interior pages are
always resident: 310 of them in v100, and stock's LRU keeps the hottest pages
of the tree; BF keeps interior pages outside its budget, as the reference keeps
its inner nodes on the heap).  So

    device reads/op  =  P(lookup misses the cache)

and the miss probability follows from three things this script computes
exactly, not by assuming an ideal Zipf law:

  1. the per-key access probabilities of bfbench's OWN sampler: YCSB's
     approximate Zipfian generator (Gray et al.), rank used directly
     (zipf-raw) or scrambled through FNV-1a mod N (zipf) -- bfbench.c
     zipfNext/fnv64/pickKey, reimplemented in numpy, interval by interval;
  2. the per-LEAF probabilities: the real leaf boundaries of the dataset,
     read from SQLite's dbstat in key order (v100: 111,013 leaves, 36.03
     records each);
  3. the cache: stock = a page cache of C leaves; BF = a record cache of
     K = ring_bytes / bytes_per_record records.

Cache policies, from the caching-theory literature (all steady state):

  * LRU, Che's approximation (Che, Tung, Wang, IEEE JSAC 2002; validated by
    Fricker, Robert, Roberts, ITC 2012): item i hits with
    h_i = 1 - exp(-p_i T), the characteristic time T solving sum h_i = C.
  * FIFO / RANDOM (same framework; Gelenbe 1973, Martina, Garetto, Leonardi,
    INFOCOM 2014): h_i = p_i T / (1 + p_i T).
  * q-LRU -- admission with probability q on a miss, LRU otherwise (Martina
    et al. 2014): h_i = q(1-e^{-p_i T}) / (e^{-p_i T} + q(1-e^{-p_i T})).
    BF admits a missed record with probability promotion_rate (30%).
  * STATIC-OPTIMAL: the K most probable items, always cached -- the bound no
    online policy beats (the "Zipf ideal" earlier entries compared against).

stock: LRU over leaves (SQLite's pcache1 is LRU).  BF: its ring is FIFO with a
copy-on-access second chance and probabilistic admission; no closed form is
exact for that, so BF is reported as a BAND -- q-FIFO-like (pessimistic) to
q-LRU (second chance makes FIFO approach LRU) -- with static-optimal as the
floor.  Full-page caching (M2/M3/X1) is NOT modelled: BF here is a pure record
cache, so where measured BF beats the record model, full pages did it.

Usage:
  python3 costmodel.py --db work/datasets/v100.db --records 4000000 --dist zipf
      --budget 32M --bytes-per-record 120
  python3 costmodel.py --results results/thesis_regress   # model vs measured
"""
import argparse, functools, json, math, os, sqlite3, sys
import numpy as np

# ---------------------------------------------------------------- workload --

def zipf_rank_pmf(n, theta):
    """Exact pmf of bfbench's zipfNext over ranks 0..n-1 (u ~ U[0,1))."""
    zetan = float(np.sum(1.0 / np.power(np.arange(1, n + 1, dtype=np.float64), theta)))
    zeta2 = 1.0 + 1.0 / 2.0 ** theta
    alpha = 1.0 / (1.0 - theta)
    eta = (1.0 - (2.0 / n) ** (1.0 - theta)) / (1.0 - zeta2 / zetan)
    pmf = np.zeros(n, dtype=np.float64)
    u0 = 1.0 / zetan                       # uz < 1          -> rank 0
    u1 = (1.0 + 0.5 ** theta) / zetan      # uz < 1+0.5^th   -> rank 1
    pmf[0] += u0
    pmf[1] += u1 - u0
    # u >= u1: ret = floor(n * v^alpha), v = eta*u - eta + 1, clamped to n-1.
    # ret == r  <=>  v in [(r/n)^(1/alpha), ((r+1)/n)^(1/alpha))
    #           <=>  u in [(v_lo - 1 + eta)/eta, (v_hi - 1 + eta)/eta)
    r = np.arange(n + 1, dtype=np.float64)
    v_edge = np.power(r / n, 1.0 / alpha)            # v at rank edges 0..n
    u_edge = (v_edge - 1.0 + eta) / eta
    lo = np.clip(u_edge[:-1], u1, 1.0)
    hi = np.clip(u_edge[1:], u1, 1.0)
    pmf += np.maximum(hi - lo, 0.0)
    # v^alpha can exceed 1 near u=1 (ret >= n): clamped to n-1.
    if u_edge[-1] < 1.0:
        pmf[n - 1] += 1.0 - max(u_edge[-1], u1)
    return pmf / pmf.sum()


def fnv64_np(v):
    """bfbench.c fnv64 over uint64 array (FNV-1a over the 8 little-endian bytes)."""
    h = np.full(v.shape, 0xcbf29ce484222325, dtype=np.uint64)
    prime = np.uint64(0x100000001b3)
    with np.errstate(over="ignore"):
        for i in range(8):
            h ^= (v >> np.uint64(8 * i)) & np.uint64(0xff)
            h *= prime
    return h


@functools.lru_cache(maxsize=None)
def key_pmf(n, theta, dist):
    """Probability of each key index 0..n-1 under bfbench's pickKey."""
    rp = zipf_rank_pmf(n, theta)
    if dist == "zipf-raw":
        return rp
    idx = fnv64_np(np.arange(n, dtype=np.uint64)) % np.uint64(n)
    out = np.zeros(n, dtype=np.float64)
    np.add.at(out, idx.astype(np.int64), rp)
    return out


@functools.lru_cache(maxsize=None)
def leaf_sizes(db):
    """Cells per table leaf, in key order (dbstat path order), cached beside the db."""
    cache = db + ".leaves.npy"
    if os.path.exists(cache) and os.path.getmtime(cache) >= os.path.getmtime(db):
        return np.load(cache)
    c = sqlite3.connect("file:%s?mode=ro" % db, uri=True)
    rows = c.execute("SELECT ncell FROM dbstat WHERE name='usertable' AND pagetype='leaf' "
                     "ORDER BY path").fetchall()
    a = np.array([r[0] for r in rows], dtype=np.int64)
    try:
        np.save(cache, a)
    except OSError:
        pass
    return a


def leaf_pmf(kp, sizes):
    if sizes.sum() != len(kp):
        sys.exit("leaf cell counts sum to %d, dataset has %d records" % (sizes.sum(), len(kp)))
    edges = np.concatenate(([0], np.cumsum(sizes)))
    return np.add.reduceat(kp, edges[:-1])

# ---------------------------------------------------------------- policies --

def _solve_T(f, C, n):
    """Characteristic time T with sum_i f(T)_i = C (monotone in T)."""
    if C >= n:
        return math.inf
    lo, hi = 0.0, 1.0
    while f(hi) < C:
        hi *= 2.0
    for _ in range(60):              # 2^-60 relative: far below what we print
        mid = 0.5 * (lo + hi)
        if f(mid) < C:
            lo = mid
        else:
            hi = mid
    return 0.5 * (lo + hi)


def miss_lru(p, C):
    """Che's approximation: steady-state miss probability of an LRU cache of C items."""
    T = _solve_T(lambda t: float(np.sum(-np.expm1(-p * t))), C, len(p))
    if math.isinf(T):
        return 0.0
    return float(np.sum(p * np.exp(-p * T)))


def miss_fifo(p, C):
    T = _solve_T(lambda t: float(np.sum(p * t / (1.0 + p * t))), C, len(p))
    if math.isinf(T):
        return 0.0
    return float(np.sum(p / (1.0 + p * T)))


def miss_qlru(p, C, q):
    def h(t):
        e = np.exp(-p * t)
        return q * (1.0 - e) / (e + q * (1.0 - e))
    T = _solve_T(lambda t: float(np.sum(h(t))), C, len(p))
    if math.isinf(T):
        return 0.0
    return float(np.sum(p * (1.0 - h(T))))


def miss_qfifo(p, C, q):
    """q-admission over FIFO (Martina et al.: h = q p T / (1 + q p T))."""
    T = _solve_T(lambda t: float(np.sum(q * p * t / (1.0 + q * p * t))), C, len(p))
    if math.isinf(T):
        return 0.0
    return float(np.sum(p / (1.0 + q * p * T)))


def miss_static(p, C):
    if C >= len(p):
        return 0.0
    s = np.sort(p)[::-1]
    return float(1.0 - s[:int(C)].sum())

# ----------------------------------------------------------------- driver --

def parse_bytes(s):
    s = str(s).strip().upper()
    mult = {"K": 1 << 10, "M": 1 << 20, "G": 1 << 30}
    return int(float(s[:-1]) * mult[s[-1]]) if s[-1] in mult else int(s)


@functools.lru_cache(maxsize=None)
def predict(db, records, theta, dist, budget, bpr, q, interior_resident=True):
    kp = key_pmf(records, theta, dist)
    sizes = leaf_sizes(db)
    lp = leaf_pmf(kp, sizes)
    page = 4096
    n_int = _interior_pages(db)
    # stock: one page cache of `budget` bytes; interior pages are its hottest
    # entries and stay in it, the rest holds leaves.
    c_leaf = max(0, budget // page - (n_int if interior_resident else 0))
    out = {
        "leaves": int(len(sizes)), "recs_per_leaf": float(sizes.mean()),
        "interior_pages": n_int,
        "stock_leaf_capacity": int(c_leaf),
        "stock_lru": miss_lru(lp, c_leaf),
        "stock_static": miss_static(lp, c_leaf),
    }
    if bpr:
        k = budget / bpr
        out.update({
            "bf_record_capacity": int(k),
            "bf_static": miss_static(kp, k),
            "bf_lru": miss_lru(kp, k),
            "bf_qlru": miss_qlru(kp, k, q),
            "bf_fifo": miss_fifo(kp, k),
            "bf_qfifo": miss_qfifo(kp, k, q),
        })
    return out


_INT_CACHE = {}
def _interior_pages(db):
    if db not in _INT_CACHE:
        c = sqlite3.connect("file:%s?mode=ro" % db, uri=True)
        _INT_CACHE[db] = c.execute("SELECT count(*) FROM dbstat WHERE name='usertable' "
                                   "AND pagetype='internal'").fetchone()[0]
    return _INT_CACHE[db]


def vs_results(resdir, datasets_dir, q_default):
    """Model next to measurement for every read=100 direct-I/O cell of a campaign."""
    rows = [json.loads(l) for l in open(os.path.join(resdir, "results.jsonl"))]
    cells = {}
    for r in rows:
        res = (r.get("run") or {}).get("result") or {}
        cfg = (r.get("run") or {}).get("config") or {}
        if r.get("workload") != "read=100" or not cfg.get("direct_io") or not res.get("ops"):
            continue
        key = (r["experiment"], r["dataset"], r["dist"], r["sut"], json.dumps(r.get("axis")))
        bf = res.get("bf") or {}
        cells.setdefault(key, []).append({
            "reads": res["dio"]["reads"] / res["ops"],
            "bpr": (bf["mini_page_bytes"] / bf["cached_records"]
                    if bf.get("cached_records") else None),
            "cached": bf.get("cached_records"), "evict": bf.get("evictions"),
            "budget": cfg.get("bf_cache_bytes") or cfg.get("page_cache_bytes"),
            "theta": cfg.get("theta", 0.9), "records": cfg.get("records"),
            "q": (cfg.get("promotion_rate") or q_default * 100) / 100.0,
        })
    print("| experiment | sut | axis | measured | model | band / bound | cached recs | evictions |")
    print("|---|---|---|---|---|---|---|---|")
    for (exp, ds, dist, sut, axis), ms in sorted(cells.items()):
        m = ms[0]
        meas = float(np.median([x["reads"] for x in ms]))
        db = os.path.join(datasets_dir, ds + ".db")
        if sut.startswith("stock"):
            p = predict(db, m["records"], m["theta"], dist, parse_bytes(m["budget"]), None, m["q"])
            print("| %s | %s | %s | %.4f | LRU %.4f | static %.4f | - | - |" % (
                exp, sut, axis, meas, p["stock_lru"], p["stock_static"]))
        else:
            bprs = [x["bpr"] for x in ms if x["bpr"]]
            if not bprs:
                continue
            bpr = float(np.median(bprs))
            p = predict(db, m["records"], m["theta"], dist, parse_bytes(m["budget"]), bpr, m["q"])
            print("| %s | %s | %s | %.4f | q-LRU %.4f | q-FIFO %.4f .. static %.4f (K=%d at %.0f B/rec) | %s | %s |" % (
                exp, sut, axis, meas, p["bf_qlru"], p["bf_qfifo"], p["bf_static"],
                p["bf_record_capacity"], bpr, m["cached"], m["evict"]))


def breakeven_bpr(db, records, theta, dist, budget, q):
    """Bytes per cached record at which BF's q-LRU record model matches stock's LRU.
    Below it a pure record cache needs fewer device reads than the page cache."""
    target = predict(db, records, theta, dist, budget, None, q)["stock_lru"]
    kp = key_pmf(records, theta, dist)
    lo, hi = 8.0, 4096.0
    if miss_qlru(kp, budget / hi, q) < target:
        return hi
    if miss_qlru(kp, budget / lo, q) > target:
        return None
    for _ in range(40):
        mid = math.sqrt(lo * hi)
        if miss_qlru(kp, budget / mid, q) <= target:
            lo = mid
        else:
            hi = mid
    return lo


def sweep(db, records, theta, dist, budgets, bpr, q):
    print("| budget | stock LRU | BF q-LRU | BF q-FIFO | BF static | stock/BF (q-LRU) | break-even B/rec |")
    print("|---|---|---|---|---|---|---|")
    for b in budgets:
        p = predict(db, records, theta, dist, parse_bytes(b), bpr, q)
        be = breakeven_bpr(db, records, theta, dist, parse_bytes(b), q)
        print("| %s | %.4f | %.4f | %.4f | %.4f | %.2fx | %s |" % (
            b, p["stock_lru"], p["bf_qlru"], p["bf_qfifo"], p["bf_static"],
            p["stock_lru"] / p["bf_qlru"] if p["bf_qlru"] else float("inf"),
            ("%.0f" % be) if be else "-"))


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("--db", help="dataset .db (leaf boundaries via dbstat)")
    ap.add_argument("--records", type=int)
    ap.add_argument("--theta", type=float, default=0.9)
    ap.add_argument("--dist", default="zipf", choices=["zipf", "zipf-raw"])
    ap.add_argument("--budget", default="32M")
    ap.add_argument("--bytes-per-record", type=float, default=None,
                    help="BF ring bytes per cached record (measured gauge)")
    ap.add_argument("--promotion", type=float, default=0.30)
    ap.add_argument("--results", help="campaign dir: model vs measured, per cell")
    ap.add_argument("--sweep", help="comma-separated budgets, e.g. 8M,16M,32M,64M,128M")
    ap.add_argument("--datasets", default=os.path.join(os.path.dirname(os.path.abspath(__file__)),
                                                       "work", "datasets"))
    a = ap.parse_args()
    if a.results:
        vs_results(a.results, a.datasets, a.promotion)
        return
    if not (a.db and a.records):
        ap.error("--db and --records, or --results")
    if a.sweep:
        if not a.bytes_per_record:
            ap.error("--sweep needs --bytes-per-record")
        sweep(a.db, a.records, a.theta, a.dist, a.sweep.split(","),
              a.bytes_per_record, a.promotion)
        return
    p = predict(a.db, a.records, a.theta, a.dist, parse_bytes(a.budget),
                a.bytes_per_record, a.promotion)
    for k, v in p.items():
        print("%-22s %s" % (k, ("%.4f" % v) if isinstance(v, float) else v))


if __name__ == "__main__":
    main()
