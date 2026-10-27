import argparse
import random
import sys
import time

import pystats

try:
    import faststats
except ImportError:
    faststats = None

IMPLEMENTATIONS = {"python": pystats}
if faststats is not None:
    IMPLEMENTATIONS["c"] = faststats


def generate(size, seed=42, outlier_rate=0.001):
    rng = random.Random(seed)
    data = [rng.gauss(50.0, 10.0) for _ in range(size)]
    for i in rng.sample(range(size), min(size, max(1, int(size * outlier_rate)))):
        data[i] = rng.choice((-1.0, 1.0)) * rng.uniform(150.0, 300.0)
    return data


def load(path):
    with open(path) as fh:
        text = fh.read().replace(",", " ")
    return [float(tok) for tok in text.split()]


def analyse(impl, data, threshold, lower, upper):
    t0 = time.perf_counter()
    result = {
        "summary": impl.summary(data, threshold),
        "sample_variance": impl.variance(data, ddof=1) if len(data) > 1 else None,
        "outside": impl.count_outside(data, lower, upper),
        "normalized": impl.normalize(data),
    }
    return result, time.perf_counter() - t0


def report(name, data, result, elapsed, lower, upper):
    s = result["summary"]
    norm = result["normalized"]
    print(f"[{name}] n={s['count']}  time={elapsed * 1000:.2f} ms")
    print(f"  mean={s['mean']:.6f}  variance={s['variance']:.6f}  std={s['std']:.6f}")
    if result["sample_variance"] is not None:
        print(f"  sample variance={result['sample_variance']:.6f}")
    print(f"  min={s['min']:.6f}  max={s['max']:.6f}")
    print(f"  outliers (|z| > threshold)={s['outliers']}")
    print(f"  outside [{lower}, {upper}]={result['outside']}")
    preview = ", ".join(f"{x:.4f}" for x in norm[:5])
    print(f"  normalized[:5]=[{preview}{', ...' if len(norm) > 5 else ''}]")


def main(argv=None):
    ap = argparse.ArgumentParser(description="Statistical summary using pure Python or the C extension.")
    ap.add_argument("--impl", choices=["python", "c", "both"], default="c")
    src = ap.add_mutually_exclusive_group()
    src.add_argument("--size", type=int, default=100000, help="number of generated values")
    src.add_argument("--file", help="text/CSV file of numbers to load")
    ap.add_argument("--seed", type=int, default=42)
    ap.add_argument("--threshold", type=float, default=3.0, help="z-score threshold for outliers")
    ap.add_argument("--lower", type=float, default=20.0)
    ap.add_argument("--upper", type=float, default=80.0)
    args = ap.parse_args(argv)

    if args.size < 0:
        ap.error("--size must be >= 0")
    if args.impl in ("c", "both") and faststats is None:
        sys.exit("faststats is not built; run: python3 setup.py build_ext --inplace")

    data = load(args.file) if args.file else generate(args.size, args.seed)
    names = ["python", "c"] if args.impl == "both" else [args.impl]

    results = {}
    try:
        for name in names:
            res, elapsed = analyse(IMPLEMENTATIONS[name], data, args.threshold, args.lower, args.upper)
            report(name, data, res, elapsed, args.lower, args.upper)
            results[name] = (res, elapsed)
    except (TypeError, ValueError) as exc:
        sys.exit(f"error: {exc}")

    if len(results) == 2:
        (rp, tp), (rc, tc) = results["python"], results["c"]
        print(f"identical results: {rp == rc}")
        print(f"speedup: {tp / tc:.1f}x")


if __name__ == "__main__":
    main()
