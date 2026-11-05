#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_approx.hpp>

#include <cmath>
#include <string>

#include "libslic3r/TPMS.hpp"
#include "libslic3r/TriangleMesh.hpp"
#include "libslic3r/TriangleMeshSlicer.hpp"

using namespace Slic3r;
using namespace Slic3r::tpms;
using Catch::Approx;

namespace {

constexpr double pi = 3.14159265358979323846;

double volume_fraction(const indexed_triangle_set &mesh, const Vec3d &size)
{
    return double(its_volume(mesh)) / (size.x() * size.y() * size.z());
}

// The fraction of the volume of one period where the function is at most the threshold, counted on a grid that
// is not the one the threshold was computed on.
double counted_fraction(Type type, double threshold)
{
    constexpr int n = 61;
    size_t below = 0;
    for (int i = 0; i < n; ++i)
        for (int j = 0; j < n; ++j)
            for (int k = 0; k < n; ++k)
                below += field(type, Vec3d(i * 2. * pi / n, j * 2. * pi / n, k * 2. * pi / n)) <= threshold;
    return double(below) / (double(n) * n * n);
}

} // namespace

TEST_CASE("The lattice functions take their known values", "[TPMS]")
{
    CHECK(field(Type::SchwarzP, Vec3d(0., 0., 0.)) == Approx(3.));
    CHECK(field(Type::SchwarzP, Vec3d(pi, pi, pi)) == Approx(-3.));
    CHECK(field(Type::SchwarzP, Vec3d(pi / 2., pi / 2., pi / 2.)) == Approx(0.).margin(1e-12));
    CHECK(field(Type::Diamond, Vec3d(0., 0., 0.)) == Approx(0.).margin(1e-12));
    CHECK(field(Type::Diamond, Vec3d(pi / 2., pi / 2., pi / 2.)) == Approx(1.));

    // One period is 2 pi in every direction.
    const Vec3d p(0.3, 1.1, 2.7);
    for (Type type : { Type::SchwarzP, Type::Diamond }) {
        CHECK(field(type, p) == Approx(field(type, p + Vec3d(2. * pi, 0., 0.))).margin(1e-12));
        CHECK(field(type, p) == Approx(field(type, p + Vec3d(0., 2. * pi, 4. * pi))).margin(1e-12));
    }
}

TEST_CASE("The threshold of a fraction leaves that fraction of the volume below it", "[TPMS]")
{
    for (Type type : { Type::SchwarzP, Type::Diamond }) {
        // Both functions are as often positive as negative.
        CHECK(threshold_for_fraction(type, 0.5) == Approx(0.).margin(2e-3));
        double previous = -100.;
        for (double fraction : { 0.1, 0.2, 0.3, 0.5, 0.7, 0.9 }) {
            const double threshold = threshold_for_fraction(type, fraction);
            CHECK(threshold > previous);
            previous = threshold;
            CHECK(counted_fraction(type, threshold) == Approx(fraction).margin(0.01));
        }
    }
}

TEST_CASE("The density along the axis is linear between its ends", "[TPMS]")
{
    Params p;
    p.size        = Vec3d(10., 10., 40.);
    p.density     = 0.2;
    p.density_end = 0.6;
    p.axis        = 2;
    CHECK(local_density(p, 0.) == Approx(0.2));
    CHECK(local_density(p, 10.) == Approx(0.3));
    CHECK(local_density(p, 40.) == Approx(0.6));
    CHECK(local_density(p, 80.) == Approx(0.6));
    CHECK(expected_volume_fraction(p) == Approx(0.4));

    p.density_end = -1.;
    CHECK(local_density(p, 25.) == Approx(0.2));
    CHECK(expected_volume_fraction(p) == Approx(0.2));
}

