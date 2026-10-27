#define PY_SSIZE_T_CLEAN
#include <Python.h>
#include <math.h>

typedef struct {
    double *v;
    Py_ssize_t n;
} DoubleArray;

static int is_text(PyObject *o)
{
    return PyUnicode_Check(o) || PyBytes_Check(o) || PyByteArray_Check(o);
}

static void raise_with_type(PyObject *exc, const char *fmt, PyObject *obj)
{
    PyObject *name = PyType_GetName(Py_TYPE(obj));
    if (name) {
        PyErr_Format(exc, fmt, name);
        Py_DECREF(name);
    }
}

static int element_to_double(PyObject *item, Py_ssize_t i, double *out)
{
    double x;
    int rc = -1;

    if (PyFloat_CheckExact(item)) {
        x = PyFloat_AS_DOUBLE(item);
        if (!isfinite(x)) {
            PyErr_Format(PyExc_ValueError, "element %zd is not a finite number", i);
            return -1;
        }
        *out = x;
        return 0;
    }

    Py_INCREF(item);
    if (is_text(item)) {
        goto bad_type;
    }
    PyObject *f = PyNumber_Float(item);
    if (f == NULL) {
        if (PyErr_ExceptionMatches(PyExc_TypeError)) {
            PyErr_Clear();
            goto bad_type;
        }
        goto done;
    }
    x = PyFloat_AS_DOUBLE(f);
    Py_DECREF(f);
    if (!isfinite(x)) {
        PyErr_Format(PyExc_ValueError, "element %zd is not a finite number", i);
        goto done;
    }
    *out = x;
    rc = 0;
    goto done;

bad_type:
    {
        PyObject *name = PyType_GetName(Py_TYPE(item));
        if (name) {
            PyErr_Format(PyExc_TypeError,
                         "element %zd: expected a real number, got %R", i, name);
            Py_DECREF(name);
        }
    }
done:
    Py_DECREF(item);
    return rc;
}

static int to_array(PyObject *data, DoubleArray *arr)
{
    arr->v = NULL;
    arr->n = 0;

    if (is_text(data) || PyDict_Check(data) || PyAnySet_Check(data) ||
            !PyObject_HasAttrString((PyObject *)Py_TYPE(data), "__getitem__")) {
        raise_with_type(PyExc_TypeError,
                        "data must be a sequence of real numbers, not %R", data);
        return -1;
    }

    PyObject *seq = PySequence_Fast(data, "data must be iterable");
    if (seq == NULL) {
        return -1;
    }

    Py_ssize_t n = PySequence_Fast_GET_SIZE(seq);
    if (n == 0) {
        Py_DECREF(seq);
        PyErr_SetString(PyExc_ValueError, "data must not be empty");
        return -1;
    }
    if ((size_t)n > (size_t)PY_SSIZE_T_MAX / sizeof(double)) {
        Py_DECREF(seq);
        PyErr_NoMemory();
        return -1;
    }

    double *v = PyMem_Malloc((size_t)n * sizeof(double));
    if (v == NULL) {
        Py_DECREF(seq);
        PyErr_NoMemory();
        return -1;
    }

    for (Py_ssize_t i = 0; i < n; i++) {
        if (PySequence_Fast_GET_SIZE(seq) != n) {
            PyErr_SetString(PyExc_RuntimeError, "data changed size during conversion");
            goto fail;
        }
        if (element_to_double(PySequence_Fast_GET_ITEM(seq, i), i, &v[i]) < 0) {
            goto fail;
        }
    }

    Py_DECREF(seq);
    arr->v = v;
    arr->n = n;
    return 0;

fail:
    Py_DECREF(seq);
    PyMem_Free(v);
    return -1;
}

static int as_real(PyObject *o, const char *name, double *out)
{
    if (!is_text(o)) {
        PyObject *f = PyNumber_Float(o);
        if (f != NULL) {
            *out = PyFloat_AS_DOUBLE(f);
            Py_DECREF(f);
            return 0;
        }
        if (!PyErr_ExceptionMatches(PyExc_TypeError)) {
            return -1;
        }
        PyErr_Clear();
    }
    PyObject *tname = PyType_GetName(Py_TYPE(o));
    if (tname) {
        PyErr_Format(PyExc_TypeError, "%s must be a real number, not %R", name, tname);
        Py_DECREF(tname);
    }
    return -1;
}

