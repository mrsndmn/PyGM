#include <pybind11/numpy.h>
#include <pybind11/operators.h>
#include <pybind11/pybind11.h>
#include <pybind11/stl.h>

#include <algorithm>
#include <unordered_map>
#include <vector>

#include "pgm_index.hpp"

namespace py = pybind11;
using namespace pybind11::literals;

class PGMWrapper {
    using K = int64_t;
    K *data;
    size_t n;
    PGMIndex<K> pgm;

  public:
    PGMWrapper(const std::vector<K> &vector) : n(vector.size()) {
        data = new K[n];
        std::copy_n(vector.data(), n, data);
        if (!std::is_sorted(begin(), end()))
            std::sort(begin(), end());

        py::gil_scoped_release release;
        pgm = decltype(pgm)(begin(), end());
    }

    PGMWrapper(py::iterator it) {
        std::vector<int64_t> v;
        for (; it != py::iterator::sentinel(); ++it)
            v.push_back(it->cast<int64_t>());
        n = v.size();
        data = new K[n];
        std::copy_n(v.data(), n, data);

        py::gil_scoped_release release;
        pgm = decltype(pgm)(begin(), end());
    }

    PGMWrapper(K *data, size_t n) : data(data), n(n) {
        py::gil_scoped_release release;
        pgm = decltype(pgm)(begin(), end());
    }

    PGMWrapper(py::array_t<K> array) {
        auto r = array.unchecked<1>();
        n = r.shape(0);
        data = new K[n];
        std::copy_n(r.data(0), n, data);
        if (!std::is_sorted(begin(), end()))
            std::sort(begin(), end());

        py::gil_scoped_release release;
        pgm = decltype(pgm)(begin(), end());
    }

    bool contains(K x) const {
        auto ap = pgm.find_approximate_position(x);
        return std::binary_search(data + ap.lo, data + ap.hi, x);
    }

    K *lower_bound(K x) const {
        auto ap = pgm.find_approximate_position(x);
        return std::lower_bound(data + ap.lo, data + ap.hi, x);
    }

    K *upper_bound(K x) const {
        auto ap = pgm.find_approximate_position(x);
        return std::upper_bound(data + ap.lo, data + ap.hi, x);
    }

    std::unordered_map<std::string, size_t> stats() {
        std::unordered_map<std::string, size_t> stats;
        stats["leaf segments"] = pgm.segments_count();
        stats["data size"] = sizeof(K) * n;
        stats["index size"] = pgm.size_in_bytes();
        stats["height"] = pgm.height();
        return stats;
    }

    K operator[](size_t i) const { return begin()[i]; }

    size_t size() const { return n; }

    K *begin() const { return data; }

    K *end() const { return data + n; }

    ~PGMWrapper() { delete[] data; }
};

