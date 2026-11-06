#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_approx.hpp>

#include <regex>
#include <sstream>

#include "libslic3r/Droplet.hpp"
#include "libslic3r/DropletPattern.hpp"
#include "libslic3r/ParameterSweep.hpp"
#include "libslic3r/TriangleMesh.hpp"
#include "libslic3r/Exception.hpp"
#include "libslic3r/Model.hpp"
#include "libslic3r/Print.hpp"

#include "test_data.hpp"

using namespace Slic3r;

namespace {

std::vector<std::string> split_lines(const std::string &gcode)
{
    std::vector<std::string> out;
    std::istringstream stream(gcode);
    for (std::string line; std::getline(stream, line); )
        out.push_back(line);
    return out;
}

bool has_prefix(const std::string &line, const std::string &prefix)
{
    return line.compare(0, prefix.size(), prefix) == 0;
}

size_t find_prefix(const std::vector<std::string> &lines, const std::string &prefix, size_t from = 0)
{
    for (size_t i = from; i < lines.size(); ++ i)
        if (has_prefix(lines[i], prefix))
            return i;
    return std::string::npos;
}

size_t count_prefix(const std::vector<std::string> &lines, const std::string &prefix)
{
    size_t n = 0;
    for (const std::string &line : lines)
        n += has_prefix(line, prefix);
    return n;
}

// Extruder 1 extrudes, extruder 2 cures resin, extruder 3 is a droplet channel.
DynamicPrintConfig droplet_config(std::initializer_list<Slic3r::ConfigBase::SetDeserializeItem> extra = {})
{
    DynamicPrintConfig config = Slic3r::DynamicPrintConfig::full_print_config_with({
        { "nozzle_diameter",              "0.4,0.4,0.4" },
        { "layer_height",                 0.4 },
        { "first_layer_height",           0.4 },
        { "skirts",                       0 },
        { "gcode_flavor",                 "klipper" },
        { "channel_type",                 "fff;sla;droplet" },
        { "filament_settings_id",         "PLA @X;Resin @X;Water @X" },
        { "sla_material_extruder",        "0,1,0" },
        { "sla_material_video_names",     "fdm;resin;water" },
        { "sla_material_video_paths",     "/tmp/fdm.mkv;/tmp/resin.mkv;/tmp/water.mkv" },
        { "sla_material_video_embed",     "0,0,0" },
        { "filament_droplet_nl_per_us",   "0.05,0.05,0.05" },
        { "filament_droplet_frequency",   "2,2,2" },
        { "droplet_standoff",             "2,2,2" },
        { "droplet_settle_time",          "0.2,0.2,0.2" },
        { "start_gcode",                  "PRINT_START" },
        { "toolchange_gcode",             "SELECT_MATERIAL MATERIAL=[next_material]" },
        { "toolchange_omit_tool_command", 1 }
    });
    config.set_deserialize_strict(extra);
    return config;
}

DropletSet make_set(int extruder, std::initializer_list<DropletPoint> points)
{
    DropletSet set;
    set.name     = "test";
    set.extruder = extruder;
    set.points   = points;
    return set;
}

DropletPoint point(double x, double y, double z, double volume_nl = 50., int count = 1)
{
    DropletPoint p;
    p.pos       = Vec3d(x, y, z);
    p.volume_nl = volume_nl;
    p.count     = count;
    return p;
}

} // namespace

TEST_CASE("Droplet sets survive their text form", "[Droplet]")
{
    DropletSets sets { make_set(3, { point(1.5, 2.25, 0.4), point(10, 20, 30, 12.5, 4) }), make_set(0, {}) };
    sets[0].dip        = true;
    sets[0].keep_order = true;
    sets[0].name       = "well A1 with spaces";
    const DropletSets read = droplet_sets_from_text(droplet_sets_to_text(sets));
    REQUIRE(read.size() == 2);
    CHECK(read == sets);
    CHECK(read[0].name == "well A1 with spaces");
}

TEST_CASE("The pulse of a droplet is its volume over the volume per pulse", "[Droplet]")
{
    CHECK(droplet_pulse_us(50., 0.05) == Catch::Approx(1000.));
    CHECK(droplet_pulse_us(50., 0.) == 0.);
}

