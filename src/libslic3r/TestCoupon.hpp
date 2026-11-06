#ifndef slic3r_TestCoupon_hpp_
#define slic3r_TestCoupon_hpp_

#include <string>
#include <vector>

#include "Droplet.hpp"
#include "TriangleMesh.hpp"

namespace Slic3r {

// Parametric test coupons for calibrating a printer and a material. All lengths are in millimetres and all angles
// in degrees. A coupon stands on z = 0 with its minimum corner at x = 0, y = 0. Round features are regular polygons
// of kCouponCircleSegments sides inscribed in the nominal circle.
constexpr int kCouponCircleSegments = 64;

struct Coupon
{
    // Empty when the parameters cannot make a coupon; error then says why.
    indexed_triangle_set mesh;
    std::string          error;
    // The stepped feature sizes, in the order along the coupon, and their name and unit.
    std::string          parameter;
    std::string          unit;
    std::vector<double>  values;
    // Droplets to dispense on the coupon, in the coordinates of the mesh. Only the droplet volume coupon has any.
    DropletSets          droplets;

    bool valid() const { return error.empty() && ! mesh.indices.empty(); }
};

// Free-standing walls of increasing thickness on a base plate. Shows the thinnest wall that prints.
struct WallThicknessCouponParams
{
    double first_thickness { 0.4 };
    double step { 0.2 };
    int    count { 6 };
    double wall_length { 20. };
    double wall_height { 10. };
    double base_thickness { 1.5 };
    // Space between walls and around them.
    double gap { 3. };
};
Coupon make_wall_thickness_coupon(const WallThicknessCouponParams &params);

enum class ChannelProfile { Round, Square };

// A block with channels of different widths running through it along y. Shows the narrowest channel that stays open.
// `pitch` is the distance between channel centres, 0 = the widest channel plus two walls.
struct ChannelCouponParams
{
    ChannelProfile profile { ChannelProfile::Round };
    double first_width { 0.6 };
    double step { 0.2 };
    int    count { 6 };
    double pitch { 0. };
    // Solid material between channels and around them.
    double wall { 1.5 };
    double length { 15. };
};
Coupon make_channel_coupon(const ChannelCouponParams &params);

// Arms leaning outwards from posts at increasing angles from the vertical. 0 degrees is a wall, 90 a bridge.
// Shows the steepest overhang that prints without support.
struct OverhangCouponParams
{
    double first_angle { 20. };
    double step { 10. };
    int    count { 6 };
    double arm_length { 8. };
    // Vertical thickness of an arm.
    double arm_thickness { 1.5 };
    double post_width { 2. };
    double post_height { 12. };
    // Height at which the arm leaves the post.
    double arm_start { 3. };
    double depth { 10. };
    double base_thickness { 1.5 };
    double gap { 2. };
};
Coupon make_overhang_coupon(const OverhangCouponParams &params);

// A row of round pins standing on a plate and a row of round holes through it, both getting smaller.
// Shows the smallest pin and hole that print.
struct ResolutionCouponParams
{
    double first_diameter { 1.0 };
    // How much smaller each next feature is.
    double step { 0.1 };
    int    count { 8 };
    double pin_height { 3. };
    double plate_thickness { 1.5 };
    double margin { 2. };
};
Coupon make_resolution_coupon(const ResolutionCouponParams &params);

// A row of round SLA wells, each with one droplet of a growing volume. Shows the volume that fills a well without
// overflowing and the smallest volume that lands where it should. The volumes are in nanolitres, and the wells
// are named A1, A2, ... in the order of the volumes.
struct DropletVolumeCouponParams
{
    double first_volume_nl { 10. };
    double step_nl { 10. };
    int    count { 6 };
    // Inner diameter of a well, the wall around it, and the height of the wall above the floor.
    double well_diameter { 6. };
    double wall { 0.8 };
    double wall_height { 3. };
    double floor_thickness { 0.4 };
    // Space between the walls of neighbouring wells.
    double gap { 1. };
    // Drops fired at each well; the volume is that of one drop.
    int    drops { 1 };
};
Coupon make_droplet_volume_coupon(const DropletVolumeCouponParams &params);

} // namespace Slic3r

#endif
