#ifndef slic3r_VesselPrintability_hpp_
#define slic3r_VesselPrintability_hpp_

#include <limits>
#include <string>
#include <vector>

#include <admesh/stl.h>

#include "libslic3r/Vessel/VesselTree.hpp"

namespace Slic3r { namespace vessel {

// Two channels, or a channel and the outside of the block, that are closer than the minimum feature size.
struct ThinWall
{
    int    branch_a = Branch::NONE;
    // The other branch, or Branch::NONE when the wall is between the channel and the surface of the block.
    int    branch_b = Branch::NONE;
    // Thickness of the wall in mm. Zero or negative means the two channels touch or overlap.
    double wall     = 0.;
};

struct PrintabilityReport
{
    static constexpr double NONE = std::numeric_limits<double>::infinity();

    // Smallest channel diameter in mm and the branch it belongs to.
    double min_channel_diameter = NONE;
    int    narrowest_branch     = Branch::NONE;
    // Thinnest wall between two channels that are not neighbours in the tree, in mm.
    double min_wall_between_channels = NONE;
    int    wall_branch_a        = Branch::NONE;
    int    wall_branch_b        = Branch::NONE;
    // Thinnest wall between a channel and the surface of the block, in mm. NONE if no block was given.
    double min_wall_to_surface  = NONE;
    int    surface_wall_branch  = Branch::NONE;

    // Branches whose diameter is below the minimum feature size.
    std::vector<int>      narrow_branches;
    // Walls thinner than the minimum feature size.
    std::vector<ThinWall> thin_walls;

    bool ok() const { return narrow_branches.empty() && thin_walls.empty(); }
};

// Checks a vessel tree that is subtracted from a block, so that the tree is the channel. The numbers refer to the channel
// walls (the material between the channels), all in mm.
// `inset` is the wall thickness of a hollow tree printed as it is: then the channels are the lumens, with the radius
// of every branch reduced by it. Use 0 for a solid tree subtracted from a block.
// Branches that touch in the tree (a branch and its parent, two siblings) are joined on purpose and are not compared.
// The wall to the surface of `block` is measured sideways: from points along the axis of each branch (at most half
// its radius apart) rays go out perpendicular to the axis in 48 directions, so a channel that opens through a face of the block
// is not reported as having a wall of zero there. Because only 48 directions are tried the wall can be
// overestimated by up to 0.3 percent of the distance from the axis to the surface. The tree is expected to be inside the block. The material in front of the
// closed end of a leaf is not measured.
PrintabilityReport check_channel_printability(const Skeleton &skeleton, double min_feature_size, double inset = 0.,
                                              const indexed_triangle_set *block = nullptr);

// The findings as sentences for the user, empty if everything is fine.
std::vector<std::string> printability_warnings(const PrintabilityReport &report, double min_feature_size);

// Smallest distance between two line segments.
double segment_distance(const Vec3d &a0, const Vec3d &a1, const Vec3d &b0, const Vec3d &b1);

}} // namespace Slic3r::vessel

#endif // slic3r_VesselPrintability_hpp_
