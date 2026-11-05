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

// Closed, outward-oriented mesh. Torus, Ellipsoid and Capsule are centered on the
// origin. Frustum, Tube and Wedge have their base on z = 0 and are centered in X/Y.
// Returns an empty mesh when primitive_validate() reports a problem.
indexed_triangle_set make_primitive(PrimitiveType type, const std::vector<double> &params);

// Volume of the ideal solid in mm^3.
double primitive_volume(PrimitiveType type, const std::vector<double> &params);

// Upper bound of (ideal volume - mesh volume) / ideal volume for the given segment counts.
// It contains a 1e-5 allowance for single precision vertex storage.
double primitive_volume_tolerance(PrimitiveType type, const std::vector<double> &params);

} // namespace Slic3r

#endif // slic3r_MeshPrimitives_hpp_
