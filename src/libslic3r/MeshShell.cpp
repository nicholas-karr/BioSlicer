#include "MeshShell.hpp"

#include <algorithm>
#include <cmath>
#include <map>
#include <utility>
#include <vector>

#include "libslic3r/AABBTreeIndirect.hpp"
#include "libslic3r/BoundingBox.hpp"
#include "libslic3r/I18N.hpp"
#include "libslic3r/MeshBoolean.hpp"
#include "libslic3r/MeshPrimitives.hpp"
#include "libslic3r/OpenVDBUtils.hpp"

namespace Slic3r {

double mesh_offset_auto_voxel_size(const indexed_triangle_set &its, double distance_mm)
{
    const BoundingBoxf3 bb      = bounding_box(its);
    const double        extent  = bb.defined ? bb.size().maxCoeff() : 1.;
    return std::max(std::abs(distance_mm) / 8., extent / 500.);
}

namespace {

using Tree = AABBTreeIndirect::Tree<3, float>;

// Distance from p to the surface, and the closest point of the surface.
double surface_distance(const indexed_triangle_set &its, const Tree &tree, const Vec3d &p, Vec3d &closest)
{
    size_t idx = 0;
    return std::sqrt(AABBTreeIndirect::squared_distance_to_indexed_triangle_set(its.vertices, its.indices, tree, p, idx, closest));
}

// Moves p onto the exact offset surface: the point `dist` away from the surface along the line from its closest
// point. The closest point can change as p moves, so this is repeated a few times.
Vec3d project_to_offset(const indexed_triangle_set &its, const Tree &tree, Vec3d p, double dist)
{
    for (int i = 0; i < 6; ++i) {
        Vec3d        closest;
        const double r = surface_distance(its, tree, p, closest);
        if (!(r > 1e-6 * dist))
            break;
        const Vec3d next = closest + (p - closest) * (dist / r);
        const bool  done = (next - p).norm() < 1e-7 * dist;
        p                = next;
        if (done)
            break;
    }
    return p;
}

// Splits the edges whose midpoint, or the triangles whose centroid, lies farther from the exact offset surface
// than `tol`, until none does. The flat pieces of an offset meet at creases (concave edges outward, convex edges
// inward) that a chord cuts; splitting puts more vertices on the crease. At most one edge of a triangle is split
// in a pass, so every pass keeps the surface closed and consistently oriented.
void refine_creases(indexed_triangle_set &out, const indexed_triangle_set &its, const Tree &tree, double dist, double tol)
{
    auto deviation = [&](const Vec3d &p) {
        Vec3d closest;
        return std::abs(surface_distance(its, tree, p, closest) - dist);
    };
    auto vertex = [&](int i) { return out.vertices[i].cast<double>(); };
    const size_t max_faces = 8 * out.indices.size() + 1000;
    for (int pass = 0; pass < 24 && out.indices.size() < max_faces; ++pass) {
        // Worst edge of every triangle that needs a split, with how bad it is.
        std::vector<std::pair<double, std::pair<int, int>>> wanted;
        std::map<std::pair<int, int>, double>               edge_dev;
        for (const Vec3i &f : out.indices) {
            double best = 0.;
            std::pair<int, int> best_edge{-1, -1}, longest{-1, -1};
            double              longest_len = -1.;
            for (int k = 0; k < 3; ++k) {
                const int  a = f[k], b = f[(k + 1) % 3];
                const auto key = std::make_pair(std::min(a, b), std::max(a, b));
                auto       it  = edge_dev.find(key);
                if (it == edge_dev.end())
                    it = edge_dev.emplace(key, deviation(0.5 * (vertex(a) + vertex(b)))).first;
                if (it->second > tol && it->second > best) {
                    best      = it->second;
                    best_edge = key;
                }
                const double len = (vertex(a) - vertex(b)).squaredNorm();
                if (len > longest_len) {
                    longest_len = len;
                    longest     = key;
                }
            }
            if (best_edge.first < 0) {
                const double c = deviation((vertex(f[0]) + vertex(f[1]) + vertex(f[2])) / 3.);
                if (c > tol) {
                    best      = c;
                    best_edge = longest;
                }
            }
            if (best_edge.first >= 0)
                wanted.push_back({best, best_edge});
        }
        if (wanted.empty())
            break;
        std::sort(wanted.begin(), wanted.end(), [](const auto &l, const auto &r) { return l.first > r.first; });

        std::map<std::pair<int, int>, std::vector<size_t>> faces_of_edge;
        for (size_t i = 0; i < out.indices.size(); ++i)
            for (int k = 0; k < 3; ++k) {
                const int a = out.indices[i][k], b = out.indices[i][(k + 1) % 3];
                faces_of_edge[{std::min(a, b), std::max(a, b)}].push_back(i);
            }
        std::vector<char> used(out.indices.size(), 0);
        std::vector<Vec3i> added;
        for (const auto &w : wanted) {
            const std::pair<int, int> e = w.second;
            const std::vector<size_t> &faces = faces_of_edge[e];
            if (faces.size() != 2 || used[faces[0]] || used[faces[1]])
                continue;
            const Vec3d m = project_to_offset(its, tree, 0.5 * (vertex(e.first) + vertex(e.second)), dist);
            const int   n = int(out.vertices.size());
            out.vertices.emplace_back(m.cast<float>());
            for (size_t fi : faces) {
                used[fi]  = 1;
                Vec3i &f  = out.indices[fi];
                int    k  = 0;
                while (!((f[k] == e.first && f[(k + 1) % 3] == e.second) || (f[k] == e.second && f[(k + 1) % 3] == e.first)))
                    ++k;
                const int a = f[k], b = f[(k + 1) % 3], c = f[(k + 2) % 3];
                f = Vec3i(a, n, c);
                added.emplace_back(n, b, c);
            }
        }
        if (added.empty())
            break;
        out.indices.insert(out.indices.end(), added.begin(), added.end());
    }
}

} // namespace

indexed_triangle_set mesh_offset(const indexed_triangle_set &its, double distance_mm, double voxel_size_mm)
{
    if (its.indices.empty())
        return {};
    if (voxel_size_mm <= 0.)
        voxel_size_mm = mesh_offset_auto_voxel_size(its, distance_mm);

    const float scale = float(1. / voxel_size_mm);
    // The narrow band must reach the iso surface, plus a few voxels for the surface extraction.
    const float outside = float(std::max(distance_mm, 0.) * scale) + 3.f;
    const float inside  = float(std::max(-distance_mm, 0.) * scale) + 3.f;

    VoxelGridPtr grid = mesh_to_grid(its, MeshToGridParams{}.voxel_scale(scale).exterior_bandwidth(outside).interior_bandwidth(inside));
    if (!grid || is_grid_empty(*grid))
        return {};
    // Level set values are in voxel units, the extracted points are in mm.
    indexed_triangle_set out = grid_to_mesh(*grid, distance_mm * scale, 0.);
    if (out.indices.empty() || distance_mm == 0.)
        return out;

    // The voxel surface is within a fraction of a voxel of the offset. Put every vertex on the exact offset
    // surface, then refine where a triangle still cuts a crease.
    const double dist = std::abs(distance_mm);
    const Tree   tree = AABBTreeIndirect::build_aabb_tree_over_indexed_triangle_set(its.vertices, its.indices);
    for (Vec3f &v : out.vertices)
        v = project_to_offset(its, tree, v.cast<double>(), dist).cast<float>();
    refine_creases(out, its, tree, dist, 0.002 * dist);
    its_remove_degenerate_faces(out);
    its_compactify_vertices(out);
    return out;
}

bool find_shell_port(const indexed_triangle_set &its, const Vec3d &direction, double radius_mm, ShellPort &out)
{
    if (its.indices.empty() || direction.norm() < EPSILON)
        return false;
    const Vec3d         dir = direction.normalized();
    const BoundingBoxf3 bb  = bounding_box(its);
    const Vec3d         far = bb.center() + dir * bb.size().norm();

    const auto tree = AABBTreeIndirect::build_aabb_tree_over_indexed_triangle_set(its.vertices, its.indices);
    igl::Hit   hit;
    if (!AABBTreeIndirect::intersect_ray_first_hit(its.vertices, its.indices, tree, far, Vec3d(-dir), hit))
        return false;
    out.position  = far - dir * double(hit.t);
    out.direction = dir;
    out.radius_mm = radius_mm;
    return true;
}

indexed_triangle_set make_shell(const indexed_triangle_set &its, const ShellParams &params, std::string *error)
{
    auto fail = [error](const char *msg) {
        if (error)
            *error = msg;
        return indexed_triangle_set{};
    };
    if (params.wall_thickness_mm <= 0.)
        return fail(L("The wall thickness must be larger than 0 mm."));

    indexed_triangle_set cavity = mesh_offset(its, -params.wall_thickness_mm, params.voxel_size_mm);
    if (cavity.indices.empty())
        return fail(L("The wall is too thick for this shape: there is no room left for a cavity."));
    its_flip_triangles(cavity);

    indexed_triangle_set out = its;
    its_merge(out, cavity);

    // An opening is cut only where it reaches a cavity: through a part of the wall that stays solid it would only
    // drill a blind hole.
    if (!params.ports.empty()) {
        const auto cavity_tree = AABBTreeIndirect::build_aabb_tree_over_indexed_triangle_set(cavity.vertices, cavity.indices);
        for (size_t i = 0; i < params.ports.size(); ++i) {
            const ShellPort &port = params.ports[i];
            if (port.direction.norm() < EPSILON)
                continue;
            igl::Hit hit;
            if (!AABBTreeIndirect::intersect_ray_first_hit(cavity.vertices, cavity.indices, cavity_tree, port.position, Vec3d(-port.direction.normalized()), hit) ||
                double(hit.t) > 3. * params.wall_thickness_mm) {
                if (error)
                    *error = std::string(L("Opening")) + " " + std::to_string(i + 1) + " " +
                             L("does not reach the cavity: the wall is solid there. Move the opening or use a thinner wall.");
                return {};
            }
        }
    }

    if (!params.ports.empty()) {
        // The cutter starts inside the cavity side of the wall and ends beyond the outer surface.
        const double inward  = 3. * params.wall_thickness_mm;
        const double outward = 2. * params.wall_thickness_mm;
        indexed_triangle_set cutters;
        for (const ShellPort &port : params.ports) {
            if (port.radius_mm <= 0. || port.direction.norm() < EPSILON)
                return fail(L("A port needs a radius larger than 0 mm and a direction."));
            const Vec3d dir = port.direction.normalized();
            indexed_triangle_set cyl = make_primitive(PrimitiveType::Frustum,
                                                      {port.radius_mm, port.radius_mm, inward + outward, 48.});
            const Transform3d tr = Transform3d(Eigen::Translation3d(port.position - dir * inward) *
                                               Eigen::Quaterniond::FromTwoVectors(Vec3d::UnitZ(), dir));
            its_transform(cyl, tr);
            its_merge(cutters, cyl);
        }
        try {
            MeshBoolean::cgal::minus(out, cutters);
        } catch (const std::exception &) {
            return fail(L("The port holes could not be cut. Try a smaller radius or another position."));
        }
    }
    return out;
}

} // namespace Slic3r
