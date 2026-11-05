#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_approx.hpp>

#include <regex>
#include <set>
#include <string>

#include <boost/filesystem.hpp>
#include <boost/nowide/fstream.hpp>
#include "nlohmann/json.hpp"

#include "libslic3r/ExperimentRecord.hpp"
#include "libslic3r/Model.hpp"
#include "libslic3r/ParameterSweep.hpp"
#include "libslic3r/Print.hpp"

#include "test_data.hpp"

using namespace Slic3r;
using Catch::Approx;
using json = nlohmann::json;

namespace {

// Two 20 mm cubes; the first has its own layer height and a droplet.
struct Experiment
{
    Model  model;
    Print  print;
    DynamicPrintConfig config;

    explicit Experiment(bool record)
    {
        config = DynamicPrintConfig::full_print_config();
        config.set_deserialize_strict({ { "skirts", 0 }, { "layer_height", 0.2 }, { "first_layer_height", 0.2 },
                                        { "experiment_record", record ? 1 : 0 }, { "fill_density", "25%" },
                                        { "print_settings_id", "My print" }, { "printer_settings_id", "My printer" },
                                        { "gcode_flavor", "klipper" }, { "nozzle_diameter", "0.4,0.4" }, { "channel_type", "fff;droplet" }, { "sla_material_extruder", "0,0" },
                                        { "filament_settings_id", "PLA;Water" } });
        Test::init_print({ Test::TestMesh::cube_20x20x20, Test::TestMesh::cube_20x20x20 }, print, model, config);
        model.objects[0]->name = "Copy 1";
        model.objects[1]->name = "Copy 2";
        model.objects[0]->config.set_key_value("layer_height", new ConfigOptionFloat(0.1));
        DropletSet set;
        set.name = "A1";
        set.extruder = 2;
        DropletPoint point;
        point.pos = Vec3d(1., 2., 0.5);
        point.volume_nl = 40.;
        set.points.push_back(point);
        model.objects[0]->droplet_sets.push_back(set);
        print.apply(model, config);
        print.set_status_silent();
    }
};

json read_json(const std::string &path)
{
    boost::nowide::ifstream file(path);
    return json::parse(file);
}

} // namespace

TEST_CASE("The experiment record path replaces the G-code extension", "[ExperimentRecord]")
{
    CHECK(experiment_record_path("dir/part.gcode") == (boost::filesystem::path("dir") / "part.experiment.json").string());
    CHECK(experiment_record_path("part.bgcode") == "part.experiment.json");
}

TEST_CASE("An export with the experiment record on writes what is needed to repeat it", "[ExperimentRecord]")
{
    Experiment e(true);
    const boost::filesystem::path gcode = boost::filesystem::unique_path(boost::filesystem::temp_directory_path() / "bioslicer_%%%%%%%%.gcode");
    e.print.process();
    e.print.export_gcode(gcode.string(), nullptr, nullptr);

    const std::string record_path = experiment_record_path(gcode.string());
    REQUIRE(boost::filesystem::exists(record_path));
    const json record = read_json(record_path);
    boost::filesystem::remove(gcode);
    boost::filesystem::remove(record_path);

    CHECK(std::regex_match(record["bioslicer_version"].get<std::string>(), std::regex("[0-9]+\\.[0-9]+\\.[0-9]+.*")));
    CHECK(record["presets"]["print_settings_id"] == "My print");
    CHECK(record["presets"]["printer_settings_id"] == "My printer");

    // Changed from default: the layer height, the infill and the record switch itself, but not an untouched setting.
    const json &changed = record["settings_changed_from_default"];
    CHECK(changed["fill_density"] == "25%");
    CHECK(changed["experiment_record"] == "1");
    CHECK(changed.contains("layer_height"));
    CHECK_FALSE(changed.contains("perimeters"));

    REQUIRE(record["objects"].size() == 2);
    const json &first = record["objects"][0];
    CHECK(first["name"] == "Copy 1");
    // The object's own layer height is recorded with its value in millimetres.
    CHECK(std::stod(first["settings"]["layer_height"].get<std::string>()) == Approx(0.1));
    CHECK(record["objects"][1]["settings"].empty());
    REQUIRE(first["instances"].size() == 1);
    CHECK(first["instances"][0]["scale"][0].get<double>() == Approx(1.));
    REQUIRE(first["volumes"].size() == 1);
    CHECK(first["volumes"][0]["size_mm"][0].get<double>() == Approx(20.).margin(1e-3));
    CHECK(first["volumes"][0]["type"] == "part");

    // The droplet is listed in bed coordinates: its place on the object plus the offset of the instance.
    REQUIRE(record["droplets"].size() == 1);
    const json &droplet = record["droplets"][0];
    CHECK(droplet["volume_nl"].get<double>() == Approx(40.));
    CHECK(droplet["z_mm"].get<double>() == Approx(0.5));
    CHECK(droplet["x_mm"].get<double>() == Approx(1. + record["objects"][0]["instances"][0]["offset_mm"][0].get<double>()).margin(1e-6));
    CHECK(droplet["object"] == 0);
}

