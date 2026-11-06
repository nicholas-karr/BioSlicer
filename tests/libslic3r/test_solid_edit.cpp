#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_approx.hpp>
#include <test_utils.hpp>

#include <libslic3r/Format/3mf.hpp>
#include <libslic3r/Model.hpp>
#include <libslic3r/SolidEdit.hpp>
#include <libslic3r/TriangleMesh.hpp>

#include <boost/filesystem/operations.hpp>

#include <cmath>
#include <set>

using namespace Slic3r;
using namespace Slic3r::SolidEdit;
using Catch::Approx;

namespace {

// Relative tolerance for volumes, 0.1 %.
constexpr double rel_tol = 1e-3;

double volume_of(const TriangleMesh &mesh) { return double(its_volume(mesh.its)); }

bool is_watertight(const TriangleMesh &mesh) { return its_num_open_edges(mesh.its) == 0; }

TriangleMesh cube(double x, double y, double z, Vec3d at = Vec3d::Zero())
{
    TriangleMesh m = make_cube(x, y, z);
    m.translate(at.cast<float>());
    return m;
}

// Volume of the polygonal prism that make_cylinder() builds.
double cylinder_volume(double r, double h, double fa)
{
    const double n = std::ceil(2. * PI / fa);
    return 0.5 * n * r * r * std::sin(2. * PI / n) * h;
}

double circle_area(double diameter) { return PI * diameter * diameter / 4.; }

ModelObject *make_object(Model &model, const TriangleMesh &mesh, const Vec3d &offset = Vec3d::Zero())
{
    ModelObject *obj = model.add_object();
    obj->name = "test";
    obj->add_volume(TriangleMesh(mesh));
    obj->add_instance()->set_offset(offset);
    return obj;
}

double part_volume(const ModelVolume &v)
{
    TriangleMesh m = v.mesh();
    m.transform(v.get_matrix(), true);
    return volume_of(m);
}

BoundingBoxf3 part_bbox(const ModelVolume &v) { return v.mesh().transformed_bounding_box(v.get_matrix()); }

} // namespace

TEST_CASE("Boolean subtract box minus cylinder", "[SolidEdit]")
{
    const double fa = 2. * PI / 64.;
    TriangleMesh box = cube(20, 20, 10);
    TriangleMesh cyl = make_cylinder(3., 14., fa);
    cyl.translate(10.f, 10.f, -2.f);

    Result r = boolean(BooleanOp::Subtract, box, { cyl });
    REQUIRE(r.ok);
    CHECK(is_watertight(r.mesh));
    CHECK(volume_of(r.mesh) == Approx(4000. - cylinder_volume(3., 10., fa)).epsilon(rel_tol));
    const BoundingBoxf3 bb = r.mesh.bounding_box();
    CHECK(bb.size().x() == Approx(20.).margin(1e-4));
    CHECK(bb.size().y() == Approx(20.).margin(1e-4));
    CHECK(bb.size().z() == Approx(10.).margin(1e-4));
}

TEST_CASE("Boolean intersect sphere and box", "[SolidEdit]")
{
    const double fa = 2. * PI / 128.;
    TriangleMesh sphere = make_sphere(10., fa);
    // The box lies completely inside the sphere (corner distance 8.66 mm), so the intersection is the box.
    Result inside = boolean(BooleanOp::Intersect, sphere, { cube(10, 10, 10, Vec3d(-5, -5, -5)) });
    REQUIRE(inside.ok);
    CHECK(is_watertight(inside.mesh));
    CHECK(volume_of(inside.mesh) == Approx(1000.).epsilon(rel_tol));

    // A box that contains the whole sphere leaves the sphere unchanged.
    Result around = boolean(BooleanOp::Intersect, sphere, { cube(30, 30, 30, Vec3d(-15, -15, -15)) });
    REQUIRE(around.ok);
    CHECK(volume_of(around.mesh) == Approx(volume_of(sphere)).epsilon(rel_tol));

    // The two halves of a sphere cut at z = 0 add up to the whole sphere.
    TriangleMesh upper = cube(30, 30, 15, Vec3d(-15, -15, 0));
    Result top = boolean(BooleanOp::Intersect, sphere, { upper });
    Result bottom = boolean(BooleanOp::Subtract, sphere, { upper });
    REQUIRE(top.ok);
    REQUIRE(bottom.ok);
    CHECK(is_watertight(top.mesh));
    CHECK(is_watertight(bottom.mesh));
    CHECK(volume_of(top.mesh) + volume_of(bottom.mesh) == Approx(volume_of(sphere)).epsilon(rel_tol));
}

TEST_CASE("Boolean union of overlapping boxes", "[SolidEdit]")
{
    Result r = boolean(BooleanOp::Union, cube(10, 10, 10), { cube(10, 10, 10, Vec3d(5, 0, 0)) });
    REQUIRE(r.ok);
    CHECK(is_watertight(r.mesh));
    CHECK(volume_of(r.mesh) == Approx(1500.).epsilon(rel_tol));
    CHECK(r.mesh.bounding_box().size().x() == Approx(15.).margin(1e-4));
}

TEST_CASE("Boolean with nested solids", "[SolidEdit]")
{
    TriangleMesh outer = cube(10, 10, 10);
    TriangleMesh inner = cube(4, 4, 4, Vec3d(3, 3, 3));

    Result sub = boolean(BooleanOp::Subtract, outer, { inner });
    REQUIRE(sub.ok);
    CHECK(is_watertight(sub.mesh));
    CHECK(volume_of(sub.mesh) == Approx(1000. - 64.).epsilon(rel_tol));

    Result uni = boolean(BooleanOp::Union, outer, { inner });
    REQUIRE(uni.ok);
    CHECK(volume_of(uni.mesh) == Approx(1000.).epsilon(rel_tol));

    Result isect = boolean(BooleanOp::Intersect, outer, { inner });
    REQUIRE(isect.ok);
    CHECK(volume_of(isect.mesh) == Approx(64.).epsilon(rel_tol));
}

