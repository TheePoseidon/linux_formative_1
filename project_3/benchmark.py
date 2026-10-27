import argparse
import csv
import platform
import statistics
import timeit

import faststats
import pystats
from stats_app import generate

OPERATIONS = [
    ("mean", ()),
    ("variance", ()),
    ("min_max", ()),
    ("count_outliers", (3.0,)),
    ("count_outside", (20.0, 80.0)),
    ("normalize", ()),
    ("summary", (3.0,)),
]


def measure(fn, data, args, repeats, number):
    runs = timeit.Timer(lambda: fn(data, *args)).repeat(repeat=repeats, number=number)
    per_call = [r / number for r in runs]
    return statistics.mean(per_call), statistics.stdev(per_call) if repeats > 1 else 0.0


def main():
    ap = argparse.ArgumentParser(description="Benchmark pystats vs faststats.")
    ap.add_argument("--sizes", type=int, nargs="+", default=[1_000, 100_000, 1_000_000])
    ap.add_argument("--repeats", type=int, default=10)
    ap.add_argument("--csv", default="benchmark_results.csv")
    ap.add_argument("--md", default="benchmark_results.md")
    args = ap.parse_args()

    rows = []
    for size in args.sizes:
        data = generate(size, seed=1)
        number = max(1, 100_000 // size)
        for name, extra in OPERATIONS:
            py_fn, c_fn = getattr(pystats, name), getattr(faststats, name)
            if py_fn(data, *extra) != c_fn(data, *extra):
                raise SystemExit(f"result mismatch: {name} n={size}")
            py_mean, py_sd = measure(py_fn, data, extra, args.repeats, number)
            c_mean, c_sd = measure(c_fn, data, extra, args.repeats, number)
            rows.append({
                "size": size, "operation": name,
                "python_ms": py_mean * 1e3, "python_sd_ms": py_sd * 1e3,
                "c_ms": c_mean * 1e3, "c_sd_ms": c_sd * 1e3,
                "speedup": py_mean / c_mean,
            })
            print(f"n={size:>9,}  {name:<15} python {py_mean * 1e3:10.4f} ms   "
                  f"C {c_mean * 1e3:9.4f} ms   speedup {py_mean / c_mean:6.1f}x")

    with open(args.csv, "w", newline="") as fh:
        w = csv.DictWriter(fh, fieldnames=list(rows[0]))
        w.writeheader()
        for r in rows:
            w.writerow({k: (f"{v:.6f}" if isinstance(v, float) else v) for k, v in r.items()})

    with open(args.md, "w") as fh:
        fh.write(f"Python {platform.python_version()} on {platform.machine()}, "
                 f"{args.repeats} repeats per measurement, times are average ms per call.\n\n")
        fh.write("| Size | Operation | Python (ms) | C extension (ms) | Speedup |\n")
        fh.write("|---:|---|---:|---:|---:|\n")
        for r in rows:
            fh.write(f"| {r['size']:,} | {r['operation']} | {r['python_ms']:.4f} ± {r['python_sd_ms']:.4f} | "
                     f"{r['c_ms']:.4f} ± {r['c_sd_ms']:.4f} | {r['speedup']:.1f}x |\n")
    print(f"saved {args.csv} and {args.md}")


if __name__ == "__main__":
    main()
