#ifndef slic3r_VesselTree_hpp_
#define slic3r_VesselTree_hpp_

// Branching vessel trees: straight or curved tubes that meet in round junctions. A tree is grown by
// branching rules, grown to fill a closed mesh, or built from a table of centreline points.
// All lengths are in millimetres, angles in degrees unless a name says otherwise.

#include <cstdint>
#include <string>
#include <vector>

#include <admesh/stl.h>

#include "libslic3r/Point.hpp"
#include "libslic3r/Vessel/VesselTube.hpp"

namespace Slic3r { namespace vessel {

enum class RadiusRule {
    // Murray's law: parent^e = sum of the children^e with e = 3 by default.
    Murray,
    // Every child gets the parent radius times `radius_ratio`.
    FixedRatio,
};

enum class AngleRule {
    // Branching angles that minimise the power needed to pump the flow (Murray, 1926): each child pulls
    // along its axis with its radius squared, and the pulls balance the pull of the parent.
    MurrayOptimal,
    // `total_angle` between two children, shared in inverse proportion to the child radii.
    // With three children each child leaves at half of `total_angle` from the parent axis.
    Fixed,
};

struct BifurcationRule
{
    RadiusRule radius_rule     = RadiusRule::Murray;
    // Exponent e of Murray's law. 3 is the classic value for laminar flow.
    double     murray_exponent = 3.;
    // Share of the parent flow that goes into the first child, between 0 and 1 (Murray only).
    // The other children share the rest equally.
    double     flow_split      = 0.5;
    // Child radius divided by parent radius (FixedRatio only).
    double     radius_ratio    = 0.8;
    AngleRule  angle_rule      = AngleRule::MurrayOptimal;
    // Angle in degrees between two children (Fixed only).
    double     total_angle     = 75.;
};

// Empty string if the rule gives `children` (2 or 3) distinct child directions, otherwise a plain-language reason why not.
std::string validate_rule(const BifurcationRule &rule, int children);

// Radii and directions of the children of one junction.
struct JunctionGeometry
{
    std::vector<double> radii;
    // Angle in radians between the parent axis (extended past the junction) and each child axis.
    std::vector<double> angles;
    // Angle in radians around the parent axis; child 0 at 0, the others spread evenly.
    std::vector<double> azimuths;
};

// Child radii and angles for `children` vessels leaving a parent of the given radius (mm). Values out of range are
// clamped; use validate_rule() first to tell the user. Throws InvalidArgument unless `children` is 2 or 3.
JunctionGeometry compute_junction(double parent_radius, int children, const BifurcationRule &rule);

struct BifurcationGeometry
{
    double radius_a = 0.;
    double radius_b = 0.;
    // Angles in radians between the parent axis (extended past the junction) and each child axis.
    // The children lie on opposite sides of the parent axis.
    double angle_a  = 0.;
    double angle_b  = 0.;
};

// compute_junction() for two children.
BifurcationGeometry compute_bifurcation(double parent_radius, const BifurcationRule &rule);

// A vessel between two junctions, or between a junction and an end.
struct Branch
{
    static constexpr int NONE = -1;

    int              parent = NONE;
    std::vector<int> children;
    // Number of junctions between the root and this branch.
    int              depth  = 0;
    // Centreline with the radius at every point, at least two points. The first point is the junction at
    // the end of the parent, the last point the junction where the children start.
    std::vector<PathPoint> path;

    const Vec3d &start() const { return path.front().pos; }
    const Vec3d &end() const { return path.back().pos; }
    double       start_radius() const { return path.front().radius; }
    double       end_radius() const { return path.back().radius; }
    // Length of the polyline through the points.
    double       length() const { return polyline_length(path); }
    // Unit vectors along the first and the last piece of the centreline.
    Vec3d        start_direction() const { return (path[1].pos - path[0].pos).normalized(); }
    Vec3d        end_direction() const { return (path.back().pos - path[path.size() - 2].pos).normalized(); }
    bool         is_leaf() const { return children.empty(); }
};

// The branches of a tree. Branch 0 is the root and every branch comes after its parent.
struct Skeleton
{
    std::vector<Branch> branches;
    // Number of branches that were shortened or left out because they would leave the bounding mesh.
    size_t              clipped_by_bounds = 0;