PYBIND11_MODULE(pypgm, m) {
    py::class_<PGMWrapper>(m, "PGMIndex")
        .def(py::init<py::array_t<int64_t>>())

        .def(py::init<py::iterator>())

        // sequence protocol
        .def("__len__", &PGMWrapper::size, "Return len(self)")

        .def("__contains__", &PGMWrapper::contains)

        .def("__getitem__",
             [](const PGMWrapper &p, py::slice slice) -> PGMWrapper * {
                 size_t start, stop, step, length;
                 if (!slice.compute(p.size(), &start, &stop, &step, &length))
                     throw py::error_already_set();

                 auto data = new int64_t[length];
                 for (size_t i = 0; i < length; ++i) {
                     data[i] = p[start];
                     start += step;
                 }

                 return new PGMWrapper(data, length);
             })

        .def("__getitem__",
             [](const PGMWrapper &p, ssize_t i) {
                 if (i < 0)
                     i += p.size();
                 if (i < 0 || (size_t) i >= p.size())
                     throw py::index_error();
                 return p[i];
             })

        // iterator protocol
        .def(
            "__iter__",
            [](const PGMWrapper &p) { return py::make_iterator(p.begin(), p.end()); },
            py::keep_alive<0, 1>())

        // query operations
        .def(
            "find_lt",
            [](const PGMWrapper &p, int64_t x) {
                auto it = p.lower_bound(x);
                if (it <= p.begin())
                    return py::object(py::cast(nullptr));
                return py::cast(*(it - 1));
            },
            "Find the rightmost value less than x.",
            "x"_a)

        .def(
            "find_le",
            [](const PGMWrapper &p, int64_t x) {
                auto it = p.upper_bound(x);
                if (it <= p.begin())
                    return py::object(py::cast(nullptr));
                return py::cast(*(it - 1));
            },
            "Find the rightmost value less than or equal to x.",
            "x"_a)

        .def(
            "find_gt",
            [](const PGMWrapper &p, int64_t x) -> py::object {
                auto it = p.upper_bound(x);
                if (it >= p.end())
                    return py::object(py::cast(nullptr));
                return py::cast(*it);
            },
            "Find the leftmost value greater than x.",
            "x"_a)

        .def(
            "find_ge",
            [](const PGMWrapper &p, int64_t x) -> py::object {
                auto it = p.lower_bound(x);
                if (it >= p.end())
                    return py::object(py::cast(nullptr));
                return py::cast(*it);
            },
            "Find the leftmost value greater than or equal to x.",
            "x"_a)

        .def(
            "rank",
            [](const PGMWrapper &p, int64_t x) { return std::distance(p.begin(), p.upper_bound(x)); },
            "Number of values less than or equal to x.",
            "x"_a)

        .def(
            "count",
            [](const PGMWrapper &p, int64_t x) -> size_t {
                auto lb = p.lower_bound(x);
                if (lb >= p.end() || *lb != x)
                    return 0;
                return std::distance(lb, p.upper_bound(x));
            },
            "Number of values equal to x.",
            "x"_a)

        .def(
            "range",
            [](const PGMWrapper &p, int64_t a, int64_t b, std::pair<bool, bool> inclusive, bool reverse) {
                auto l_it = inclusive.first ? p.lower_bound(a) : p.upper_bound(a);
                auto r_it = inclusive.second ? p.lower_bound(b) : p.upper_bound(b);
                if (reverse)
                    return py::make_iterator(std::make_reverse_iterator(r_it), std::make_reverse_iterator(l_it));
                return py::make_iterator(l_it, r_it);
            },
            "a"_a,
            "b"_a,
            "inclusive"_a = std::make_pair(true, true),
            "reverse"_a = false,
            py::keep_alive<0, 1>())

        // list-like operations
        // FIXME: optional arguments
        .def(
            "index",
            [](const PGMWrapper &p, int64_t x, ssize_t *start, ssize_t *stop) -> py::object {
                auto it = p.lower_bound(x);
                auto index = (size_t) std::distance(p.begin(), it);

                size_t left, right, step, length;
                auto slice = py::slice(start ? *start : 0, stop ? *stop : ssize_t(p.size()), 1);
                slice.compute(p.size(), &left, &right, &step, &length);

                if (it >= p.end() || *it != x || index < left || index > right)
                    throw py::value_error(std::to_string(x) + " is not in PGMIndex");
                return py::cast(index);
            },
            "Return the first index of x. Raises ValueError if x is not present.",
            "x"_a,
            "start"_a = nullptr,
            "stop"_a = nullptr)

        // multiset operations
        .def(
            "__add__",
            [](const PGMWrapper &p, py::array_t<int64_t> a) {
                auto r = a.unchecked<1>();
                auto a_size = r.shape(0);
                auto a_begin = r.data(0);
                auto a_end = r.data(r.shape(0));

                auto out_size = p.size() + a_size;
                auto out = new int64_t[out_size];

                if (std::is_sorted(a_begin, a_end)) {
                    std::merge(p.begin(), p.end(), a_begin, a_end, out);
                    return new PGMWrapper(out, out_size);
                }

                auto tmp = new int64_t[a_size];
                std::copy(a_begin, a_end, tmp);
                std::sort(tmp, tmp + a_size);
                std::merge(p.begin(), p.end(), tmp, tmp + a_size, out);
                delete[] tmp;
                return new PGMWrapper(out, out_size);
            },
            "")

        .def(
            "__add__",
            [](const PGMWrapper &p, const PGMWrapper &q) {
                auto out_size = p.size() + q.size();
                auto out = new int64_t[out_size];
                std::merge(p.begin(), p.end(), q.begin(), q.end(), out);
                return new PGMWrapper(out, out_size);
            },
            "")

        .def("__sub__",
             [](const PGMWrapper &p, py::array_t<int64_t> a) {
                 auto r = a.unchecked<1>();
                 auto a_size = r.shape(0);
                 auto a_begin = r.data(0);
                 auto a_end = r.data(r.shape(0));

                 auto tmp_out = new int64_t[p.size()];
                 int64_t *tmp_out_end;

                 if (std::is_sorted(a_begin, a_end))
                     tmp_out_end = std::set_difference(p.begin(), p.end(), a_begin, a_end, tmp_out);
                 else {
                     auto tmp = new int64_t[a_size];
                     std::copy(a_begin, a_end, tmp);
                     std::sort(tmp, tmp + a_size);
                     tmp_out_end = std::set_difference(p.begin(), p.end(), tmp, tmp + a_size, tmp_out);
                     delete[] tmp;
                 }

                 auto out_size = (size_t) std::distance(tmp_out, tmp_out_end);
                 if (out_size == p.size())
                     return new PGMWrapper(tmp_out, out_size);

                 auto out = new int64_t[out_size];
                 std::copy_n(tmp_out, out_size, out);
                 delete[] tmp_out;
                 return new PGMWrapper(out, out_size);
             })

        .def(
            "__sub__",
            [](const PGMWrapper &p, const PGMWrapper &q) {
                auto tmp_out = new int64_t[p.size()];
                auto tmp_out_end = std::set_difference(p.begin(), p.end(), q.begin(), q.end(), tmp_out);
                auto out_size = (size_t) std::distance(tmp_out, tmp_out_end);

                if (out_size == p.size())
                    return new PGMWrapper(tmp_out, out_size);

                auto out = new int64_t[out_size];
                std::copy_n(tmp_out, out_size, out);
                delete[] tmp_out;
                return new PGMWrapper(out, out_size);
            },
            "")

        .def("drop_duplicates",
             [](const PGMWrapper &p) {
                 auto tmp = new int64_t[p.size()];
                 auto size = (size_t) std::distance(tmp, std::unique_copy(p.begin(), p.end(), tmp));

                 if (size == p.size())
                     return new PGMWrapper(tmp, size);

                 auto data = new int64_t[size];
                 std::copy_n(tmp, size, data);
                 delete[] tmp;
                 return new PGMWrapper(data, size);
             })

        // other methods
        .def("stats",
             &PGMWrapper::stats,
             "Return a dict containing stats about self, such as the occupied space in bytes.");
}