TEST_CASE("A Schwarz P lattice fills the volume fraction asked for", "[TPMS]")
{
    Params p;
    p.type    = Type::SchwarzP;
    p.size    = Vec3d(30., 30., 30.);
    p.cell    = 5.;
    p.density = 0.3;
    std::string error;
    const indexed_triangle_set mesh = make_lattice(p, &error);
    REQUIRE(error.empty());
    REQUIRE_FALSE(mesh.indices.empty());

    // Within 2 % of the asked fraction (0.294 .. 0.306).
    CHECK(volume_fraction(mesh, p.size) == Approx(0.3).epsilon(0.02));
    CHECK(its_num_open_edges(mesh) == 0);

    const BoundingBoxf3 bb = TriangleMesh(mesh).bounding_box();
    for (int a = 0; a < 3; ++a) {
        CHECK(bb.min[a] == Approx(0.).margin(0.15));
        CHECK(bb.max[a] == Approx(30.).margin(0.15));
    }
}

TEST_CASE("A diamond lattice fills the volume fraction asked for", "[TPMS]")
{
    Params p;
    p.type    = Type::Diamond;
    p.size    = Vec3d(24., 24., 24.);
    p.cell    = 6.;
    p.density = 0.4;
    const indexed_triangle_set mesh = make_lattice(p);
    REQUIRE_FALSE(mesh.indices.empty());
    CHECK(volume_fraction(mesh, p.size) == Approx(0.4).epsilon(0.02));
    CHECK(its_num_open_edges(mesh) == 0);
}

TEST_CASE("A lattice of a box that is not a cube is bounded by that box", "[TPMS]")
{
    Params p;
    p.type    = Type::SchwarzP;
    p.size    = Vec3d(20., 10., 15.);
    p.cell    = 5.;
    p.density = 0.5;
    const indexed_triangle_set mesh = make_lattice(p);
    REQUIRE_FALSE(mesh.indices.empty());
    const BoundingBoxf3 bb = TriangleMesh(mesh).bounding_box();
    CHECK(bb.size().x() == Approx(20.).margin(0.15));
    CHECK(bb.size().y() == Approx(10.).margin(0.15));
    CHECK(bb.size().z() == Approx(15.).margin(0.15));
    CHECK(volume_fraction(mesh, p.size) == Approx(0.5).epsilon(0.02));
}

TEST_CASE("A graded lattice gets denser along its axis", "[TPMS]")
{
    Params p;
    p.type        = Type::SchwarzP;
    p.size        = Vec3d(24., 24., 36.);
    p.cell        = 6.;
    p.density     = 0.2;
    p.density_end = 0.5;
    p.axis        = 2;
    const indexed_triangle_set mesh = make_lattice(p);
    REQUIRE_FALSE(mesh.indices.empty());

    // The whole box: the mean of 0.2 and 0.5.
    CHECK(volume_fraction(mesh, p.size) == Approx(0.35).epsilon(0.02));

    // Each half is three cells long; the density along it averages 0.275 and 0.425.
    indexed_triangle_set upper, lower;
    cut_mesh(mesh, 18.f, &upper, &lower, true);
    const double half = p.size.x() * p.size.y() * 18.;
    CHECK(double(its_volume(lower)) / half == Approx(0.275).epsilon(0.04));
    CHECK(double(its_volume(upper)) / half == Approx(0.425).epsilon(0.04));
}

TEST_CASE("A lattice that cannot be made says why", "[TPMS]")
{
    Params p;
    std::string error;

    p.density = 0.01;
    CHECK(make_lattice(p, &error).indices.empty());
    CHECK_FALSE(error.empty());

    p = {};
    p.density_end = 0.99;
    error.clear();
    CHECK(make_lattice(p, &error).indices.empty());
    CHECK_FALSE(error.empty());

    p = {};
    p.size = Vec3d(4., 30., 30.);   // less than one cell of 5 mm
    error.clear();
    CHECK(make_lattice(p, &error).indices.empty());
    CHECK_FALSE(error.empty());

    p = {};
    p.size  = Vec3d(300., 300., 300.);
    p.cell  = 5.;
    p.voxel = 0.1;                  // 27 thousand million samples
    error.clear();
    CHECK(make_lattice(p, &error).indices.empty());
    CHECK_FALSE(error.empty());
}
