#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_approx.hpp>

#include <cmath>

#include "libslic3r/ShapeGen.hpp"
#include "libslic3r/TriangleMesh.hpp"

using namespace Slic3r;
using Catch::Approx;

namespace {

ShapeGenParams make_params(int shape, int pattern, double size = 20., double height = 15.)
{
    ShapeGenParams p;
    p.shape   = shape;
    p.pattern = pattern;
    p.size    = size;
    p.sla_h   = height;
    return p;
}

double volume_of(const indexed_triangle_set &its) { return double(its_volume(its)); }

double total_volume(const std::vector<indexed_triangle_set> &meshes)
{
    double v = 0.;
    for (const indexed_triangle_set &m : meshes)
        v += volume_of(m);
    return v;
}

BoundingBoxf3 bbox_of(const indexed_triangle_set &its) { return TriangleMesh(its).bounding_box(); }

constexpr double pi = 3.14159265358979323846;

} // namespace

TEST_CASE("Single shapes have the volume of the solid they name", "[ShapeGen]")
{
    const double size = 20., height = 15., r = size / 2.;

    SECTION("Cube: size x size x height") {
        const auto meshes = generate_shape(make_params(0, 0, size, height));
        REQUIRE(meshes.size() == 1);
        CHECK(volume_of(meshes[0]) == Approx(size * size * height).epsilon(1e-4));
        const Vec3d extent = bbox_of(meshes[0]).size();
        CHECK(extent.x() == Approx(size).margin(1e-3));
        CHECK(extent.y() == Approx(size).margin(1e-3));
        CHECK(extent.z() == Approx(height).margin(1e-3));
        CHECK(bbox_of(meshes[0]).min.z() == Approx(0.).margin(1e-4));
    }
    SECTION("Cylinder: pi r^2 h within the 512-gon error") {
        const auto meshes = generate_shape(make_params(1, 0, size, height));
        REQUIRE(meshes.size() == 1);
        CHECK(volume_of(meshes[0]) == Approx(pi * r * r * height).epsilon(1e-3));
        const BoundingBoxf3 bb = bbox_of(meshes[0]);
        CHECK(bb.size().x() == Approx(size).margin(0.01));
        CHECK(bb.size().z() == Approx(height).margin(1e-3));
        CHECK(bb.min.z() == Approx(0.).margin(1e-4));
    }
    SECTION("Cone: pi r^2 h / 3") {
        const auto meshes = generate_shape(make_params(2, 0, size, height));
        REQUIRE(meshes.size() == 1);
        CHECK(volume_of(meshes[0]) == Approx(pi * r * r * height / 3.).epsilon(2e-3));
        CHECK(bbox_of(meshes[0]).size().z() == Approx(height).margin(1e-3));
    }
    SECTION("Sphere: 4/3 pi r^3 and a diameter equal to the size") {
        const auto meshes = generate_shape(make_params(3, 0, size, height));
        REQUIRE(meshes.size() == 1);
        CHECK(volume_of(meshes[0]) == Approx(4. / 3. * pi * r * r * r).epsilon(0.01));
        const BoundingBoxf3 bb = bbox_of(meshes[0]);
        CHECK(bb.min.z() == Approx(0.).margin(1e-3));
        CHECK(bb.size().z() == Approx(size).margin(0.02));
    }
    SECTION("Pyramid: size^2 h / 3") {
        const auto meshes = generate_shape(make_params(4, 0, size, height));
        REQUIRE(meshes.size() == 1);
        CHECK(volume_of(meshes[0]) == Approx(size * size * height / 3.).epsilon(1e-3));
        CHECK(bbox_of(meshes[0]).size().z() == Approx(height).margin(1e-3));
    }
}

TEST_CASE("The number of materials follows the pattern and the shape", "[ShapeGen]")
{
    CHECK(shape_gen_material_count(make_params(1, 0)) == 1);

    ShapeGenParams stripes = make_params(1, 1);
    stripes.n_materials = 3;
    CHECK(shape_gen_material_count(stripes) == 3);
    // A pattern needs at least two materials.
    stripes.n_materials = 1;
    CHECK(shape_gen_material_count(stripes) == 2);

    ShapeGenParams helix = make_params(5, 0);
    helix.hs_strands = 3;
    CHECK(shape_gen_material_count(helix) == 4);
    helix.hs_strands = 0;
    CHECK(shape_gen_material_count(helix) == 2);

    // generate_shape() returns exactly that many meshes.
    for (int shape = 0; shape <= 5; ++shape)
        for (int pattern = 0; pattern <= 4; ++pattern) {
            ShapeGenParams p = make_params(shape, pattern);
            p.n_materials = 3;
            CHECK(int(generate_shape(p).size()) == shape_gen_material_count(p));
        }
}

TEST_CASE("Stripes divide the body into equal z bands", "[ShapeGen]")
{
    ShapeGenParams p = make_params(1, 1, 20., 15.);
    p.stripe_rows = 4;
    p.n_materials = 2;
    const auto meshes = generate_shape(p);
    REQUIRE(meshes.size() == 2);

    const double cylinder = pi * 10. * 10. * 15.;
    CHECK(total_volume(meshes) == Approx(cylinder).epsilon(1e-3));
    // Rows 0 and 2 go to material 0, rows 1 and 3 to material 1: two bands of 3.75 mm each.
    CHECK(volume_of(meshes[0]) == Approx(cylinder / 2.).epsilon(1e-3));
    CHECK(volume_of(meshes[1]) == Approx(cylinder / 2.).epsilon(1e-3));
    CHECK(bbox_of(meshes[0]).min.z() == Approx(0.).margin(1e-4));
    CHECK(bbox_of(meshes[0]).max.z() == Approx(11.25).margin(1e-4));
    CHECK(bbox_of(meshes[1]).min.z() == Approx(3.75).margin(1e-4));
    CHECK(bbox_of(meshes[1]).max.z() == Approx(15.).margin(1e-4));
}

