from math import isfinite, sqrt
import operator

__all__ = ["mean", "variance", "min_max", "count_outliers", "count_outside",
           "normalize", "summary"]

_TEXT_TYPES = (str, bytes, bytearray)
_FLOAT_ONLY = frozenset({float})

def _check_container(data):
    if (isinstance(data, _TEXT_TYPES) or isinstance(data, (dict, set, frozenset))
            or not hasattr(type(data), "__getitem__")):
        raise TypeError("data must be a sequence of real numbers, not %r"
                        % type(data).__name__)

def _coerce(x, i):
    """Convert one element exactly like the C extension does."""
    if isinstance(x, _TEXT_TYPES):
        raise TypeError("element %d: expected a real number, got %r"
                        % (i, type(x).__name__))
    try:
        v = float(x)
    except TypeError:
        raise TypeError("element %d: expected a real number, got %r"
                        % (i, type(x).__name__)) from None
    if not isfinite(v):
        raise ValueError("element %d is not a finite number" % i)
    return v

def _as_floats(data):
    """Validate ``data`` and return it as a list of finite Python floats."""
    _check_container(data)
    try:
        if type(data) in (list, tuple) and set(map(type, data)) <= _FLOAT_ONLY:

            values = data if type(data) is list else list(data)
        else:
            values = [x if type(x) is float else _coerce(x, i)
                      for i, x in enumerate(data)]
        ok = all(map(isfinite, values))
    except (TypeError, ValueError, OverflowError):
        ok = False
    if not ok:
        values = [_coerce(x, i) for i, x in enumerate(data)]
    if not values:
        raise ValueError("data must not be empty")
    return values

def _as_real(value, name):
    if isinstance(value, _TEXT_TYPES):
        raise TypeError("%s must be a real number, not %r" % (name, type(value).__name__))
    try:
        return float(value)
    except TypeError:
        raise TypeError("%s must be a real number, not %r"
                        % (name, type(value).__name__)) from None

def _check_threshold(threshold):
    t = _as_real(threshold, "threshold")
    if not isfinite(t) or t < 0.0:
        raise ValueError("threshold must be a finite number >= 0")
    return t

def _mean(vals):
    return sum(vals) / len(vals)

def _sum_sq_dev(vals, m):
    return sum([(x - m) * (x - m) for x in vals])

def _outliers(vals, m, std, threshold):
    if std == 0.0:
        return 0
    limit = threshold * std
    return sum(1 for x in vals if abs(x - m) > limit)

def mean(data):
    """Arithmetic mean of ``data``."""
    return _mean(_as_floats(data))

def variance(data, ddof=0):
    """Variance of ``data``: population (ddof=0, default) or sample (ddof=1)."""
    ddof = operator.index(ddof)
    vals = _as_floats(data)
    n = len(vals)
    if ddof < 0 or ddof >= n:
        raise ValueError("ddof must satisfy 0 <= ddof < len(data)")
    return _sum_sq_dev(vals, _mean(vals)) / (n - ddof)

def min_max(data):
    """Return ``(minimum, maximum)`` of ``data`` as floats."""
    vals = _as_floats(data)
    return (min(vals), max(vals))


def count_outliers(data, threshold=3.0):
    """Count values whose z-score magnitude exceeds ``threshold``:
    |x - mean| > threshold * population_std."""
    t = _check_threshold(threshold)
    vals = _as_floats(data)
    m = _mean(vals)
    std = sqrt(_sum_sq_dev(vals, m) / len(vals))
    return _outliers(vals, m, std, t)

def count_outside(data, lower, upper):
    """Count values strictly below ``lower`` or strictly above ``upper``."""
    lo = _as_real(lower, "lower")
    hi = _as_real(upper, "upper")
    if lo != lo or hi != hi:
        raise ValueError("lower and upper must not be NaN")
    if lo > hi:
        raise ValueError("lower must be <= upper")
    vals = _as_floats(data)
    return sum(1 for x in vals if x < lo or x > hi)

def normalize(data):
    """Min-max scale ``data`` to [0, 1]; returns a new list of floats.
    A constant dataset maps to all zeros."""
    vals = _as_floats(data)
    lo = min(vals)
    rng = max(vals) - lo
    if rng == 0.0:
        return [0.0] * len(vals)
    return [(x - lo) / rng for x in vals]

def summary(data, threshold=3.0):
    """All statistics from a single validation pass, as a dict."""
    t = _check_threshold(threshold)
    vals = _as_floats(data)
    n = len(vals)
    m = _mean(vals)
    var = _sum_sq_dev(vals, m) / n
    std = sqrt(var)
    return {
        "count": n,
        "mean": m,
        "variance": var,
        "std": std,
        "min": min(vals),
        "max": max(vals),
        "outliers": _outliers(vals, m, std, t),
    }