#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_approx.hpp>

#include "libslic3r/MeshShell.hpp"
#include "libslic3r/MeshThickness.hpp"
#include "libslic3r/MeshPrimitives.hpp"
#include "mesh_test_utils.hpp"

using namespace Slic3r;

namespace {
constexpr double R = 10.;
indexed_triangle_set fine_sphere(double r = R) { return its_make_sphere(r, 2. * PI / 180.); }
}

TEST_CASE("Mesh offset changes a sphere radius by the requested distance", "[MeshShell]")
{
    const indexed_triangle_set sphere = fine_sphere();
    for (double d : {2., -2.}) {
        const indexed_triangle_set off = mesh_offset(sphere, d, 0.1);
        REQUIRE_FALSE(off.indices.empty());
        const double expected = 4. / 3. * PI * std::pow(R + d, 3);
        const double v        = test::volume_mm3(off);
        INFO("offset " << d << " mm: volume " << v << " expected " << expected);
        CHECK(v == Catch::Approx(expected).epsilon(0.01));
        CHECK(test::is_closed_manifold(off));
        const BoundingBoxf3 bb = bounding_box(off);
        CHECK(bb.size().x() == Catch::Approx(2. * (R + d)).margin(0.1));
        CHECK(bb.size().z() == Catch::Approx(2. * (R + d)).margin(0.1));
    }
}

TEST_CASE("Mesh offset larger than the solid removes it", "[MeshShell]")
{
    CHECK(mesh_offset(fine_sphere(), -R - 1., 0.2).indices.empty());
}

TEST_CASE("Closed shell of a sphere has the analytic volume and wall thickness", "[MeshShell]")
{
    const double t = 2.;
    ShellParams  p;
    p.wall_thickness_mm = t;
    p.voxel_size_mm     = 0.1;
    std::string err;
    const indexed_triangle_set shell = make_shell(fine_sphere(), p, &err);
    REQUIRE(err.empty());
    REQUIRE_FALSE(shell.indices.empty());

    const double expected = 4. / 3. * PI * (std::pow(R, 3) - std::pow(R - t, 3));
    const double v        = test::volume_mm3(shell);
    INFO("shell volume " << v << " expected " << expected);
    CHECK(v == Catch::Approx(expected).epsilon(0.01));
    CHECK(test::is_closed_manifold(shell));
    CHECK(its_number_of_patches(shell) == 2); // outer surface and cavity

    const WallThicknessResult w = analyze_wall_thickness(shell, t * 0.5);
    REQUIRE(w.valid);
    INFO("minimum thickness " << w.min_thickness_mm);
    // The measured minimum may be slightly thinner than t because of the voxel surface and the
    // faceted outer sphere (sagitta of a 2 degree facet at R=10 is 0.002 mm).
    CHECK(w.min_thickness_mm == Catch::Approx(t).margin(0.1));
    CHECK(w.samples_below == 0);
}

TEST_CASE("Shell with ports is open and loses the volume of the drilled holes", "[MeshShell]")
{
    const double t = 2., port_r = 1.5;
    const indexed_triangle_set sphere = fine_sphere();
    ShellParams p;
    p.wall_thickness_mm = t;
    p.voxel_size_mm     = 0.1;
    ShellPort top, bottom;
    REQUIRE(find_shell_port(sphere, Vec3d::UnitZ(), port_r, top));
    REQUIRE(find_shell_port(sphere, -Vec3d::UnitZ(), port_r, bottom));
    CHECK(top.position.z() == Catch::Approx(R).margin(0.05));
    CHECK(bottom.position.z() == Catch::Approx(-R).margin(0.05));
    p.ports = {top, bottom};

    std::string err;
    const indexed_triangle_set closed = make_shell(sphere, ShellParams{t, {}, 0.1}, &err);
    const indexed_triangle_set open   = make_shell(sphere, p, &err);
    REQUIRE(err.empty());
    REQUIRE_FALSE(open.indices.empty());
    CHECK(test::is_closed_manifold(open));
    CHECK(its_number_of_patches(open) == 1); // cavity connects to the outside

    // Each port removes a cylinder of the wall thickness along its axis (approximately, the
    // wall is a spherical cap, so the removed height is t within a small curvature correction).
    const double removed = test::volume_mm3(closed) - test::volume_mm3(open);
    const double per_port = PI * port_r * port_r * t;
    INFO("removed " << removed << " expected about " << 2. * per_port);
    CHECK(removed == Catch::Approx(2. * per_port).epsilon(0.05));
}

TEST_CASE("Shell reports a wall that is too thick", "[MeshShell]")
{
    std::string err;
    ShellParams p;
    p.wall_thickness_mm = R + 1.;
    CHECK(make_shell(fine_sphere(), p, &err).indices.empty());
    CHECK_FALSE(err.empty());
    p.wall_thickness_mm = 0.;
    CHECK(make_shell(fine_sphere(), p, &err).indices.empty());
}
