#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_approx.hpp>

#include <cmath>
#include <limits>
#include <numeric>
#include <string>

#include "libslic3r/MeshBoolean.hpp"
#include "libslic3r/MeshPrimitives.hpp"
#include "libslic3r/MeshShell.hpp"
#include "libslic3r/MeshThickness.hpp"
#include "mesh_test_utils.hpp"

using namespace Slic3r;

// Stress corpus for mesh_offset() and make_shell(): thin walls, sharp concave corners, nearly touching
// surfaces, 0.1 mm to 200 mm scale, open and closed ports, non-convex chambers and boolean results.
// Every offset must be a closed manifold without self-intersections, every point of it within 1 % of the
// requested distance from the input, and the volume must match the closed form where one exists.

namespace {

// Largest allowed |measured distance - requested distance| / requested distance.
constexpr double k_offset_tol = 0.01;

struct Expect
{
    double volume     = std::numeric_limits<double>::quiet_NaN(); // closed form volume of the exact offset
    double volume_tol = 0.005;                                    // relative
    int    patches    = 1;                                        // connected surface patches
    long   euler      = 2;                                        // Euler characteristic of the whole surface
};

indexed_triangle_set check_offset(const std::string &name, const indexed_triangle_set &input, double d, const Expect &e, double voxel_size = 0.)
{
    INFO(name << ", offset " << d << " mm");
    const indexed_triangle_set out = mesh_offset(input, d, voxel_size);
    REQUIRE_FALSE(out.indices.empty());
    CHECK(test::is_closed_manifold(out));
    CHECK(test::has_no_degenerate_triangles(out));
    CHECK_FALSE(test::self_intersects(out));
    CHECK(int(its_number_of_patches(out)) == e.patches);
    CHECK(test::euler_characteristic(out) == e.euler);
    const test::OffsetMeasurement m = test::measure_offset(input, out, d);
    INFO("rays " << m.rays << ", misses " << m.misses << ", max ray error " << m.max_ray_error << ", max deviation "
                 << m.max_deviation << ", mean deviation " << m.mean_deviation << ", triangles " << out.indices.size());
    CHECK(m.rays > 0);
    CHECK(m.misses == 0);
    CHECK(m.max_ray_error <= k_offset_tol);
    CHECK(m.max_deviation <= k_offset_tol);
    if (!std::isnan(e.volume)) {
        const double v = test::volume_mm3(out);
        INFO("volume " << v << " mm3, expected " << e.volume << " mm3");
        CHECK(v == Catch::Approx(e.volume).epsilon(e.volume_tol));
    }
    return out;
}

indexed_triangle_set sphere(double r) { return its_make_sphere(r, 2. * PI / 180.); }
double               ball_volume(double r) { return 4. / 3. * PI * r * r * r; }

indexed_triangle_set translated(indexed_triangle_set its, const Vec3f &t)
{
    for (Vec3f &v : its.vertices)
        v += t;
    return its;
}

indexed_triangle_set merged(indexed_triangle_set a, const indexed_triangle_set &b)
{
    its_merge(a, b);
    return a;
}

indexed_triangle_set united(indexed_triangle_set a, const indexed_triangle_set &b)
{
    MeshBoolean::cgal::plus(a, b);
    return a;
}

indexed_triangle_set subtracted(indexed_triangle_set a, const indexed_triangle_set &b)
{
    MeshBoolean::cgal::minus(a, b);
    return a;
}

// Ear clipping of a simple counter-clockwise polygon.
std::vector<Vec3i> triangulate(const std::vector<Vec2d> &pts)
{
    std::vector<int> idx(pts.size());
    std::iota(idx.begin(), idx.end(), 0);
    auto cross = [](const Vec2d &o, const Vec2d &a, const Vec2d &b) {
        return (a.x() - o.x()) * (b.y() - o.y()) - (a.y() - o.y()) * (b.x() - o.x());
    };
    std::vector<Vec3i> tris;
    while (idx.size() > 3) {
        bool clipped = false;
        for (size_t i = 0; i < idx.size() && !clipped; ++i) {
            const int    ip = idx[(i + idx.size() - 1) % idx.size()], ic = idx[i], in = idx[(i + 1) % idx.size()];
            const Vec2d &p = pts[ip], &c = pts[ic], &n = pts[in];
            if (cross(p, c, n) <= 0.)
                continue;
            bool empty = true;
            for (int j : idx) {
                if (j == ip || j == ic || j == in)
                    continue;
                const Vec2d &q = pts[j];
                if (cross(p, c, q) >= 0. && cross(c, n, q) >= 0. && cross(n, p, q) >= 0.) {
                    empty = false;
                    break;
                }
            }
            if (!empty)
                continue;
            tris.emplace_back(ip, ic, in);
            idx.erase(idx.begin() + i);
            clipped = true;
        }
        REQUIRE(clipped);
    }
    tris.emplace_back(idx[0], idx[1], idx[2]);
    return tris;
}

// Right prism over a counter-clockwise polygon from z = 0 to z = height, triangles facing outward.
indexed_triangle_set extrude(const std::vector<Vec2d> &poly, double height)
{
    indexed_triangle_set its;
    const int            n = int(poly.size());
    for (const Vec2d &p : poly)
        its.vertices.emplace_back(float(p.x()), float(p.y()), 0.f);
    for (const Vec2d &p : poly)
        its.vertices.emplace_back(float(p.x()), float(p.y()), float(height));
    for (const Vec3i &t : triangulate(poly)) {
        its.indices.emplace_back(t[0], t[2], t[1]);
        its.indices.emplace_back(t[0] + n, t[1] + n, t[2] + n);
    }
    for (int i = 0; i < n; ++i) {
        const int j = (i + 1) % n;
        its.indices.emplace_back(i, j, j + n);
        its.indices.emplace_back(i, j + n, i + n);
    }
    return its;
}

// (u, v, w) -> (x, y, z) = (w, u, v): a cyclic permutation, so the orientation is kept.
indexed_triangle_set prism_along_x(indexed_triangle_set its)
{
    for (Vec3f &v : its.vertices)
        v = Vec3f(v.z(), v.x(), v.y());
    return its;
}

// L shaped cross-section: a 20 x 20 square without its 10 x 10 corner. Area 300, perimeter 80, 5 convex
// and 1 concave corner.
indexed_triangle_set l_prism(double height) { return extrude({{0., 0.}, {20., 0.}, {20., 10.}, {10., 10.}, {10., 20.}, {0., 20.}}, height); }

// A 20 mm cube with a 1 mm wide, 10 mm deep slot through it along X. Cross-section area 390, perimeter
// 100, 6 convex and 2 concave corners.
indexed_triangle_set slotted_cube()
{
    return prism_along_x(extrude({{0., 0.}, {20., 0.}, {20., 20.}, {10.5, 20.}, {10.5, 10.}, {9.5, 10.}, {9.5, 20.}, {0., 20.}}, 20.));
}

// Outward offset volume of a right prism whose edges meet at right angles, for d below half of every
// feature size: faces add area times d, convex edges add quarter cylinders, concave edges lose the doubly
// counted d x d column, convex corners add an eighth of a ball and both ends of every concave edge lose
// the overlap of two quarter cylinders, 2/3 d^3. Checked against Monte Carlo sampling to 0.02 %.
double prism_outward_volume(double area, double perimeter, int convex, int reflex, double height, double d)
{
    const double volume = area * height, surface = 2. * area + perimeter * height;
    const double convex_edges = 2. * perimeter + convex * height, concave_edges = reflex * height;
    return volume + surface * d + PI / 4. * d * d * convex_edges - d * d * concave_edges + PI / 6. * d * d * d * (2 * convex) -
           2. / 3. * d * d * d * (2 * reflex);
}

// Erosion of the same prism: the cross-section loses a band of width d, convex corners give back the doubly
// counted square and concave corners turn into arcs of radius d. Checked against Monte Carlo sampling.
double prism_inward_volume(double area, double perimeter, int convex, int reflex, double height, double d)
{
    const double eroded = area - perimeter * d + convex * d * d - reflex * PI / 4. * d * d;
    return eroded * (height - 2. * d);
}

// Volume of two equal balls of radius r whose centres are D apart (lens formula for the overlap).
double two_balls_volume(double r, double D)
{
    const double lens = D < 2. * r ? PI * (4. * r + D) * (2. * r - D) * (2. * r - D) / 12. : 0.;
    return 2. * ball_volume(r) - lens;
}

// Height of the first surface met by a ray travelling down from far above (x, y).
double top_z(const indexed_triangle_set &its, double x, double y)
{
    const test::Tree tree = test::build_tree(its);
    const double     z0   = bounding_box(its).max.z() + 10.;
    const double     t    = test::ray_distance(its, tree, Vec3d(x, y, z0), -Vec3d::UnitZ());
    return t < 0. ? std::numeric_limits<double>::quiet_NaN() : z0 - t;
}

} // namespace

