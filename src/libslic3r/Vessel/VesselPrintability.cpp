#include "VesselPrintability.hpp"

#include <algorithm>
#include <cmath>

#include "libslic3r/AABBTreeIndirect.hpp"
#include "libslic3r/Exception.hpp"
#include "LocalesUtils.hpp"

namespace Slic3r { namespace vessel {

double segment_distance(const Vec3d &p1, const Vec3d &q1, const Vec3d &p2, const Vec3d &q2)
{
    // Closest points of two segments (Ericson, Real-Time Collision Detection, 5.1.9).
    const Vec3d  d1 = q1 - p1, d2 = q2 - p2, r = p1 - p2;
    const double a = d1.dot(d1), e = d2.dot(d2), f = d2.dot(r);
    double       s = 0., t = 0.;
    constexpr double eps = 1e-12;
    if (a <= eps && e <= eps) {
        return r.norm();
    } else if (a <= eps) {
        t = std::clamp(f / e, 0., 1.);
    } else {
        const double c = d1.dot(r);
        if (e <= eps) {
            s = std::clamp(-c / a, 0., 1.);
        } else {
            const double b = d1.dot(d2), denom = a * e - b * b;
            s = denom > eps ? std::clamp((b * f - c * e) / denom, 0., 1.) : 0.;
            t = (b * s + f) / e;
            if (t < 0.) {
                t = 0.;
                s = std::clamp(-c / a, 0., 1.);
            } else if (t > 1.) {
                t = 1.;
                s = std::clamp((b - c) / a, 0., 1.);
            }
        }
    }
    return ((p1 + d1 * s) - (p2 + d2 * t)).norm();
}

PrintabilityReport check_channel_printability(const Skeleton &sk, double min_feature_size, double inset, const indexed_triangle_set *block)
{
    PrintabilityReport rep;
    const size_t       n = sk.branches.size();
    if (n == 0)
        return rep;
    if (!(min_feature_size >= 0.) || inset < 0.)
        throw InvalidArgument("The minimum feature size and the wall thickness cannot be negative.");

    std::vector<double> radius(n);
    for (size_t i = 0; i < n; ++i) {
        radius[i] = sk.branches[i].radius - inset;
        if (!(radius[i] > 0.))
            throw InvalidArgument("The wall thickness must be smaller than the radius of every branch.");
        const double diameter = 2. * radius[i];
        if (diameter < rep.min_channel_diameter) {
            rep.min_channel_diameter = diameter;
            rep.narrowest_branch     = int(i);
        }
        if (diameter < min_feature_size)
            rep.narrow_branches.push_back(int(i));
    }

    auto neighbours = [&](size_t i, size_t j) {
        const Branch &a = sk.branches[i], &b = sk.branches[j];
        return a.parent == int(j) || b.parent == int(i) || (a.parent != Branch::NONE && a.parent == b.parent);
    };
    for (size_t i = 0; i < n; ++i)
        for (size_t j = i + 1; j < n; ++j) {
            if (neighbours(i, j))
                continue;
            const Branch &a = sk.branches[i], &b = sk.branches[j];
            // Cheap rejection: bounding spheres of the two vessels that are farther apart than any wall that would matter.
            const double limit = std::max(min_feature_size, rep.min_wall_between_channels);
            const double apart = (0.5 * (a.start + a.end) - 0.5 * (b.start + b.end)).norm() - 0.5 * a.length() - 0.5 * b.length() - radius[i] - radius[j];
            if (std::isfinite(limit) && apart > limit)
                continue;
            const double wall = segment_distance(a.start, a.end, b.start, b.end) - radius[i] - radius[j];
            if (wall < rep.min_wall_between_channels) {
                rep.min_wall_between_channels = wall;
                rep.wall_branch_a = int(i);
                rep.wall_branch_b = int(j);
            }
            if (wall < min_feature_size)
                rep.thin_walls.push_back({int(i), int(j), wall});
        }

    if (block != nullptr && !block->indices.empty()) {
        const auto tree = AABBTreeIndirect::build_aabb_tree_over_indexed_triangle_set(block->vertices, block->indices);
        constexpr int DIRECTIONS = 48;
        for (size_t i = 0; i < n; ++i) {
            const Branch &b      = sk.branches[i];
            const double  length = b.length();
            const double  step   = std::max(0.5 * radius[i], 0.02);
            const int     count  = std::max(1, int(std::ceil(length / step)));
            const Vec3d   dir    = (b.end - b.start) / length;
            const Vec3d   u      = dir.cross(std::abs(dir.x()) < 0.9 ? Vec3d::UnitX() : Vec3d::UnitY()).normalized();
            const Vec3d   v      = dir.cross(u);
            double        thinnest = PrintabilityReport::NONE;
            for (int k = 0; k <= count; ++k) {
                const Vec3d p = b.start + dir * (length * double(k) / count);
                for (int d = 0; d < DIRECTIONS; ++d) {
                    // Sideways from the axis: the end of a channel that opens through a face is then not mistaken for a thin wall.
                    const double a = 2. * PI * (double(d) + 0.5) / DIRECTIONS;
                    igl::Hit     hit;
                    if (AABBTreeIndirect::intersect_ray_first_hit(block->vertices, block->indices, tree, p, Vec3d(u * std::cos(a) + v * std::sin(a)), hit))
                        thinnest = std::min(thinnest, double(hit.t) - radius[i]);
                }
            }
            if (thinnest < rep.min_wall_to_surface) {
                rep.min_wall_to_surface = thinnest;
                rep.surface_wall_branch = int(i);
            }
            if (thinnest < min_feature_size)
                rep.thin_walls.push_back({int(i), Branch::NONE, thinnest});
        }
    }
    return rep;
}

std::vector<std::string> printability_warnings(const PrintabilityReport &r, double min_feature_size)
{
    auto mm = [](double v) { return float_to_string_decimal_point(v, 2) + " mm"; };
    std::vector<std::string> out;
    if (!r.narrow_branches.empty())
        out.push_back(std::to_string(r.narrow_branches.size()) + " of the channels are narrower than the smallest feature your printer can make (" +
                      mm(min_feature_size) + "). The narrowest is " + mm(r.min_channel_diameter) + " across. Make the root thicker, use fewer branching levels or stop the branches at a larger radius.");
    size_t between = 0, surface = 0;
    for (const ThinWall &w : r.thin_walls)
        (w.branch_b == Branch::NONE ? surface : between)++;
    if (between > 0)
        out.push_back(std::to_string(between) + (between == 1 ? " wall between two channels is" : " walls between channels are") + " thinner than " +
                      mm(min_feature_size) + ". The thinnest is " + mm(std::max(r.min_wall_between_channels, 0.)) +
                      (r.min_wall_between_channels <= 0. ? " (channels touch or overlap)" : "") + ", between vessels " + std::to_string(r.wall_branch_a) + " and " +
                      std::to_string(r.wall_branch_b) + ". Use a smaller branching angle spread, shorter child vessels or a larger block.");
    if (surface > 0)
        out.push_back(std::to_string(surface) + (surface == 1 ? " channel comes" : " channels come") + " closer than " + mm(min_feature_size) +
                      " to the outside of the block. The thinnest wall is " + mm(std::max(r.min_wall_to_surface, 0.)) + ". Move the root inwards or shorten the branches.");
    return out;
}

}} // namespace Slic3r::vessel