TEST_CASE("Boolean with touching solids", "[SolidEdit]")
{
    TriangleMesh a = cube(10, 10, 10);
    TriangleMesh b = cube(10, 10, 10, Vec3d(10, 0, 0));

    Result uni = boolean(BooleanOp::Union, a, { b });
    REQUIRE(uni.ok);
    CHECK(is_watertight(uni.mesh));
    CHECK(volume_of(uni.mesh) == Approx(2000.).epsilon(rel_tol));
    CHECK(uni.mesh.bounding_box().size().x() == Approx(20.).margin(1e-4));

    Result sub = boolean(BooleanOp::Subtract, a, { b });
    REQUIRE(sub.ok);
    CHECK(is_watertight(sub.mesh));
    CHECK(volume_of(sub.mesh) == Approx(1000.).epsilon(rel_tol));

    // Boxes that only share a face have no common volume, which is reported as an error.
    Result isect = boolean(BooleanOp::Intersect, a, { b });
    CHECK((! isect.ok || volume_of(isect.mesh) < 1e-3));
    if (! isect.ok)
        CHECK(! isect.error.empty());
}

TEST_CASE("Boolean with coplanar faces", "[SolidEdit]")
{
    // The tool shares the bottom, top and two side faces with the base.
    TriangleMesh base = cube(10, 10, 10);
    TriangleMesh tool = cube(5, 5, 10);

    Result sub = boolean(BooleanOp::Subtract, base, { tool });
    REQUIRE(sub.ok);
    CHECK(is_watertight(sub.mesh));
    CHECK(volume_of(sub.mesh) == Approx(750.).epsilon(rel_tol));

    Result isect = boolean(BooleanOp::Intersect, base, { tool });
    REQUIRE(isect.ok);
    CHECK(volume_of(isect.mesh) == Approx(250.).epsilon(rel_tol));

    Result uni = boolean(BooleanOp::Union, base, { tool });
    REQUIRE(uni.ok);
    CHECK(volume_of(uni.mesh) == Approx(1000.).epsilon(rel_tol));
}

TEST_CASE("Boolean rejects invalid input", "[SolidEdit]")
{
    SECTION("self-intersecting mesh")
    {
        indexed_triangle_set its = cube(10, 10, 10).its;
        its_merge(its, cube(10, 10, 10, Vec3d(5, 0, 0)).its);
        TriangleMesh bad(its);
        CHECK(! check_solid(bad, "Shape").empty());
        Result r = boolean(BooleanOp::Union, bad, { cube(1, 1, 1) });
        CHECK(! r.ok);
        CHECK(r.error.find("intersects itself") != std::string::npos);
    }
    SECTION("open mesh")
    {
        indexed_triangle_set its = cube(10, 10, 10).its;
        its.indices.pop_back();
        TriangleMesh open(its);
        Result r = boolean(BooleanOp::Subtract, cube(20, 20, 20), { open });
        CHECK(! r.ok);
        CHECK(r.error.find("not watertight") != std::string::npos);
    }
    SECTION("empty mesh")
    {
        Result r = boolean(BooleanOp::Union, cube(1, 1, 1), { TriangleMesh() });
        CHECK(! r.ok);
    }
    SECTION("no tools")
    {
        CHECK(! boolean(BooleanOp::Union, cube(1, 1, 1), {}).ok);
    }
    SECTION("valid solids pass")
    {
        CHECK(check_solid(cube(1, 1, 1), "Shape").empty());
    }
}

TEST_CASE("Boolean on parts keeps the base part settings", "[SolidEdit]")
{
    Model model;
    ModelObject *obj = make_object(model, cube(10, 10, 10));
    obj->volumes.front()->name = "base";
    obj->volumes.front()->config.set("extruder", 2);
    ModelVolume *tool = obj->add_volume(cube(10, 10, 10, Vec3d(5, 0, 0)));
    tool->config.set("extruder", 3);
    REQUIRE(obj->volumes.size() == 2);

    SECTION("subtract")
    {
        Result r = boolean_volumes(*obj, BooleanOp::Subtract, { 0, 1 });
        REQUIRE(r.ok);
        REQUIRE(obj->volumes.size() == 1);
        CHECK(part_volume(*obj->volumes.front()) == Approx(500.).epsilon(rel_tol));
        CHECK(obj->volumes.front()->name == "base");
        CHECK(obj->volumes.front()->config.opt_int("extruder") == 2);
        CHECK(part_bbox(*obj->volumes.front()).size().x() == Approx(5.).margin(1e-4));
    }
    SECTION("union")
    {
        Result r = boolean_volumes(*obj, BooleanOp::Union, { 0, 1 });
        REQUIRE(r.ok);
        REQUIRE(obj->volumes.size() == 1);
        CHECK(part_volume(*obj->volumes.front()) == Approx(1500.).epsilon(rel_tol));
    }
    SECTION("intersect")
    {
        Result r = boolean_volumes(*obj, BooleanOp::Intersect, { 0, 1 });
        REQUIRE(r.ok);
        REQUIRE(obj->volumes.size() == 1);
        CHECK(part_volume(*obj->volumes.front()) == Approx(500.).epsilon(rel_tol));
    }
    SECTION("the first selected part is the base even when it is not first in the object")
    {
        Result r = boolean_volumes(*obj, BooleanOp::Subtract, { 1, 0 });
        REQUIRE(r.ok);
        REQUIRE(obj->volumes.size() == 1);
        CHECK(part_volume(*obj->volumes.front()) == Approx(500.).epsilon(rel_tol));
        CHECK(obj->volumes.front()->config.opt_int("extruder") == 3);
        // The remaining half is the one that sticks out on the +X side: x 10..15 in object coordinates.
        // With a single part left its offset is folded into the instance, so check through both.
        const BoundingBoxf3 bb = obj->volumes.front()->mesh().transformed_bounding_box(
            obj->instances.front()->get_matrix() * obj->volumes.front()->get_matrix());
        CHECK(bb.min.x() == Approx(10.).margin(1e-4));
        CHECK(bb.max.x() == Approx(15.).margin(1e-4));
    }
    SECTION("failure leaves the object unchanged")
    {
        ModelVolume *open = obj->add_volume(cube(10, 10, 10));
        const_cast<TriangleMesh &>(open->mesh()).its.indices.pop_back();
        const size_t count = obj->volumes.size();
        Result r = boolean_volumes(*obj, BooleanOp::Subtract, { 0, 2 });
        CHECK(! r.ok);
        CHECK(! r.error.empty());
        CHECK(obj->volumes.size() == count);
        CHECK(part_volume(*obj->volumes.front()) == Approx(1000.).epsilon(rel_tol));
    }
    SECTION("only solid parts are accepted")
    {
        obj->add_volume(cube(1, 1, 1), ModelVolumeType::PARAMETER_MODIFIER);
        Result r = boolean_volumes(*obj, BooleanOp::Union, { 0, 2 });
        CHECK(! r.ok);
        CHECK(obj->volumes.size() == 3);
    }
}

