#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_approx.hpp>

#include <boost/filesystem/operations.hpp>

#include "libslic3r/Model.hpp"
#include "libslic3r/Format/3mf.hpp"
#include "libslic3r/Format/OBJ.hpp"
#include "libslic3r/Format/STL.hpp"
#include "libslic3r/MeshPrimitives.hpp"
#include "libslic3r/NumericTransform.hpp"
#include "mesh_test_utils.hpp"

using namespace Slic3r;

namespace {

constexpr double one_micron = 1e-3; // mm

// Largest distance from a vertex of a to the nearest vertex of b.
double max_vertex_deviation(const indexed_triangle_set &a, const indexed_triangle_set &b)
{
    double worst = 0.;
    for (const Vec3f &va : a.vertices) {
        double best = 1e30;
        for (const Vec3f &vb : b.vertices)
            best = std::min(best, (va.cast<double>() - vb.cast<double>()).norm());
        worst = std::max(worst, best);
    }
    return worst;
}

// Wedge with typed dimensions placed far from the origin, where float rounding and short text formats matter most.
indexed_triangle_set typed_part()
{
    indexed_triangle_set its = make_primitive(PrimitiveType::Wedge, {123.456789, 87.654321, 45.678901, 12.345678});
    its_translate(its, Vec3f(150.123457f, 120.654321f, 33.333333f));
    return its;
}

std::string tmp_path(const char *name) { return (boost::filesystem::temp_directory_path() / name).string(); }

void check_bbox(const indexed_triangle_set &its, const std::string &what)
{
    const BoundingBoxf3 bb = bounding_box(its);
    INFO(what << " size " << bb.size().transpose());
    CHECK(bb.size().x() == Catch::Approx(123.456789).margin(one_micron));
    CHECK(bb.size().y() == Catch::Approx(87.654321).margin(one_micron));
    CHECK(bb.size().z() == Catch::Approx(45.678901).margin(one_micron));
}

} // namespace

TEST_CASE("STL export keeps typed dimensions within one micron", "[ExportPrecision]")
{
    for (bool binary : {true, false}) {
        TriangleMesh mesh(typed_part());
        const std::string path = tmp_path(binary ? "bio_precision_bin.stl" : "bio_precision_ascii.stl");
        REQUIRE(store_stl(path.c_str(), &mesh, binary));
        Model model;
        REQUIRE(load_stl(path.c_str(), &model));
        boost::filesystem::remove(path);
        REQUIRE(model.objects.size() == 1);
        const indexed_triangle_set loaded = model.objects.front()->volumes.front()->mesh().its;
        // load_stl centres the mesh on the object; compare shape and size, not absolute position.
        check_bbox(loaded, binary ? "binary STL" : "ASCII STL");
        CHECK(test::volume_mm3(loaded) == Catch::Approx(test::volume_mm3(mesh.its)).epsilon(1e-6));
    }
}

TEST_CASE("OBJ export keeps typed dimensions and positions within one micron", "[ExportPrecision]")
{
    TriangleMesh mesh(typed_part());
    const std::string path = tmp_path("bio_precision.obj");
    REQUIRE(store_obj(path.c_str(), &mesh));
    TriangleMesh loaded;
    REQUIRE(load_obj(path.c_str(), &loaded));
    boost::filesystem::remove(path);
    check_bbox(loaded.its, "OBJ");
    CHECK(max_vertex_deviation(mesh.its, loaded.its) < one_micron);
    CHECK(max_vertex_deviation(loaded.its, mesh.its) < one_micron);
}

TEST_CASE("3MF export keeps typed dimensions and positions within one micron", "[ExportPrecision]")
{
    Model src;
    ModelObject *obj = src.add_object();
    obj->name = "typed";
    obj->add_volume(TriangleMesh(make_primitive(PrimitiveType::Wedge, {123.456789, 87.654321, 45.678901, 12.345678})));
    obj->add_instance();
    NumericTransform t;
    t.position_mm  = Vec3d(150.123457, 120.654321, 60.);
    t.rotation_deg = Vec3d(0., 0., 0.);
    t.size_mm      = Vec3d(123.456789, 87.654321, 45.678901);
    REQUIRE(apply_numeric_transform(*obj, 0, t).empty());

    const std::string path = tmp_path("bio_precision.3mf");
    REQUIRE(store_3mf(path.c_str(), &src, nullptr, false));
    Model dst;
    DynamicPrintConfig cfg;
    {
        ConfigSubstitutionContext ctxt{ForwardCompatibilitySubstitutionRule::Disable};
        boost::optional<Semver> version;
        REQUIRE(load_3mf(path.c_str(), cfg, ctxt, &dst, false, version));
    }
    boost::filesystem::remove(path);
    REQUIRE(dst.objects.size() == 1);

    const BoundingBoxf3 a = src.objects.front()->instance_bounding_box(0);
    const BoundingBoxf3 b = dst.objects.front()->instance_bounding_box(0);
    INFO("saved " << a.min.transpose() << " / " << a.max.transpose() << " loaded " << b.min.transpose() << " / " << b.max.transpose());
    CHECK((a.min - b.min).cwiseAbs().maxCoeff() < one_micron);
    CHECK((a.max - b.max).cwiseAbs().maxCoeff() < one_micron);
    CHECK(b.size().x() == Catch::Approx(123.456789).margin(one_micron));
    CHECK(b.size().z() == Catch::Approx(45.678901).margin(one_micron));
}

TEST_CASE("3MF export keeps a rotated and scaled instance within one micron", "[ExportPrecision]")
{
    Model src;
    ModelObject *obj = src.add_object();
    obj->add_volume(TriangleMesh(make_primitive(PrimitiveType::Tube, {40.123456, 30.654321, 55.5, 96.})));
    obj->add_instance();
    NumericTransform t = get_numeric_transform(*obj, 0);
    t.position_mm  = Vec3d(110.5, 95.25, 40.);
    t.rotation_deg = Vec3d(17.5, -33.25, 61.125);
    t.size_mm      = Vec3d(80.246912, 61.308642, 55.5);
    REQUIRE(apply_numeric_transform(*obj, 0, t).empty());

    const std::string path = tmp_path("bio_precision_rot.3mf");
    REQUIRE(store_3mf(path.c_str(), &src, nullptr, false));
    Model dst;
    DynamicPrintConfig cfg;
    {
        ConfigSubstitutionContext ctxt{ForwardCompatibilitySubstitutionRule::Disable};
        boost::optional<Semver> version;
        REQUIRE(load_3mf(path.c_str(), cfg, ctxt, &dst, false, version));
    }
    boost::filesystem::remove(path);
    const indexed_triangle_set a = src.mesh().its, b = dst.mesh().its;
    REQUIRE(a.vertices.size() == b.vertices.size());
    double worst = 0.;
    for (size_t i = 0; i < a.vertices.size(); ++i)
        worst = std::max(worst, double((a.vertices[i] - b.vertices[i]).cast<double>().norm()));
    INFO("worst vertex deviation " << worst << " mm");
    CHECK(worst < one_micron);
}
