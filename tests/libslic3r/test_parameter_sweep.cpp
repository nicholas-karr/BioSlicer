#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_approx.hpp>

#include <algorithm>
#include <cmath>
#include <sstream>
#include <string>
#include <vector>

#include "libslic3r/Model.hpp"
#include "libslic3r/ParameterSweep.hpp"
#include "libslic3r/TriangleMesh.hpp"

using namespace Slic3r;
using Catch::Approx;

namespace {

constexpr double pi = 3.14159265358979323846;

std::vector<std::vector<std::string>> parse_csv(const std::string &csv)
{
    std::vector<std::vector<std::string>> rows;
    std::istringstream lines(csv);
    for (std::string line; std::getline(lines, line); ) {
        std::vector<std::string> cells;
        std::istringstream cols(line);
        for (std::string cell; std::getline(cols, cell, ','); )
            cells.emplace_back(cell);
        if (! line.empty() && line.back() == ',')
            cells.emplace_back();
        rows.emplace_back(std::move(cells));
    }
    return rows;
}

double circle_area(double diameter)
{
    const double r = diameter * 0.5;
    return 0.5 * kCouponCircleSegments * r * r * std::sin(2. * pi / kCouponCircleSegments);
}

SweepParams channel_sweep(int count, bool engrave)
{
    SweepParams p;
    p.subject       = SweepSubject::ChannelDiameter;
    p.start         = 0.6;
    p.step          = 0.2;
    p.count         = count;
    p.engrave_index = engrave;
    return p;
}

} // namespace

TEST_CASE("A channel diameter sweep steps the diameter and keeps the rest", "[Sweep]")
{
    SweepParams p = channel_sweep(4, false);
    const SweepResult sweep = make_parameter_sweep(p);
    REQUIRE(sweep.valid());
    REQUIRE(sweep.items.size() == 4);
    CHECK(sweep.parameter == "channel diameter");
    CHECK(sweep.unit == "mm");

    // Default block: walls of 1.5 mm around one channel, 15 mm long.
    for (size_t i = 0; i < 4; ++i) {
        const SweepItem &item = sweep.items[i];
        const double d = 0.6 + 0.2 * double(i);
        CHECK(item.index == int(i) + 1);
        CHECK(item.value == Approx(d));
        CHECK(item.size.x() == Approx(d + 3.).margin(1e-4));
        CHECK(item.size.y() == Approx(15.).margin(1e-4));
        CHECK(item.size.z() == Approx(d + 3.).margin(1e-4));
        CHECK(item.volume_mm3 == Approx(((d + 3.) * (d + 3.) - circle_area(d)) * 15.).epsilon(1e-3));
    }
}

TEST_CASE("The CSV lists every copy with the numbers used to make it", "[Sweep]")
{
    const SweepParams p = channel_sweep(3, true);
    const SweepResult sweep = make_parameter_sweep(p);
    REQUIRE(sweep.valid());

    const auto rows = parse_csv(sweep.csv);
    REQUIRE(rows.size() == 4);
    const std::vector<std::string> header { "index", "label", "subject", "parameter", "value", "unit", "x_mm", "y_mm",
                                            "width_mm", "depth_mm", "height_mm", "volume_mm3", "droplets", "layer_height_mm" };
    CHECK(rows[0] == header);
    for (size_t i = 1; i < rows.size(); ++i) {
        REQUIRE(rows[i].size() == header.size());
        const SweepItem &item = sweep.items[i - 1];
        CHECK(std::stoi(rows[i][0]) == item.index);
        CHECK(rows[i][1] == std::to_string(item.index));
        CHECK(rows[i][2] == "channel");
        CHECK(rows[i][3] == "channel diameter");
        CHECK(std::stod(rows[i][4]) == Approx(0.6 + 0.2 * double(i - 1)).margin(5e-4));
        CHECK(rows[i][5] == "mm");
        CHECK(std::stod(rows[i][6]) == Approx(item.plate_position.x()).margin(5e-4));
        CHECK(std::stod(rows[i][7]) == Approx(item.plate_position.y()).margin(5e-4));
        CHECK(std::stod(rows[i][8]) == Approx(item.size.x()).margin(5e-4));
        CHECK(std::stod(rows[i][9]) == Approx(item.size.y()).margin(5e-4));
        CHECK(std::stod(rows[i][10]) == Approx(item.size.z()).margin(5e-4));
        CHECK(std::stod(rows[i][11]) == Approx(item.volume_mm3).margin(5e-4));
        CHECK(rows[i][12] == "0");
        // The layer height column is empty unless the layer height is what was stepped.
        CHECK(rows[i].back().empty());
    }
    // The values have three decimals and a point, whatever the locale.
    CHECK(rows[1][4] == "0.600");
    CHECK(rows[3][4] == "1.000");
    CHECK(sweep.csv.find('\r') == std::string::npos);
    CHECK(sweep.csv.back() == '\n');
}