TEST_CASE("Offset of a sphere at 0.1 mm, 20 mm and 200 mm scale", "[MeshOffset]")
{
    SECTION("0.1 mm sphere offset by 0.01 mm") {
        const indexed_triangle_set s = sphere(0.05);
        check_offset("sphere R 0.05", s, 0.01, {ball_volume(0.06)});
        check_offset("sphere R 0.05", s, -0.01, {ball_volume(0.04)});
    }
    SECTION("20 mm sphere") {
        const indexed_triangle_set s = sphere(10.);
        check_offset("sphere R 10", s, 2., {ball_volume(12.)});
        check_offset("sphere R 10", s, -2., {ball_volume(8.)});
        check_offset("sphere R 10", s, 0.5, {ball_volume(10.5)});
        check_offset("sphere R 10", s, -0.5, {ball_volume(9.5)});
    }
    SECTION("200 mm sphere with a coarse voxel keeps the distance exact") {
        const indexed_triangle_set s = sphere(100.);
        check_offset("sphere R 100", s, 5., {ball_volume(105.)}, 2.);
        check_offset("sphere R 100", s, -5., {ball_volume(95.)}, 2.);
        check_offset("sphere R 100", s, 1., {ball_volume(101.)}, 1.);
    }
}

TEST_CASE("Offset of convex solids follows the Steiner formula", "[MeshOffset]")
{
    SECTION("cube") {
        const indexed_triangle_set cube = its_make_cube(20., 20., 20.);
        check_offset("cube 20", cube, 1., {test::steiner_volume(cube, 1.), 0.002});
        check_offset("cube 20", cube, -1., {18. * 18. * 18., 0.002});
        check_offset("cube 20", cube, -8., {4. * 4. * 4., 0.01});
    }
    SECTION("ellipsoid") {
        const indexed_triangle_set e = make_primitive(PrimitiveType::Ellipsoid, {10., 8., 6., 180., 90.});
        check_offset("ellipsoid 10 8 6", e, 1., {test::steiner_volume(e, 1.), 0.003});
        // The smallest curvature radius is 3.6 mm, so the inner parallel body is smooth and the formula holds.
        check_offset("ellipsoid 10 8 6", e, -1., {test::steiner_volume(e, -1.), 0.005});
    }
    SECTION("cylinder") {
        const indexed_triangle_set c = make_primitive(PrimitiveType::Frustum, {10., 10., 20., 180.});
        check_offset("cylinder R 10 h 20", c, 1., {test::steiner_volume(c, 1.), 0.003});
        check_offset("cylinder R 10 h 20", c, -1., {PI * 81. * 18., 0.005});
    }
    SECTION("capsule stays a capsule") {
        const indexed_triangle_set c = make_primitive(PrimitiveType::Capsule, {5., 20., 180., 45.});
        check_offset("capsule r 5 L 20", c, 1., {primitive_volume(PrimitiveType::Capsule, {6., 20., 180., 45.}), 0.005});
        check_offset("capsule r 5 L 20", c, -1., {primitive_volume(PrimitiveType::Capsule, {4., 20., 180., 45.}), 0.005});
    }
}