TEST_CASE("An export with the experiment record off writes only the G-code", "[ExperimentRecord]")
{
    Experiment e(false);
    const boost::filesystem::path gcode = boost::filesystem::unique_path(boost::filesystem::temp_directory_path() / "bioslicer_%%%%%%%%.gcode");
    e.print.process();
    e.print.export_gcode(gcode.string(), nullptr, nullptr);
    CHECK(boost::filesystem::exists(gcode));
    CHECK_FALSE(boost::filesystem::exists(experiment_record_path(gcode.string())));
    boost::filesystem::remove(gcode);
}

TEST_CASE("An object with its own layer height is sliced with that layer height", "[ExperimentRecord]")
{
    Experiment e(false);
    e.print.process();
    REQUIRE(e.print.objects().size() == 2);
    // Object 0 has 0.1 mm layers and object 1 has 0.2 mm layers, so the first has twice as many.
    const PrintObject *a = e.print.objects()[0];
    const PrintObject *b = e.print.objects()[1];
    const PrintObject *fine   = a->layer_count() > b->layer_count() ? a : b;
    const PrintObject *coarse = fine == a ? b : a;
    CHECK(coarse->layer_count() == 100);
    // 19.8 mm above the first layer of 0.2 mm: 198 layers of 0.1 mm and the first.
    CHECK(fine->layer_count() == 199);
    CHECK(fine->layers().back()->print_z == Approx(20.).margin(1e-6));
    CHECK(fine->layers()[1]->print_z - fine->layers()[0]->print_z == Approx(0.1).margin(1e-6));
}

TEST_CASE("A layer height sweep shows in the G-code as layers of different heights", "[ExperimentRecord][Sweep]")
{
    SweepParams params;
    params.subject       = SweepSubject::LayerHeight;
    params.start         = 0.1;
    params.step          = 0.15;
    params.count         = 2;
    params.engrave_index = false;
    const SweepResult sweep = make_parameter_sweep(params);
    REQUIRE(sweep.valid());

    Model model;
    add_sweep_to_model(model, sweep, Vec2d(100., 100.));
    DynamicPrintConfig config = DynamicPrintConfig::full_print_config();
    config.set_deserialize_strict({ { "skirts", 0 }, { "layer_height", 0.3 }, { "first_layer_height", 0.1 }, { "perimeters", 1 } });
    Print print;
    print.apply(model, config);

    const std::string gcode = Test::gcode(print);
    std::set<int> heights;   // micrometres
    const std::regex z_move("\nG1 Z([0-9.]+)");
    for (std::sregex_iterator it(gcode.begin(), gcode.end(), z_move), end; it != end; ++it)
        heights.insert(int(std::lround(std::stod((*it)[1]) * 1000.)));
    // Copy 1 prints layers every 0.1 mm (0.2, 0.3, ...), copy 2 every 0.25 mm after the first (0.35, 0.6, ...).
    CHECK(heights.count(200) == 1);
    CHECK(heights.count(350) == 1);
    CHECK(heights.count(600) == 1);
}