TEST_CASE("Droplets are ordered for the shortest travel", "[Droplet]")
{
    auto at = [](double x, double y) { ResolvedDroplet d; d.pos = Vec3d(x, y, 0.); return d; };
    // On a line, shuffled: nearest neighbour from the origin walks the line in order.
    std::vector<ResolvedDroplet> drops { at(30, 0), at(10, 0), at(40, 0), at(20, 0) };
    order_droplets_by_travel(drops, Vec2d(0., 0.));
    REQUIRE(drops.size() == 4);
    for (size_t i = 0; i < drops.size(); ++ i)
        CHECK(drops[i].pos.x() == Catch::Approx(10. * double(i + 1)));
}

TEST_CASE("Droplets of an object follow its instances", "[Droplet]")
{
    DynamicPrintConfig config = droplet_config();
    PrintConfig print_config;
    print_config.apply(config, true);

    Model model;
    ModelObject *object = model.add_object();
    ModelInstance *first  = object->add_instance();
    ModelInstance *second = object->add_instance();
    first->set_offset(Vec3d(10., 0., 0.));
    second->set_offset(Vec3d(0., 50., 0.));
    object->droplet_sets.push_back(make_set(3, { point(1., 2., 3.) }));
    model.droplet_sets.push_back(make_set(0, { point(100., 100., 1.) }));

    size_t skipped = 0;
    const std::vector<ResolvedDroplet> drops = collect_droplets(model, print_config, &skipped);
    CHECK(skipped == 0);
    REQUIRE(drops.size() == 3);
    CHECK(drops[0].pos.isApprox(Vec3d(11., 2., 3.)));
    CHECK(drops[1].pos.isApprox(Vec3d(1., 52., 3.)));
    // A set without extruder goes to the first droplet channel.
    CHECK(drops[2].extruder == 3);
    CHECK(drops[2].pos.isApprox(Vec3d(100., 100., 1.)));

    // A set on a channel that is not a droplet channel is reported, not dispensed.
    model.droplet_sets.push_back(make_set(1, { point(5., 5., 5.), point(6., 6., 6.) }));
    skipped = 0;
    CHECK(collect_droplets(model, print_config, &skipped).size() == 3);
    CHECK(skipped == 2);
}

SCENARIO("Droplets are dispensed after the layer that reaches their height", "[Droplet]")
{
    // The SLA cube cures on extruder 2. A droplet lands at 2.0 mm, which is the 5th layer of 0.4 mm.
    DynamicPrintConfig config = droplet_config({
        { "perimeter_extruder", 2 }, { "infill_extruder", 2 }, { "solid_infill_extruder", 2 }
    });
    Print print;
    Model model;
    Slic3r::Test::init_print({ Slic3r::Test::TestMesh::cube_20x20x20 }, print, model, config);
    model.droplet_sets.push_back(make_set(3, { point(100., 100., 2.0, 50., 1), point(105., 100., 2.0, 25., 3) }));
    print.apply(model, config);
    REQUIRE(print.validate() == "");
    const std::vector<std::string> lines = split_lines(Slic3r::Test::gcode(print));

    THEN("the droplets come after the cure of their layer and before the next layer") {
        REQUIRE(count_prefix(lines, "BIOSLICER_DROPLET") == 2);
        const size_t first = find_prefix(lines, "BIOSLICER_DROPLET");

        // The layer containing the droplets is the one whose ;Z: is 2.
        size_t layer = std::string::npos;
        for (size_t i = 0; i < lines.size(); ++ i)
            if (lines[i] == ";LAYER_CHANGE" && i + 1 < lines.size() && has_prefix(lines[i + 1], ";Z:") &&
                std::abs(std::stod(lines[i + 1].substr(3)) - 2.0) < 0.001)
                layer = i;
        REQUIRE(layer != std::string::npos);
        const size_t next_layer = find_prefix(lines, ";LAYER_CHANGE", layer + 1);
        REQUIRE(first > layer);
        REQUIRE(first < next_layer);

        // After the exposure and the projector going dark again.
        const size_t frame = find_prefix(lines, "BIOSLICER_SLA_EXPOSE", layer);
        REQUIRE(frame < first);
        size_t clear = frame;
        while (lines[clear] != "SLA_CLEAR")
            ++ clear;
        CHECK(clear < first);
    }

    THEN("the droplet carries its position, its nozzle height and its volume, not a pulse time") {
        const size_t first = find_prefix(lines, "BIOSLICER_DROPLET");
        REQUIRE(first != std::string::npos);
        CHECK(lines[first] == "BIOSLICER_DROPLET EXTRUDER=2 X=100.000 Y=100.000 Z=4.000 SAFE_Z=4.000 VOLUME_NL=50 COUNT=1");
        CHECK(lines[first - 1].find("pulse_us=1000.0") != std::string::npos);
        CHECK(lines[find_prefix(lines, "BIOSLICER_DROPLET", first + 1)].find("COUNT=3") != std::string::npos);
    }

    THEN("the droplet material is selected by name and the layer height is restored afterwards") {
        const size_t first = find_prefix(lines, "BIOSLICER_DROPLET");
        const size_t select = find_prefix(lines, "SELECT_MATERIAL MATERIAL=DROPLET_WATER_X");
        REQUIRE(select != std::string::npos);
        CHECK(select < first);
        const size_t last = find_prefix(lines, "BIOSLICER_DROPLET", first + 1);
        const size_t back = find_prefix(lines, "G1 Z", last);
        REQUIRE(back < find_prefix(lines, ";LAYER_CHANGE", last));
    }

    THEN("the calibration reaches the printer as process parameters") {
        CHECK(find_prefix(lines, "SET_GCODE_VARIABLE MACRO=BIOSLICER_PROCESS_PARAMS VARIABLE=droplet_nl_per_us VALUE={2:0.05}") != std::string::npos);
        CHECK(find_prefix(lines, "SET_GCODE_VARIABLE MACRO=BIOSLICER_PROCESS_PARAMS VARIABLE=droplet_frequency VALUE={2:2}") != std::string::npos);
        // The droplet channel is no FFF extruder.
        CHECK(find_prefix(lines, "SET_GCODE_VARIABLE MACRO=BIOSLICER_PROCESS_PARAMS VARIABLE=fff_temp VALUE={0:200}") != std::string::npos);
    }
}