TEST_CASE("Boolean on objects creates one object in bed coordinates", "[SolidEdit]")
{
    Model model;
    // Object space is 0..10 in every axis, so in bed coordinates A spans x 100..110 and B spans x 105..115.
    ModelObject *a = make_object(model, cube(10, 10, 10), Vec3d(100, 50, 5));
    ModelObject *b = make_object(model, cube(10, 10, 10), Vec3d(105, 50, 5));
    a->volumes.front()->config.set("extruder", 2);
    a->config.set("perimeters", 4);

    Result r = boolean_objects(model, BooleanOp::Subtract, { 0, 1 });
    REQUIRE(r.ok);
    REQUIRE(r.object != nullptr);
    CHECK(model.objects.size() == 3);
    CHECK(a == model.objects[0]);
    CHECK(b == model.objects[1]);

    ModelObject *res = r.object;
    REQUIRE(res->volumes.size() == 1);
    REQUIRE(res->instances.size() == 1);
    CHECK(part_volume(*res->volumes.front()) == Approx(500.).epsilon(rel_tol));
    CHECK(res->volumes.front()->config.opt_int("extruder") == 2);
    CHECK(res->config.opt_int("perimeters") == 4);

    // World position: B removes x 105..110 from A, leaving x 100..105.
    const Transform3d world = res->instances.front()->get_matrix() * res->volumes.front()->get_matrix();
    const BoundingBoxf3 bb = res->volumes.front()->mesh().transformed_bounding_box(world);
    CHECK(bb.min.x() == Approx(100.).margin(1e-3));
    CHECK(bb.max.x() == Approx(105.).margin(1e-3));
    CHECK(bb.size().y() == Approx(10.).margin(1e-3));
    CHECK(bb.size().z() == Approx(10.).margin(1e-3));
    CHECK(bb.min.y() == Approx(50.).margin(1e-3));
    CHECK(bb.min.z() == Approx(5.).margin(1e-3));

    SECTION("objects that do not overlap cannot be intersected")
    {
        make_object(model, cube(10, 10, 10), Vec3d(500, 500, 5));
        const size_t count = model.objects.size();
        Result bad = boolean_objects(model, BooleanOp::Intersect, { 0, 3 });
        CHECK(! bad.ok);
        CHECK(model.objects.size() == count);
    }
}

TEST_CASE("Apply negative volumes", "[SolidEdit]")
{
    const double fa = 2. * PI / 64.;
    Model model;
    ModelObject *obj = make_object(model, cube(20, 20, 10));
    obj->volumes.front()->config.set("extruder", 2);

    TriangleMesh cyl = make_cylinder(3., 14., fa);
    cyl.translate(10.f, 10.f, -2.f);
    ModelVolume *neg = obj->add_volume(std::move(cyl), ModelVolumeType::NEGATIVE_VOLUME);
    REQUIRE(neg->is_negative_volume());

    SECTION("single part")
    {
        Result r = apply_negative_volumes(*obj);
        REQUIRE(r.ok);
        REQUIRE(obj->volumes.size() == 1);
        const ModelVolume &v = *obj->volumes.front();
        CHECK(v.is_model_part());
        CHECK(part_volume(v) == Approx(4000. - cylinder_volume(3., 10., fa)).epsilon(rel_tol));
        CHECK(part_bbox(v).size().x() == Approx(20.).margin(1e-4));
        CHECK(part_bbox(v).size().z() == Approx(10.).margin(1e-4));
        CHECK(v.config.opt_int("extruder") == 2);
        CHECK(is_watertight(v.mesh()));
    }
    SECTION("each part keeps its own material")
    {
        // Two stacked parts and a cutter through both.
        Model stacked;
        ModelObject *o = make_object(stacked, cube(20, 20, 10));
        o->volumes.front()->config.set("extruder", 2);
        ModelVolume *second = o->add_volume(cube(20, 20, 10, Vec3d(0, 0, 10)));
        second->config.set("extruder", 4);
        TriangleMesh tall = make_cylinder(3., 30., fa);
        tall.translate(10.f, 10.f, -2.f);
        o->add_volume(std::move(tall), ModelVolumeType::NEGATIVE_VOLUME);

        Result r = apply_negative_volumes(*o);
        REQUIRE(r.ok);
        REQUIRE(o->volumes.size() == 2);
        const double expected = 4000. - cylinder_volume(3., 10., fa);
        std::set<int> extruders;
        for (const ModelVolume *v : o->volumes) {
            CHECK(v->is_model_part());
            CHECK(part_volume(*v) == Approx(expected).epsilon(rel_tol));
            extruders.insert(v->config.opt_int("extruder"));
        }
        CHECK(extruders == std::set<int>({ 2, 4 }));
    }
    SECTION("a negative volume that touches nothing is just removed")
    {
        obj->volumes.back()->set_offset(Vec3d(500, 500, 0));
        Result r = apply_negative_volumes(*obj);
        REQUIRE(r.ok);
        REQUIRE(obj->volumes.size() == 1);
        CHECK(part_volume(*obj->volumes.front()) == Approx(4000.).epsilon(rel_tol));
    }
    SECTION("a negative volume that swallows everything is refused")
    {
        obj->delete_volume(1);
        obj->add_volume(cube(100, 100, 100, Vec3d(-40, -40, -40)), ModelVolumeType::NEGATIVE_VOLUME);
        Result r = apply_negative_volumes(*obj);
        CHECK(! r.ok);
        CHECK(obj->volumes.size() == 2);
        CHECK(part_volume(*obj->volumes.front()) == Approx(4000.).epsilon(rel_tol));
    }
    SECTION("nothing to apply")
    {
        obj->delete_volume(1);
        CHECK(! apply_negative_volumes(*obj).ok);
    }
}