TEST_CASE("Every copy has its index engraved in a label strip", "[Sweep]")
{
    SweepParams plain = channel_sweep(12, false);
    SweepParams labelled = plain;
    labelled.engrave_index = true;
    const SweepResult a = make_parameter_sweep(plain);
    const SweepResult b = make_parameter_sweep(labelled);
    REQUIRE(a.valid());
    REQUIRE(b.valid());

    const double h      = labelled.label_height;
    const double margin = h / 3.;
    for (size_t i = 0; i < 12; ++i) {
        const std::string label = std::to_string(i + 1);
        CHECK(b.items[i].label == label);
        const double text_w  = double(label.size()) * 0.6 * h + double(label.size() - 1) * 0.3 * h;
        const double plate_w = std::max(a.items[i].size.x(), text_w + 2. * margin);
        const double plate_d = h + 2. * margin;
        // The strip is in front of the piece, and the copy is as wide as the wider of the two.
        CHECK(b.items[i].size.x() == Approx(plate_w).margin(1e-4));
        CHECK(b.items[i].size.y() == Approx(a.items[i].size.y() + plate_d).margin(1e-4));
        // Strip volume minus the engraved digits, to a depth of 0.6 mm.
        const double strip = plate_w * plate_d * labelled.label_thickness - sweep_label_area(label, h) * labelled.label_depth;
        CHECK(b.items[i].volume_mm3 - a.items[i].volume_mm3 == Approx(strip).epsilon(1e-3));
        CHECK(sweep_label_area(label, h) > 0.);
    }
}

TEST_CASE("The engraved area of a digit follows from its strokes", "[Sweep]")
{
    // Height 3 mm: strokes 0.5 mm thick, 0.75 mm long between strokes, 1.8 mm wide.
    CHECK(sweep_label_area("8", 3.) == Approx(3. * 1.8 * 0.5 + 4. * 0.5 * 0.75).epsilon(1e-4));
    CHECK(sweep_label_area("1", 3.) == Approx(2. * 0.5 * 0.75 + 0.25).epsilon(1e-4));
    CHECK(sweep_label_area("0", 3.) == Approx(2. * 1.8 * 0.5 + 4. * 0.5 * 0.75 + 2. * 0.25).epsilon(1e-4));
    // Digits do not touch, so the area of a number is the sum of its digits.
    CHECK(sweep_label_area("10", 3.) == Approx(sweep_label_area("1", 3.) + sweep_label_area("0", 3.)).epsilon(1e-4));
    // Twice the height is four times the area.
    CHECK(sweep_label_area("8", 6.) == Approx(4. * sweep_label_area("8", 3.)).epsilon(1e-4));
    // Every digit has a different shape, so no two have the same area unless their strokes add up alike.
    CHECK(sweep_label_area("7", 3.) == Approx(sweep_label_area("1", 3.) + 1.8 * 0.5).epsilon(1e-4));
}

TEST_CASE("Copies are arranged in rows on the plate without touching", "[Sweep]")
{
    SweepParams p = channel_sweep(12, true);
    p.plate_size = Vec2d(60., 100.);
    p.spacing    = 4.;
    const SweepResult sweep = make_parameter_sweep(p);
    REQUIRE(sweep.valid());
    CHECK(sweep.fits);
    CHECK(sweep.arrangement_size.x() <= 60. + 1e-9);
    CHECK(sweep.arrangement_size.y() <= 100. + 1e-9);

    for (size_t i = 0; i < sweep.items.size(); ++i) {
        const SweepItem &a = sweep.items[i];
        // On the plate.
        CHECK(a.plate_position.x() - a.size.x() * 0.5 >= -1e-9);
        CHECK(a.plate_position.x() + a.size.x() * 0.5 <= 60. + 1e-9);
        CHECK(a.plate_position.y() - a.size.y() * 0.5 >= -1e-9);
        CHECK(a.plate_position.y() + a.size.y() * 0.5 <= 100. + 1e-9);
        for (size_t j = i + 1; j < sweep.items.size(); ++j) {
            const SweepItem &b = sweep.items[j];
            const double gap_x = std::abs(a.plate_position.x() - b.plate_position.x()) - 0.5 * (a.size.x() + b.size.x());
            const double gap_y = std::abs(a.plate_position.y() - b.plate_position.y()) - 0.5 * (a.size.y() + b.size.y());
            // Either a column or a row apart by at least the spacing.
            CHECK(std::max(gap_x, gap_y) >= 4. - 1e-9);
        }
    }

    // The arrangement is centred: the extremes on either side of the plate centre are equal.
    double min_x = 1e9, max_x = -1e9;
    for (const SweepItem &item : sweep.items) {
        min_x = std::min(min_x, item.plate_position.x() - item.size.x() * 0.5);
        max_x = std::max(max_x, item.plate_position.x() + item.size.x() * 0.5);
    }
    CHECK(0.5 * (min_x + max_x) == Approx(30.).margin(3.));
}

