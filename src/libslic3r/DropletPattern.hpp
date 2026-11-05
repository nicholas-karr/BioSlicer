#ifndef slic3r_DropletPattern_hpp_
#define slic3r_DropletPattern_hpp_

#include <vector>

#include "Droplet.hpp"
#include "ExPolygon.hpp"
#include "TriangleMesh.hpp"

namespace Slic3r {

// Patterns of droplet points and the walls to hold them, for the shape generator and the wall tool of the GUI.
// Lengths are in millimetres; ExPolygons are in scaled coordinates.

// A row-major grid of droplets starting at `origin`. The volume (or the count) steps by volume_step per row
// and by volume_step_col per column, which gives the sweeps of dose-response studies.
struct DropletGridParams
{
    Vec3d  origin { Vec3d::Zero() };
    int    cols { 1 };
    int    rows { 1 };
    double pitch_x { 1. };
    double pitch_y { 1. };
    double volume_nl { 50. };
    double volume_step_row { 0. };
    double volume_step_col { 0. };
    int    count { 1 };
    // Every other row runs backwards, so the nozzle does not travel back across the grid.
    bool   serpentine { false };
};
DropletSet make_droplet_grid(const DropletGridParams &params);

// `count` droplets evenly spread from `start` to `end`, both included.
DropletSet make_droplet_line(const Vec3d &start, const Vec3d &end, int count, double volume_nl, int drops);

// The footprint of a wall of the given thickness along a polyline, centred on it. A closed polyline gives a ring.
ExPolygons wall_footprint(const Points &polyline, bool closed, double thickness_mm);

// A closed mesh of the footprint extruded from z0 to z1 (mm).
indexed_triangle_set make_wall_mesh(const ExPolygons &footprint, double z0, double z1);

enum class WellShape { Round, Rectangular };

// A rows x cols array of wells, well (0, 0) centred on `origin`: a wall around every well and optionally a floor under
// all of them, with the droplets of every well. The mesh stands on z = 0.
struct WellArrayParams
{
    WellShape shape { WellShape::Round };
    int    cols { 3 };
    int    rows { 2 };
    double pitch_x { 10. };
    double pitch_y { 10. };
    // Diameter of a round well, size of a rectangular one.
    double inner_x { 6. };
    double inner_y { 6. };
    double wall_thickness { 0.8 };
    double wall_height { 3. };
    // 0 = no floor, the droplets then land on z = 0.
    double floor_thickness { 0.4 };
    // Droplets per well are laid out on an n x n grid inside the well; 1 = a single droplet in the centre.
    int    droplets_n { 1 };
    double volume_nl { 50. };
    int    count { 1 };
};
struct WellArray
{
    indexed_triangle_set mesh;
    // One set per well, in row-major order, named "A1", "A2", ... like a plate.
    DropletSets          droplets;
};
WellArray make_well_array(const WellArrayParams &params);

} // namespace Slic3r

#endif
