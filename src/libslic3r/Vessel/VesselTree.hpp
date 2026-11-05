#ifndef slic3r_VesselTree_hpp_
#define slic3r_VesselTree_hpp_

// Bifurcations and branching vessel trees built from straight tubes joined by
// ball-shaped junctions. All lengths are in millimetres, angles in degrees unless
// a name says otherwise.

#include <cstdint>
#include <string>
#include <vector>

#include <admesh/stl.h>

#include "libslic3r/Point.hpp"
#include "libslic3r/Vessel/VesselTube.hpp"

namespace Slic3r { namespace vessel {

enum class RadiusRule {
    // Murray's law: parent^e = childA^e + childB^e with e = 3 by default.
    Murray,
    // Both children get the parent radius times `radius_ratio`.
    FixedRatio,
};

enum class AngleRule {
    // Branching angles that minimise the power needed to pump the flow (Murray, 1926).
    MurrayOptimal,
    // `total_angle` between the two child axes, shared in inverse proportion to the child radii.
    Fixed,
};

struct BifurcationRule
{
    RadiusRule radius_rule     = RadiusRule::Murray;
    // Exponent e of Murray's law. 3 is the classic value for laminar flow.
    double     murray_exponent = 3.;
    // Share of the parent flow that goes into child A, between 0 and 1 (Murray only).
    double     flow_split      = 0.5;
    // Child radius divided by parent radius (FixedRatio only).
    double     radius_ratio    = 0.8;
    AngleRule  angle_rule      = AngleRule::MurrayOptimal;
    // Angle in degrees between the two child axes (Fixed only).
    double     total_angle     = 75.;
};

struct BifurcationGeometry
{
    double radius_a = 0.;
    double radius_b = 0.;
    // Angles in radians between the parent axis (extended past the junction) and each child axis.
    double angle_a  = 0.;
    double angle_b  = 0.;
};

// Child radii and branching angles for a parent of the given radius (mm).
BifurcationGeometry compute_bifurcation(double parent_radius, const BifurcationRule &rule);

// A straight vessel segment.
struct Branch
{
    static constexpr int NONE = -1;

    int    parent  = NONE;
    int    child_a = NONE;
    int    child_b = NONE;
    // Number of bifurcations between the root and this branch.
    int    depth   = 0;
    Vec3d  start   = Vec3d::Zero();
    Vec3d  end     = Vec3d::Zero();
    double radius  = 1.;

    double length() const { return (end - start).norm(); }
    bool   is_leaf() const { return child_a == NONE && child_b == NONE; }
};

// The branches of a tree. Branch 0 is the root. A branch ends in a ball shaped
// junction where its children start.
struct Skeleton
{
    std::vector<Branch> branches;
    // Number of branches that were shortened or left out because they would leave the bounding mesh.
    size_t              clipped_by_bounds = 0;

    size_t leaf_count() const;
};

struct TreeParams
{
    Vec3d    root_position  = Vec3d::Zero();
    Vec3d    root_direction = Vec3d::UnitZ();
    double   root_radius    = 2.;
    double   root_length    = 20.;
    // Number of bifurcation levels. 0 gives the root branch alone.
    int      depth          = 3;
    // Length of a child branch divided by the length of its parent.
    double   length_ratio   = 0.8;
    BifurcationRule rule;
    // Random variation of each branching angle, +- this many degrees.
    double   angle_jitter   = 0.;
    // Random variation of each branch length, +- this fraction of the length.
    double   length_jitter  = 0.;
    // Branches thinner than this are not grown. 0 means no limit.
    double   min_radius     = 0.;
    uint32_t seed           = 1;
};

// Grows a tree. Deterministic: the same parameters and seed always give the same skeleton.
// If `bounds` is given (a closed mesh), branches stop inside it: a branch that would
// leave the volume is shortened to end one radius before the surface, or left out
// if too short. The root position must be inside the bounds, else InvalidArgument is thrown.
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
    // Radius of the junction ball divided by the radius of the branch that enters it.
    double   joint_scale    = 1.1;
};

// Empty string if the skeleton can be meshed, otherwise a plain-language reason why not.
std::string validate_mesh_params(const Skeleton &skeleton, const VesselMeshParams &params);

// One watertight triangle mesh of the whole skeleton (union of all branches and junctions;
// minus the lumen for a hollow tree). Throws InvalidArgument if validate_mesh_params() fails.
indexed_triangle_set make_tree_mesh(const Skeleton &skeleton, const VesselMeshParams &params);

// A single bifurcation: a parent tube entering a junction and two child tubes leaving it.
struct BifurcationParams
{
    Vec3d  junction         = Vec3d::Zero();
    // Unit vector along the parent axis, pointing towards the junction.
    Vec3d  parent_direction = Vec3d::UnitZ();
    // The child axes lie in the plane perpendicular to this vector.
    Vec3d  plane_normal     = Vec3d::UnitY();
    double parent_radius    = 2.;
    double parent_length    = 10.;
    double child_length_a   = 10.;
    double child_length_b   = 10.;
    BifurcationRule rule;
};

Skeleton         make_bifurcation_skeleton(const BifurcationParams &params);
indexed_triangle_set make_bifurcation(const BifurcationParams &params, const VesselMeshParams &mesh_params);

// Everything needed to regenerate a vessel tree object.
struct VesselSpec
{
    TreeParams       tree;
    VesselMeshParams mesh;
    // Name of the object whose volume the tree must stay inside, empty for none.
    std::string      bounds_object;
};

// Plain text, one "key = value" per line, numbers with a decimal point.
std::string spec_to_text(const VesselSpec &spec);
// Returns false if the text holds no recognisable vessel parameters. Unknown keys are ignored,
// missing keys keep the defaults.
bool        spec_from_text(const std::string &text, VesselSpec &spec);

}} // namespace Slic3r::vessel

#endif // slic3r_VesselTree_hpp_