static int parse_threshold(PyObject *obj, double *out)
{
    if (obj == NULL) {
        *out = 3.0;
        return 0;
    }
    if (as_real(obj, "threshold", out) < 0) {
        return -1;
    }
    if (!isfinite(*out) || *out < 0.0) {
        PyErr_SetString(PyExc_ValueError, "threshold must be a finite number >= 0");
        return -1;
    }
    return 0;
}

static double neumaier_sum(const double *v, Py_ssize_t n)
{
    double s = 0.0, c = 0.0;
    for (Py_ssize_t i = 0; i < n; i++) {
        double x = v[i];
        double t = s + x;
        if (fabs(s) >= fabs(x)) {
            c += (s - t) + x;
        } else {
            c += (x - t) + s;
        }
        s = t;
    }
    if (c != 0.0 && isfinite(c)) {
        s += c;
    }
    return s;
}

static double sum_sq_dev(const double *v, Py_ssize_t n, double m)
{
    double s = 0.0, c = 0.0;
    for (Py_ssize_t i = 0; i < n; i++) {
        double x = (v[i] - m) * (v[i] - m);
        double t = s + x;
        if (fabs(s) >= fabs(x)) {
            c += (s - t) + x;
        } else {
            c += (x - t) + s;
        }
        s = t;
    }
    if (c != 0.0 && isfinite(c)) {
        s += c;
    }
    return s;
}

static void find_min_max(const double *v, Py_ssize_t n, double *lo, double *hi)
{
    double a = v[0], b = v[0];
    for (Py_ssize_t i = 1; i < n; i++) {
        if (v[i] < a) {
            a = v[i];
        }
        if (v[i] > b) {
            b = v[i];
        }
    }
    *lo = a;
    *hi = b;
}

static Py_ssize_t outliers(const double *v, Py_ssize_t n, double m, double sd, double t)
{
    if (sd == 0.0) {
        return 0;
    }
    double limit = t * sd;
    Py_ssize_t count = 0;
    for (Py_ssize_t i = 0; i < n; i++) {
        if (fabs(v[i] - m) > limit) {
            count++;
        }
    }
    return count;
}

static PyObject *fs_mean(PyObject *Py_UNUSED(self), PyObject *args, PyObject *kwargs)
{
    static char *kwlist[] = {"data", NULL};
    PyObject *data;
    DoubleArray a;
    double r;

    if (!PyArg_ParseTupleAndKeywords(args, kwargs, "O:mean", kwlist, &data)) {
        return NULL;
    }
    if (to_array(data, &a) < 0) {
        return NULL;
    }
    Py_BEGIN_ALLOW_THREADS
    r = neumaier_sum(a.v, a.n) / (double)a.n;
    Py_END_ALLOW_THREADS
    PyMem_Free(a.v);
    return PyFloat_FromDouble(r);
}

static PyObject *fs_variance(PyObject *Py_UNUSED(self), PyObject *args, PyObject *kwargs)
{
    static char *kwlist[] = {"data", "ddof", NULL};
    PyObject *data, *ddof_obj = NULL;
    long long ddof = 0;
    int overflow = 0;
    DoubleArray a;
    double r;

    if (!PyArg_ParseTupleAndKeywords(args, kwargs, "O|O:variance", kwlist,
                                     &data, &ddof_obj)) {
        return NULL;
    }
    if (ddof_obj != NULL) {
        PyObject *idx = PyNumber_Index(ddof_obj);
        if (idx == NULL) {
            return NULL;
        }
        ddof = PyLong_AsLongLongAndOverflow(idx, &overflow);
        Py_DECREF(idx);
        if (ddof == -1 && PyErr_Occurred()) {
            return NULL;
        }
    }
    if (to_array(data, &a) < 0) {
        return NULL;
    }
    if (overflow || ddof < 0 || ddof >= (long long)a.n) {
        PyMem_Free(a.v);
        PyErr_SetString(PyExc_ValueError, "ddof must satisfy 0 <= ddof < len(data)");
        return NULL;
    }
    Py_BEGIN_ALLOW_THREADS
    double m = neumaier_sum(a.v, a.n) / (double)a.n;
    r = sum_sq_dev(a.v, a.n, m) / (double)(a.n - (Py_ssize_t)ddof);
    Py_END_ALLOW_THREADS
    PyMem_Free(a.v);
    return PyFloat_FromDouble(r);
}

