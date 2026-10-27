# Building a CPython Extension for High-Performance Data Analysis

## Files

| File | Content |
|---|---|
| `pystats.py` | Pure-Python baseline: `mean`, `variance`, `min_max`, `count_outliers`, `count_outside`, `normalize`, `summary` |
| `faststats.c` | C extension implementing the same functions with the same API, results and errors |
| `setup.py` | Build script for the extension |
| `faststats.cpython-314-x86_64-linux-gnu.so` | Compiled, importable module |
| `stats_app.py` | Application: generates or loads data and runs either implementation, or both |
| `test_equivalence.py` / `test_results.txt` | Equivalence tests and their output |
| `benchmark.py` / `benchmark_results.md` / `.csv` | Benchmark script and results |

## Build and Run

```bash
python3 setup.py build_ext --inplace
python3 stats_app.py --impl both --size 1000000
python3 stats_app.py --impl c --file data.csv --threshold 2.5 --lower 20 --upper 80
python3 -m unittest -v test_equivalence
python3 benchmark.py --sizes 1000 100000 1000000 --repeats 10
```

To build without setuptools:

```bash
gcc -O3 -Wall -Wextra -ffp-contract=off -shared -fPIC $(python3-config --includes) faststats.c -o faststats$(python3-config --extension-suffix)
```

The source needs Python ≥ 3.11 headers for `PyType_GetName`. `-ffp-contract=off` stops GCC from fusing `a*b+c` into FMA instructions, which would change rounding compared with Python.

## Implementation

**Inputs:**
- `data` must be a sequence: list, tuple, `range`, `array`, or any type with `__getitem__`.
- `str`, `bytes`, `bytearray`, `dict` and `set` are rejected with `TypeError`.
- Each element is converted with `PyNumber_Float`, which behaves like `float(x)`, so `int`, `bool`, `Fraction`, `Decimal` and objects with `__float__`/`__index__` are accepted.
- Text elements and other non-numeric elements raise `TypeError("element i: ...")`.
- NaN and ±inf raise `ValueError`.
- An empty dataset raises `ValueError("data must not be empty")`.
- `threshold` must be finite and ≥ 0.
- `lower ≤ upper`, and neither may be NaN.
- `ddof` must be an integer with `0 ≤ ddof < n`.

**Memory:**
- Values are copied once into a `PyMem_Malloc` buffer of C `double`, which is freed on every exit path.
- Each item gets a temporary strong reference while it is converted, and the code detects a list that is resized during conversion.
- The size product is checked for overflow before allocation.
- The numeric loops run with the GIL released (`Py_BEGIN_ALLOW_THREADS`).

**Results:**
- Return values are ordinary Python objects: `float`, `int`, a `(min, max)` tuple, a new `list` from `normalize`, and a `dict` from `summary`.
- Sums use the same Neumaier compensated summation as CPython's built-in `sum()`, so results are bit-identical to `pystats`, not just close.

## Equivalence Evidence

`test_results.txt` shows all 9 tests passing.

- **Data and functions:** 17 datasets × 13 calls (221 comparisons, 220 successful results plus one matching `ValueError`). Floats are compared bit for bit, including the sign of zero. The datasets range from 1 value to 1,000,000 values and include:
  - ints and mixed numeric types, `range`, `array('d')` and a custom sequence class
  - constant data and signed zeros
  - values near 1e±300 and denormals
  - catastrophic-cancellation data and data with outliers
- **Errors:** 24 invalid inputs × 13 calls, plus 17 invalid parameters. Each produces the same exception type and message in both implementations. Wrong argument counts give `TypeError` in both, but the message text comes from the interpreter, so it differs.
- **Cross-check:** results agree with `statistics.fmean`, `statistics.pvariance` and `statistics.variance` for 1M values.
- **Resource checks:** `sys.getrefcount` stays unchanged and `tracemalloc` shows no memory growth across repeated successful and failing calls, so nothing leaks.

`stats_app.py --impl both --size 1000000` prints `identical results: True`.

## Benchmark Results

Setup: Intel i5-6300U, Python 3.14.4, GCC 15.2, `-O3`. Each figure is the average of 10 repeats, using `timeit` with GC disabled. The same list of floats is passed to both implementations. Full times with standard deviations are in [benchmark_results.md](benchmark_results.md).