TEST_CASE("Offset of a thin plate", "[MeshOffset]")
{
    const indexed_triangle_set plate = its_make_cube(40., 40., 1.);
    check_offset("plate 40 x 40 x 1", plate, 0.5, {test::steiner_volume(plate, 0.5), 0.003});
    // 0.2 mm remains of the plate.
    check_offset("plate 40 x 40 x 1", plate, -0.4, {39.2 * 39.2 * 0.2, 0.01});
    // More than half the thickness removes the plate entirely.
    CHECK(mesh_offset(plate, -0.6).indices.empty());
    // 0.1 mm remains of a 10 x 10 x 1 plate, a tenth of the distance itself.
    const indexed_triangle_set small = its_make_cube(10., 10., 1.);
    check_offset("plate 10 x 10 x 1", small, -0.45, {9.1 * 9.1 * 0.1, 0.02});
}

TEST_CASE("Offset of a torus keeps its genus", "[MeshOffset]")
{
    const indexed_triangle_set torus = make_primitive(PrimitiveType::Torus, {15., 4., 180., 90.});
    auto torus_volume = [](double r) { return 2. * PI * PI * 15. * r * r; };
    // The 180 x 90 mesh is 0.1 % smaller than the ideal torus.
    check_offset("torus R 15 r 4", torus, 1., {torus_volume(5.), 0.005, 1, 0});
    check_offset("torus R 15 r 4", torus, -1., {torus_volume(3.), 0.005, 1, 0});
    // Only a 0.5 mm thick ring remains.
    check_offset("torus R 15 r 4", torus, -3.5, {torus_volume(0.5), 0.02, 1, 0});
}

