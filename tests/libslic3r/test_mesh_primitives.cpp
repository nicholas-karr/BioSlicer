#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_approx.hpp>

#include "libslic3r/MeshPrimitives.hpp"
#include "mesh_test_utils.hpp"

using namespace Slic3r;

namespace {

indexed_triangle_set make(PrimitiveType t, std::vector<double> p)
{
    REQUIRE(primitive_validate(t, p).empty());
    return make_primitive(t, p);
}

// Mesh volume must be positive, below the ideal volume and within the documented tolerance.
void check_volume(PrimitiveType t, const std::vector<double> &p)
{
    const indexed_triangle_set its = make(t, p);
    const double ideal = primitive_volume(t, p);
    const double v     = test::volume_mm3(its);
    const double tol   = primitive_volume_tolerance(t, p);
    INFO(primitive_name(t) << " ideal " << ideal << " mesh " << v << " relative error " << (ideal - v) / ideal << " tolerance " << tol);
    CHECK(test::is_closed_manifold(its));
    CHECK(v > 0.);
    CHECK(v <= ideal * (1. + 1e-5));
    CHECK((ideal - v) / ideal <= tol);
}

} // namespace

TEST_CASE("Primitive volumes follow the analytic formulas within the segment tolerance", "[MeshPrimitives]")
{
    for (double seg : {16., 64., 180.}) {
        check_volume(PrimitiveType::Torus, {15., 4., seg, seg / 2.});
        check_volume(PrimitiveType::Ellipsoid, {10., 8., 6., seg, seg / 2.});
        check_volume(PrimitiveType::Capsule, {5., 20., seg, seg / 4.});
        check_volume(PrimitiveType::Frustum, {10., 5., 20., seg});
        check_volume(PrimitiveType::Tube, {10., 8., 30., seg});
    }
    check_volume(PrimitiveType::Wedge, {20., 10., 10., 0.});
    check_volume(PrimitiveType::Wedge, {20., 10., 10., 4.});
}

TEST_CASE("Primitive special cases", "[MeshPrimitives]")
{
    SECTION("Cone has a single apex and an exact volume formula") {
        check_volume(PrimitiveType::Frustum, {10., 0., 20., 90.});
        check_volume(PrimitiveType::Frustum, {0., 10., 20., 90.});
    }
    SECTION("Capsule without straight section is a sphere") {
        check_volume(PrimitiveType::Capsule, {5., 0., 90., 24.});
    }
    SECTION("Cylinder through equal radii") {
        const std::vector<double> p = {10., 10., 20., 180.};
        check_volume(PrimitiveType::Frustum, p);
        CHECK(primitive_volume(PrimitiveType::Frustum, p) == Catch::Approx(M_PI * 100. * 20.).epsilon(1e-12));
    }
}

TEST_CASE("Primitive bounding boxes are exact in millimetres", "[MeshPrimitives]")
{
    SECTION("Torus") {
        // 64 and 32 segments contain the extreme points (angles 0, 90, 180, 270 degrees).
        const BoundingBoxf3 bb = bounding_box(make(PrimitiveType::Torus, {15., 4., 64., 32.}));
        CHECK(bb.size().x() == Catch::Approx(38.).margin(1e-4));
        CHECK(bb.size().y() == Catch::Approx(38.).margin(1e-4));
        CHECK(bb.size().z() == Catch::Approx(8.).margin(1e-4));
    }
    SECTION("Ellipsoid") {
        const BoundingBoxf3 bb = bounding_box(make(PrimitiveType::Ellipsoid, {10., 8., 6., 64., 32.}));
        CHECK(bb.size().x() == Catch::Approx(20.).margin(1e-4));
        CHECK(bb.size().y() == Catch::Approx(16.).margin(1e-4));
        CHECK(bb.size().z() == Catch::Approx(12.).margin(1e-4));
    }
    SECTION("Capsule length is straight section plus two radii") {
        const BoundingBoxf3 bb = bounding_box(make(PrimitiveType::Capsule, {5., 20., 64., 16.}));
        CHECK(bb.size().z() == Catch::Approx(30.).margin(1e-4));
        CHECK(bb.size().x() == Catch::Approx(10.).margin(1e-4));
    }
    SECTION("Frustum, tube and wedge sit on z = 0") {
        BoundingBoxf3 bb = bounding_box(make(PrimitiveType::Frustum, {10., 5., 20., 64.}));
        CHECK(bb.min.z() == Catch::Approx(0.).margin(1e-6));
        CHECK(bb.max.z() == Catch::Approx(20.).margin(1e-5));
        bb = bounding_box(make(PrimitiveType::Tube, {10., 8., 30., 64.}));
        CHECK(bb.size().x() == Catch::Approx(20.).margin(1e-4));
        CHECK(bb.size().z() == Catch::Approx(30.).margin(1e-5));
        bb = bounding_box(make(PrimitiveType::Wedge, {20., 10., 10., 4.}));
        CHECK(bb.size().x() == Catch::Approx(20.).margin(1e-5));
        CHECK(bb.size().y() == Catch::Approx(10.).margin(1e-5));
        CHECK(bb.size().z() == Catch::Approx(10.).margin(1e-5));
    }
}

TEST_CASE("Tube wall is exactly outer minus inner radius", "[MeshPrimitives]")
{
    const indexed_triangle_set its = make(PrimitiveType::Tube, {10., 8., 30., 64.});
    // Vertices of the first ring sit at radius 10 and 8 on the X axis.
    double max_r = 0., min_r = 1e9;
    for (const Vec3f &v : its.vertices) {
        const double r = std::hypot(double(v.x()), double(v.y()));
        max_r = std::max(max_r, r);
        min_r = std::min(min_r, r);
    }
    CHECK(max_r - min_r == Catch::Approx(2.).margin(1e-5));
}

TEST_CASE("Tolerance shrinks as segments grow", "[MeshPrimitives]")
{
    double prev = 1.;
    for (double seg : {12., 24., 48., 96., 192.}) {
        const double tol = primitive_volume_tolerance(PrimitiveType::Ellipsoid, {10., 8., 6., seg, seg / 2.});
        CHECK(tol < prev);
        prev = tol;
    }
    // A 180 segment cylinder loses about 0.02 % of its volume.
    CHECK(primitive_volume_tolerance(PrimitiveType::Tube, {10., 8., 30., 180.}) < 2.5e-4);
}

TEST_CASE("Primitive parameter validation", "[MeshPrimitives]")
{
    CHECK_FALSE(primitive_validate(PrimitiveType::Torus, {4., 15., 64., 32.}).empty());   // tube larger than ring
    CHECK_FALSE(primitive_validate(PrimitiveType::Tube, {8., 10., 30., 64.}).empty());    // inner larger than outer
    CHECK_FALSE(primitive_validate(PrimitiveType::Frustum, {0., 0., 20., 64.}).empty());  // no radius at all
    CHECK_FALSE(primitive_validate(PrimitiveType::Wedge, {20., 10., 4., 10.}).empty());   // low end taller
    CHECK_FALSE(primitive_validate(PrimitiveType::Ellipsoid, {10., 8., 6., 64.5, 32.}).empty()); // fractional count
    CHECK_FALSE(primitive_validate(PrimitiveType::Capsule, {5., 20., 2., 16.}).empty());  // too few segments
    CHECK(make_primitive(PrimitiveType::Tube, {8., 10., 30., 64.}).indices.empty());
    for (int i = 0; i < int(PrimitiveType::Count); ++i)
        CHECK(primitive_validate(PrimitiveType(i), primitive_default_params(PrimitiveType(i))).empty());
}