TEST_CASE("Straight channel volume", "[SolidEdit]")
{
    const double d = 2.;
    TriangleMesh box = cube(20, 20, 20);
    const std::vector<Vec3d> path { Vec3d(5, 10, 10), Vec3d(15, 10, 10) };

    SECTION("tool volume is pi r^2 L")
    {
        ChannelParams p;
        p.diameter = d;
        Result tool = make_channel_tool(path, p);
        REQUIRE(tool.ok);
        CHECK(volume_of(tool.mesh) == Approx(circle_area(d) * 10.).epsilon(rel_tol));
        const BoundingBoxf3 bb = tool.mesh.bounding_box();
        CHECK(bb.size().x() == Approx(10.).margin(1e-4));
        CHECK(bb.size().y() == Approx(d).epsilon(0.01));
    }
    SECTION("closed channel leaves a void inside")
    {
        ChannelParams p;
        p.diameter = d;
        Result r = subtract_channel(box, path, p);
        REQUIRE(r.ok);
        CHECK(is_watertight(r.mesh));
        CHECK(volume_of(r.mesh) == Approx(8000. - circle_area(d) * 10.).epsilon(rel_tol));
        CHECK(r.mesh.bounding_box().size().x() == Approx(20.).margin(1e-4));
        CHECK(its_split(r.mesh.its).size() == 2);
    }
    SECTION("inlet port opens the channel to the outside")
    {
        ChannelParams p;
        p.diameter = d;
        p.inlet_extension = 10.;
        Result r = subtract_channel(box, path, p);
        REQUIRE(r.ok);
        CHECK(is_watertight(r.mesh));
        // The channel now runs from the surface at x = 0 to x = 15.
        CHECK(volume_of(r.mesh) == Approx(8000. - circle_area(d) * 15.).epsilon(rel_tol));
        CHECK(its_split(r.mesh.its).size() == 1);
    }
    SECTION("inlet and outlet ports go through the solid")
    {
        ChannelParams p;
        p.diameter = d;
        p.inlet_extension = 10.;
        p.outlet_extension = 10.;
        Result r = subtract_channel(box, path, p);
        REQUIRE(r.ok);
        CHECK(is_watertight(r.mesh));
        CHECK(volume_of(r.mesh) == Approx(8000. - circle_area(d) * 20.).epsilon(rel_tol));
        CHECK(its_split(r.mesh.its).size() == 1);
    }
    SECTION("slanted channel")
    {
        ChannelParams p;
        p.diameter = d;
        const Vec3d a(4, 4, 4), b(16, 13, 4 + 12);
        const double len = (b - a).norm();
        Result r = subtract_channel(box, { a, b }, p);
        REQUIRE(r.ok);
        CHECK(volume_of(r.mesh) == Approx(8000. - circle_area(d) * len).epsilon(rel_tol));
    }
}

TEST_CASE("Bent channel volume is exact", "[SolidEdit]")
{
    ChannelParams p;
    p.diameter = 2.;
    TriangleMesh box = cube(20, 20, 20);

    SECTION("right angle bend: tool volume is pi r^2 times the path length")
    {
        const std::vector<Vec3d> path { Vec3d(5, 5, 10), Vec3d(15, 5, 10), Vec3d(15, 15, 10) };
        Result tool = make_channel_tool(path, p);
        REQUIRE(tool.ok);
        CHECK(is_watertight(tool.mesh));
        CHECK(volume_of(tool.mesh) == Approx(circle_area(2.) * 20.).epsilon(rel_tol));
        INFO(tool.error);
        Result res = subtract_channel(box, path, p);
        INFO(res.error);
        REQUIRE(res.ok);
        CHECK(is_watertight(res.mesh));
        CHECK(volume_of(res.mesh) == Approx(8000. - circle_area(2.) * 20.).epsilon(rel_tol));
        CHECK(its_split(res.mesh.its).size() == 2);
    }
    SECTION("three dimensional path with several bends")
    {
        const std::vector<Vec3d> path { Vec3d(4, 4, 4), Vec3d(12, 5, 6), Vec3d(15, 12, 9), Vec3d(8, 15, 15), Vec3d(5, 8, 16) };
        double length = 0.;
        for (size_t i = 1; i < path.size(); ++ i)
            length += (path[i] - path[i - 1]).norm();
        Result res = subtract_channel(box, path, p);
        INFO(res.error);
        REQUIRE(res.ok);
        CHECK(is_watertight(res.mesh));
        CHECK(volume_of(res.mesh) == Approx(8000. - circle_area(2.) * length).epsilon(rel_tol));
    }
    SECTION("collinear middle point does not change the volume")
    {
        Result res = subtract_channel(box, { Vec3d(5, 10, 10), Vec3d(10, 10, 10), Vec3d(15, 10, 10) }, p);
        REQUIRE(res.ok);
        CHECK(volume_of(res.mesh) == Approx(8000. - circle_area(2.) * 10.).epsilon(rel_tol));
    }
    SECTION("bends that fold back are refused with a plain message")
    {
        Result res = make_channel_tool({ Vec3d(0, 0, 0), Vec3d(10, 0, 0), Vec3d(0, 1, 0) }, p);
        CHECK(! res.ok);
        CHECK(res.error.find("too sharply") != std::string::npos);
    }
    SECTION("segments much shorter than the diameter are refused instead of producing a broken solid")
    {
        ChannelParams fat;
        fat.diameter = 10.;
        Result res = make_channel_tool({ Vec3d(0, 0, 0), Vec3d(1, 0, 0), Vec3d(1, 1, 0), Vec3d(2, 1, 0) }, fat);
        CHECK(! res.ok);
        CHECK(! res.error.empty());
    }
}

