#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_approx.hpp>

#include <cmath>
#include <string>

#include "libslic3r/Format/STEP.hpp"
#include "libslic3r/Model.hpp"
#include "libslic3r/TriangleMesh.hpp"

using namespace Slic3r;
using Catch::Approx;

namespace {

constexpr double pi = 3.14159265358979323846;

std::string fixture(const char *name) { return std::string(TEST_DATA_DIR) + "/" + name; }

} // namespace

TEST_CASE("A STEP box is imported with its dimensions in millimetres", "[STEP]")
{
    Model model;
    REQUIRE(load_step(fixture("box_10x20x30.step").c_str(), &model));
    REQUIRE(model.objects.size() == 1);
    REQUIRE(model.objects.front()->volumes.size() == 1);

    const TriangleMesh &mesh = model.objects.front()->volumes.front()->mesh();
    const BoundingBoxf3 bb   = mesh.bounding_box();
    // The importer keeps the corner at the origin of the file in the mesh; the model centres the object.
    CHECK(bb.center().norm() == Approx(0.).margin(1e-4));
    CHECK(bb.size().x() == Approx(10.).margin(1e-4));
    CHECK(bb.size().y() == Approx(20.).margin(1e-4));
    CHECK(bb.size().z() == Approx(30.).margin(1e-4));

    // The faces of a box are flat, so the tessellation is exact.
    CHECK(double(its_volume(mesh.its)) == Approx(10. * 20. * 30.).epsilon(1e-4));
    CHECK(mesh.its.indices.size() >= 12);
    CHECK(mesh.stats().open_edges == 0);
}

TEST_CASE("A STEP cylinder is tessellated within the requested deflection", "[STEP]")
{
    const double r = 5., h = 12.;
    const double exact = pi * r * r * h;

    SECTION("Fine deflection: volume within 0.5 %") {
        Model model;
        // Linear deflection 0.005 mm, angular deflection 0.05 rad.
        REQUIRE(load_step(fixture("cylinder_r5_h12.step").c_str(), &model, std::make_pair(0.005, 0.05)));
        REQUIRE(model.objects.size() == 1);
        const TriangleMesh &mesh = model.objects.front()->volumes.front()->mesh();
        const BoundingBoxf3 bb   = mesh.bounding_box();
        CHECK(bb.size().x() == Approx(2. * r).margin(0.01));
        CHECK(bb.size().y() == Approx(2. * r).margin(0.01));
        CHECK(bb.size().z() == Approx(h).margin(1e-4));
        CHECK(double(its_volume(mesh.its)) == Approx(exact).epsilon(0.005));
        // A tessellation lies inside the true solid.
        CHECK(double(its_volume(mesh.its)) <= exact * 1.0001);
        CHECK(mesh.stats().open_edges == 0);
    }
    SECTION("A coarser deflection uses fewer triangles and a smaller volume") {
        Model fine, coarse;
        REQUIRE(load_step(fixture("cylinder_r5_h12.step").c_str(), &fine, std::make_pair(0.005, 0.05)));
        REQUIRE(load_step(fixture("cylinder_r5_h12.step").c_str(), &coarse, std::make_pair(0.5, 0.8)));
        const TriangleMesh &mf = fine.objects.front()->volumes.front()->mesh();
        const TriangleMesh &mc = coarse.objects.front()->volumes.front()->mesh();
        CHECK(mc.its.indices.size() < mf.its.indices.size());
        CHECK(double(its_volume(mc.its)) < double(its_volume(mf.its)));
        CHECK(double(its_volume(mc.its)) > 0.8 * exact);
    }
}
