#ifndef SLIC3R_TESTS_MESH_TEST_UTILS_HPP
#define SLIC3R_TESTS_MESH_TEST_UTILS_HPP

#include <map>
#include <utility>

#include "libslic3r/TriangleMesh.hpp"

namespace Slic3r { namespace test {

// Signed volume in mm^3 summed in double precision.
inline double volume_mm3(const indexed_triangle_set &its)
{
    double v = 0.;
    for (const Vec3i &f : its.indices) {
        const Vec3d a = its.vertices[f[0]].cast<double>(), b = its.vertices[f[1]].cast<double>(),
                    c = its.vertices[f[2]].cast<double>();
        v += a.dot(b.cross(c)) / 6.;
    }
    return v;
}

// True when every directed edge is used exactly once and its reverse exactly once, i.e. the
// surface is closed, consistently oriented and no edge is shared by more than two triangles.
inline bool is_closed_manifold(const indexed_triangle_set &its)
{
    std::map<std::pair<int, int>, int> edges;
    for (const Vec3i &f : its.indices)
        for (int k = 0; k < 3; ++k)
            ++edges[{f[k], f[(k + 1) % 3]}];
    for (const auto &e : edges) {
        if (e.second != 1)
            return false;
        auto rev = edges.find({e.first.second, e.first.first});
        if (rev == edges.end() || rev->second != 1)
            return false;
    }
    return !edges.empty();
}

}} // namespace Slic3r::test

#endif
