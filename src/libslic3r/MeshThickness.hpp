#ifndef slic3r_MeshThickness_hpp_
#define slic3r_MeshThickness_hpp_

#include <cstddef>
#include <limits>

#include "libslic3r/Point.hpp"
#include "libslic3r/TriangleMesh.hpp"

namespace Slic3r {

struct WallThicknessResult
{
    bool   valid = false;                                  // false when the mesh has no usable surface
    double min_thickness_mm = std::numeric_limits<double>::infinity();
    Vec3d  min_location     = Vec3d::Zero();               // point on the surface where the minimum was measured
    Vec3d  min_opposite     = Vec3d::Zero();               // point on the opposite wall
    size_t samples          = 0;                           // surface points measured
    size_t samples_below    = 0;                           // measured points thinner than the threshold
};

// Wall thickness by casting a ray from each sample point into the material, against the surface
// normal, and measuring the distance to the first surface it meets. Samples are the centre and three
// off-centre points of every triangle. The minimum is therefore exact for flat walls and
// overestimates by at most the sampling spacing on strongly curved ones. Points whose ray leaves the
// mesh without a hit (open meshes) are skipped. threshold_mm only drives samples_below.
WallThicknessResult analyze_wall_thickness(const indexed_triangle_set &its, double threshold_mm = 0.);

} // namespace Slic3r

#endif // slic3r_MeshThickness_hpp_