TEST_CASE("Offset of sharp concave corners", "[MeshOffset]")
{
    SECTION("L prism with one concave edge") {
        const indexed_triangle_set l = l_prism(10.);
        REQUIRE(test::is_closed_manifold(l));
        REQUIRE(test::volume_mm3(l) == Catch::Approx(3000.));
        for (double d : {1., 2.}) {
            check_offset("L prism", l, d, {prism_outward_volume(300., 80., 5, 1, 10., d), 0.003});
            check_offset("L prism", l, -d, {prism_inward_volume(300., 80., 5, 1, 10., d), 0.005});
        }
    }
    SECTION("slotted cube, the slot faces are 1 mm apart") {
        const indexed_triangle_set cube = slotted_cube();
        REQUIRE(test::is_closed_manifold(cube));
        REQUIRE(test::volume_mm3(cube) == Catch::Approx(7800.));
        // 0.4 mm of the slot remains.
        check_offset("slotted cube", cube, 0.3, {prism_outward_volume(390., 100., 6, 2, 20., 0.3), 0.003});
        check_offset("slotted cube", cube, -0.3, {prism_inward_volume(390., 100., 6, 2, 20., 0.3), 0.005});
    }
}

TEST_CASE("Offset closes a slot narrower than twice the distance", "[MeshOffset]")
{
    const indexed_triangle_set cube = slotted_cube();
    const double               d    = 0.8;
    const indexed_triangle_set out  = check_offset("slotted cube", cube, d, {});
    // Above the middle of the closed slot the two rounded lips meet at 20 + sqrt(d^2 - (w/2)^2).
    CHECK(top_z(out, 10., 10.) == Catch::Approx(20. + std::sqrt(d * d - 0.25)).margin(k_offset_tol * d));
    CHECK(top_z(out, 10., 5.) == Catch::Approx(20. + d).margin(k_offset_tol * d));
}

TEST_CASE("Offset of nearly touching spheres", "[MeshOffset]")
{
    // Two spheres 0.5 mm apart.
    const indexed_triangle_set two = merged(sphere(10.), translated(sphere(10.), Vec3f(20.5f, 0.f, 0.f)));
    // 0.3 mm remains between them.
    check_offset("two spheres", two, 0.1, {2. * ball_volume(10.1), 0.005, 2, 4});
    check_offset("two spheres", two, -1., {2. * ball_volume(9.), 0.005, 2, 4});
    // They merge into one body with a sharp neck.
    check_offset("two spheres", two, 0.5, {two_balls_volume(10.5, 20.5), 0.005, 1, 2});
}