TEST_CASE("Channel in an object uses bed coordinates and keeps materials", "[SolidEdit]")
{
    Model model;
    // Object space is 0..20, so the instance offset puts the box at x 90..110, y 40..60, z 0..20 in bed coordinates.
    ModelObject *obj = make_object(model, cube(20, 20, 20), Vec3d(90, 40, 0));
    obj->volumes.front()->config.set("extruder", 2);
    ChannelParams p;
    p.diameter = 2.;
    p.inlet_extension = 10.;

    SECTION("channel is cut")
    {
        Result r = subtract_channel(*obj, 0, { Vec3d(95, 50, 10), Vec3d(105, 50, 10) }, p);
        REQUIRE(r.ok);
        REQUIRE(obj->volumes.size() == 1);
        CHECK(part_volume(*obj->volumes.front()) == Approx(8000. - circle_area(2.) * 15.).epsilon(rel_tol));
        CHECK(obj->volumes.front()->config.opt_int("extruder") == 2);
        CHECK(part_bbox(*obj->volumes.front()).size().x() == Approx(20.).margin(1e-3));
    }
    SECTION("channel outside the object is reported and nothing changes")
    {
        Result r = subtract_channel(*obj, 0, { Vec3d(0, 0, 10), Vec3d(10, 0, 10) }, p);
        CHECK(! r.ok);
        CHECK(r.error.find("does not touch") != std::string::npos);
        CHECK(part_volume(*obj->volumes.front()) == Approx(8000.).epsilon(rel_tol));
    }
    SECTION("channel through a multi-part object cuts each part")
    {
        ModelVolume *second = obj->add_volume(cube(20, 20, 20, Vec3d(20, 0, 0)));
        second->config.set("extruder", 3);
        Result r = subtract_channel(*obj, 0, { Vec3d(95, 50, 10), Vec3d(125, 50, 10) }, ChannelParams{ 2., 0., 0., 64 });
        REQUIRE(r.ok);
        REQUIRE(obj->volumes.size() == 2);
        double total = 0.;
        for (const ModelVolume *v : obj->volumes)
            total += part_volume(*v);
        CHECK(total == Approx(16000. - circle_area(2.) * 30.).epsilon(rel_tol));
    }
}

TEST_CASE("Channel input validation", "[SolidEdit]")
{
    ChannelParams p;
    p.diameter = 2.;
    CHECK(! make_channel_tool({ Vec3d(0, 0, 0) }, p).ok);
    CHECK(! make_channel_tool({ Vec3d(0, 0, 0), Vec3d(0, 0, 0) }, p).ok);
    ChannelParams zero = p;
    zero.diameter = 0.;
    CHECK(! make_channel_tool({ Vec3d(0, 0, 0), Vec3d(1, 0, 0) }, zero).ok);
    ChannelParams negative = p;
    negative.inlet_extension = -1.;
    CHECK(! make_channel_tool({ Vec3d(0, 0, 0), Vec3d(1, 0, 0) }, negative).ok);
    CHECK(make_channel_tool({ Vec3d(0, 0, 0), Vec3d(1, 0, 0) }, p).ok);
}

namespace {

// Volume of all solid parts of the object in bed coordinates.
double world_volume(const ModelObject &object, size_t instance_idx = 0)
{
    double total = 0.;
    for (const ModelVolume *v : object.volumes)
        if (v->is_model_part()) {
            TriangleMesh m = v->mesh();
            m.transform(object.instances[instance_idx]->get_matrix() * v->get_matrix(), true);
            total += volume_of(m);
        }
    return total;
}

} // namespace

TEST_CASE("Channel stays round and exact in a scaled or rotated instance", "[SolidEdit]")
{
    ChannelParams p;
    p.diameter = 2.;
    const double expected_removed = circle_area(2.) * 6.;

    SECTION("regression: non-uniform instance scale must not stretch the channel")
    {
        Model model;
        ModelObject *obj = make_object(model, cube(10, 10, 10));
        obj->instances.front()->set_scaling_factor(Vec3d(2., 1., 1.));
        REQUIRE(world_volume(*obj) == Approx(2000.).epsilon(rel_tol));
        // A closed channel along Y in the middle of the stretched box (bed x 0..20, y 0..10, z 0..10).
        Result r = subtract_channel(*obj, 0, { Vec3d(10, 2, 5), Vec3d(10, 8, 5) }, p);
        REQUIRE(r.ok);
        CHECK(world_volume(*obj) == Approx(2000. - expected_removed).epsilon(rel_tol));
    }
    SECTION("rotated instance")
    {
        Model model;
        ModelObject *obj = make_object(model, cube(10, 10, 10));
        obj->instances.front()->set_rotation(Z, PI / 2.);
        // Rotating 90 degrees about Z maps the box to bed x -10..0, y 0..10.
        Result r = subtract_channel(*obj, 0, { Vec3d(-8, 5, 5), Vec3d(-2, 5, 5) }, p);
        REQUIRE(r.ok);
        CHECK(world_volume(*obj) == Approx(1000. - expected_removed).epsilon(rel_tol));
    }
}

TEST_CASE("Boolean on a mirrored part", "[SolidEdit]")
{
    Model model;
    ModelObject *obj = make_object(model, cube(10, 10, 10));
    obj->volumes.front()->set_mirror(X, -1.);
    ModelVolume *tool = obj->add_volume(cube(10, 10, 10, Vec3d(0, 0, 5)));
    // Mirroring must not turn the part inside out.
    TriangleMesh mirrored = obj->volumes.front()->mesh();
    mirrored.transform(obj->volumes.front()->get_matrix(), true);
    REQUIRE(volume_of(mirrored) == Approx(1000.).epsilon(rel_tol));
    (void)tool;

    Result r = boolean_volumes(*obj, BooleanOp::Intersect, { 0, 1 });
    REQUIRE(r.ok);
    CHECK(is_watertight(r.mesh));
    CHECK(volume_of(r.mesh) > 0.);
}

TEST_CASE("Boolean with tiny features", "[SolidEdit]")
{
    SECTION("a 0.1 mm wall is kept to 0.1 percent")
    {
        // 10 mm box with a 9.8 mm cubic cavity: walls of 0.1 mm.
        Result r = boolean(BooleanOp::Subtract, cube(10, 10, 10), { cube(9.8, 9.8, 9.8, Vec3d(0.1, 0.1, 0.1)) });
        REQUIRE(r.ok);
        CHECK(is_watertight(r.mesh));
        CHECK(volume_of(r.mesh) == Approx(1000. - 9.8 * 9.8 * 9.8).epsilon(rel_tol));
        CHECK(r.mesh.bounding_box().size().x() == Approx(10.).margin(1e-4));
    }
    SECTION("a 0.01 mm deep notch removes exactly its volume")
    {
        // 2 x 2 mm notch, 0.01 mm deep, cut into the top face.
        Result r = boolean(BooleanOp::Subtract, cube(10, 10, 10), { cube(2, 2, 1, Vec3d(4, 4, 9.99)) });
        REQUIRE(r.ok);
        CHECK(is_watertight(r.mesh));
        CHECK(volume_of(r.mesh) == Approx(1000. - 2. * 2. * 0.01).margin(2e-3));
        CHECK(r.mesh.bounding_box().size().z() == Approx(10.).margin(1e-4));
    }
    SECTION("a 0.05 mm slit through the whole box")
    {
        Result r = boolean(BooleanOp::Subtract, cube(10, 10, 10), { cube(0.05, 20, 20, Vec3d(5, -5, -5)) });
        REQUIRE(r.ok);
        CHECK(is_watertight(r.mesh));
        CHECK(volume_of(r.mesh) == Approx(1000. - 0.05 * 10. * 10.).margin(2e-3));
        CHECK(its_split(r.mesh.its).size() == 2);
    }
}