SCENARIO("A print of droplets alone exports", "[Droplet]")
{
    DynamicPrintConfig config = droplet_config();
    Print print;
    Model model;
    model.droplet_sets.push_back(make_set(3, {
        point(50., 50., 0.),
        point(60., 50., 0.),
        point(50., 60., 0.5, 80., 2)
    }));
    print.apply(model, config);
    print.set_status_silent();

    THEN("it validates without warnings, has a file name and every droplet is in the G-code") {
        std::vector<std::string> warnings;
        REQUIRE(print.validate(&warnings) == "");
        CHECK(warnings.empty());
        CHECK_NOTHROW(print.output_filename(""));
        const std::vector<std::string> lines = split_lines(Slic3r::Test::gcode(print));
        CHECK(count_prefix(lines, "BIOSLICER_DROPLET") == 3);
        CHECK(find_prefix(lines, "G1 X") == std::string::npos);
    }
}

SCENARIO("Droplets the valve cannot dispense are rejected", "[Droplet]")
{
    auto validate = [](double volume_nl, int count, const std::string &frequency = "2,2,2") {
        DynamicPrintConfig config = droplet_config({ { "filament_droplet_frequency", frequency } });
        Print print;
        Model model;
        model.droplet_sets.push_back(make_set(3, { point(50., 50., 0., volume_nl, count) }));
        print.apply(model, config);
        return print.validate();
    };

    CHECK(validate(50., 1) == "");
    // 1 nL is 20 us at 0.05 nL/us, below the 100 us the valve pulses.
    CHECK(validate(1., 1) != "");
    // 10 drops of 1000 us: 50 Hz is 5% duty, 200 Hz is 20%, above the 10% the valve allows.
    CHECK(validate(50., 10, "50,50,50") == "");
    CHECK(validate(50., 10, "200,200,200") != "");
}

TEST_CASE("A droplet grid keeps its volumes in place when it is run as a serpentine", "[Droplet]")
{
    DropletGridParams params;
    params.origin = Vec3d(10., 20., 0.5);
    params.cols = 3;
    params.rows = 2;
    params.pitch_x = 2.;
    params.pitch_y = 4.;
    params.volume_nl = 50.;
    params.volume_step_col = 10.;
    params.volume_step_row = 100.;
    params.serpentine = true;
    const DropletSet set = make_droplet_grid(params);
    REQUIRE(set.points.size() == 6);
    // Row 0 left to right, row 1 right to left.
    CHECK(set.points[0].pos.isApprox(Vec3d(10., 20., 0.5)));
    CHECK(set.points[2].pos.isApprox(Vec3d(14., 20., 0.5)));
    CHECK(set.points[3].pos.isApprox(Vec3d(14., 24., 0.5)));
    CHECK(set.points[5].pos.isApprox(Vec3d(10., 24., 0.5)));
    CHECK(set.points[0].volume_nl == Catch::Approx(50.));
    CHECK(set.points[3].volume_nl == Catch::Approx(50. + 20. + 100.));
    CHECK(set.points[5].volume_nl == Catch::Approx(50. + 100.));
}

