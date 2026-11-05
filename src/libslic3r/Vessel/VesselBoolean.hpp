#ifndef slic3r_VesselBoolean_hpp_
#define slic3r_VesselBoolean_hpp_

#include <vector>

#include <admesh/stl.h>

namespace Slic3r { namespace vessel {

// Union of closed, outward facing triangle meshes, minus the union of the `holes`.
// Every step is done in exact arithmetic and only the final vertices are rounded to float,
// so a long chain of operations cannot leave the intermediate surfaces slightly self-intersecting.
// Slivers shorter than a micron are collapsed before rounding. Throws RuntimeError with a plain message if the
// surfaces cannot be combined, or if the rounded result has a zero-area triangle or intersects itself.
indexed_triangle_set combine_meshes(const std::vector<indexed_triangle_set> &solids, const std::vector<indexed_triangle_set> &holes);

}} // namespace Slic3r::vessel

#endif // slic3r_VesselBoolean_hpp_