TEST_CASE("Copies that do not fit the plate are reported", "[Sweep]")
{
    SweepParams p = channel_sweep(12, true);
    p.plate_size = Vec2d(30., 30.);
    const SweepResult sweep = make_parameter_sweep(p);
    CHECK_FALSE(sweep.fits);
    CHECK_FALSE(sweep.valid());
    CHECK(sweep.error.find("do not fit") != std::string::npos);
    CHECK(sweep.items.size() == 12);
}

TEST_CASE("A wall thickness sweep makes one wall per copy", "[Sweep]")
{
    SweepParams p;
    p.subject       = SweepSubject::WallThickness;
    p.start         = 0.3;
    p.step          = 0.1;
    p.count         = 5;
    p.engrave_index = false;
    const SweepResult sweep = make_parameter_sweep(p);
    REQUIRE(sweep.valid());
    CHECK(sweep.parameter == "wall thickness");
    for (size_t i = 0; i < 5; ++i) {
        const double t = 0.3 + 0.1 * double(i);
        // Default piece: a 20 mm long, 10 mm tall wall on a 1.5 mm base with 3 mm around it.
        const double base = (t + 6.) * 26. * 1.5;
        CHECK(sweep.items[i].volume_mm3 == Approx(base + t * 20. * 10.).epsilon(1e-4));
        CHECK(sweep.items[i].size.z() == Approx(11.5).margin(1e-4));
    }
}

TEST_CASE("A pitch sweep moves two channels apart", "[Sweep]")
{
    SweepParams p;
    p.subject       = SweepSubject::ChannelPitch;
    p.start         = 2.;
    p.step          = 0.5;
    p.count         = 4;
    p.engrave_index = false;
    const SweepResult sweep = make_parameter_sweep(p);
    REQUIRE(sweep.valid());
    CHECK(sweep.parameter == "channel pitch");
    const double d = p.channel.first_width;
    for (size_t i = 0; i < 4; ++i) {
        const double pitch = 2. + 0.5 * double(i);
        CHECK(sweep.items[i].size.x() == Approx(pitch + d + 2. * p.channel.wall).margin(1e-4));
        const double height = d + 2. * p.channel.wall;
        CHECK(sweep.items[i].volume_mm3
            == Approx(((pitch + d + 2. * p.channel.wall) * height - 2. * circle_area(d)) * p.channel.length).epsilon(1e-3));
    }
}

TEST_CASE("A droplet volume sweep changes the droplet and not the well", "[Sweep]")
{
    SweepParams p;
    p.subject       = SweepSubject::DropletVolume;
    p.start         = 10.;
    p.step          = 10.;
    p.count         = 3;
    p.engrave_index = true;
    const SweepResult sweep = make_parameter_sweep(p);
    REQUIRE(sweep.valid());
    CHECK(sweep.unit == "nl");
    REQUIRE(sweep.items.size() == 3);

    for (size_t i = 0; i < 3; ++i) {
        const SweepItem &item = sweep.items[i];
        // Only the engraved digits differ between the copies.
        const double engraved = sweep_label_area(item.label, p.label_height) * p.label_depth;
        const double engraved0 = sweep_label_area(sweep.items[0].label, p.label_height) * p.label_depth;
        CHECK(item.volume_mm3 + engraved == Approx(sweep.items[0].volume_mm3 + engraved0).margin(1e-3));
        REQUIRE(item.droplets.size() == 1);
        REQUIRE(item.droplets.front().points.size() == 1);
        const DropletPoint &point = item.droplets.front().points.front();
        CHECK(point.volume_nl == Approx(10. * double(i + 1)));
        // The droplet is inside the copy and lands on the floor of the well.
        CHECK(point.pos.x() > 0.);
        CHECK(point.pos.x() < item.size.x());
        CHECK(point.pos.y() > 0.);
        CHECK(point.pos.y() < item.size.y());
        CHECK(point.pos.z() == Approx(p.well.floor_thickness).margin(1e-9));
    }
    const auto rows = parse_csv(sweep.csv);
    REQUIRE(rows.size() == 4);
    CHECK(rows[2][5] == "nl");
    CHECK(rows[2][4] == "20.000");
    CHECK(rows[2][12] == "1");
}

