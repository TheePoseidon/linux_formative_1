import math
import random
import sys
import unittest
from array import array
from decimal import Decimal
from fractions import Fraction

import faststats
import pystats
from stats_app import generate


def same(a, b):
    if isinstance(a, float) and isinstance(b, float):
        return a == b and math.copysign(1.0, a) == math.copysign(1.0, b)
    if isinstance(a, (list, tuple)) and isinstance(b, (list, tuple)):
        return type(a) is type(b) and len(a) == len(b) and all(map(same, a, b))
    if isinstance(a, dict) and isinstance(b, dict):
        return list(a) == list(b) and all(same(a[k], b[k]) for k in a)
    return type(a) is type(b) and a == b


class WithFloat:
    def __init__(self, v):
        self.v = v

    def __float__(self):
        return self.v


class WithIndex:
    def __init__(self, v):
        self.v = v

    def __index__(self):
        return self.v


class Seq:
    def __init__(self, items):
        self.items = list(items)

    def __len__(self):
        return len(self.items)

    def __getitem__(self, i):
        return self.items[i]


rng = random.Random(2024)

DATASETS = {
    "single": [3.5],
    "two": [1.0, 2.0],
    "small_ints": [1, 2, 3, 4, 5],
    "negatives": [-1.5, 2.25, 0.0, -7.75, 3.0],
    "mixed_types": [1, 2.5, True, Fraction(1, 3), Decimal("2.75"), WithFloat(4.0), WithIndex(6)],
    "tuple": (4.0, 8.0, 15.0, 16.0, 23.0, 42.0),
    "range": range(1000),
    "array_d": array("d", [rng.uniform(-1, 1) for _ in range(500)]),
    "custom_seq": Seq([2.0, 4.0, 4.0, 4.0, 5.0, 5.0, 7.0, 9.0]),
    "constant": [7.0] * 100,
    "signed_zero": [0.0, -0.0, 0.0],
    "extreme_magnitude": [1e300, -1e300, 1e-300, 5e-324],
    "cancellation": [1e16, 1.0, -1e16, 1.0] * 50,
    "with_outliers": [10.0] * 99 + [1000.0],
    "uniform_100k": [rng.uniform(0, 1000) for _ in range(100_000)],
    "descending_10k": sorted((rng.random() for _ in range(10_000)), reverse=True),
    "gauss_1m": generate(1_000_000, seed=7),
}

CALLS = [
    ("mean", ()),
    ("variance", ()),
    ("variance", (1,)),
    ("min_max", ()),
    ("count_outliers", ()),
    ("count_outliers", (0,)),
    ("count_outliers", (2.5,)),
    ("count_outside", (20, 80)),
    ("count_outside", (-math.inf, 0.5)),
    ("count_outside", (0.0, 0.0)),
    ("normalize", ()),
    ("summary", ()),
    ("summary", (1,)),
]


def outcome(fn, *args, **kwargs):
    try:
        return ("ok", fn(*args, **kwargs))
    except Exception as exc:
        return (type(exc).__name__, str(exc))


