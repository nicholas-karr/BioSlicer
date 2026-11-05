#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_approx.hpp>

#include <algorithm>
#include <cmath>
#include <numeric>

#include "libslic3r/TestCoupon.hpp"
#include "libslic3r/TriangleMesh.hpp"

using namespace Slic3r;
using Catch::Approx;

namespace {

constexpr double pi = 3.14159265358979323846;

// Area of a round feature of the given diameter, which is a regular polygon.
double circle_area(double diameter)
{
    const double r = diameter * 0.5;
    return 0.5 * kCouponCircleSegments * r * r * std::sin(2. * pi / kCouponCircleSegments);
}

double volume_of(const Coupon &coupon) { return double(its_volume(coupon.mesh)); }

BoundingBoxf3 bbox_of(const Coupon &coupon) { return TriangleMesh(coupon.mesh).bounding_box(); }

double sum(const std::vector<double> &v) { return std::accumulate(v.begin(), v.end(), 0.); }

} // namespace

TEST_CASE("Wall thickness coupon has the volume of its plate and walls", "[Coupon]")
{
    WallThicknessCouponParams p;
    p.first_thickness = 0.4;
    p.step            = 0.2;
    p.count           = 6;
    p.wall_length     = 20.;
    p.wall_height     = 10.;
    p.base_thickness  = 1.5;
    p.gap             = 3.;
    const Coupon coupon = make_wall_thickness_coupon(p);
    REQUIRE(coupon.valid());

    REQUIRE(coupon.values.size() == 6);
    CHECK(coupon.values.front() == Approx(0.4));
    CHECK(coupon.values.back() == Approx(1.4));
    CHECK(coupon.unit == "mm");

    // Walls 0.4 .. 1.4 mm add to 5.4 mm; six walls and seven gaps of 3 mm make the plate 26.4 mm long.
    const double plate_x = 5.4 + 7 * 3.;
    const double plate_y = 20. + 2 * 3.;
    CHECK(volume_of(coupon) == Approx(plate_x * plate_y * 1.5 + 5.4 * 20. * 10.).epsilon(1e-4));

    const BoundingBoxf3 bb = bbox_of(coupon);
    CHECK(bb.min.isApprox(Vec3d::Zero(), 1e-4));
    CHECK(bb.size().x() == Approx(plate_x).margin(1e-4));
    CHECK(bb.size().y() == Approx(plate_y).margin(1e-4));
    CHECK(bb.size().z() == Approx(11.5).margin(1e-4));
}

TEST_CASE("Wall thickness coupon refuses walls that cannot exist", "[Coupon]")
{
    WallThicknessCouponParams p;
    p.count = 0;
    CHECK_FALSE(make_wall_thickness_coupon(p).valid());
    p = {};
    p.first_thickness = 0.3;
    p.step            = -0.1;
    p.count           = 6;    // the last wall would be -0.2 mm
    const Coupon coupon = make_wall_thickness_coupon(p);
    CHECK_FALSE(coupon.valid());
    CHECK_FALSE(coupon.error.empty());
    CHECK(coupon.mesh.indices.empty());
}

TEST_CASE("Round channel coupon has the volume of its block minus its channels", "[Coupon]")
{
    ChannelCouponParams p;
    p.profile     = ChannelProfile::Round;
    p.first_width = 0.6;
    p.step        = 0.2;
    p.count       = 6;
    p.wall        = 1.5;
    p.length      = 15.;
    const Coupon coupon = make_channel_coupon(p);
    REQUIRE(coupon.valid());

    // Widths 0.6 .. 1.6 mm; the pitch is the widest channel plus a wall, 3.1 mm.
    const double width  = 5 * 3.1 + 1.6 + 2 * 1.5;
    const double height = 1.6 + 2 * 1.5;
    double channels = 0.;
    for (double w : coupon.values)
        channels += circle_area(w);
    CHECK(volume_of(coupon) == Approx((width * height - channels) * 15.).epsilon(1e-3));

    const BoundingBoxf3 bb = bbox_of(coupon);
    CHECK(bb.min.isApprox(Vec3d::Zero(), 1e-4));
    CHECK(bb.size().x() == Approx(width).margin(1e-4));
    CHECK(bb.size().y() == Approx(15.).margin(1e-4));
    CHECK(bb.size().z() == Approx(height).margin(1e-4));
}