TEST_CASE("Boolean with partly coplanar faces", "[SolidEdit]")
{
    TriangleMesh base = cube(10, 10, 10);

    SECTION("a smaller block standing on the top face")
    {
        Result r = boolean(BooleanOp::Union, base, { cube(5, 5, 5, Vec3d(2, 2, 10)) });
        REQUIRE(r.ok);
        CHECK(is_watertight(r.mesh));
        CHECK(volume_of(r.mesh) == Approx(1125.).epsilon(rel_tol));
        CHECK(r.mesh.bounding_box().size().z() == Approx(15.).margin(1e-4));
    }
    SECTION("a block flush with the top face removes nothing from the top")
    {
        Result r = boolean(BooleanOp::Subtract, base, { cube(5, 5, 5, Vec3d(2, 2, 5)) });
        REQUIRE(r.ok);
        CHECK(is_watertight(r.mesh));
        CHECK(volume_of(r.mesh) == Approx(875.).epsilon(rel_tol));
        CHECK(r.mesh.bounding_box().size().z() == Approx(10.).margin(1e-4));
    }
    SECTION("identical solids")
    {
        Result uni = boolean(BooleanOp::Union, base, { cube(10, 10, 10) });
        REQUIRE(uni.ok);
        CHECK(volume_of(uni.mesh) == Approx(1000.).epsilon(rel_tol));
        Result isect = boolean(BooleanOp::Intersect, base, { cube(10, 10, 10) });
        REQUIRE(isect.ok);
        CHECK(volume_of(isect.mesh) == Approx(1000.).epsilon(rel_tol));
        Result sub = boolean(BooleanOp::Subtract, base, { cube(10, 10, 10) });
        CHECK(! sub.ok);
        CHECK(! sub.error.empty());
    }
}

TEST_CASE("Non-manifold and inverted input is rejected with a plain message", "[SolidEdit]")
{
    SECTION("an edge shared by three faces")
    {
        indexed_triangle_set its = cube(10, 10, 10).its;
        its.indices.emplace_back(its.indices.front());
        TriangleMesh bad(its);
        Result r = boolean(BooleanOp::Union, bad, { cube(1, 1, 1) });
        CHECK(! r.ok);
        CHECK(! r.error.empty());
        CHECK(r.error.find("Repair") != std::string::npos);
    }
    SECTION("a turned inside out solid")
    {
        indexed_triangle_set its = cube(10, 10, 10).its;
        for (Vec3i &f : its.indices)
            std::swap(f[1], f[2]);
        TriangleMesh bad(its);
        Result r = boolean(BooleanOp::Subtract, cube(20, 20, 20), { bad });
        CHECK(! r.ok);
        CHECK(r.error.find("inside out") != std::string::npos);
    }
    SECTION("the tool is named in the message")
    {
        indexed_triangle_set its = cube(10, 10, 10).its;
        its.indices.pop_back();
        Result r = boolean(BooleanOp::Subtract, cube(20, 20, 20), { cube(1, 1, 1), TriangleMesh(its) });
        CHECK(! r.ok);
        CHECK(r.error.find("Shape 3") != std::string::npos);
    }
}

TEST_CASE("Whole object boolean keeps the world position with rotated and scaled instances", "[SolidEdit]")
{
    // Regression: the recentering shift is in object coordinates and the base object may carry an
    // origin translation from loading.
    Model model;
    ModelObject *a = make_object(model, cube(10, 10, 10), Vec3d(50, 20, 0));
    a->instances.front()->set_rotation(Z, PI / 2.);
    a->instances.front()->set_scaling_factor(Vec3d(2., 1., 1.));
    a->origin_translation = Vec3d(3., 4., 5.);

    const BoundingBoxf3 a_world = a->volumes.front()->mesh().transformed_bounding_box(
        a->instances.front()->get_matrix() * a->volumes.front()->get_matrix());
    // A big box that covers the part of A with x above its center.
    const double cx = a_world.center().x();
    ModelObject *b = make_object(model, cube(100, 100, 100, Vec3d(0, 0, 0)));
    b->instances.front()->set_offset(Vec3d(cx, -50, -50) - Vec3d(50, 50, 50) + Vec3d(50, 50, 50));
    const BoundingBoxf3 b_world = b->volumes.front()->mesh().transformed_bounding_box(
        b->instances.front()->get_matrix() * b->volumes.front()->get_matrix());
    REQUIRE(b_world.min.x() == Approx(cx).margin(1e-4));

    Result r = boolean_objects(model, BooleanOp::Intersect, { 0, 1 });
    REQUIRE(r.ok);
    const ModelObject &res = *r.object;
    const BoundingBoxf3 bb = res.volumes.front()->mesh().transformed_bounding_box(
        res.instances.front()->get_matrix() * res.volumes.front()->get_matrix());
    CHECK(bb.min.x() == Approx(cx).margin(1e-3));
    CHECK(bb.max.x() == Approx(a_world.max.x()).margin(1e-3));
    CHECK(bb.min.y() == Approx(a_world.min.y()).margin(1e-3));
    CHECK(bb.max.y() == Approx(a_world.max.y()).margin(1e-3));
    CHECK(bb.min.z() == Approx(a_world.min.z()).margin(1e-3));
    CHECK(bb.max.z() == Approx(a_world.max.z()).margin(1e-3));
    CHECK(res.origin_translation.norm() == Approx(0.).margin(1e-9));
    // Half of the stretched box: 2000 / 2 mm3.
    CHECK(r.measurement.volume_mm3 == Approx(1000.).epsilon(rel_tol));
}