TEST_CASE("A sweep that cannot be made says which copy failed", "[Sweep]")
{
    SweepParams p = channel_sweep(0, true);
    SweepResult sweep = make_parameter_sweep(p);
    CHECK_FALSE(sweep.valid());
    CHECK_FALSE(sweep.error.empty());

    // The channel of the second copy would be 0.01 mm.
    p = channel_sweep(3, true);
    p.start = 0.21;
    p.step  = -0.2;
    sweep = make_parameter_sweep(p);
    CHECK_FALSE(sweep.valid());
    CHECK(sweep.error.find("Copy 2") != std::string::npos);
    CHECK(sweep.items.empty());

    p = channel_sweep(3, true);
    p.label_depth = p.label_thickness;
    CHECK_FALSE(make_parameter_sweep(p).valid());
}

TEST_CASE("A sweep becomes one object per copy with its droplets", "[Sweep]")
{
    SweepParams p;
    p.subject = SweepSubject::DropletVolume;
    p.start   = 25.;
    p.step    = 25.;
    p.count   = 4;
    const SweepResult sweep = make_parameter_sweep(p);
    REQUIRE(sweep.valid());

    Model model;
    add_sweep_to_model(model, sweep, Vec2d(100., 90.), 3);
    REQUIRE(model.objects.size() == 4);
    for (size_t i = 0; i < 4; ++i) {
        const ModelObject *object = model.objects[i];
        CHECK(object->name == "Sweep " + std::to_string(i + 1) + " of 4: droplet volume " + std::to_string(25 * (i + 1)) + ".00 nl");
        REQUIRE(object->instances.size() == 1);
        const Vec3d offset = object->instances.front()->get_offset();
        CHECK(offset.x() == Approx(100. + sweep.items[i].offset.x()).margin(1e-6));
        CHECK(offset.y() == Approx(90. + sweep.items[i].offset.y()).margin(1e-6));
        // The copy stands on the bed.
        CHECK(offset.z() - 0.5 * sweep.items[i].size.z() == Approx(0.).margin(1e-6));

        REQUIRE(object->droplet_sets.size() == 1);
        CHECK(object->droplet_sets.front().extruder == 3);
        CHECK(object->droplet_sets.front().dip);
        REQUIRE(object->droplet_sets.front().points.size() == 1);
        const DropletPoint &point = object->droplet_sets.front().points.front();
        CHECK(point.volume_nl == Approx(25. * double(i + 1)));
        // The object is centred, and the droplet moved with it: it is still over the floor of the well.
        const BoundingBoxf3 box = object->raw_mesh_bounding_box();
        CHECK(box.center().x() == Approx(0.).margin(1e-4));
        CHECK(point.pos.z() - box.min.z() == Approx(p.well.floor_thickness).margin(1e-4));
        CHECK(point.pos.x() > box.min.x());
        CHECK(point.pos.x() < box.max.x());
    }
}

TEST_CASE("A layer height sweep gives every copy its own layer height", "[Sweep]")
{
    SweepParams p;
    p.subject       = SweepSubject::LayerHeight;
    p.start         = 0.1;
    p.step          = 0.05;
    p.count         = 4;
    p.engrave_index = true;
    const SweepResult sweep = make_parameter_sweep(p);
    REQUIRE(sweep.valid());
    CHECK(sweep.parameter == "layer height");
    CHECK(sweep.unit == "mm");

    // The copies are the same piece; only the layer height differs.
    for (size_t i = 0; i < 4; ++i) {
        CHECK(sweep.items[i].layer_height_mm == Approx(0.1 + 0.05 * double(i)));
        CHECK(sweep.items[i].volume_mm3 + sweep_label_area(sweep.items[i].label, p.label_height) * p.label_depth
            == Approx(sweep.items[0].volume_mm3 + sweep_label_area("1", p.label_height) * p.label_depth).margin(1e-3));
    }
    const auto rows = parse_csv(sweep.csv);
    REQUIRE(rows.size() == 5);
    CHECK(rows[0].back() == "layer_height_mm");
    CHECK(rows[1][4] == "0.100");
    CHECK(rows[4][4] == "0.250");
    CHECK(rows[4].back() == "0.250");

    Model model;
    add_sweep_to_model(model, sweep, Vec2d(100., 100.));
    REQUIRE(model.objects.size() == 4);
    for (size_t i = 0; i < 4; ++i) {
        REQUIRE(model.objects[i]->config.has("layer_height"));
        CHECK(model.objects[i]->config.opt_float("layer_height") == Approx(0.1 + 0.05 * double(i)));
    }

    p.start = 0.01;
    CHECK_FALSE(make_parameter_sweep(p).valid());
}