TEST_CASE("Square channel coupon removes the square of each width", "[Coupon]")
{
    ChannelCouponParams p;
    p.profile     = ChannelProfile::Square;
    p.first_width = 0.5;
    p.step        = 0.5;
    p.count       = 4;
    p.wall        = 1.;
    p.length      = 10.;
    const Coupon coupon = make_channel_coupon(p);
    REQUIRE(coupon.valid());

    // Widths 0.5, 1, 1.5, 2 mm; pitch 3 mm.
    const double width  = 3 * 3. + 2. + 2 * 1.;
    const double height = 2. + 2 * 1.;
    const double holes  = 0.25 + 1. + 2.25 + 4.;
    CHECK(volume_of(coupon) == Approx((width * height - holes) * 10.).epsilon(1e-4));
}

TEST_CASE("Channel coupon pitch sets the distance between channels", "[Coupon]")
{
    ChannelCouponParams p;
    p.profile     = ChannelProfile::Square;
    p.first_width = 1.;
    p.step        = 0.;
    p.count       = 2;
    p.pitch       = 4.;
    p.wall        = 1.;
    p.length      = 8.;
    const Coupon coupon = make_channel_coupon(p);
    REQUIRE(coupon.valid());
    CHECK(bbox_of(coupon).size().x() == Approx(4. + 1. + 2.).margin(1e-4));
    CHECK(volume_of(coupon) == Approx((7. * 3. - 2. * 1.) * 8.).epsilon(1e-4));

    // Channels that would run into each other are refused.
    p.pitch = 1.05;
    CHECK_FALSE(make_channel_coupon(p).valid());
    p.pitch = 4.;
    p.first_width = 0.01;
    CHECK_FALSE(make_channel_coupon(p).valid());
}

TEST_CASE("Overhang coupon arms lean at the requested angles", "[Coupon]")
{
    OverhangCouponParams p;
    p.first_angle   = 20.;
    p.step          = 10.;
    p.count         = 6;
    p.arm_length    = 8.;
    p.arm_thickness = 1.5;
    p.post_width    = 2.;
    p.post_height   = 12.;
    p.arm_start     = 3.;
    p.depth         = 10.;
    p.base_thickness = 1.5;
    p.gap           = 2.;
    const Coupon coupon = make_overhang_coupon(p);
    REQUIRE(coupon.valid());
    REQUIRE(coupon.values.size() == 6);
    CHECK(coupon.unit == "degrees");
    CHECK(coupon.values.back() == Approx(70.));

    double reach = 0., arms = 0., top = 0.;
    for (double angle : coupon.values) {
        const double a = angle * pi / 180.;
        reach += 8. * std::sin(a);
        // The arm is a parallelogram of base 8 mm along the arm and 1.5 mm vertical side.
        arms += 8. * 1.5 * std::sin(a);
        top = std::max(top, 3. + 8. * std::cos(a) + 1.5);
    }
    const double length = 2. + 6 * (2. + 2.) + reach;
    const double posts  = 6 * 2. * 12.;
    CHECK(volume_of(coupon) == Approx((length * 1.5 + posts + arms) * 10.).epsilon(1e-3));

    const BoundingBoxf3 bb = bbox_of(coupon);
    CHECK(bb.size().x() == Approx(length).margin(1e-3));
    CHECK(bb.size().y() == Approx(10.).margin(1e-4));
    CHECK(bb.size().z() == Approx(1.5 + std::max(12., top)).margin(1e-3));
}

TEST_CASE("Overhang coupon refuses angles that are not overhangs", "[Coupon]")
{
    OverhangCouponParams p;
    p.first_angle = 0.;
    CHECK_FALSE(make_overhang_coupon(p).valid());
    p.first_angle = 80.;
    p.step        = 10.;
    p.count       = 3;   // 100 degrees
    CHECK_FALSE(make_overhang_coupon(p).valid());
    p = {};
    p.arm_start = 11.5;  // the arm would leave through the top of the post
    CHECK_FALSE(make_overhang_coupon(p).valid());
}