TEST_CASE("A line of droplets includes both ends", "[Droplet]")
{
    const DropletSet set = make_droplet_line(Vec3d(0., 0., 1.), Vec3d(10., 0., 1.), 5, 30., 2);
    REQUIRE(set.points.size() == 5);
    CHECK(set.points.front().pos.isApprox(Vec3d(0., 0., 1.)));
    CHECK(set.points.back().pos.isApprox(Vec3d(10., 0., 1.)));
    CHECK(set.points[2].pos.x() == Catch::Approx(5.));
    CHECK(set.points[2].count == 2);
}

TEST_CASE("A wall along a polyline has the area of its length times its thickness", "[Droplet]")
{
    const Points line { scaled<coord_t>(Vec2d(0., 0.)), scaled<coord_t>(Vec2d(20., 0.)) };
    const ExPolygons wall = wall_footprint(line, false, 1.0);
    REQUIRE(wall.size() == 1);
    // Square end caps add half the thickness at each end.
    CHECK(unscaled(unscaled(wall.front().area())) == Catch::Approx(21.).margin(0.05));

    const indexed_triangle_set mesh = make_wall_mesh(wall, 0.2, 3.2);
    CHECK(its_volume(mesh) == Catch::Approx(21.f * 3.f).margin(0.5f));
    const BoundingBoxf3 bb = TriangleMesh(mesh).bounding_box();
    CHECK(bb.min.z() == Catch::Approx(0.2).margin(1e-4));
    CHECK(bb.max.z() == Catch::Approx(3.2).margin(1e-4));

    // A closed wall is a ring: it has a hole.
    const Points square { scaled<coord_t>(Vec2d(0., 0.)), scaled<coord_t>(Vec2d(10., 0.)), scaled<coord_t>(Vec2d(10., 10.)), scaled<coord_t>(Vec2d(0., 10.)) };
    const ExPolygons ring = wall_footprint(square, true, 1.0);
    REQUIRE(ring.size() == 1);
    CHECK(ring.front().holes.size() == 1);
}

TEST_CASE("A well array has a wall around every well and a droplet set in each", "[Droplet]")
{
    WellArrayParams params;
    params.cols = 3;
    params.rows = 2;
    params.pitch_x = 10.;
    params.pitch_y = 12.;
    params.inner_x = 6.;
    params.wall_thickness = 1.;
    params.wall_height = 3.;
    params.floor_thickness = 0.5;
    params.droplets_n = 2;
    const WellArray array = make_well_array(params);

    REQUIRE(array.droplets.size() == 6);
    CHECK(array.droplets[0].name == "A1");
    CHECK(array.droplets[2].name == "A3");
    CHECK(array.droplets[3].name == "B1");
    for (const DropletSet &set : array.droplets) {
        REQUIRE(set.points.size() == 4);
        for (const DropletPoint &p : set.points)
            CHECK(p.pos.z() == Catch::Approx(0.5));
    }

    const BoundingBoxf3 bb = TriangleMesh(array.mesh).bounding_box();
    CHECK(bb.min.z() == Catch::Approx(0.).margin(1e-4));
    // The wall stands on the floor.
    CHECK(bb.max.z() == Catch::Approx(3.5).margin(1e-4));
    CHECK(bb.min.x() == Catch::Approx(-4.).margin(0.05));
    CHECK(bb.max.x() == Catch::Approx(24.).margin(0.05));
    CHECK(its_volume(array.mesh) > 0.f);

    // Every droplet lies inside its well: within the inner radius of its centre.
    for (size_t w = 0; w < array.droplets.size(); ++ w) {
        const Vec2d centre((w % 3) * 10., (w / 3) * 12.);
        for (const DropletPoint &p : array.droplets[w].points)
            CHECK((Vec2d(p.pos.x(), p.pos.y()) - centre).norm() < 3.);
    }
}