static PyObject *fs_min_max(PyObject *Py_UNUSED(self), PyObject *args, PyObject *kwargs)
{
    static char *kwlist[] = {"data", NULL};
    PyObject *data;
    DoubleArray a;
    double lo, hi;

    if (!PyArg_ParseTupleAndKeywords(args, kwargs, "O:min_max", kwlist, &data)) {
        return NULL;
    }
    if (to_array(data, &a) < 0) {
        return NULL;
    }
    Py_BEGIN_ALLOW_THREADS
    find_min_max(a.v, a.n, &lo, &hi);
    Py_END_ALLOW_THREADS
    PyMem_Free(a.v);
    return Py_BuildValue("(dd)", lo, hi);
}

static PyObject *fs_count_outliers(PyObject *Py_UNUSED(self), PyObject *args, PyObject *kwargs)
{
    static char *kwlist[] = {"data", "threshold", NULL};
    PyObject *data, *thr_obj = NULL;
    DoubleArray a;
    double t;
    Py_ssize_t count;

    if (!PyArg_ParseTupleAndKeywords(args, kwargs, "O|O:count_outliers", kwlist,
                                     &data, &thr_obj)) {
        return NULL;
    }
    if (parse_threshold(thr_obj, &t) < 0) {
        return NULL;
    }
    if (to_array(data, &a) < 0) {
        return NULL;
    }
    Py_BEGIN_ALLOW_THREADS
    double m = neumaier_sum(a.v, a.n) / (double)a.n;
    double sd = sqrt(sum_sq_dev(a.v, a.n, m) / (double)a.n);
    count = outliers(a.v, a.n, m, sd, t);
    Py_END_ALLOW_THREADS
    PyMem_Free(a.v);
    return PyLong_FromSsize_t(count);
}

static PyObject *fs_count_outside(PyObject *Py_UNUSED(self), PyObject *args, PyObject *kwargs)
{
    static char *kwlist[] = {"data", "lower", "upper", NULL};
    PyObject *data, *lo_obj, *hi_obj;
    DoubleArray a;
    double lo, hi;
    Py_ssize_t count = 0;

    if (!PyArg_ParseTupleAndKeywords(args, kwargs, "OOO:count_outside", kwlist,
                                     &data, &lo_obj, &hi_obj)) {
        return NULL;
    }
    if (as_real(lo_obj, "lower", &lo) < 0 || as_real(hi_obj, "upper", &hi) < 0) {
        return NULL;
    }
    if (isnan(lo) || isnan(hi)) {
        PyErr_SetString(PyExc_ValueError, "lower and upper must not be NaN");
        return NULL;
    }
    if (lo > hi) {
        PyErr_SetString(PyExc_ValueError, "lower must be <= upper");
        return NULL;
    }
    if (to_array(data, &a) < 0) {
        return NULL;
    }
    Py_BEGIN_ALLOW_THREADS
    for (Py_ssize_t i = 0; i < a.n; i++) {
        if (a.v[i] < lo || a.v[i] > hi) {
            count++;
        }
    }
    Py_END_ALLOW_THREADS
    PyMem_Free(a.v);
    return PyLong_FromSsize_t(count);
}