TEST_CASE("Offset of boolean results", "[MeshOffset]")
{
    SECTION("sphere with a spherical dent") {
        const indexed_triangle_set dented = subtracted(sphere(10.), translated(sphere(6.), Vec3f(0.f, 0.f, 10.f)));
        REQUIRE(test::is_closed_manifold(dented));
        check_offset("dented sphere", dented, 1., {});
        check_offset("dented sphere", dented, -1., {});
    }
    SECTION("sphere with a tube stub") {
        indexed_triangle_set stub = its_make_cylinder(3., 15.);
        for (Vec3f &v : stub.vertices)
            v.z() += 5.f;
        const indexed_triangle_set chamber = united(sphere(10.), stub);
        REQUIRE(test::is_closed_manifold(chamber));
        check_offset("sphere with stub", chamber, 0.5, {});
        check_offset("sphere with stub", chamber, -0.5, {});
    }
}

TEST_CASE("Offset is deterministic", "[MeshOffset]")
{
    const indexed_triangle_set torus = make_primitive(PrimitiveType::Torus, {15., 4., 180., 90.});
    const indexed_triangle_set a = mesh_offset(torus, -1.), b = mesh_offset(torus, -1.);
    REQUIRE_FALSE(a.indices.empty());
    CHECK(a.vertices == b.vertices);
    CHECK(a.indices == b.indices);
}

TEST_CASE("Shell of non-convex chambers has the closed form volume and wall", "[MeshShell]")
{
    SECTION("L prism") {
        const indexed_triangle_set l = l_prism(10.);
        std::string                err;
        const indexed_triangle_set shell = make_shell(l, ShellParams{1., {}, 0.}, &err);
        REQUIRE(err.empty());
        CHECK(test::is_closed_manifold(shell));
        CHECK_FALSE(test::self_intersects(shell));
        CHECK(its_number_of_patches(shell) == 2);
        CHECK(test::volume_mm3(shell) == Catch::Approx(3000. - prism_inward_volume(300., 80., 5, 1, 10., 1.)).epsilon(0.005));
        const WallThicknessResult w = analyze_wall_thickness(shell);
        REQUIRE(w.valid);
        CHECK(w.min_thickness_mm == Catch::Approx(1.).epsilon(k_offset_tol));
    }
    SECTION("torus") {
        const indexed_triangle_set torus = make_primitive(PrimitiveType::Torus, {15., 4., 180., 90.});
        std::string                err;
        const indexed_triangle_set shell = make_shell(torus, ShellParams{1., {}, 0.}, &err);
        REQUIRE(err.empty());
        CHECK(test::is_closed_manifold(shell));
        CHECK_FALSE(test::self_intersects(shell));
        CHECK(its_number_of_patches(shell) == 2);
        CHECK(test::euler_characteristic(shell) == 0);
        CHECK(test::volume_mm3(shell) == Catch::Approx(2. * PI * PI * 15. * (16. - 9.)).epsilon(0.005));
        CHECK(analyze_wall_thickness(shell).min_thickness_mm == Catch::Approx(1.).epsilon(k_offset_tol));
    }
}

TEST_CASE("Shell leaves a wall solid where it is thinner than twice the wall thickness", "[MeshShell]")
{
    // A 20 mm cube with a 0.8 mm flange sticking out 5 mm all around.
    const indexed_triangle_set body = united(its_make_cube(20., 20., 20.), translated(its_make_cube(30., 30., 0.8), Vec3f(-5.f, -5.f, 19.5f)));
    REQUIRE(test::is_closed_manifold(body));
    std::string                err;
    const indexed_triangle_set shell = make_shell(body, ShellParams{0.5, {}, 0.}, &err);
    REQUIRE(err.empty());
    CHECK(test::is_closed_manifold(shell));
    CHECK_FALSE(test::self_intersects(shell));
    CHECK(its_number_of_patches(shell) == 2);
    // The flange stays solid, so the thinnest wall is the flange itself.
    const WallThicknessResult w = analyze_wall_thickness(shell);
    REQUIRE(w.valid);
    CHECK(w.min_thickness_mm == Catch::Approx(0.8).epsilon(k_offset_tol));
    CHECK(w.min_location.z() >= 19.5 - 1e-3);
    CHECK(w.min_location.z() <= 20.3 + 1e-3);
}