TEST_CASE("Objects with several instances are refused", "[SolidEdit]")
{
    Model model;
    ModelObject *a = make_object(model, cube(10, 10, 10));
    a->add_instance()->set_offset(Vec3d(100, 0, 0));
    make_object(model, cube(10, 10, 10), Vec3d(5, 0, 0));
    const size_t objects = model.objects.size();

    Result r = boolean_objects(model, BooleanOp::Union, { 0, 1 });
    CHECK(! r.ok);
    CHECK(r.error.find("several instances") != std::string::npos);
    CHECK(model.objects.size() == objects);

    ChannelParams p;
    p.diameter = 1.;
    Result c = subtract_channel(*a, 0, { Vec3d(1, 5, 5), Vec3d(9, 5, 5) }, p);
    CHECK(! c.ok);
    CHECK(c.error.find("several instances") != std::string::npos);
    CHECK(part_volume(*a->volumes.front()) == Approx(1000.).epsilon(rel_tol));
}

TEST_CASE("Point list parsing is strict", "[SolidEdit]")
{
    std::vector<Vec3d> pts;
    std::string err;
    CHECK(parse_points("1 2 3\n4,5,6\n\n  7;8;9  \n", pts, err));
    REQUIRE(pts.size() == 3);
    CHECK(pts[1].isApprox(Vec3d(4, 5, 6)));
    CHECK(pts[2].isApprox(Vec3d(7, 8, 9)));
    // Regression: a mistyped line must be reported, not skipped as if it were blank.
    CHECK(! parse_points("1 2 3\nabc 1 2\n4 5 6", pts, err));
    CHECK(err.find("Line 2") != std::string::npos);
    CHECK(! parse_points("1 2 3\n1O 5 5\n4 5 6", pts, err));
    CHECK(err.find("Line 2") != std::string::npos);
    CHECK(! parse_points("1 2 3\n4 5\n", pts, err));
    CHECK(! parse_points("1 2 3 4\n5 6 7", pts, err));
    CHECK(! parse_points("1 2 3", pts, err));
    CHECK(err.find("at least two") != std::string::npos);
}

TEST_CASE("Subtracting several tools stops when nothing is left", "[SolidEdit]")
{
    // Regression: the second tool was applied to an empty mesh and the failure was reported as a touching problem.
    Result r = boolean(BooleanOp::Subtract, cube(10, 10, 10), { cube(20, 20, 20, Vec3d(-5, -5, -5)), cube(2, 2, 2) });
    CHECK(! r.ok);
    CHECK(r.error.find("result would be empty") != std::string::npos);
}

TEST_CASE("Measurement reports volume, millilitres and bounding box", "[SolidEdit]")
{
    // 20 x 20 x 10 mm = 4000 mm3 = 4 ml.
    Measurement m = measure(cube(20, 20, 10), Transform3d(Eigen::Translation3d(100., 50., 5.)));
    REQUIRE(m.valid);
    CHECK(m.volume_mm3 == Approx(4000.).epsilon(1e-6));
    CHECK(m.volume_ml == Approx(4.).epsilon(1e-6));
    CHECK(m.bbox.min.isApprox(Vec3d(100, 50, 5), 1e-5));
    CHECK(m.bbox.max.isApprox(Vec3d(120, 70, 15), 1e-5));
    CHECK(m.text() == "Volume 4000.00 mm3 (4.000 ml). Size 20.00 x 20.00 x 10.00 mm, from (100.00, 50.00, 5.00) to (120.00, 70.00, 15.00) mm.");
    CHECK(! measure(TriangleMesh()).valid);

    SECTION("every operation on a model reports it in bed coordinates")
    {
        Model model;
        ModelObject *a = make_object(model, cube(20, 20, 10), Vec3d(100, 50, 5));
        make_object(model, cube(10, 20, 10), Vec3d(110, 50, 5));
        Result r = boolean_objects(model, BooleanOp::Subtract, { 0, 1 });
        REQUIRE(r.ok);
        CHECK(r.measurement.volume_mm3 == Approx(2000.).epsilon(rel_tol));
        CHECK(r.measurement.volume_ml == Approx(2.).epsilon(rel_tol));
        CHECK(r.measurement.bbox.min.x() == Approx(100.).margin(1e-3));
        CHECK(r.measurement.bbox.max.x() == Approx(110.).margin(1e-3));
        (void)a;

        Model m2;
        ModelObject *o = make_object(m2, cube(20, 20, 20), Vec3d(90, 40, 0));
        ChannelParams p;
        p.diameter = 2.;
        Result c = subtract_channel(*o, 0, { Vec3d(95, 50, 10), Vec3d(105, 50, 10) }, p);
        REQUIRE(c.ok);
        CHECK(c.measurement.volume_mm3 == Approx(8000. - circle_area(2.) * 10.).epsilon(rel_tol));
        CHECK(c.measurement.bbox.min.x() == Approx(90.).margin(1e-3));
        CHECK(c.measurement.bbox.max.z() == Approx(20.).margin(1e-3));
    }
}