TEST_CASE("Resolution coupon has pins on its plate and holes through it", "[Coupon]")
{
    ResolutionCouponParams p;
    p.first_diameter  = 1.0;
    p.step            = 0.1;
    p.count           = 8;
    p.pin_height      = 3.;
    p.plate_thickness = 1.5;
    p.margin          = 2.;
    const Coupon coupon = make_resolution_coupon(p);
    REQUIRE(coupon.valid());
    REQUIRE(coupon.values.size() == 8);
    CHECK(coupon.values.front() == Approx(1.0));
    CHECK(coupon.values.back() == Approx(0.3));

    // Pitch 3 mm: the plate is 8 x 3 + 2 = 26 mm by 2 x 3 + 2 = 8 mm.
    double area = 0.;
    for (double d : coupon.values)
        area += circle_area(d);
    const double expected = 26. * 8. * 1.5 - area * 1.5 + area * 3.;
    CHECK(volume_of(coupon) == Approx(expected).epsilon(1e-3));

    const BoundingBoxf3 bb = bbox_of(coupon);
    CHECK(bb.size().x() == Approx(26.).margin(1e-4));
    CHECK(bb.size().y() == Approx(8.).margin(1e-4));
    CHECK(bb.size().z() == Approx(4.5).margin(1e-4));
    CHECK(sum(coupon.values) == Approx(5.2));
}

TEST_CASE("Resolution coupon features must get smaller and stay printable", "[Coupon]")
{
    ResolutionCouponParams p;
    p.step = -0.1;      // larger and larger
    CHECK_FALSE(make_resolution_coupon(p).valid());
    p = {};
    p.step  = 0.2;
    p.count = 6;        // 1.0 down to 0.0
    CHECK_FALSE(make_resolution_coupon(p).valid());
}

TEST_CASE("Droplet volume coupon has one well per volume with its droplet inside", "[Coupon]")
{
    DropletVolumeCouponParams p;
    p.first_volume_nl = 10.;
    p.step_nl         = 10.;
    p.count           = 6;
    const Coupon coupon = make_droplet_volume_coupon(p);
    REQUIRE(coupon.valid());
    CHECK(coupon.unit == "nL");
    REQUIRE(coupon.values.size() == 6);
    CHECK(coupon.values.back() == Approx(60.));

    // Six wells of 6 mm inner diameter with 0.8 mm walls, 1 mm apart: 7.6 mm each plus gaps.
    const BoundingBoxf3 bb = bbox_of(coupon);
    CHECK(bb.min.isApprox(Vec3d::Zero(), 1e-3));
    CHECK(bb.size().x() == Approx(6 * 7.6 + 5 * 1.).margin(0.05));
    CHECK(bb.size().y() == Approx(7.6).margin(0.05));
    CHECK(bb.size().z() == Approx(0.4 + 3.).margin(1e-4));

    REQUIRE(coupon.droplets.size() == 6);
    for (size_t i = 0; i < 6; ++i) {
        REQUIRE(coupon.droplets[i].points.size() == 1);
        const DropletPoint &d = coupon.droplets[i].points.front();
        CHECK(d.volume_nl == Approx(10. * double(i + 1)));
        // The droplet sits over the middle of its well, on the floor.
        CHECK(d.pos.x() == Approx(3.8 + double(i) * 8.6).margin(0.05));
        CHECK(d.pos.y() == Approx(3.8).margin(0.05));
        CHECK(d.pos.z() == Approx(0.4).margin(1e-6));
        CHECK(coupon.droplets[i].dip);
    }
    CHECK(coupon.droplets[0].name == "A1");
    CHECK(coupon.droplets[5].name == "A6");
}

TEST_CASE("Droplet volume coupon refuses droplets that cannot be dispensed", "[Coupon]")
{
    DropletVolumeCouponParams p;
    p.first_volume_nl = 5.;
    p.step_nl         = -5.;
    p.count           = 3;      // 5, 0, -5 nL
    CHECK_FALSE(make_droplet_volume_coupon(p).valid());
    p = {};
    p.drops = 0;
    CHECK_FALSE(make_droplet_volume_coupon(p).valid());
}

TEST_CASE("Droplet volume coupon checks every droplet, not only the last", "[Coupon]")
{
    DropletVolumeCouponParams p;
    // The smallest droplet is the first one, 0.05 nL.
    p.first_volume_nl = 0.05;
    p.step_nl         = 0.1;
    p.count           = 3;
    const Coupon small = make_droplet_volume_coupon(p);
    CHECK_FALSE(small.valid());
    CHECK(small.error.find("0.1 nL") != std::string::npos);

    // A 6 mm well 3 mm tall holds 84 800 nL; 5 drops of 20 000 nL do not fit.
    p = {};
    p.first_volume_nl = 20000.;
    p.step_nl         = 0.;
    p.count           = 2;
    p.drops           = 5;
    const Coupon too_big = make_droplet_volume_coupon(p);
    CHECK_FALSE(too_big.valid());
    CHECK(too_big.error.find("does not fit") != std::string::npos);

    p.drops = 4;
    CHECK(make_droplet_volume_coupon(p).valid());
}