| Operation | Python 1K | C 1K | Speedup | Python 100K | C 100K | Speedup | Python 1M | C 1M | Speedup |
|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| mean | 0.060 ms | 0.007 ms | 8.9x | 5.50 ms | 0.76 ms | 7.2x | 59.4 ms | 9.6 ms | 6.2x |
| variance | 0.145 ms | 0.008 ms | 18.4x | 14.92 ms | 0.94 ms | 15.9x | 178.1 ms | 11.4 ms | 15.6x |
| min_max | 0.076 ms | 0.006 ms | 12.2x | 9.74 ms | 0.73 ms | 13.3x | 89.2 ms | 8.8 ms | 10.2x |
| count_outliers | 0.227 ms | 0.009 ms | 25.4x | 25.18 ms | 1.00 ms | 25.3x | 253.4 ms | 12.0 ms | 21.1x |
| count_outside | 0.117 ms | 0.006 ms | 20.4x | 11.68 ms | 0.83 ms | 14.1x | 95.3 ms | 6.5 ms | 14.8x |
| normalize | 0.159 ms | 0.027 ms | 5.8x | 19.04 ms | 5.39 ms | 3.5x | 182.2 ms | 60.0 ms | 3.0x |
| summary | 0.231 ms | 0.012 ms | 18.8x | 25.89 ms | 1.31 ms | 19.7x | 290.0 ms | 15.1 ms | 19.2x |

**Effect of dataset size:**
- Both implementations scale linearly, O(n): roughly 10× more time for 10× more data.
- Speedups are largest for operations with per-element Python work, such as generator expressions, `abs()` and comparisons. `count_outliers` and `summary` reach about 20–25×.
- Speedups shrink slightly at 1M values. 1M float objects (24 MB) plus the list (8 MB) exceed the 3 MB L3 cache, so the C code becomes memory-bound, following a pointer to each `PyFloat` object.
- At 1K values, the fixed per-call cost of argument parsing and allocation is a larger share of the C time.

**Operations that improve less:**
- **`mean` (6–9×):** the baseline already uses the built-in `sum()`, which is implemented in C. Most of the C time (about 9 ms of 9.6 ms at 1M) goes to reading and validating the Python objects. `min_max` costs nearly the same, which shows the conversion cost dominates.
- **`normalize` (3–6×):** both versions must allocate 1M new `float` objects and a list for the result. Object allocation dominates, so the arithmetic done in C saves little.

## Analysis

**How the C API connects Python to C:**
- `import faststats` makes the interpreter `dlopen()` the `.so` and call its exported `PyInit_faststats`. That is the only dynamic symbol (`nm -D --defined-only`).
- `PyInit_faststats` builds a module object from the `PyModuleDef` and its `PyMethodDef` table.
- Each table entry maps a Python name to a C function of type `PyObject *(PyObject *self, PyObject *args, PyObject *kwargs)`.
- On each call, `PyArg_ParseTupleAndKeywords` unpacks the arguments and the function converts them with C API calls (`PyNumber_Float`, `PySequence_Fast`). It returns new references (`PyFloat_FromDouble`, `PyList_New`, `Py_BuildValue`), or `NULL` with an exception set, which Python raises.

**How compilation produces an importable module:**
- `setup.py build_ext --inplace` compiles `faststats.c` with the interpreter's own flags and headers (`-fPIC -I/usr/include/python3.14`) and links it with `-shared`.
- The output is named with the ABI tag, e.g. `cpython-314-x86_64-linux-gnu.so`. This tag tells the import system which interpreter version and platform the module was built for.

**Trade-offs:**
- **Gains:** 3–25× speed, native `double` arithmetic, and the GIL can be released.
- **Costs:**
  - Manual reference counting and memory management, where a mistake causes leaks or crashes instead of exceptions.
  - The module has to be compiled for each Python version and platform.
  - Error messages and validation have to be duplicated by hand.
  - It is harder to debug, and is about 3× more code than the Python version.

**Limitations:**
- Input still arrives as Python objects, so per-element conversion caps the gains. A buffer-protocol path (`array('d')`, NumPy) could skip conversion entirely.
- Each call converts the data again. `summary` avoids this by doing everything in one pass.
- Precision is IEEE-754 double. Integers above 2⁵³ lose precision, and integers above about 1.8e308 raise `OverflowError`. Both implementations behave the same way here.
- The module uses single-phase initialisation and is not marked as safe for free-threaded builds.
