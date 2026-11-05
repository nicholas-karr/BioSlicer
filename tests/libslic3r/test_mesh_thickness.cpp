#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_approx.hpp>

#include "libslic3r/MeshThickness.hpp"
#include "libslic3r/MeshPrimitives.hpp"

using namespace Slic3r;

TEST_CASE("Wall thickness of a box is its smallest dimension", "[MeshThickness]")
{
    const indexed_triangle_set box = its_make_cube(30., 20., 3.);
    const WallThicknessResult  w   = analyze_wall_thickness(box, 5.);
    REQUIRE(w.valid);
    CHECK(w.min_thickness_mm == Catch::Approx(3.).margin(1e-3));
    // The minimum is measured on the big top or bottom face, 3 mm from the opposite one.
    CHECK(std::abs(w.min_location.z() - w.min_opposite.z()) == Catch::Approx(3.).margin(1e-3));
    CHECK(w.samples_below > 0);
}

TEST_CASE("Wall thickness finds a thin region and where it is", "[MeshThickness]")
{
    // A tube with 2 mm wall, and a second one with 0.8 mm wall merged at x = 40.
    indexed_triangle_set a = make_primitive(PrimitiveType::Tube, {10., 8., 30., 120.});
    indexed_triangle_set b = make_primitive(PrimitiveType::Tube, {10., 9.2, 30., 120.});
    for (Vec3f &v : b.vertices)
        v.x() += 40.f;
    its_merge(a, b);
    const WallThicknessResult w = analyze_wall_thickness(a, 1.);
    REQUIRE(w.valid);
    CHECK(w.min_thickness_mm == Catch::Approx(0.8).margin(0.02));
    CHECK(w.min_location.x() > 25.);
    CHECK(w.samples_below > 0);
}

TEST_CASE("Wall thickness of an empty mesh is invalid", "[MeshThickness]")
{
    CHECK_FALSE(analyze_wall_thickness(indexed_triangle_set{}).valid);
}