namespace {

// Area of the regular n-gon of circumradius r, which is what a round well is made of.
double polygon_area(int n, double r) { return 0.5 * n * r * r * std::sin(2. * 3.14159265358979323846 / n); }

} // namespace

TEST_CASE("A round well wall has the volume of its ring times its height", "[Droplet]")
{
    WellArrayParams params;
    params.shape = WellShape::Round;
    params.cols = 1;
    params.rows = 1;
    params.inner_x = 6.;
    params.wall_thickness = 1.;
    params.wall_height = 3.;
    params.floor_thickness = 0.;
    const WellArray array = make_well_array(params);

    // Inner radius 3 mm, outer radius 4 mm, both 96-gons.
    const double ring = polygon_area(96, 4.) - polygon_area(96, 3.);
    CHECK(double(its_volume(array.mesh)) == Catch::Approx(ring * 3.).epsilon(2e-3));
    const BoundingBoxf3 bb = TriangleMesh(array.mesh).bounding_box();
    CHECK(bb.min.z() == Catch::Approx(0.).margin(1e-4));
    CHECK(bb.max.z() == Catch::Approx(3.).margin(1e-4));
    CHECK(bb.size().x() == Catch::Approx(8.).margin(0.02));
    CHECK(bb.size().y() == Catch::Approx(8.).margin(0.02));

    // With no floor the droplet lands on the bed, in the centre of the well.
    REQUIRE(array.droplets.size() == 1);
    REQUIRE(array.droplets.front().points.size() == 1);
    CHECK(array.droplets.front().points.front().pos.isApprox(Vec3d(0., 0., 0.), 1e-9));
}

TEST_CASE("A rectangular well wall has the volume of its ring times its height", "[Droplet]")
{
    WellArrayParams params;
    params.shape = WellShape::Rectangular;
    params.cols = 1;
    params.rows = 1;
    params.inner_x = 6.;
    params.inner_y = 4.;
    params.wall_thickness = 1.;
    params.wall_height = 5.;
    params.floor_thickness = 0.;
    const WellArray array = make_well_array(params);

    // Outer 8 x 6 mm minus inner 6 x 4 mm.
    CHECK(double(its_volume(array.mesh)) == Catch::Approx((8. * 6. - 6. * 4.) * 5.).epsilon(1e-3));
    const BoundingBoxf3 bb = TriangleMesh(array.mesh).bounding_box();
    CHECK(bb.size().x() == Catch::Approx(8.).margin(1e-3));
    CHECK(bb.size().y() == Catch::Approx(6.).margin(1e-3));
}

TEST_CASE("Neighbouring wells share the wall where their walls overlap", "[Droplet]")
{
    WellArrayParams params;
    params.shape = WellShape::Rectangular;
    params.cols = 2;
    params.rows = 1;
    params.inner_x = 6.;
    params.inner_y = 6.;
    params.wall_thickness = 1.;
    params.wall_height = 4.;
    params.floor_thickness = 0.;

    // Centres 8 mm apart: the walls (outer 8 mm) touch and share one 1 mm thick wall.
    params.pitch_x = 8.;
    const WellArray shared = make_well_array(params);
    // Union outline 16 x 8 minus two 6 x 6 wells.
    CHECK(double(its_volume(shared.mesh)) == Catch::Approx((16. * 8. - 2. * 36.) * 4.).epsilon(1e-3));

    // Centres 10 mm apart: separate walls, each 8 x 8 minus 6 x 6.
    params.pitch_x = 10.;
    const WellArray apart = make_well_array(params);
    CHECK(double(its_volume(apart.mesh)) == Catch::Approx(2. * (64. - 36.) * 4.).epsilon(1e-3));
}

TEST_CASE("Wells are named like a plate, past row Z as well", "[Droplet]")
{
    WellArrayParams params;
    params.cols = 1;
    params.rows = 28;
    const WellArray array = make_well_array(params);
    REQUIRE(array.droplets.size() == 28);
    CHECK(array.droplets[0].name == "A1");
    CHECK(array.droplets[25].name == "Z1");
    CHECK(array.droplets[26].name == "AA1");
    CHECK(array.droplets[27].name == "AB1");
}

