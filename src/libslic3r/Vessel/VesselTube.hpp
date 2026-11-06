#ifndef slic3r_VesselTube_hpp_
#define slic3r_VesselTube_hpp_

// Tubes swept along a centreline, used to model blood vessels, channels and other
// round or elliptical lumens. All lengths are in millimetres.

#include <string>
#include <vector>

#include <admesh/stl.h>

#include "libslic3r/Point.hpp"

namespace Slic3r { namespace vessel {

// One control point of a centreline. The radius is the semi-axis of the cross
// section in mm; for an elliptical section it is the major semi-axis.
struct PathPoint
{
    Vec3d  pos    = Vec3d::Zero();
    double radius = 1.;
};

enum class CapStyle {
    // The tube ends in a flat disc at the end point.
    Flat,
    // The tube ends in a half ellipsoid that reaches one radius beyond the end point.
    Round,
};

struct TubeParams
{
    // Interpolate the control points with a centripetal Catmull-Rom spline, sampled every `sample_spacing` mm.
    // When false the control points are joined by straight pieces that meet in exact mitre joints, and only the
    // control points carry a cross section.
    bool     spline         = true;
    // Maximum distance in mm between neighbouring cross sections along a spline centreline.
    double   sample_spacing = 0.5;
    // Number of vertices of each cross section.
    int      sides          = 48;
    // Rotation of the cross section vertices around the centreline, in radians. The pieces of a tree are given
    // different offsets so that no two pieces share a vertex.
    double   ring_offset    = 0.;
    // Minor semi-axis divided by the major semi-axis, 1 for a round section.
    double   aspect         = 1.;
    // Wall thickness in mm. Zero makes a solid tube. A positive value makes a
    // hollow tube with open ends, the cap styles are ignored.
    double   wall_thickness = 0.;
    CapStyle cap_start      = CapStyle::Round;
    CapStyle cap_end        = CapStyle::Round;
    // Number of rings between the tube end and the apex of a round cap.
    int      cap_rings      = 8;
};

// Centreline samples: the control points (or the spline through them) resampled
// so that the arc length between neighbours is at most `spacing`.
// The radius is interpolated linearly along the arc length between control points.
std::vector<PathPoint> sample_centreline(const std::vector<PathPoint> &control, bool spline, double spacing);

// Length of the polyline through the points, in mm.
double polyline_length(const std::vector<PathPoint> &points);

// Empty string if the tube can be built, otherwise a plain-language reason why not.
// A spline path is rejected when it bends more sharply than its radius allows, a path of straight pieces when
// two mitre joints would cut into each other; both would make the surface fold into itself.
std::string validate_tube(const std::vector<PathPoint> &control, const TubeParams &params);

// Triangle mesh of the tube: watertight, outward facing triangles (counter-clockwise seen from outside).
// A single control point makes a ball (round caps on both sides). Throws InvalidArgument if validate_tube() fails.
indexed_triangle_set make_tube(const std::vector<PathPoint> &control, const TubeParams &params);

// Result of checking a triangle mesh for use as a printable solid.
struct MeshCheck
{
    size_t open_edges        = 0; // directed edges without an opposite edge
    size_t nonmanifold_edges = 0; // directed edges used by more than one triangle
    size_t degenerate_faces  = 0; // triangles with zero area
    double volume            = 0.; // signed volume in mm^3, positive for outward facing triangles
    bool   watertight() const { return open_edges == 0 && nonmanifold_edges == 0; }
};

MeshCheck check_mesh(const indexed_triangle_set &its);

// Signed volume in mm^3 computed in double precision.
double mesh_volume(const indexed_triangle_set &its);

}} // namespace Slic3r::vessel

#endif // slic3r_VesselTube_hpp_