TEST_CASE("Boolean history is recorded, regenerated and edited", "[SolidEdit]")
{
    Model model;
    ModelObject *obj = make_object(model, cube(10, 10, 10));
    obj->volumes.front()->name = "block";
    ModelVolume *cut = obj->add_volume(cube(10, 10, 10, Vec3d(5, 0, 0)));
    cut->name = "cut";
    ModelVolume *add = obj->add_volume(cube(10, 10, 10, Vec3d(0, 0, 10)));
    add->name = "lid";
    REQUIRE(boolean_volumes(*obj, BooleanOp::Subtract, { 0, 1 }).ok);
    REQUIRE(obj->volumes.size() == 2);
    // The lid was selected after the first operation; continue the chain on the result.
    REQUIRE(boolean_volumes(*obj, BooleanOp::Union, { 0, 1 }).ok);
    REQUIRE(obj->volumes.size() == 1);
    CHECK(part_volume(*obj->volumes.front()) == Approx(500. + 1000.).epsilon(rel_tol));

    History h;
    REQUIRE(get_history(*obj->volumes.front(), h));
    REQUIRE(h.describe().size() == 3);
    CHECK(h.describe()[0] == "Base: block");
    CHECK(h.describe()[1] == "Subtract: cut");
    CHECK(h.describe()[2] == "Union: lid");

    SECTION("regenerating reproduces the part")
    {
        Result r = regenerate_volume(*obj, 0, h);
        REQUIRE(r.ok);
        CHECK(part_volume(*obj->volumes.front()) == Approx(1500.).epsilon(rel_tol));
        CHECK(r.measurement.volume_mm3 == Approx(1500.).epsilon(rel_tol));
    }
    SECTION("switching a step off and changing an operation re-edits the chain")
    {
        h.steps[0].enabled = false;
        Result off = regenerate_volume(*obj, 0, h);
        REQUIRE(off.ok);
        CHECK(h.describe()[1] == "Subtract: cut (off)");
        // Block plus lid, without the cut.
        CHECK(part_volume(*obj->volumes.front()) == Approx(2000.).epsilon(rel_tol));

        h.steps[0].enabled = true;
        h.steps[0].op = BooleanOp::Intersect;
        h.steps[1].enabled = false;
        Result isect = regenerate_volume(*obj, 0, h);
        REQUIRE(isect.ok);
        CHECK(part_volume(*obj->volumes.front()) == Approx(500.).epsilon(rel_tol));
        History stored;
        REQUIRE(get_history(*obj->volumes.front(), stored));
        CHECK(stored.describe()[1] == "Intersect: cut");
        CHECK(stored.describe()[2] == "Union: lid (off)");
    }
    SECTION("a failing step leaves the part unchanged and says which step failed")
    {
        h.steps[0].op = BooleanOp::Intersect;
        h.steps[1].op = BooleanOp::Intersect; // the lid does not overlap the intersection
        const double before = part_volume(*obj->volumes.front());
        Result r = regenerate_volume(*obj, 0, h);
        CHECK(! r.ok);
        CHECK(r.error.find("Step 2") != std::string::npos);
        CHECK(part_volume(*obj->volumes.front()) == Approx(before).epsilon(1e-9));
    }
    SECTION("moving the part does not break the chain")
    {
        obj->volumes.front()->set_offset(obj->volumes.front()->get_offset() + Vec3d(100., 0., 0.));
        const double min_x_before = part_bbox(*obj->volumes.front()).min.x();
        h.steps[1].enabled = false;
        REQUIRE(regenerate_volume(*obj, 0, h).ok);
        CHECK(part_volume(*obj->volumes.front()) == Approx(500.).epsilon(rel_tol));
        CHECK(part_bbox(*obj->volumes.front()).min.x() == Approx(min_x_before).margin(1e-3));
    }
    SECTION("text form round trips")
    {
        History back;
        REQUIRE(history_from_text(history_to_text(h), back));
        CHECK(back.describe() == h.describe());
        REQUIRE(back.steps.size() == h.steps.size());
        CHECK(volume_of(back.steps[0].mesh) == Approx(volume_of(h.steps[0].mesh)).epsilon(1e-6));
        CHECK(! history_from_text("garbage", back));
        CHECK(! history_from_text("csg1;a;1 0 0 0;5", back));
    }
    SECTION("names with unusual characters survive")
    {
        h.steps[0].name = "tool; \"A\" <1> & 100%";
        History back;
        REQUIRE(history_from_text(history_to_text(h), back));
        CHECK(back.steps[0].name == h.steps[0].name);
        CHECK(history_to_text(h).find_first_of("<>&\"'\n") == std::string::npos);
    }
    SECTION("history is saved in the 3mf file and survives copying the object")
    {
        const std::string file = std::string(TEST_DATA_DIR) + "/test_3mf/solid_edit_history.3mf";
        REQUIRE(store_3mf(file.c_str(), &model, nullptr, false));
        Model loaded;
        DynamicPrintConfig config;
        ConfigSubstitutionContext ctxt{ ForwardCompatibilitySubstitutionRule::Disable };
        boost::optional<Semver> version;
        const bool ok = load_3mf(file.c_str(), config, ctxt, &loaded, false, version);
        boost::filesystem::remove(file);
        REQUIRE(ok);
        REQUIRE(loaded.objects.size() == 1);
        History back;
        REQUIRE(get_history(*loaded.objects.front()->volumes.front(), back));
        CHECK(back.describe() == h.describe());

        // Regenerating after loading gives the same dimensions.
        REQUIRE(regenerate_volume(*loaded.objects.front(), 0, back).ok);
        CHECK(part_volume(*loaded.objects.front()->volumes.front()) == Approx(1500.).epsilon(rel_tol));

        Model copy = model;
        CHECK(copy.objects.front()->volumes.front()->csg_history == obj->volumes.front()->csg_history);
    }
}

TEST_CASE("History continues through negative volumes and channels", "[SolidEdit]")
{
    Model model;
    ModelObject *obj = make_object(model, cube(20, 20, 10));
    obj->volumes.front()->name = "slab";
    TriangleMesh cyl = make_cylinder(3., 14., 2. * PI / 64.);
    cyl.translate(10.f, 10.f, -2.f);
    obj->add_volume(std::move(cyl), ModelVolumeType::NEGATIVE_VOLUME)->name = "hole";
    REQUIRE(apply_negative_volumes(*obj).ok);
    ChannelParams p;
    p.diameter = 1.;
    p.inlet_extension = 20.;
    // Object space is 0..20 / 0..20 / 0..10 and the instance is at the origin.
    REQUIRE(subtract_channel(*obj, 0, { Vec3d(2, 2, 5), Vec3d(6, 2, 5) }, p).ok);

    History h;
    REQUIRE(get_history(*obj->volumes.front(), h));
    REQUIRE(h.describe().size() == 3);
    CHECK(h.describe()[0] == "Base: slab");
    CHECK(h.describe()[1] == "Subtract: hole");
    CHECK(h.describe()[2] == "Subtract: Channel 1 mm");

    const double expected = part_volume(*obj->volumes.front());
    // Switching off the channel gives the plate with only the hole.
    h.steps[1].enabled = false;
    REQUIRE(regenerate_volume(*obj, 0, h).ok);
    CHECK(part_volume(*obj->volumes.front()) > expected + 1.);
    const double hole_only = 4000. - cylinder_volume(3., 10., 2. * PI / 64.);
    CHECK(part_volume(*obj->volumes.front()) == Approx(hole_only).epsilon(rel_tol));
}
