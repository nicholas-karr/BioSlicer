#ifndef slic3r_MeshPrimitives_hpp_
#define slic3r_MeshPrimitives_hpp_

#include <string>
#include <vector>

#include "libslic3r/TriangleMesh.hpp"

namespace Slic3r {

// Parametric primitives for dimensionally exact design work. Every length is in
// millimetres. Vertices lie exactly on the ideal surface, so a mesh is always
// slightly smaller than the ideal solid; primitive_volume_tolerance() gives the
// relative bound that follows from the segment counts.
enum class PrimitiveType {
    Torus,      // major_radius, minor_radius, major_segments, minor_segments
    Ellipsoid,  // radius_x, radius_y, radius_z, segments, rings
    Capsule,    // radius, length, segments, rings (rings per hemisphere)
    Frustum,    // bottom_radius, top_radius, height, segments (cone when one radius is 0)
    Tube,       // outer_radius, inner_radius, height, segments
    Wedge,      // length, width, height_high, height_low (ramp when height_low is 0)
    Count
};

struct PrimitiveParamInfo
{
    const char *key;     // stable identifier
    const char *label;   // plain-language label shown to the user
    const char *unit;    // "mm" or "" for counts
    double      def;
    double      min;
    double      max;
    bool        integer;
};

const char                              *primitive_name(PrimitiveType type);
const std::vector<PrimitiveParamInfo>   &primitive_param_info(PrimitiveType type);
std::vector<double>                      primitive_default_params(PrimitiveType type);

// Empty string when params are usable, otherwise a plain-language reason.
std::string primitive_validate(PrimitiveType type, const std::vector<double> &params);

// Number of triangles make_primitive() produces for valid params.
size_t primitive_triangle_count(PrimitiveType type, const std::vector<double> &params);

// Larger meshes are refused by primitive_validate() so a typo cannot freeze the application.
constexpr size_t primitive_max_triangles = 2000000;

// Closed, outward-oriented mesh. Torus, Ellipsoid and Capsule are centered on the
// origin. Frustum, Tube and Wedge have their base on z = 0 and are centered in X/Y.
// Returns an empty mesh when primitive_validate() reports a problem.
indexed_triangle_set make_primitive(PrimitiveType type, const std::vector<double> &params);

// Volume of the ideal solid in mm^3.
double primitive_volume(PrimitiveType type, const std::vector<double> &params);

// Upper bound of (ideal volume - mesh volume) / ideal volume for the given segment counts.
// It contains a 1e-5 allowance for single precision vertex storage.
double primitive_volume_tolerance(PrimitiveType type, const std::vector<double> &params);

// Raises the segment and ring counts of params (rounded up to multiples of 4, so the extreme points lie
// on the axes and bounding boxes are exact) until primitive_volume_tolerance() is at most
// max_relative_loss, or the limits of the parameters are reached. Lengths are not changed.
// More segments mean a larger mesh: triangle count grows with the product of the counts.
std::vector<double> primitive_params_for_tolerance(PrimitiveType type, std::vector<double> params, double max_relative_loss);

} // namespace Slic3r

#endif // slic3r_MeshPrimitives_hpp_
