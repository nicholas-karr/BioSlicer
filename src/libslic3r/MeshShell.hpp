#ifndef slic3r_MeshShell_hpp_
#define slic3r_MeshShell_hpp_

#include <string>
#include <vector>

#include "libslic3r/Point.hpp"
#include "libslic3r/TriangleMesh.hpp"

namespace Slic3r {

// Offset the surface of a closed mesh by distance_mm. Positive grows the solid, negative shrinks it.
// The result is a new closed mesh. It starts from a voxel surface (voxel_size_mm of 0 picks a size from
// the distance and the mesh extent, halved where the voxels are too coarse for the shape), whose
// vertices are then moved onto the exact offset surface. An empty mesh is returned when the offset
// removes the whole solid or the input cannot be voxelised.
indexed_triangle_set mesh_offset(const indexed_triangle_set &its, double distance_mm, double voxel_size_mm = 0.);

// Voxel size used by mesh_offset() and make_shell() when none is given.
double mesh_offset_auto_voxel_size(const indexed_triangle_set &its, double distance_mm);

// A circular opening through the wall. The axis passes through position (a point on the outer
// surface) along direction (pointing out of the solid).
struct ShellPort
{
    Vec3d  position  = Vec3d::Zero();
    Vec3d  direction = Vec3d::UnitZ();
    double radius_mm = 1.;
};

struct ShellParams
{
    double                wall_thickness_mm = 1.;
    std::vector<ShellPort> ports;          // none: closed cavity, otherwise open cavity
    double                voxel_size_mm     = 0.;
};

// Hollow a closed mesh: the outer surface is kept exactly, a cavity is created
// wall_thickness_mm below it and ports are drilled through the wall. On failure the returned
// mesh is empty and error (when given) holds a plain-language reason.
indexed_triangle_set make_shell(const indexed_triangle_set &its, const ShellParams &params, std::string *error = nullptr);

// Port on the outer surface where a ray travelling against direction through the centre of the
// bounding box first enters the mesh. Returns false when the ray misses the mesh.
bool find_shell_port(const indexed_triangle_set &its, const Vec3d &direction, double radius_mm, ShellPort &out);

} // namespace Slic3r

#endif // slic3r_MeshShell_hpp_