TEST_CASE("Shell ports have the requested radius", "[MeshShell]")
{
    SECTION("sphere with openings on top and bottom") {
        const double               R = 10., t = 2., port_r = 1.5;
        const indexed_triangle_set s = sphere(R);
        ShellPort                  top, bottom;
        REQUIRE(find_shell_port(s, Vec3d::UnitZ(), port_r, top));
        REQUIRE(find_shell_port(s, -Vec3d::UnitZ(), port_r, bottom));
        std::string                err;
        const indexed_triangle_set shell = make_shell(s, ShellParams{t, {top, bottom}, 0.}, &err);
        REQUIRE(err.empty());
        CHECK(test::is_closed_manifold(shell));
        CHECK_FALSE(test::self_intersects(shell));
        CHECK(its_number_of_patches(shell) == 1);
        for (double angle : {0., 45., 90., 135., 180., 225., 270., 315.}) {
            const double c = std::cos(angle * PI / 180.), sn = std::sin(angle * PI / 180.);
            // Just inside the radius the ray passes through both openings, just outside it meets the top.
            const double inside = port_r * (1. - k_offset_tol), outside = port_r * (1. + k_offset_tol);
            CHECK(std::isnan(top_z(shell, inside * c, inside * sn)));
            CHECK(top_z(shell, outside * c, outside * sn) == Catch::Approx(std::sqrt(R * R - outside * outside)).margin(0.05));
        }
    }
    SECTION("L prism with one opening on the top face") {
        const indexed_triangle_set l = l_prism(10.);
        ShellPort                  port;
        port.position  = Vec3d(15., 5., 10.);
        port.direction = Vec3d::UnitZ();
        port.radius_mm = 1.;
        std::string                err;
        const indexed_triangle_set shell = make_shell(l, ShellParams{1., {port}, 0.}, &err);
        REQUIRE(err.empty());
        CHECK(test::is_closed_manifold(shell));
        CHECK_FALSE(test::self_intersects(shell));
        CHECK(its_number_of_patches(shell) == 1);
        for (double angle : {0., 90., 180., 270.}) {
            const double c = std::cos(angle * PI / 180.), sn = std::sin(angle * PI / 180.);
            const double inside = 1. - k_offset_tol, outside = 1. + k_offset_tol;
            // Through the opening the ray lands on the cavity floor, 1 mm above the bottom.
            CHECK(top_z(shell, 15. + inside * c, 5. + inside * sn) == Catch::Approx(1.).margin(k_offset_tol));
            CHECK(top_z(shell, 15. + outside * c, 5. + outside * sn) == Catch::Approx(10.).margin(1e-3));
        }
    }
}

TEST_CASE("Shell reports an opening that does not reach the cavity", "[MeshShell]")
{
    // Two 5 mm spheres joined by a 1.5 mm neck: a 1.6 mm wall leaves the neck solid.
    indexed_triangle_set neck = its_make_cylinder(1.5, 14.);
    for (Vec3f &v : neck.vertices)
        v = Vec3f(v.z() - 7.f, v.y(), v.x());
    its_flip_triangles(neck); // the axis swap mirrored the mesh
    const indexed_triangle_set dumbbell = united(united(translated(sphere(5.), Vec3f(-7.f, 0.f, 0.f)), translated(sphere(5.), Vec3f(7.f, 0.f, 0.f))), neck);
    REQUIRE(test::is_closed_manifold(dumbbell));
    REQUIRE(test::volume_mm3(dumbbell) > 2. * ball_volume(5.));

    std::string                err;
    const indexed_triangle_set closed = make_shell(dumbbell, ShellParams{1.6, {}, 0.}, &err);
    REQUIRE(err.empty());
    CHECK(test::is_closed_manifold(closed));
    CHECK_FALSE(test::self_intersects(closed));
    CHECK(its_number_of_patches(closed) == 3);
    // Each cavity contains at least the eroded sphere.
    CHECK(test::volume_mm3(closed) <= test::volume_mm3(dumbbell) - 2. * ball_volume(3.4) * (1. - 0.01));
    CHECK(analyze_wall_thickness(closed).min_thickness_mm >= 1.6 * (1. - k_offset_tol));

    ShellPort port;
    port.position  = Vec3d(0., 0., 1.5);
    port.direction = Vec3d::UnitZ();
    port.radius_mm = 0.5;
    CHECK(make_shell(dumbbell, ShellParams{1.6, {port}, 0.}, &err).indices.empty());
    CHECK_FALSE(err.empty());
}
