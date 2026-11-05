#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_approx.hpp>
#include <test_utils.hpp>

#include <libslic3r/Model.hpp>
#include <libslic3r/SolidEdit.hpp>
#include <libslic3r/TriangleMesh.hpp>

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

TEST_CASE("Bent channel volume is bounded", "[SolidEdit]")
{
    ChannelParams p;
    p.diameter = 2.;
    const double r = 1.;
    TriangleMesh box = cube(20, 20, 20);
    Result res = subtract_channel(box, { Vec3d(5, 5, 10), Vec3d(15, 5, 10), Vec3d(15, 15, 10) }, p);
    REQUIRE(res.ok);
    CHECK(is_watertight(res.mesh));
    const double removed = 8000. - volume_of(res.mesh);
    // Two legs of 10 mm overlap in a quarter of a Steinmetz solid (4/3 r^3) at the corner, and the sphere
    // adds at most its own volume.
    CHECK(removed >= circle_area(2.) * 20. - 4. / 3. * r * r * r - 1e-3);
    CHECK(removed <= circle_area(2.) * 20. + 4. / 3. * PI * r * r * r);
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
