#include "VesselBoolean.hpp"

#include <memory>
#include <utility>

#include <CGAL/Exact_predicates_exact_constructions_kernel.h>
#include <CGAL/Polygon_mesh_processing/corefinement.h>
#include <CGAL/Surface_mesh.h>

#include "libslic3r/Exception.hpp"

namespace Slic3r { namespace vessel {

namespace {

namespace PMP = CGAL::Polygon_mesh_processing;
using Kernel  = CGAL::Exact_predicates_exact_constructions_kernel;
using Mesh    = CGAL::Surface_mesh<Kernel::Point_3>;
using MeshPtr = std::unique_ptr<Mesh>;

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

indexed_triangle_set to_its(const Mesh &mesh)
{
    indexed_triangle_set its;
    its.vertices.reserve(mesh.number_of_vertices());
    std::vector<int> index(mesh.num_vertices() + mesh.number_of_removed_vertices(), -1);
    for (Mesh::Vertex_index v : mesh.vertices()) {
        const Kernel::Point_3 &p = mesh.point(v);
        index[size_t(v)]         = int(its.vertices.size());
        its.vertices.emplace_back(float(CGAL::to_double(p.x())), float(CGAL::to_double(p.y())), float(CGAL::to_double(p.z())));
    }
    its.indices.reserve(mesh.number_of_faces());
    for (Mesh::Face_index f : mesh.faces()) {
        int k = 0;
        stl_triangle_vertex_indices tri(0, 0, 0);
        for (Mesh::Vertex_index v : mesh.vertices_around_face(mesh.halfedge(f)))
            if (k < 3)
                tri(k++) = index[size_t(v)];
        its.indices.emplace_back(tri);
    }
    return its;
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

} // namespace

indexed_triangle_set combine_meshes(const std::vector<indexed_triangle_set> &solids, const std::vector<indexed_triangle_set> &holes)
{
    if (solids.empty())
        return {};
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
        return to_its(*result);
    } catch (const Slic3r::Exception &) {
        throw;
    } catch (const std::exception &e) {
        throw RuntimeError(std::string("The vessel surfaces could not be combined: ") + e.what());
    }
}

}} // namespace Slic3r::vessel
