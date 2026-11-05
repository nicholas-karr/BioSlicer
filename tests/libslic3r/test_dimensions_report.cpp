#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_approx.hpp>

#include <sstream>

#include "libslic3r/Model.hpp"
#include "libslic3r/DimensionsReport.hpp"
#include "libslic3r/MeshPrimitives.hpp"
#include "libslic3r/NumericTransform.hpp"

using namespace Slic3r;

namespace {

ModelObject *add_box(Model &model, double x, double y, double z, const char *name = "box")
{
    ModelObject *obj = model.add_object();
    obj->name = name;
    obj->add_volume(TriangleMesh(its_make_cube(x, y, z)));
    obj->add_instance();
    return obj;
}

std::vector<std::string> split(const std::string &s, char sep)
{
    std::vector<std::string> out;
    std::stringstream ss(s);
    std::string item;
    while (std::getline(ss, item, sep))
        out.push_back(item);
    return out;
}

} // namespace

TEST_CASE("Dimensions report of a box matches closed-form values", "[DimensionsReport]")
{
    Model model;
    ModelObject *obj = add_box(model, 20., 30., 40.);
    const DimensionsReport r = make_dimensions_report(*obj);
    CHECK(r.size.x() == Catch::Approx(20.).margin(1e-5));
    CHECK(r.size.y() == Catch::Approx(30.).margin(1e-5));
    CHECK(r.size.z() == Catch::Approx(40.).margin(1e-5));
    CHECK(r.volume_mm3 == Catch::Approx(24000.).epsilon(1e-6));
    CHECK(r.volume_ml == Catch::Approx(24.).epsilon(1e-6));
    CHECK(r.surface_area_mm2 == Catch::Approx(2. * (20. * 30. + 20. * 40. + 30. * 40.)).epsilon(1e-6));
    REQUIRE(r.thickness_valid);
    CHECK(r.min_wall_thickness_mm == Catch::Approx(20.).margin(1e-3));
    CHECK(r.triangles == 12);
    CHECK(r.open_edges == 0);
}

TEST_CASE("Dimensions report follows scale, rotation and typed position", "[DimensionsReport]")
{
    Model model;
    ModelObject *obj = add_box(model, 20., 30., 40.);
    NumericTransform t = get_numeric_transform(*obj, 0);
    t.size_mm      = Vec3d(10., 15., 20.);          // half size on every axis
    t.rotation_deg = Vec3d(0., 0., 90.);
    t.position_mm  = Vec3d(100., 50., 25.);
    REQUIRE(apply_numeric_transform(*obj, 0, t).empty());

    const DimensionsReport r = make_dimensions_report(*obj);
    CHECK(r.volume_mm3 == Catch::Approx(3000.).epsilon(1e-6));
    // Rotating 90 degrees around Z swaps X and Y.
    CHECK(r.size.x() == Catch::Approx(15.).margin(1e-4));
    CHECK(r.size.y() == Catch::Approx(10.).margin(1e-4));
    CHECK(r.size.z() == Catch::Approx(20.).margin(1e-4));
    CHECK((r.bbox_min + r.bbox_max).x() / 2. == Catch::Approx(100.).margin(1e-4));
    CHECK((r.bbox_min + r.bbox_max).y() / 2. == Catch::Approx(50.).margin(1e-4));
    CHECK(r.min_wall_thickness_mm == Catch::Approx(10.).margin(1e-3));
}

TEST_CASE("Dimensions report of a mirrored instance has positive volume", "[DimensionsReport]")
{
    Model model;
    ModelObject *obj = add_box(model, 20., 30., 40.);
    obj->instances.front()->set_mirror(Vec3d(-1., 1., 1.));
    const DimensionsReport r = make_dimensions_report(*obj);
    CHECK(r.volume_mm3 == Catch::Approx(24000.).epsilon(1e-6));
    CHECK(r.open_edges == 0);
    REQUIRE(r.thickness_valid);
    CHECK(r.min_wall_thickness_mm == Catch::Approx(20.).margin(1e-3));
}

TEST_CASE("Dimensions report of a torus matches Pappus formulas", "[DimensionsReport]")
{
    Model model;
    ModelObject *obj = model.add_object();
    const std::vector<double> p = {15., 4., 128., 64.};
    obj->add_volume(TriangleMesh(make_primitive(PrimitiveType::Torus, p)));
    obj->add_instance();
    const DimensionsReport r = make_dimensions_report(*obj, 0, false);
    const double ideal_area = 4. * PI * PI * 15. * 4.;
    CHECK((primitive_volume(PrimitiveType::Torus, p) - r.volume_mm3) / primitive_volume(PrimitiveType::Torus, p) <= primitive_volume_tolerance(PrimitiveType::Torus, p));
    CHECK(r.surface_area_mm2 <= ideal_area);
    CHECK(r.surface_area_mm2 == Catch::Approx(ideal_area).epsilon(0.005));
    CHECK_FALSE(r.thickness_valid);
}

TEST_CASE("Dimensions report CSV is machine readable", "[DimensionsReport]")
{
    Model model;
    ModelObject *a = add_box(model, 20., 30., 40., "plain");
    ModelObject *b = add_box(model, 5., 5., 5., "has, comma and \"quote\"");
    const std::string csv = dimensions_report_csv({make_dimensions_report(*a), make_dimensions_report(*b)});
    const std::vector<std::string> lines = split(csv, '\n');
    REQUIRE(lines.size() == 3);
    const size_t header_cols = split(lines[0], ',').size();
    CHECK(header_cols == 20);
    // Plain row: 20 fields, volume column parses back exactly.
    const std::vector<std::string> row = split(lines[1], ',');
    REQUIRE(row.size() == header_cols);
    CHECK(row[0] == "\"plain\"");
    CHECK(std::stod(row[11]) == Catch::Approx(24000.).epsilon(1e-9));
    CHECK(std::stod(row[12]) == Catch::Approx(24.).epsilon(1e-9));
    // Quoted name keeps its comma and doubled quote.
    CHECK(lines[2].rfind("\"has, comma and \"\"quote\"\"\",", 0) == 0);
    // No locale dependent decimal comma anywhere in the numbers of the first row.
    CHECK(lines[1].find("24000.000000") != std::string::npos);
}

TEST_CASE("Dimensions report of an object without parts is empty", "[DimensionsReport]")
{
    Model model;
    ModelObject *obj = model.add_object();
    obj->add_instance();
    const DimensionsReport r = make_dimensions_report(*obj);
    CHECK(r.volume_mm3 == 0.);
    CHECK(r.triangles == 0);
    CHECK_FALSE(r.thickness_valid);
    const std::string csv = dimensions_report_csv({r});
    CHECK(split(csv, '\n').size() == 2);
}