class EquivalenceTests(unittest.TestCase):
    def test_datasets(self):
        for dname, data in DATASETS.items():
            for fname, args in CALLS:
                with self.subTest(dataset=dname, func=fname, args=args):
                    p = outcome(getattr(pystats, fname), data, *args)
                    c = outcome(getattr(faststats, fname), data, *args)
                    self.assertTrue(same(p, c), f"python={p!r:.200} c={c!r:.200}")

    def test_keywords(self):
        d = DATASETS["uniform_100k"]
        for mod in (pystats, faststats):
            self.assertTrue(same(mod.variance(data=d, ddof=1), pystats.variance(d, 1)))
            self.assertTrue(same(mod.count_outliers(d, threshold=2), pystats.count_outliers(d, 2)))
            self.assertTrue(same(mod.count_outside(d, lower=100, upper=900), pystats.count_outside(d, 100, 900)))

    def test_against_statistics_module(self):
        import statistics
        d = DATASETS["gauss_1m"]
        self.assertEqual(faststats.mean(d), statistics.fmean(d))
        self.assertAlmostEqual(faststats.variance(d), statistics.pvariance(d), places=9)
        self.assertAlmostEqual(faststats.variance(d, 1), statistics.variance(d), places=9)

    def test_normalize_properties(self):
        d = DATASETS["gauss_1m"]
        out = faststats.normalize(d)
        self.assertIsInstance(out, list)
        self.assertIsNot(out, d)
        self.assertEqual((min(out), max(out)), (0.0, 1.0))
        self.assertEqual(faststats.normalize([5, 5, 5]), [0.0, 0.0, 0.0])

    def test_errors_match(self):
        bad_data = [
            [], (), "abc", b"12", bytearray(b"1"), {1: 2}, {1.0}, frozenset(), 5, None, 3.5,
            (x for x in [1.0]), iter([1.0]),
            [1.0, "2"], [1.0, b"2"], [1.0, None], [1.0, [2.0]], [1.0, 2j],
            [1.0, float("nan")], [float("inf")], [1.0, -math.inf], [1, 10 ** 400],
            [Decimal("NaN")], [WithFloat("x")],
        ]
        for data in bad_data:
            for fname, args in CALLS:
                with self.subTest(data=repr(data)[:40], func=fname):
                    p = outcome(getattr(pystats, fname), data, *args)
                    c = outcome(getattr(faststats, fname), data, *args)
                    self.assertNotEqual(p[0], "ok")
                    self.assertEqual(p, c)

    def test_parameter_errors_match(self):
        d = [1.0, 2.0, 3.0]
        cases = [
            ("variance", (d, 1.5)), ("variance", (d, -1)), ("variance", (d, 3)),
            ("variance", (d, 10 ** 30)), ("variance", (d, "1")), ("variance", ([1.0], 1)),
            ("count_outliers", (d, -1)), ("count_outliers", (d, math.nan)),
            ("count_outliers", (d, math.inf)), ("count_outliers", (d, "3")),
            ("count_outliers", (d, None)), ("summary", (d, -0.5)), ("summary", ([], -1)),
            ("count_outside", (d, 5, 1)), ("count_outside", (d, math.nan, 1)),
            ("count_outside", (d, 0, "1")), ("count_outside", (d, None, 1)),
        ]
        for fname, args in cases:
            with self.subTest(func=fname, args=repr(args)[:50]):
                p = outcome(getattr(pystats, fname), *args)
                c = outcome(getattr(faststats, fname), *args)
                self.assertNotEqual(p[0], "ok")
                self.assertEqual(p, c)

    def test_wrong_argument_count(self):
        d = [1.0, 2.0, 3.0]
        for fname, args in [("count_outside", (d, 0)), ("mean", ()), ("mean", (d, d))]:
            with self.subTest(func=fname, nargs=len(args)):
                p = outcome(getattr(pystats, fname), *args)
                c = outcome(getattr(faststats, fname), *args)
                self.assertEqual((p[0], c[0]), ("TypeError", "TypeError"))

    def test_no_reference_leaks(self):
        marker = 12345.678
        data = [marker] * 1000 + [1.0, 2.0]
        before_item, before_list = sys.getrefcount(marker), sys.getrefcount(data)
        for _ in range(200):
            faststats.summary(data)
            faststats.normalize(data)
            outcome(faststats.mean, data + ["bad"])
        self.assertEqual(sys.getrefcount(marker), before_item)
        self.assertEqual(sys.getrefcount(data), before_list)

    def test_no_memory_growth(self):
        import tracemalloc
        data = DATASETS["uniform_100k"]
        faststats.summary(data)
        tracemalloc.start()
        snap = tracemalloc.take_snapshot()
        for _ in range(50):
            faststats.summary(data)
            faststats.variance(data, 1)
            outcome(faststats.mean, data + [None])
        grown = sum(s.size_diff for s in tracemalloc.take_snapshot().compare_to(snap, "filename"))
        tracemalloc.stop()
        self.assertLess(grown, 64 * 1024)


if __name__ == "__main__":
    unittest.main(verbosity=2)
