#ifndef SLIC3R_TESTS_MESH_TEST_UTILS_HPP
#define SLIC3R_TESTS_MESH_TEST_UTILS_HPP

#include <cmath>
#include <map>
#include <set>
#include <utility>
#include <vector>

#include "libslic3r/AABBTreeIndirect.hpp"
#include "libslic3r/MeshBoolean.hpp"
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

// Surface area in mm^2 summed in double precision.
inline double area_mm2(const indexed_triangle_set &its)
{
    double a = 0.;
    for (const Vec3i &f : its.indices) {
        const Vec3d p = its.vertices[f[0]].cast<double>(), q = its.vertices[f[1]].cast<double>(),
                    r = its.vertices[f[2]].cast<double>();
        a += 0.5 * (q - p).cross(r - p).norm();
    }
    return a;
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

// Euler characteristic V - E + F over the referenced vertices. A closed surface made of sphere-like
// patches has 2 per patch, a torus-like patch contributes 0.
inline long euler_characteristic(const indexed_triangle_set &its)
{
    std::vector<char> used(its.vertices.size(), 0);
    std::set<std::pair<int, int>> edges;
    for (const Vec3i &f : its.indices)
        for (int k = 0; k < 3; ++k) {
            used[f[k]] = 1;
            const int a = f[k], b = f[(k + 1) % 3];
            edges.insert({std::min(a, b), std::max(a, b)});
        }
    long v = 0;
    for (char u : used)
        v += u;
    return v - long(edges.size()) + long(its.indices.size());
}

// True when every coordinate is finite and every triangle has a positive area.
inline bool has_no_degenerate_triangles(const indexed_triangle_set &its)
{
    for (const Vec3f &v : its.vertices)
        if (!v.allFinite())
            return false;
    for (const Vec3i &f : its.indices) {
        const Vec3d p = its.vertices[f[0]].cast<double>(), q = its.vertices[f[1]].cast<double>(),
                    r = its.vertices[f[2]].cast<double>();
        if ((q - p).cross(r - p).norm() <= 0.)
            return false;
    }
    return true;
}

// Integrated mean curvature of a closed polyhedron: the sum over edges of edge length times the
// exterior dihedral angle, halved. For a convex solid the outward offset by d has the volume
// V + A d + M d^2 + 4/3 pi d^3 (Steiner formula), and a smooth convex solid eroded by d smaller than
// its smallest curvature radius has V - A d + M d^2 - 4/3 pi d^3.
inline double mean_curvature_integral(const indexed_triangle_set &its)
{
    std::map<std::pair<int, int>, Vec3d> normal_of_edge;
    double m = 0.;
    for (const Vec3i &f : its.indices) {
        const Vec3d p = its.vertices[f[0]].cast<double>(), q = its.vertices[f[1]].cast<double>(),
                    r = its.vertices[f[2]].cast<double>();
        const Vec3d n = (q - p).cross(r - p).normalized();
        for (int k = 0; k < 3; ++k) {
            const int a = f[k], b = f[(k + 1) % 3];
            auto      it = normal_of_edge.find({b, a});
            if (it == normal_of_edge.end()) {
                normal_of_edge[{a, b}] = n;
                continue;
            }
            // The edge a -> b of this face is b -> a of the other face. The exterior dihedral angle is
            // positive when the solid is convex along the edge.
            const Vec3d &n_other = it->second;
            const Vec3d  edge    = (its.vertices[b] - its.vertices[a]).cast<double>();
            const double angle   = std::atan2(n_other.cross(n).dot(edge.normalized()), n_other.dot(n));
            m += 0.5 * edge.norm() * angle;
            normal_of_edge.erase(it);
        }
    }
    return m;
}

inline double steiner_volume(const indexed_triangle_set &its, double d)
{
    return volume_mm3(its) + area_mm2(its) * d + mean_curvature_integral(its) * d * d + 4. / 3. * M_PI * d * d * d;
}

// True when CGAL finds two triangles of the surface that intersect.
inline bool self_intersects(const indexed_triangle_set &its)
{
    return MeshBoolean::cgal::does_self_intersect(TriangleMesh(its));
}

using Tree = AABBTreeIndirect::Tree<3, float>;

inline Tree build_tree(const indexed_triangle_set &its)
{
    return AABBTreeIndirect::build_aabb_tree_over_indexed_triangle_set(its.vertices, its.indices);
}

// Distance from p to the surface in mm.
inline double distance_to(const indexed_triangle_set &its, const Tree &tree, const Vec3d &p)
{
    size_t idx;
    Vec3d  closest;
    return std::sqrt(AABBTreeIndirect::squared_distance_to_indexed_triangle_set(its.vertices, its.indices, tree, p, idx, closest));
}

// Distance along the ray to the first triangle it meets, negative when it meets none.
inline double ray_distance(const indexed_triangle_set &its, const Tree &tree, const Vec3d &origin, const Vec3d &dir)
{
    igl::Hit hit;
    if (!AABBTreeIndirect::intersect_ray_first_hit(its.vertices, its.indices, tree, origin, dir, hit))
        return -1.;
    return double(hit.t);
}

// Accuracy of an offset surface, relative to the requested distance |d|.
struct OffsetMeasurement
{
    size_t rays           = 0;  // sample points of the input whose exact offset point lies on the exact offset surface
    size_t misses         = 0;  // rays that did not meet the output
    double max_ray_error  = 0.; // largest |measured - |d|| / |d| over the rays that hit
    double max_deviation  = 0.; // largest |distance to input - |d|| / |d| over vertices, edge midpoints and centroids of the output
    double mean_deviation = 0.; // mean of the same
};

// Rays start on the input surface (centre and three off-centre points of every triangle) and travel
// along the normal, outward for d > 0 and inward for d < 0. A ray is measured only when the exact offset
// surface is at distance |d| along it, i.e. when no other part of the input is closer to the exact offset
// point; this excludes points next to concave corners (outward) and thin regions (inward) where the exact
// offset is farther or absent. The deviation checks the output from the other side: every point of an
// exact offset surface is at distance |d| from the input.
inline OffsetMeasurement measure_offset(const indexed_triangle_set &input, const indexed_triangle_set &output, double d)
{
    OffsetMeasurement m;
    const double      dist = std::abs(d), sign = d < 0. ? -1. : 1.;
    const Tree        tree_in = build_tree(input), tree_out = build_tree(output);

    static const Vec3d bary[4] = {Vec3d(1. / 3., 1. / 3., 1. / 3.), Vec3d(0.6, 0.2, 0.2), Vec3d(0.2, 0.6, 0.2), Vec3d(0.2, 0.2, 0.6)};
    for (const Vec3i &f : input.indices) {
        const Vec3d a = input.vertices[f[0]].cast<double>(), b = input.vertices[f[1]].cast<double>(),
                    c = input.vertices[f[2]].cast<double>();
        Vec3d n = (b - a).cross(c - a);
        if (n.norm() <= 0.)
            continue;
        n.normalize();
        for (const Vec3d &w : bary) {
            const Vec3d p = w[0] * a + w[1] * b + w[2] * c;
            const Vec3d e = p + sign * dist * n;
            if (distance_to(input, tree_in, e) < dist * (1. - 2e-3))
                continue;
            ++m.rays;
            const double t = ray_distance(output, tree_out, p, Vec3d(sign * n));
            if (t < 0.) {
                ++m.misses;
                continue;
            }
            m.max_ray_error = std::max(m.max_ray_error, std::abs(t - dist) / dist);
        }
    }

    size_t count = 0;
    auto   sample = [&](const Vec3d &q) {
        const double dev = std::abs(distance_to(input, tree_in, q) - dist) / dist;
        m.max_deviation = std::max(m.max_deviation, dev);
        m.mean_deviation += dev;
        ++count;
    };
    for (const Vec3f &v : output.vertices)
        sample(v.cast<double>());
    for (const Vec3i &f : output.indices) {
        const Vec3d a = output.vertices[f[0]].cast<double>(), b = output.vertices[f[1]].cast<double>(),
                    c = output.vertices[f[2]].cast<double>();
        sample((a + b + c) / 3.);
        // Every undirected edge of a closed manifold appears once with ascending indices.
        if (f[0] < f[1]) sample(0.5 * (a + b));
        if (f[1] < f[2]) sample(0.5 * (b + c));
        if (f[2] < f[0]) sample(0.5 * (c + a));
    }
    if (count > 0)
        m.mean_deviation /= double(count);
    return m;
}

}} // namespace Slic3r::test

#endif
