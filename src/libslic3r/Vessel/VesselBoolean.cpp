#include "VesselBoolean.hpp"

#include <algorithm>
#include <cmath>
#include <memory>
#include <optional>
#include <tuple>
#include <utility>

#include <CGAL/Exact_predicates_exact_constructions_kernel.h>
#include <CGAL/Exact_predicates_inexact_constructions_kernel.h>
#include <CGAL/Polygon_mesh_processing/corefinement.h>
#include <CGAL/Polygon_mesh_processing/self_intersections.h>
#include <CGAL/Surface_mesh.h>
#include <CGAL/boost/graph/Euler_operations.h>
#include <CGAL/boost/graph/copy_face_graph.h>

#include "libslic3r/Exception.hpp"
#include "libslic3r/Point.hpp"

namespace Slic3r { namespace vessel {

namespace {

namespace PMP = CGAL::Polygon_mesh_processing;
using Kernel  = CGAL::Exact_predicates_exact_constructions_kernel;
using Mesh    = CGAL::Surface_mesh<Kernel::Point_3>;
using MeshPtr = std::unique_ptr<Mesh>;
using FloatKernel = CGAL::Exact_predicates_inexact_constructions_kernel;
using FloatMesh   = CGAL::Surface_mesh<FloatKernel::Point_3>;

// Edges shorter than this (mm) left by the boolean operations are collapsed before the vertices are rounded to float.
// It is far below what a printer resolves and far above the float resolution of coordinates up to a metre.
constexpr double SLIVER_LENGTH = 1e-3;

MeshPtr to_cgal(const indexed_triangle_set &its)
{
    auto mesh = std::make_unique<Mesh>();
    std::vector<Mesh::Vertex_index> v;
    v.reserve(its.vertices.size());
    for (const stl_vertex &p : its.vertices)
        v.push_back(mesh->add_vertex(Kernel::Point_3(p.x(), p.y(), p.z())));
    for (const stl_triangle_vertex_indices &f : its.indices)
        if (mesh->add_face(v[f(0)], v[f(1)], v[f(2)]) == Mesh::null_face())
            throw RuntimeError("A vessel surface is not a closed, consistently oriented mesh.");
    return mesh;
}

// Rounds the exact result to doubles.
FloatMesh to_float_mesh(const Mesh &mesh)
{
    FloatMesh out;
    CGAL::copy_face_graph(mesh, out, CGAL::parameters::vertex_point_map(CGAL::make_cartesian_converter_property_map<FloatKernel::Point_3>(mesh.points())));
    return out;
}

indexed_triangle_set to_its(const FloatMesh &mesh)
{
    indexed_triangle_set its;
    its.vertices.reserve(mesh.number_of_vertices());
    std::vector<int> index(mesh.num_vertices() + mesh.number_of_removed_vertices(), -1);
    for (FloatMesh::Vertex_index v : mesh.vertices()) {
        const FloatKernel::Point_3 &p = mesh.point(v);
        index[size_t(v)]              = int(its.vertices.size());
        its.vertices.emplace_back(float(p.x()), float(p.y()), float(p.z()));
    }
    its.indices.reserve(mesh.number_of_faces());
    for (FloatMesh::Face_index f : mesh.faces()) {
        int k = 0;
        stl_triangle_vertex_indices tri(0, 0, 0);
        for (FloatMesh::Vertex_index v : mesh.vertices_around_face(mesh.halfedge(f)))
            if (k < 3)
                tri(k++) = index[size_t(v)];
        its.indices.emplace_back(tri);
    }
    return its;
}

using PointKey = std::tuple<double, double, double>;

PointKey key(const FloatKernel::Point_3 &p) { return {p.x(), p.y(), p.z()}; }

// Order of an edge that does not depend on how the mesh is stored, so that the same surface is always cleaned up the same way.
std::tuple<double, PointKey, PointKey> edge_key(const FloatMesh &mesh, FloatMesh::Halfedge_index h, double measure)
{
    PointKey a = key(mesh.point(mesh.source(h))), b = key(mesh.point(mesh.target(h)));
    if (b < a)
        std::swap(a, b);
    return {measure, a, b};
}

// Collapses edges shorter than `length` onto their midpoints, shortest first, as long as the surface stays a closed 2-manifold.
void collapse_short_edges(FloatMesh &mesh, double length)
{
    auto edge_length = [&](FloatMesh::Edge_index e) {
        const FloatMesh::Halfedge_index h = mesh.halfedge(e);
        return std::sqrt(CGAL::squared_distance(mesh.point(mesh.source(h)), mesh.point(mesh.target(h))));
    };
    for (bool changed = true; changed;) {
        changed = false;
        std::vector<std::pair<std::tuple<double, PointKey, PointKey>, FloatMesh::Edge_index>> candidates;
        for (FloatMesh::Edge_index e : mesh.edges())
            if (const double len = edge_length(e); len < length)
                candidates.emplace_back(edge_key(mesh, mesh.halfedge(e), len), e);
        std::sort(candidates.begin(), candidates.end(), [](const auto &l, const auto &r) { return l.first < r.first; });
        for (const auto &[k, e] : candidates) {
            if (mesh.is_removed(e) || edge_length(e) >= length || !CGAL::Euler::does_satisfy_link_condition(e, mesh))
                continue;
            const FloatMesh::Halfedge_index h   = mesh.halfedge(e);
            const FloatKernel::Point_3      mid = CGAL::midpoint(mesh.point(mesh.source(h)), mesh.point(mesh.target(h)));
            mesh.point(CGAL::Euler::collapse_edge(e, mesh)) = mid;
            changed = true;
        }
        mesh.collect_garbage();
    }
}

// Flips the longest edge of triangles flatter than `height` (a corner of almost 180 degrees), flattest first.
void flip_flat_triangles(FloatMesh &mesh, double height)
{
    // The longest edge of a face and the height of the opposite corner above it.
    auto flat_edge = [&](FloatMesh::Face_index f, double &h_opp) {
        FloatMesh::Halfedge_index longest    = mesh.halfedge(f);
        double                    longest_sq = 0.;
        for (FloatMesh::Halfedge_index h : CGAL::halfedges_around_face(mesh.halfedge(f), mesh)) {
            const double sq = CGAL::squared_distance(mesh.point(mesh.source(h)), mesh.point(mesh.target(h)));
            if (sq > longest_sq) {
                longest_sq = sq;
                longest    = h;
            }
        }
        h_opp = longest_sq == 0. ? 0. :
                                   std::sqrt(CGAL::squared_distance(mesh.point(mesh.target(mesh.next(longest))),
                                                                    FloatKernel::Line_3(mesh.point(mesh.source(longest)), mesh.point(mesh.target(longest)))));
        return longest;
    };
    // A flip can make a neighbouring triangle flat, so this repeats a few times.
    for (int round = 0; round < 8; ++round) {
        std::vector<std::pair<std::tuple<double, PointKey, PointKey>, FloatMesh::Face_index>> candidates;
        for (FloatMesh::Face_index f : mesh.faces()) {
            double                          h_opp;
            const FloatMesh::Halfedge_index h = flat_edge(f, h_opp);
            if (h_opp < height)
                candidates.emplace_back(edge_key(mesh, h, h_opp), f);
        }
        std::sort(candidates.begin(), candidates.end(), [](const auto &l, const auto &r) { return l.first < r.first; });
        bool changed = false;
        for (const auto &[k, f] : candidates) {
            double                          h_opp;
            const FloatMesh::Halfedge_index h = flat_edge(f, h_opp);
            // The flip must not join two corners that are already joined, and it must give two triangles that are not flat
            // and face the same way as the neighbour, else it would only move the problem.
            const FloatMesh::Vertex_index a = mesh.source(h), b = mesh.target(h);
            const FloatMesh::Vertex_index c = mesh.target(mesh.next(h)), d = mesh.target(mesh.next(mesh.opposite(h)));
            if (h_opp >= height || c == d || mesh.halfedge(c, d).is_valid())
                continue;
            const auto &pa = mesh.point(a), &pb = mesh.point(b), &pc = mesh.point(c), &pd = mesh.point(d);
            const FloatKernel::Vector_3 n_ref = CGAL::cross_product(pa - pb, pd - pb);
            const FloatKernel::Vector_3 n1    = CGAL::cross_product(pa - pc, pd - pc), n2 = CGAL::cross_product(pb - pd, pc - pd);
            auto not_flat = [&](const FloatKernel::Point_3 &p, const FloatKernel::Point_3 &q, const FloatKernel::Point_3 &r) {
                const double longest = std::sqrt(std::max({CGAL::squared_distance(p, q), CGAL::squared_distance(q, r), CGAL::squared_distance(r, p)}));
                return std::sqrt(CGAL::cross_product(q - p, r - p).squared_length()) >= height * longest;
            };
            if (n1 * n_ref <= 0. || n2 * n_ref <= 0. || !not_flat(pc, pa, pd) || !not_flat(pd, pb, pc))
                continue;
            CGAL::Euler::flip_edge(h, mesh);
            changed = true;
        }
        if (!changed)
            break;
    }
}

// Combines meshes pairwise so that the operands stay small.
MeshPtr union_all(const std::vector<indexed_triangle_set> &pieces)
{
    std::vector<MeshPtr> meshes;
    meshes.reserve(pieces.size());
    for (const indexed_triangle_set &p : pieces)
        meshes.emplace_back(to_cgal(p));
    while (meshes.size() > 1) {
        std::vector<MeshPtr> next;
        next.reserve((meshes.size() + 1) / 2);
        for (size_t i = 0; i < meshes.size(); i += 2) {
            if (i + 1 < meshes.size()) {
                auto result = std::make_unique<Mesh>();
                if (!PMP::corefine_and_compute_union(*meshes[i], *meshes[i + 1], *result))
                    throw RuntimeError("The vessel surfaces could not be combined.");
                meshes[i] = std::move(result);
            }
            next.emplace_back(std::move(meshes[i]));
        }
        meshes = std::move(next);
    }
    return std::move(meshes.front());
}

// True if a triangle has zero area once its corners are rounded to float.
bool has_zero_area_face(const indexed_triangle_set &its)
{
    for (const stl_triangle_vertex_indices &f : its.indices) {
        const Vec3d a = its.vertices[f(0)].cast<double>(), b = its.vertices[f(1)].cast<double>(), c = its.vertices[f(2)].cast<double>();
        if ((b - a).cross(c - a).norm() < 1e-12)
            return true;
    }
    return false;
}

} // namespace

indexed_triangle_set combine_meshes(const std::vector<indexed_triangle_set> &solids, const std::vector<indexed_triangle_set> &holes)
{
    if (solids.empty())
        return {};
    indexed_triangle_set its;
    try {
        MeshPtr result = union_all(solids);
        if (!holes.empty()) {
            MeshPtr lumen = union_all(holes);
            auto    cut   = std::make_unique<Mesh>();
            if (!PMP::corefine_and_compute_difference(*result, *lumen, *cut))
                throw RuntimeError("The vessel wall could not be cut open.");
            result = std::move(cut);
        }
        result->collect_garbage();

        // Where two surfaces cross at a shallow angle the cut leaves slivers: triangles with an edge a few nanometres
        // long, or a corner of almost 180 degrees. Rounded to float they would have zero area or cross their neighbours.
        FloatMesh mesh = to_float_mesh(*result);
        collapse_short_edges(mesh, SLIVER_LENGTH);
        flip_flat_triangles(mesh, SLIVER_LENGTH);
        its = to_its(mesh);
    } catch (const Slic3r::Exception &) {
        throw;
    } catch (const std::exception &e) {
        throw RuntimeError(std::string("The vessel surfaces could not be combined: ") + e.what());
    }
    if (has_zero_area_face(its))
        throw RuntimeError("The combined vessel surface has triangles of zero area.");
    FloatMesh check;
    {
        std::vector<FloatMesh::Vertex_index> v;
        for (const stl_vertex &p : its.vertices)
            v.push_back(check.add_vertex(FloatKernel::Point_3(p.x(), p.y(), p.z())));
        for (const stl_triangle_vertex_indices &f : its.indices)
            if (check.add_face(v[f(0)], v[f(1)], v[f(2)]) == FloatMesh::null_face())
                throw RuntimeError("The combined vessel surface is not a closed mesh.");
    }
    if (PMP::does_self_intersect(check))
        throw RuntimeError("The combined vessel surface intersects itself.");
    return its;
}

}} // namespace Slic3r::vessel
