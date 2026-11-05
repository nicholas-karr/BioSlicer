#include "MeshThickness.hpp"

#include <tbb/blocked_range.h>
#include <tbb/parallel_reduce.h>

#include "libslic3r/AABBTreeIndirect.hpp"

namespace Slic3r {

WallThicknessResult analyze_wall_thickness(const indexed_triangle_set &its, double threshold_mm)
{
    if (its.indices.empty())
        return {};

    const auto tree = AABBTreeIndirect::build_aabb_tree_over_indexed_triangle_set(its.vertices, its.indices);

    static const Vec3d bary[4] = {Vec3d(1. / 3., 1. / 3., 1. / 3.), Vec3d(0.8, 0.1, 0.1), Vec3d(0.1, 0.8, 0.1), Vec3d(0.1, 0.1, 0.8)};

    auto measure = [&](const tbb::blocked_range<size_t> &range, WallThicknessResult acc) {
        for (size_t fi = range.begin(); fi < range.end(); ++fi) {
            const Vec3i  &f = its.indices[fi];
            const Vec3d   a = its.vertices[f[0]].cast<double>(), b = its.vertices[f[1]].cast<double>(),
                          c = its.vertices[f[2]].cast<double>();
            Vec3d         n = (b - a).cross(c - a);
            if (n.norm() < 1e-12)
                continue;
            n.normalize();
            for (const Vec3d &w : bary) {
                const Vec3d p = w[0] * a + w[1] * b + w[2] * c;
                // Start slightly inside so the ray cannot hit its own triangle.
                const Vec3d origin = p - n * 1e-4;
                igl::Hit    hit;
                if (!AABBTreeIndirect::intersect_ray_first_hit(its.vertices, its.indices, tree, origin, Vec3d(-n), hit))
                    continue;
                const double t = double(hit.t) + 1e-4;
                ++acc.samples;
                if (t < threshold_mm)
                    ++acc.samples_below;
                if (t < acc.min_thickness_mm) {
                    acc.min_thickness_mm = t;
                    acc.min_location     = p;
                    acc.min_opposite     = p - n * t;
                }
            }
        }
        return acc;
    };
    auto join = [](const WallThicknessResult &x, const WallThicknessResult &y) {
        WallThicknessResult r = x.min_thickness_mm <= y.min_thickness_mm ? x : y;
        r.samples       = x.samples + y.samples;
        r.samples_below = x.samples_below + y.samples_below;
        return r;
    };

    WallThicknessResult res = tbb::parallel_reduce(tbb::blocked_range<size_t>(0, its.indices.size()), WallThicknessResult{}, measure, join);
    res.valid = res.samples > 0;
    return res;
}

} // namespace Slic3r