TEST_CASE("Droplet grids and lines clamp their parameters", "[Droplet]")
{
    SECTION("A negative volume step stops at zero") {
        DropletGridParams params;
        params.cols = 3;
        params.rows = 1;
        params.volume_nl = 10.;
        params.volume_step_col = -8.;
        const DropletSet set = make_droplet_grid(params);
        REQUIRE(set.points.size() == 3);
        CHECK(set.points[0].volume_nl == Catch::Approx(10.));
        CHECK(set.points[1].volume_nl == Catch::Approx(2.));
        CHECK(set.points[2].volume_nl == Catch::Approx(0.));
    }
    SECTION("Fewer than one row or column still gives one droplet") {
        DropletGridParams params;
        params.cols = 0;
        params.rows = -3;
        params.origin = Vec3d(1., 2., 3.);
        const DropletSet set = make_droplet_grid(params);
        REQUIRE(set.points.size() == 1);
        CHECK(set.points.front().pos.isApprox(Vec3d(1., 2., 3.)));
    }
    SECTION("A line of one droplet sits at its start") {
        const DropletSet set = make_droplet_line(Vec3d(1., 2., 3.), Vec3d(9., 9., 9.), 1, 20., 1);
        REQUIRE(set.points.size() == 1);
        CHECK(set.points.front().pos.isApprox(Vec3d(1., 2., 3.)));
    }
    SECTION("The pitch of a grid is the distance between neighbours") {
        DropletGridParams params;
        params.cols = 4;
        params.rows = 3;
        params.pitch_x = 1.5;
        params.pitch_y = 2.5;
        const DropletSet set = make_droplet_grid(params);
        REQUIRE(set.points.size() == 12);
        CHECK((set.points[1].pos - set.points[0].pos).norm() == Catch::Approx(1.5));
        CHECK((set.points[4].pos - set.points[0].pos).norm() == Catch::Approx(2.5));
        CHECK(set.points.back().pos.isApprox(Vec3d(4.5, 5., 0.)));
    }
}

TEST_CASE("Walls that have no length, thickness or height are empty", "[Droplet]")
{
    const Points one_point { scaled<coord_t>(Vec2d(0., 0.)) };
    CHECK(wall_footprint(one_point, false, 1.).empty());
    const Points line { scaled<coord_t>(Vec2d(0., 0.)), scaled<coord_t>(Vec2d(5., 0.)) };
    CHECK(wall_footprint(line, false, 0.).empty());
    const ExPolygons wall = wall_footprint(line, false, 1.);
    REQUIRE_FALSE(wall.empty());
    CHECK(make_wall_mesh(wall, 2., 2.).indices.empty());
    CHECK(make_wall_mesh(wall, 3., 2.).indices.empty());
    CHECK(make_wall_mesh({}, 0., 2.).indices.empty());
}

SCENARIO("A droplet volume sweep writes the volume of every copy into the G-code", "[Droplet][Sweep]")
{
    SweepParams params;
    params.subject       = SweepSubject::DropletVolume;
    params.start         = 25.;
    params.step          = 25.;
    params.count         = 3;
    params.engrave_index = false;
    const SweepResult sweep = make_parameter_sweep(params);
    REQUIRE(sweep.valid());

    Model model;
    add_sweep_to_model(model, sweep, Vec2d(100., 100.), 3);
    Print print;
    print.apply(model, droplet_config());
    print.set_status_silent();
    std::vector<std::string> warnings;
    REQUIRE(print.validate(&warnings) == "");

    const std::vector<std::string> lines = split_lines(Slic3r::Test::gcode(print));
    THEN("every copy has its droplet at its own place with its own volume") {
        std::vector<std::pair<double, double>> found;   // x, volume
        const std::regex pattern("BIOSLICER_DROPLET .* X=([0-9.]+) .* VOLUME_NL=([0-9.]+) ");
        for (const std::string &line : lines) {
            std::smatch match;
            if (has_prefix(line, "BIOSLICER_DROPLET") && std::regex_search(line, match, pattern))
                found.emplace_back(std::stod(match[1]), std::stod(match[2]));
        }
        REQUIRE(found.size() == 3);
        for (const SweepItem &item : sweep.items) {
            const double centre_x = 100. + item.offset.x();
            size_t matches = 0;
            for (const auto &[x, volume] : found)
                if (std::abs(volume - item.value) < 1e-6) {
                    ++matches;
                    CHECK(std::abs(x - centre_x) < 0.5 * item.size.x());
                }
            CHECK(matches == 1);
        }
    }
}