    size_t leaf_count() const;
    size_t junction_count() const;
};

// Empty string if the skeleton is well formed: branch 0 is the root without a parent, every other branch names a
// parent with a smaller index and starts where it ends, radii are positive, every branch has a length, and no two
// children of a junction leave in the same direction. Otherwise a plain-language reason.
std::string validate_skeleton(const Skeleton &skeleton);

// The centreline of one branch for make_skeleton(): its points with their radii and the index of its parent.
struct BranchPath
{
    int                    parent = Branch::NONE;
    std::vector<PathPoint> points;
};

// A skeleton from centrelines, for example points measured on a scan. The first path is the root and children come
// after their parents. The first point of a child is moved onto the end of its parent when it is within 0.01 mm of
// it. Depths are computed. Throws InvalidArgument with a plain message when the paths do not form a tree.
Skeleton make_skeleton(const std::vector<BranchPath> &paths);

enum class GrowthMode {
    // Every vessel ends in a junction with `children` new vessels, `depth` times.
    Branching,
    // Vessels grow step by step towards points spread through a closed mesh until it is filled.
    Fill,
};

struct TreeParams
{
    GrowthMode mode           = GrowthMode::Branching;
    Vec3d      root_position  = Vec3d::Zero();
    Vec3d      root_direction = Vec3d::UnitZ();
    double     root_radius    = 2.;
    double     root_length    = 20.;
    // Number of junction levels. 0 gives the root branch alone.
    int        depth          = 3;
    // Length of a child branch divided by the length of its parent.
    double     length_ratio   = 0.8;
    // Number of vessels that leave each junction, 2 or 3.
    int        children       = 2;
    // Radius at the end of a vessel divided by the radius at its start, from 0.2 to 1 (no narrowing).
    double     taper          = 1.;
    BifurcationRule rule;
    // Random variation of each branching angle, +- this many degrees.
    double     angle_jitter   = 0.;
    // Random variation of each branch length, +- this fraction of the length.
    double     length_jitter  = 0.;
    // Branches thinner than this are not grown. 0 means no limit.
    double     min_radius     = 0.;
    uint32_t   seed           = 1;
    // Fill mode: length of each growth step, also the distance at which a target point counts as reached.
    double     fill_step      = 3.;
    // Fill mode: number of target points spread through the volume, roughly the number of vessel ends.
    int        fill_points    = 300;
    // Fill mode: smallest distance between a vessel wall and the surface of the mesh.
    double     fill_clearance = 0.5;
};

// Empty string if the parameters can grow a tree, otherwise a plain-language reason why not.
std::string validate_tree_params(const TreeParams &params);

// Grows a tree. Deterministic: the same parameters and seed always give the same skeleton.
// Branching mode: if `bounds` is given (a closed mesh), branches stop inside it: a branch that would
// leave the volume is shortened to end one radius before the surface, or left out if too short.
// Fill mode: `bounds` is required. Target points are spread through it, vessels grow towards them in
// steps of `fill_step`, radii follow Murray's law from the number of ends each vessel feeds, and no part of
// any vessel comes closer than `fill_clearance` to the surface (branches that would are cut off).
// The root position must be inside the bounds, else InvalidArgument is thrown.
Skeleton generate_tree(const TreeParams &params, const indexed_triangle_set *bounds = nullptr);

struct VesselMeshParams
{
    // Vertices per cross section.
    int      sides          = 32;
    // 0 for a solid tree, else wall thickness in mm of a hollow tree whose root and leaves are open.
    double   wall_thickness = 0.;
    // End of the root and of the leaves of a solid tree.
    CapStyle caps           = CapStyle::Round;
    int      cap_rings      = 6;
    // A junction is the round end of the parent vessel when every child is narrower than the parent by at least
    // this fraction (0.03 = 3 percent). Otherwise it is a sphere wider than the parent and its children by this
    // fraction. From 0.005 to 0.05.
    double   junction_margin = 0.03;
    // Curved vessels: interpolate the centreline points with a spline (true) or join them with straight pieces (false)
    bool     spline         = true;
    // and place a cross section every this many mm along a spline.
    double   sample_spacing = 0.5;
};

// Empty string if the skeleton can be meshed, otherwise a plain-language reason why not.
std::string validate_mesh_params(const Skeleton &skeleton, const VesselMeshParams &params);

// One watertight triangle mesh of the whole skeleton (union of all branches and junctions, minus the lumen for a
// hollow tree), free of zero-area triangles and self-intersections. Throws InvalidArgument if
// validate_mesh_params() fails and RuntimeError if the surfaces cannot be combined.
indexed_triangle_set make_tree_mesh(const Skeleton &skeleton, const VesselMeshParams &params);

// A single junction: a parent tube entering it and two or three child tubes leaving it.
struct BifurcationParams
{
    Vec3d  junction         = Vec3d::Zero();
    // Unit vector along the parent axis, pointing towards the junction.
    Vec3d  parent_direction = Vec3d::UnitZ();
    // The first child turns away from the parent axis towards this vector (made perpendicular to the axis);
    // the other children are spread evenly around the axis.
    Vec3d  plane_normal     = Vec3d::UnitY();
    double parent_radius    = 2.;
    double parent_length    = 10.;
    // Lengths of the children; a missing entry repeats the last one.
    std::vector<double> child_lengths = {10., 10.};
    int    children         = 2;
    BifurcationRule rule;
};

Skeleton             make_bifurcation_skeleton(const BifurcationParams &params);
indexed_triangle_set make_bifurcation(const BifurcationParams &params, const VesselMeshParams &mesh_params);

// The centreline of every branch as a table for experiments and other software: one row per centreline point,
// "branch,parent,depth,point,x_mm,y_mm,z_mm,radius_mm", with `point` counting the points of a branch from 0 at its
// start and `parent` -1 for the root. Numbers use a decimal point and read back as exactly the same values.
std::string skeleton_to_csv(const Skeleton &skeleton);

// Reads the table written by skeleton_to_csv() (or one made by hand in the same layout) back into a skeleton.
// Throws InvalidArgument with a plain message naming the first line that cannot be used.
Skeleton skeleton_from_csv(const std::string &csv);

// Everything needed to regenerate a vessel tree object.
struct VesselSpec
{
    TreeParams       tree;
    VesselMeshParams mesh;
    // Name of the object whose volume the tree must stay inside, empty for none.
    std::string      bounds_object;
    // Smallest feature in mm the printer can make, used to warn about thin channels and walls.
    double           min_feature_size = 0.4;
    // When not empty, the tree is this skeleton (for example imported points) and `tree` is not used to grow one.
    Skeleton         centreline;
};

// Plain text, one "key = value" per line, numbers with a decimal point.
std::string spec_to_text(const VesselSpec &spec);
// Returns false if the text holds no recognisable vessel parameters. Unknown keys are ignored,
// missing keys keep the defaults.
bool        spec_from_text(const std::string &text, VesselSpec &spec);

}} // namespace Slic3r::vessel

#endif // slic3r_VesselTree_hpp_
