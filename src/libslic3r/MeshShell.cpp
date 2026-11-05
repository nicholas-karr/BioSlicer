#include "MeshShell.hpp"

#include <algorithm>
#include <cmath>

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
    return grid_to_mesh(*grid, distance_mm * scale, 0.);
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
