#include "MeshShell.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <map>
#include <set>
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
    return std::max(std::abs(distance_mm) / 4., extent / 500.);
}

namespace {

using Tree = AABBTreeIndirect::Tree<3, float>;

// Distance from p to the surface, and the closest point of the surface.
double surface_distance(const indexed_triangle_set &its, const Tree &tree, const Vec3d &p, Vec3d &closest)
{
    size_t idx = 0;
    return std::sqrt(AABBTreeIndirect::squared_distance_to_indexed_triangle_set(its.vertices, its.indices, tree, p, idx, closest));
}

// Moves p along the line p + t n onto the exact offset surface, the points `dist` away from the input. Moving along
// a fixed line, instead of to the closest point, keeps neighbouring vertices apart where the offset has a crease.
// `outward` is true when the offset surface lies outside the input, so the distance grows with t; inward it shrinks.
// `reach` bounds |t|. If the line does not cross the surface within it, p is returned unchanged and ok is false.
Vec3d project_to_offset(const indexed_triangle_set &its, const Tree &tree, const Vec3d &p, const Vec3d &n, double dist, bool outward, double reach,
                        bool &ok)
{
    Vec3d closest;
    auto  f = [&](double t) {
        const double g = surface_distance(its, tree, p + t * n, closest) - dist;
        return outward ? g : -g;
    };
    double lo = -reach, hi = reach, flo = f(lo), fhi = f(hi);
    ok = flo <= 0. && fhi >= 0.;
    if (!ok)
        return p;
    for (int i = 0; i < 40 && hi - lo > 1e-7 * dist; ++i) {
        const double mid = 0.5 * (lo + hi), fm = f(mid);
        if (fm < 0.) {
            lo = mid;
            flo = fm;
        } else {
            hi = mid;
            fhi = fm;
        }
    }
    // Between the two ends of the bracket, where the distance is closest to the target.
    const double t = std::abs(flo) < std::abs(fhi) ? lo : hi;
    return p + t * n;
}

// Moves p to the point `dist` away from its closest point of the input, along the line from that point. Used for
// the few vertices whose normal line misses the offset surface; it always lands on it but may slide along it.
Vec3d project_to_closest(const indexed_triangle_set &its, const Tree &tree, Vec3d p, double dist)
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
void refine_creases(indexed_triangle_set &out, std::vector<Vec3d> &normals, const indexed_triangle_set &its, const Tree &tree, double dist,
                    bool outward, double reach, double tol)
{
    auto deviation = [&](const Vec3d &p) {
        Vec3d closest;
        return std::abs(surface_distance(its, tree, p, closest) - dist);
    };
    auto vertex = [&](int i) { return out.vertices[i].cast<double>(); };
    const size_t max_faces = 3 * out.indices.size() + 1000;
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
            Vec3d mn = normals[e.first] + normals[e.second];
            mn       = mn.norm() > 1e-9 ? Vec3d(mn.normalized()) : normals[e.first];
            bool        ok = false;
            const Vec3d m  = project_to_offset(its, tree, 0.5 * (vertex(e.first) + vertex(e.second)), mn, dist, outward, reach, ok);
            if (!ok)
                continue;
            const int   n = int(out.vertices.size());
            out.vertices.emplace_back(m.cast<float>());
            normals.emplace_back(mn);
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

// Flips the longest edge of triangles that have no area or face against the surface normal. Vertices put on a
// crease line can leave three of them on one straight line, and a triangle across a sharp crease can fold over.
// A flip is kept only if both new triangles have area and face the way the surface does.
void flip_bad_triangles(indexed_triangle_set &out, const std::vector<Vec3d> &normals, double dist)
{
    const double min_area = 1e-12 * dist * dist;
    auto         vertex   = [&](int i) { return out.vertices[i].cast<double>(); };
    auto         normal   = [&](const Vec3i &f) { return Vec3d((vertex(f[1]) - vertex(f[0])).cross(vertex(f[2]) - vertex(f[0]))); };
    auto         facing   = [&](const Vec3i &f) { return normals[f[0]] + normals[f[1]] + normals[f[2]]; };
    auto         good     = [&](const Vec3i &f) {
        const Vec3d n = normal(f);
        return 0.5 * n.norm() > min_area && n.dot(facing(f)) > 0.;
    };
    for (int pass = 0; pass < 10; ++pass) {
        std::map<std::pair<int, int>, size_t> face_of_edge; // directed edge -> face
        for (size_t i = 0; i < out.indices.size(); ++i)
            for (int k = 0; k < 3; ++k)
                face_of_edge[{out.indices[i][k], out.indices[i][(k + 1) % 3]}] = i;
        bool changed = false;
        for (size_t ti = 0; ti < out.indices.size(); ++ti) {
            if (good(out.indices[ti]))
                continue;
            // Longest edge a -> b of t = (a, b, c).
            Vec3i  t = out.indices[ti];
            int    k = 0;
            double longest = -1.;
            for (int j = 0; j < 3; ++j)
                if (const double l = (vertex(t[(j + 1) % 3]) - vertex(t[j])).squaredNorm(); l > longest) {
                    longest = l;
                    k       = j;
                }
            const int a = t[k], b = t[(k + 1) % 3], c = t[(k + 2) % 3];
            auto      it = face_of_edge.find({b, a});
            if (it == face_of_edge.end())
                continue;
            const size_t ui = it->second;
            const Vec3i  u  = out.indices[ui];
            int          d  = -1;
            for (int j = 0; j < 3; ++j)
                if (u[j] != a && u[j] != b)
                    d = u[j];
            if (d < 0 || d == c || face_of_edge.count({c, d}) || face_of_edge.count({d, c}))
                continue;
            const Vec3i t2(a, d, c), u2(d, b, c);
            if (!good(t2) || !good(u2))
                continue;
            face_of_edge.erase({a, b});
            face_of_edge.erase({b, a});
            for (int j = 0; j < 3; ++j) {
                face_of_edge[{t2[j], t2[(j + 1) % 3]}] = ti;
                face_of_edge[{u2[j], u2[(j + 1) % 3]}] = ui;
            }
            out.indices[ti] = t2;
            out.indices[ui] = u2;
            changed         = true;
        }
        if (!changed)
            break;
    }
}

// Where two triangles still cross, two vertices a few microns apart have changed places. The shortest edge of the
// crossing triangles is collapsed to its midpoint, if that keeps the surface a closed manifold, until no triangles
// cross or nothing more can be collapsed. Collapsed faces are left with a repeated vertex for
// its_remove_degenerate_faces() to drop.
void untangle(indexed_triangle_set &out, std::vector<Vec3d> &normals, double dist)
{
    auto vertex = [&](int i) { return out.vertices[i].cast<double>(); };
    for (int round = 0; round < 20; ++round) {
        const std::vector<std::pair<size_t, size_t>> crossing = MeshBoolean::cgal::self_intersecting_faces(out);
        if (crossing.empty())
            return;
        std::vector<std::vector<size_t>> faces_of_vertex(out.vertices.size());
        for (size_t i = 0; i < out.indices.size(); ++i)
            for (int k = 0; k < 3; ++k)
                faces_of_vertex[out.indices[i][k]].push_back(i);
        std::vector<char> touched(out.indices.size(), 0);
        bool              changed = false;
        for (const auto &[f1, f2] : crossing) {
            if (touched[f1] || touched[f2])
                continue;
            // Edges of the two faces, shortest first; the first that can be collapsed is.
            std::vector<std::pair<double, std::pair<int, int>>> edges;
            for (size_t f : {f1, f2})
                for (int k = 0; k < 3; ++k) {
                    const int p = out.indices[f][k], q = out.indices[f][(k + 1) % 3];
                    edges.push_back({(vertex(p) - vertex(q)).norm(), {p, q}});
                }
            std::sort(edges.begin(), edges.end());
            for (const auto &[length, edge] : edges) {
                const int a = edge.first, b = edge.second;
                if (length > 0.05 * dist)
                    break;
                // Link condition: the edge has exactly two faces and a and b share no other neighbour.
                std::vector<size_t> shared;
                std::set<int>       around_a, around_b;
                bool                busy = false;
                for (size_t f : faces_of_vertex[a]) {
                    busy |= bool(touched[f]);
                    for (int k = 0; k < 3; ++k)
                        around_a.insert(out.indices[f][k]);
                    if (out.indices[f][0] == b || out.indices[f][1] == b || out.indices[f][2] == b)
                        shared.push_back(f);
                }
                for (size_t f : faces_of_vertex[b]) {
                    busy |= bool(touched[f]);
                    for (int k = 0; k < 3; ++k)
                        around_b.insert(out.indices[f][k]);
                }
                size_t common = 0;
                for (int v : around_a)
                    common += v != a && v != b && around_b.count(v);
                if (busy || shared.size() != 2 || common != 2)
                    continue;
                out.vertices[a] = (0.5 * (vertex(a) + vertex(b))).cast<float>();
                normals[a]      = (normals[a] + normals[b]).normalized();
                for (size_t f : faces_of_vertex[b]) {
                    for (int k = 0; k < 3; ++k)
                        if (out.indices[f][k] == b)
                            out.indices[f][k] = a;
                    touched[f] = 1;
                }
                for (size_t f : faces_of_vertex[a])
                    touched[f] = 1;
                changed = true;
                break;
            }
        }
        if (!changed)
            return;
        its_remove_degenerate_faces(out, false);
    }
}

} // namespace

indexed_triangle_set mesh_offset(const indexed_triangle_set &its, double distance_mm, double voxel_size_mm)
{
    if (its.indices.empty())
        return {};
    if (voxel_size_mm <= 0.)
        voxel_size_mm = mesh_offset_auto_voxel_size(its, distance_mm);
    const double dist    = std::abs(distance_mm);
    const bool   outward = distance_mm > 0.;
    const Tree   tree    = AABBTreeIndirect::build_aabb_tree_over_indexed_triangle_set(its.vertices, its.indices);

    // Where the offset leaves a feature thinner than a voxel, the voxel surface is too far from the exact one to be
    // moved onto it. Then the voxels are halved, up to three times, as long as that helps.
    struct Attempt
    {
        indexed_triangle_set its;
        std::vector<Vec3d>   normals;
        std::vector<char>    failed;
        size_t               n_failed = 0;
        double               reach    = 0.;
    };
    Attempt best;
    for (int attempt = 0; attempt < 4; ++attempt, voxel_size_mm *= 0.5) {
        const float scale = float(1. / voxel_size_mm);
        // The narrow band must reach the iso surface, plus a few voxels for the surface extraction.
        const float outside = float(std::max(distance_mm, 0.) * scale) + 3.f;
        const float inside  = float(std::max(-distance_mm, 0.) * scale) + 3.f;

        VoxelGridPtr grid = mesh_to_grid(its, MeshToGridParams{}.voxel_scale(scale).exterior_bandwidth(outside).interior_bandwidth(inside));
        if (!grid || is_grid_empty(*grid))
            return {};
        // Level set values are in voxel units, the extracted points are in mm.
        Attempt cur;
        cur.its = grid_to_mesh(*grid, distance_mm * scale, 0.);
        if (cur.its.indices.empty() || distance_mm == 0.)
            return cur.its;

        // Area weighted vertex normals of the voxel surface.
        cur.normals.assign(cur.its.vertices.size(), Vec3d::Zero());
        for (const Vec3i &f : cur.its.indices) {
            const Vec3d a = cur.its.vertices[f[0]].cast<double>(), b = cur.its.vertices[f[1]].cast<double>(), c = cur.its.vertices[f[2]].cast<double>();
            const Vec3d n = (b - a).cross(c - a);
            for (int k = 0; k < 3; ++k)
                cur.normals[f[k]] += n;
        }
        for (Vec3d &n : cur.normals)
            n = n.norm() > 0. ? n.normalized() : Vec3d::UnitZ();

        // Put every vertex on the exact offset surface.
        cur.reach = 2. * voxel_size_mm;
        cur.failed.assign(cur.its.vertices.size(), 0);
        for (size_t i = 0; i < cur.its.vertices.size(); ++i) {
            bool ok             = false;
            cur.its.vertices[i] = project_to_offset(its, tree, cur.its.vertices[i].cast<double>(), cur.normals[i], dist, outward, cur.reach, ok).cast<float>();
            cur.failed[i]       = !ok;
            cur.n_failed += !ok;
        }
        // While more than 1 % of the vertices cannot be placed, the voxels are too coarse for the shape. Below that,
        // halving is worth it only if it at least halves the share of vertices that could not be placed.
        const auto share  = [](const Attempt &x) { return double(x.n_failed) / double(std::max<size_t>(1, x.its.vertices.size())); };
        const bool better = best.its.indices.empty() || share(best) > 0.01 || share(cur) < 0.5 * share(best);
        if (!better)
            break;
        best = std::move(cur);
        if (best.n_failed <= std::max<size_t>(10, best.its.vertices.size() / 500))
            break;
    }

    indexed_triangle_set &out = best.its;
    for (size_t i = 0; i < out.vertices.size(); ++i)
        if (best.failed[i])
            out.vertices[i] = project_to_closest(its, tree, out.vertices[i].cast<double>(), dist).cast<float>();
    // Then refine where a triangle still cuts a crease, and repair what the crease left behind.
    refine_creases(out, best.normals, its, tree, dist, outward, best.reach, 0.002 * dist);
    flip_bad_triangles(out, best.normals, dist);
    untangle(out, best.normals, dist);
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