static PyObject *fs_normalize(PyObject *Py_UNUSED(self), PyObject *args, PyObject *kwargs)
{
    static char *kwlist[] = {"data", NULL};
    PyObject *data;
    DoubleArray a;
    double lo, hi;

    if (!PyArg_ParseTupleAndKeywords(args, kwargs, "O:normalize", kwlist, &data)) {
        return NULL;
    }
    if (to_array(data, &a) < 0) {
        return NULL;
    }
    Py_BEGIN_ALLOW_THREADS
    find_min_max(a.v, a.n, &lo, &hi);
    double rng = hi - lo;
    for (Py_ssize_t i = 0; i < a.n; i++) {
        a.v[i] = (rng == 0.0) ? 0.0 : (a.v[i] - lo) / rng;
    }
    Py_END_ALLOW_THREADS

    PyObject *out = PyList_New(a.n);
    if (out == NULL) {
        PyMem_Free(a.v);
        return NULL;
    }
    for (Py_ssize_t i = 0; i < a.n; i++) {
        PyObject *f = PyFloat_FromDouble(a.v[i]);
        if (f == NULL) {
            Py_DECREF(out);
            PyMem_Free(a.v);
            return NULL;
        }
        PyList_SET_ITEM(out, i, f);
    }
    PyMem_Free(a.v);
    return out;
}

static PyObject *fs_summary(PyObject *Py_UNUSED(self), PyObject *args, PyObject *kwargs)
{
    static char *kwlist[] = {"data", "threshold", NULL};
    PyObject *data, *thr_obj = NULL;
    DoubleArray a;
    double t, m, var, sd, lo, hi;
    Py_ssize_t count;

    if (!PyArg_ParseTupleAndKeywords(args, kwargs, "O|O:summary", kwlist,
                                     &data, &thr_obj)) {
        return NULL;
    }
    if (parse_threshold(thr_obj, &t) < 0) {
        return NULL;
    }
    if (to_array(data, &a) < 0) {
        return NULL;
    }
    Py_BEGIN_ALLOW_THREADS
    m = neumaier_sum(a.v, a.n) / (double)a.n;
    var = sum_sq_dev(a.v, a.n, m) / (double)a.n;
    sd = sqrt(var);
    find_min_max(a.v, a.n, &lo, &hi);
    count = outliers(a.v, a.n, m, sd, t);
    Py_END_ALLOW_THREADS
    Py_ssize_t n = a.n;
    PyMem_Free(a.v);
    return Py_BuildValue("{s:n,s:d,s:d,s:d,s:d,s:d,s:n}",
                         "count", n, "mean", m, "variance", var, "std", sd,
                         "min", lo, "max", hi, "outliers", count);
}

static PyMethodDef faststats_methods[] = {
    {"mean", (PyCFunction)(void (*)(void))fs_mean, METH_VARARGS | METH_KEYWORDS,
     "mean(data)\n--\n\nArithmetic mean of data."},
    {"variance", (PyCFunction)(void (*)(void))fs_variance, METH_VARARGS | METH_KEYWORDS,
     "variance(data, ddof=0)\n--\n\nPopulation (ddof=0) or sample (ddof=1) variance."},
    {"min_max", (PyCFunction)(void (*)(void))fs_min_max, METH_VARARGS | METH_KEYWORDS,
     "min_max(data)\n--\n\nReturn (minimum, maximum) as floats."},
    {"count_outliers", (PyCFunction)(void (*)(void))fs_count_outliers,
     METH_VARARGS | METH_KEYWORDS,
     "count_outliers(data, threshold=3.0)\n--\n\n"
     "Count values with |x - mean| > threshold * population_std."},
    {"count_outside", (PyCFunction)(void (*)(void))fs_count_outside,
     METH_VARARGS | METH_KEYWORDS,
     "count_outside(data, lower, upper)\n--\n\n"
     "Count values strictly below lower or strictly above upper."},
    {"normalize", (PyCFunction)(void (*)(void))fs_normalize, METH_VARARGS | METH_KEYWORDS,
     "normalize(data)\n--\n\nMin-max scale data to [0, 1]; constant data maps to zeros."},
    {"summary", (PyCFunction)(void (*)(void))fs_summary, METH_VARARGS | METH_KEYWORDS,
     "summary(data, threshold=3.0)\n--\n\nAll statistics from one validation pass, as a dict."},
    {NULL, NULL, 0, NULL}
};

static struct PyModuleDef faststats_module = {
    PyModuleDef_HEAD_INIT,
    "faststats",
    "C implementation of the pystats statistical operations.",
    -1,
    faststats_methods,
    NULL,
    NULL,
    NULL,
    NULL
};

PyMODINIT_FUNC PyInit_faststats(void)
{
    return PyModule_Create(&faststats_module);
}