TEST_CASE("Stripes of a cone stay inside the cone", "[ShapeGen]")
{
    ShapeGenParams p = make_params(2, 1, 20., 15.);
    p.stripe_rows = 5;
    const auto meshes = generate_shape(p);
    const double cone = pi * 10. * 10. * 15. / 3.;
    const double sum  = total_volume(meshes);
    // Each band is a cylinder inscribed in the cone, so the bands fill less than the cone but most of it.
    CHECK(sum < cone);
    CHECK(sum > 0.5 * cone);
    for (const indexed_triangle_set &m : meshes)
        for (const stl_vertex &v : m.vertices) {
            const double allowed = 10. * (1. - v.z() / 15.) + 0.01;
            CHECK(std::hypot(v.x(), v.y()) <= std::max(allowed, 0.51));
        }
}

TEST_CASE("Checkerboard cells tile the body without gaps", "[ShapeGen]")
{
    ShapeGenParams p = make_params(1, 2, 20., 15.);
    p.cb_columns  = 8;
    p.cb_rows     = 4;
    p.n_materials = 2;
    const auto meshes = generate_shape(p);
    REQUIRE(meshes.size() == 2);
    const double cylinder = pi * 10. * 10. * 15.;
    CHECK(total_volume(meshes) == Approx(cylinder).epsilon(1e-3));
    // 8 columns x 4 rows alternate evenly between the two materials.
    CHECK(volume_of(meshes[0]) == Approx(volume_of(meshes[1])).epsilon(1e-3));

    // A cube is cut into slabs along x and tiles the cube exactly.
    ShapeGenParams cube = make_params(0, 2, 20., 15.);
    cube.cb_columns = 5;
    cube.cb_rows    = 3;
    CHECK(total_volume(generate_shape(cube)) == Approx(20. * 20. * 15.).epsilon(1e-4));
}

TEST_CASE("The helix pattern tiles the body without gaps", "[ShapeGen]")
{
    ShapeGenParams p = make_params(1, 3, 20., 15.);
    p.hx_revolutions = 2.;
    p.hx_width       = 50;
    const auto meshes = generate_shape(p);
    REQUIRE(meshes.size() == 2);
    const double cylinder = pi * 10. * 10. * 15.;
    CHECK(total_volume(meshes) == Approx(cylinder).epsilon(1e-3));
    // Half the angular sectors belong to the first material at every height.
    CHECK(volume_of(meshes[0]) == Approx(cylinder / 2.).epsilon(1e-2));
}

TEST_CASE("The honeycomb pattern puts whole hexagonal cells inside the body", "[ShapeGen]")
{
    ShapeGenParams p = make_params(1, 4, 20., 15.);
    p.hc_sectors  = 6;
    p.hc_bands    = 20;
    p.n_materials = 2;
    const auto meshes = generate_shape(p);
    REQUIRE(meshes.size() == 2);

    // Material 0 is the whole body.
    const double cylinder = pi * 10. * 10. * 15.;
    CHECK(volume_of(meshes[0]) == Approx(cylinder).epsilon(1e-3));

    // Material 1 is a whole number of hexagonal prisms of circumradius R_in and the full height.
    const double R_in     = 20. / (std::sqrt(3.) * 6.) * (1. - 0.20);
    const double hex_vol  = 1.5 * std::sqrt(3.) * R_in * R_in * 15.;
    const double cells    = volume_of(meshes[1]) / hex_vol;
    REQUIRE(cells >= 1.);
    CHECK(cells == Approx(std::round(cells)).margin(0.01));

    // No cell leaves the cylinder.
    for (const stl_vertex &v : meshes[1].vertices)
        CHECK(std::hypot(v.x(), v.y()) <= 10.0001);
    CHECK(bbox_of(meshes[1]).min.z() == Approx(0.).margin(1e-4));
    CHECK(bbox_of(meshes[1]).max.z() == Approx(15.).margin(1e-4));
}

TEST_CASE("The helix shape is a cylinder with swept strands wound inside it", "[ShapeGen]")
{
    ShapeGenParams p = make_params(5, 0, 20., 15.);
    p.hs_turns    = 3;
    p.hs_tube_pct = 20;
    p.hs_strands  = 2;
    const auto meshes = generate_shape(p);
    REQUIRE(meshes.size() == 3);

    CHECK(volume_of(meshes[0]) == Approx(pi * 10. * 10. * 15.).epsilon(1e-3));

    // A strand is a tube of radius 2 mm (20 % of 10 mm) around a helix of radius 8 mm.
    const double tube_r   = 2., path_r = 8.;
    const double length   = std::hypot(2. * pi * 3. * path_r, 15.);
    // The 16-sided tube section has 97.45 % of the area of a circle. The loft of the sections along the
    // coil loses a few percent more on the inside of the bends, so the tolerance is 8 %.
    const double expected = (16. / (2. * pi)) * std::sin(2. * pi / 16.) * pi * tube_r * tube_r * length;
    for (size_t s = 1; s <= 2; ++s) {
        CHECK(volume_of(meshes[s]) == Approx(expected).epsilon(0.08));
        const BoundingBoxf3 bb = bbox_of(meshes[s]);
        CHECK(bb.min.z() >= -1e-4);
        CHECK(bb.max.z() <= 15. + 1e-4);
        for (const stl_vertex &v : meshes[s].vertices)
            CHECK(std::hypot(v.x(), v.y()) <= 10. + 1e-3);
    }
}
