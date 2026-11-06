#ifndef slic3r_TPMS_hpp_
#define slic3r_TPMS_hpp_

#include <string>

#include "Point.hpp"
#include "TriangleMesh.hpp"

namespace Slic3r {
namespace tpms {

// Lattices from triply periodic minimal surface approximations: the solid is the part of space where a periodic
// function f(x, y, z) is at most a threshold, and the threshold is chosen so that a given fraction of the volume
// is solid. All lengths are in millimetres.
enum class Type {
    // f = cos x + cos y + cos z
    SchwarzP,
    // f = sin x sin y sin z + sin x cos y cos z + cos x sin y cos z + cos x cos y sin z
    Diamond,
};

struct Params
{
    Type   type { Type::SchwarzP };
    // The lattice fills the box from the origin to size.
    Vec3d  size { 30., 30., 30. };
    // Length of one period of the lattice in every direction.
    double cell { 5. };
    // Solid fraction of the volume, between 0.02 and 0.98. With density_end >= 0 it is the fraction at the start of
    // the axis, and the fraction goes linearly to density_end at the end of the axis.
    double density { 0.3 };
    double density_end { -1. };
    // 0 = x, 1 = y, 2 = z.
    int    axis { 2 };
    // Edge of the sampling grid. 0 = a twentieth of the cell.
    double voxel { 0. };
};

// The function of the lattice at a point given in radians of its period (2 pi is one cell).
double field(Type type, const Vec3d &phase);

// The threshold of the function below which a fraction of the volume lies, for fraction in (0, 1).
double threshold_for_fraction(Type type, double fraction);

// The solid fraction asked for at a distance along the axis from the origin of the box.
double local_density(const Params &params, double position);

// The solid fraction of the whole box. It is reached when every side of the box is a whole number of cells.
double expected_volume_fraction(const Params &params);

// A closed mesh of the solid, empty with an error message when the parameters do not allow one.
indexed_triangle_set make_lattice(const Params &params, std::string *error = nullptr);

} // namespace tpms
} // namespace Slic3r

#endif
