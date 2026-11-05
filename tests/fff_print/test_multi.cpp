#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_approx.hpp>

#include <numeric>
#include <regex>
#include <sstream>

#include <boost/algorithm/string/join.hpp>
#include <boost/filesystem.hpp>
#include <boost/process.hpp>

#include "libslic3r/ClipperUtils.hpp"
#include "libslic3r/Geometry.hpp"
#include "libslic3r/Geometry/ConvexHull.hpp"
#include "libslic3r/Print.hpp"
#include "libslic3r/Exception.hpp"
#include "libslic3r/libslic3r.h"

#include "test_data.hpp"

using namespace Slic3r;
using namespace std::literals;

// Mirrors GCode.cpp's private encode_base64(): SLA video external-reference
// paths are emitted as "path_b64=<...>" (not a raw "path=<...>") so a path
// containing CR/LF can't inject G-code lines into the comment. Tests need
// their own copy since the production encoder isn't exposed outside GCode.cpp.
static std::string test_base64_encode(const std::string &s)
{
    static const char *table = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    size_t i = 0;
    for (; i + 3 <= s.size(); i += 3) {
        unsigned int n = (unsigned char)s[i] << 16 | (unsigned char)s[i + 1] << 8 | (unsigned char)s[i + 2];
        out += table[(n >> 18) & 0x3F];
        out += table[(n >> 12) & 0x3F];
        out += table[(n >> 6) & 0x3F];
        out += table[n & 0x3F];
    }
    const size_t rem = s.size() - i;
    if (rem == 1) {
        unsigned int n = (unsigned char)s[i] << 16;
        out += table[(n >> 18) & 0x3F];
        out += table[(n >> 12) & 0x3F];
        out += "==";
    } else if (rem == 2) {
        unsigned int n = (unsigned char)s[i] << 16 | (unsigned char)s[i + 1] << 8;
        out += table[(n >> 18) & 0x3F];
        out += table[(n >> 12) & 0x3F];
        out += table[(n >> 6) & 0x3F];
        out += '=';
    }
    return out;
}

SCENARIO("Basic tests", "[Multi]")
{
    WHEN("Slicing multi-material print with non-consecutive extruders") {
        std::string gcode = Slic3r::Test::slice({ Slic3r::Test::TestMesh::cube_20x20x20 }, 
            {
                { "nozzle_diameter",                "0.6, 0.6, 0.6, 0.6" },
                { "extruder",                       2 },
                { "infill_extruder",                4 },
                { "support_material_extruder",      0 }
            });
        THEN("Sliced successfully") {
            REQUIRE(! gcode.empty());
        }
        THEN("T3 toolchange command found") {
            bool T1_found = gcode.find("\nT3\n") != gcode.npos;
            REQUIRE(T1_found);
        }
    }
    WHEN("Slicing with multiple skirts with a single, non-zero extruder") {
        std::string gcode = Slic3r::Test::slice({ Slic3r::Test::TestMesh::cube_20x20x20 }, 
            {
                { "nozzle_diameter",                        "0.6, 0.6, 0.6, 0.6" },
                { "perimeter_extruder",                     2 },
                { "infill_extruder",                        2 },
                { "support_material_extruder",              2 },
                { "support_material_interface_extruder",    2 },
            });
        THEN("Sliced successfully") {
            REQUIRE(! gcode.empty());
        }
    }
}

SCENARIO("Ooze prevention", "[Multi]")
{
    DynamicPrintConfig config = Slic3r::DynamicPrintConfig::full_print_config_with({
        { "nozzle_diameter",                "0.6, 0.6, 0.6, 0.6" },
        { "raft_layers",                    2 },
        { "infill_extruder",                2 },
        { "solid_infill_extruder",          3 },
        { "support_material_extruder",      4 },
        { "ooze_prevention",                1 },
        { "extruder_offset",                "0x0, 20x0, 0x20, 20x20" },
        { "temperature",                    "200, 180, 170, 160" },
        { "first_layer_temperature",        "206, 186, 166, 156" },
        // test that it doesn't crash when this is supplied
        { "toolchange_gcode",               "T[next_extruder] ;toolchange" }
    });
    FullPrintConfig print_config;
    print_config.apply(config);

    // Since July 2019, PrusaSlicer only emits automatic Tn command in case that the toolchange_gcode is empty
    // The "T[next_extruder]" is therefore needed in this test.
    
    std::string gcode = Slic3r::Test::slice({ Slic3r::Test::TestMesh::cube_20x20x20 }, config);

    GCodeReader parser;
    int         tool = -1;
    int         tool_temp[] = { 0, 0, 0, 0};
    Points      toolchange_points;
    Points      extrusion_points;
    parser.parse_buffer(gcode, [&tool, &tool_temp, &toolchange_points, &extrusion_points, &print_config]
        (Slic3r::GCodeReader &self, const Slic3r::GCodeReader::GCodeLine &line)
    {
        // if the command is a T command, set the the current tool
        if (boost::starts_with(line.cmd(), "T")) {
            // Ignore initial toolchange.
            if (tool != -1) {
                int expected_temp = is_approx<double>(self.z(), print_config.get_abs_value("first_layer_height") + print_config.z_offset) ?
                    print_config.first_layer_temperature.get_at(tool) :
                    print_config.temperature.get_at(tool);
                if (tool_temp[tool] != expected_temp + print_config.standby_temperature_delta)
                    throw std::runtime_error("Standby temperature was not set before toolchange.");
                toolchange_points.emplace_back(self.xy_scaled());
            }
            tool = atoi(line.cmd().data() + 1);
        } else if (line.cmd_is("M104") || line.cmd_is("M109")) {
            // May not be defined on this line.
            int t = tool;
            line.has_value('T', t);
            // Should be available on this line.
            int s;
            if (! line.has_value('S', s))
                throw std::runtime_error("M104 or M109 without S");

            // Following is obsolete. The first printing extruder is newly set to its first layer temperature immediately, not to the standby.
            //if (tool_temp[t] == 0 && s != print_config.first_layer_temperature.get_at(t) + print_config.standby_temperature_delta)
            //    throw std::runtime_error("initial temperature is not equal to first layer temperature + standby delta");

            tool_temp[t] = s;
        } else if (line.cmd_is("G1") && line.extruding(self) && line.dist_XY(self) > 0) {
            extrusion_points.emplace_back(line.new_XY_scaled(self) + scaled<coord_t>(print_config.extruder_offset.get_at(tool)));
        }
    });

    Polygon convex_hull = Geometry::convex_hull(extrusion_points);
    
    // THEN("all nozzles are outside skirt at toolchange") {
    //     Points t;
    //     sort_remove_duplicates(toolchange_points);
    //     size_t inside = 0;
    //     for (const auto &point : toolchange_points)
    //         for (const Vec2d &offset : print_config.extruder_offset.values) {
    //             Point p = point + scaled<coord_t>(offset);
    //             if (convex_hull.contains(p))
    //                 ++ inside;
    //         }
    //     REQUIRE(inside == 0);
    // }

#if 0
    require "Slic3r/SVG.pm";
    Slic3r::SVG::output(
        "ooze_prevention_test.svg",
        no_arrows   => 1,
        polygons    => [$convex_hull],
        red_points  => \@t,
        points      => \@toolchange_points,
    );
#endif
    
    THEN("all toolchanges happen within expected area") {
        // offset the skirt by the maximum displacement between extruders plus a safety extra margin
        const float delta = scaled<float>(20. * sqrt(2.) + 1.);
        Polygon outer_convex_hull = expand(convex_hull, delta).front();
        size_t inside = std::count_if(toolchange_points.begin(), toolchange_points.end(), [&outer_convex_hull](const Point &p){ return outer_convex_hull.contains(p); });
        REQUIRE(inside == toolchange_points.size());
    }
}

SCENARIO("SLA material metadata and placeholders are emitted", "[Multi]")
{
    auto config = Slic3r::DynamicPrintConfig::full_print_config_with({
        { "nozzle_diameter",         "0.6,0.6" },
        // Perimeter, infill, and solid infill must agree on technology (see
        // SLAMaterial.hpp's classify_region_technology): a region with an SLA
        // extruder on some roles and an FFF extruder on others is "Mixed" and
        // contributes no SLA video content at all, rather than being treated
        // as SLA (the pre-fix behavior this test originally assumed).
        { "perimeter_extruder",      2 },
        { "infill_extruder",         2 },
        { "solid_infill_extruder",   2 },
        { "skirts",                  0 },
        { "fill_density",            15 },
        { "toolchange_gcode",        "M118 SLA name=[sla_video_name] path=[sla_video_path] embedded=[sla_video_embedded]" },
        { "sla_material_extruder",   "0,1" },
        { "sla_material_video_names", "fdm;resin A" },
        { "sla_material_video_paths", "/tmp/fdm.mkv;/tmp/resin_a.mkv" },
        { "sla_material_video_embed", "0,0" }
    });

    std::string gcode = Slic3r::Test::slice({ Slic3r::Test::TestMesh::cube_20x20x20 }, config);

    THEN("SLA metadata map and ref comments are emitted for SLA extruders") {
        REQUIRE(gcode.find("; bioslicer_sla_material_map extruder=1 name=resin_A embedded=0") != std::string::npos);
        // External reference paths are emitted as path_b64=<...>, not a raw
        // path=<...>, so a path containing CR/LF can't inject G-code lines.
        REQUIRE(gcode.find("; bioslicer_sla_video ref name=resin_A extruder=1 path_b64="
            + test_base64_encode("/tmp/resin_a.mkv")) != std::string::npos);
        REQUIRE(gcode.find("; bioslicer_sla_video begin name=resin_A") == std::string::npos);
    }

    THEN("the Tool change G-code runs where the SLA material is selected, with the SLA placeholders resolved") {
        // An SLA extruder has no toolpaths and never goes through set_extruder(). Its material is
        // selected right before its first exposure, by the Tool change G-code followed by the
        // slicer's own BIOSLICER_MATERIAL_SWAP (the SLA counterpart of T<n>).
        const size_t custom = gcode.find("M118 SLA name=resin_A path=/tmp/resin_a.mkv embedded=0\n");
        const size_t swap   = gcode.find("BIOSLICER_MATERIAL_SWAP MATERIAL=resin_A\n");
        const size_t frame  = gcode.find("SLA_SHOW_FRAME NAME=1 FRAME=0\n");
        REQUIRE(custom != std::string::npos);
        REQUIRE(swap != std::string::npos);
        REQUIRE(frame != std::string::npos);
        REQUIRE(custom < swap);
        REQUIRE(swap < frame);
    }

    THEN("the printer is told to load the videos of the file, referenced ones included") {
        const size_t load  = gcode.find("SLA_LOAD_GCODE_VIDEOS\n");
        const size_t frame = gcode.find("SLA_SHOW_FRAME NAME=1 FRAME=0\n");
        REQUIRE(load != std::string::npos);
        REQUIRE(load < frame);
    }

    THEN("a single SLA material is selected once for the whole print") {
        size_t count = 0;
        for (size_t pos = gcode.find("BIOSLICER_MATERIAL_SWAP"); pos != std::string::npos;
             pos = gcode.find("BIOSLICER_MATERIAL_SWAP", pos + 1))
            ++ count;
        REQUIRE(count == 1);
    }
}

namespace {

std::vector<std::string> gcode_lines(const std::string &gcode)
{
    std::vector<std::string> out;
    std::istringstream stream(gcode);
    for (std::string line; std::getline(stream, line); )
        out.push_back(line);
    return out;
}

bool starts_with(const std::string &line, const std::string &prefix)
{
    return line.compare(0, prefix.size(), prefix) == 0;
}

// The G-code lines of the Nth SLA exposure: from the SLA_CLEAR that opens it up to and including its
// SLA_SHOW_FRAME. This is the projector blanking, material change and layer transition of one material
// on one layer, independent of layer count and Z values.
std::vector<std::string> sla_exposure_block(const std::string &gcode, size_t occurrence = 0)
{
    const std::vector<std::string> lines = gcode_lines(gcode);
    size_t seen = 0;
    for (size_t i = 0; i < lines.size(); ++ i)
        if (starts_with(lines[i], "SLA_SHOW_FRAME") && seen ++ == occurrence) {
            size_t first = i;
            while (first > 0 && lines[first] != "SLA_CLEAR")
                -- first;
            REQUIRE(lines[first] == "SLA_CLEAR");
            return { lines.begin() + first, lines.begin() + i + 1 };
        }
    FAIL("SLA exposure " << occurrence << " not found");
    return {};
}

// Index of the first line with the given prefix, or npos.
size_t find_line(const std::vector<std::string> &lines, const std::string &prefix, size_t from = 0)
{
    for (size_t i = from; i < lines.size(); ++ i)
        if (starts_with(lines[i], prefix))
            return i;
    return std::string::npos;
}

// Z of a "G1 Z..." line. The writer omits the leading zero for values under 1 (e.g. "Z.35").
double z_of(const std::string &line)
{
    static const std::regex z_move(R"(^G1 Z(-?[0-9]*\.?[0-9]+))");
    std::smatch match;
    REQUIRE(std::regex_search(line, match, z_move));
    return std::stod(match[1]);
}

DynamicPrintConfig sla_only_config(std::initializer_list<Slic3r::ConfigBase::SetDeserializeItem> extra)
{
    DynamicPrintConfig config = Slic3r::DynamicPrintConfig::full_print_config_with({
        { "nozzle_diameter",         "0.6,0.6" },
        { "perimeter_extruder",      2 },
        { "infill_extruder",         2 },
        { "solid_infill_extruder",   2 },
        { "skirts",                  0 },
        { "fill_density",            15 },
        { "sla_material_extruder",   "0,1" },
        { "sla_material_video_names", "fdm;resin A" },
        { "sla_material_video_paths", "/tmp/fdm.mkv;/tmp/resin_a.mkv" },
        { "sla_material_video_embed", "0,0" }
    });
    config.set_deserialize_strict(extra);
    return config;
}

} // namespace

SCENARIO("SLA layer transition move is emitted per material before its cure", "[Multi]")
{
    GIVEN("a positive transition depth smaller than the first layer height") {
        const std::string gcode = Slic3r::Test::slice({ Slic3r::Test::TestMesh::cube_20x20x20 }, sla_only_config({
            { "sla_material_transition_depth", "0,0.25" },
            { "sla_material_transition_time",  "0,1.5" }
        }));

        THEN("the projector is cleared, the material is put in place, the object moves to a lower Z for the "
             "configured time and returns to cure height before the material's own SLA_SHOW_FRAME") {
            const std::vector<std::string> block = sla_exposure_block(gcode);
            REQUIRE(block.front() == "SLA_CLEAR");

            const size_t swap  = find_line(block, "BIOSLICER_MATERIAL_SWAP");
            const size_t away  = find_line(block, "G1 Z");
            const size_t dwell = find_line(block, "G4 P1500");
            REQUIRE(swap != std::string::npos);
            REQUIRE(away != std::string::npos);
            REQUIRE(dwell != std::string::npos);
            const size_t back  = find_line(block, "G1 Z", dwell);
            REQUIRE(back != std::string::npos);
            // The transition happens in the resin that is about to be cured: after the material change.
            REQUIRE(swap < away);
            REQUIRE(away < dwell);
            REQUIRE(z_of(block[back]) - z_of(block[away]) == Catch::Approx(0.25).margin(0.001));
        }

        THEN("the move recurs before every layer's cure, not just once per print") {
            const std::vector<std::string> block = sla_exposure_block(gcode, 1);
            REQUIRE(find_line(block, "G4 P1500") != std::string::npos);
            REQUIRE(find_line(block, "G1 Z") != std::string::npos);
            // The material is already in place on the second layer.
            REQUIRE(find_line(block, "BIOSLICER_MATERIAL_SWAP") == std::string::npos);
        }

        THEN("the projector is left dark after the last exposure of every layer") {
            const std::vector<std::string> lines = gcode_lines(gcode);
            size_t frames = 0;
            for (size_t i = 0; i < lines.size(); ++ i)
                if (starts_with(lines[i], "SLA_SHOW_FRAME")) {
                    ++ frames;
                    // Only the exposure dwells may sit between the frame and the blanking.
                    size_t next = i + 1;
                    while (next < lines.size() && starts_with(lines[next], "G4 "))
                        ++ next;
                    REQUIRE(next < lines.size());
                    REQUIRE(lines[next] == "SLA_CLEAR");
                }
            REQUIRE(frames > 1);
        }
    }

    GIVEN("a negative transition depth") {
        const std::string gcode = Slic3r::Test::slice({ Slic3r::Test::TestMesh::cube_20x20x20 }, sla_only_config({
            { "sla_material_transition_depth", "0,-2.5" },
            { "sla_material_transition_time",  "0,1.5" }
        }));

        THEN("the object moves to a higher Z and back") {
            const std::vector<std::string> block = sla_exposure_block(gcode);
            const size_t away  = find_line(block, "G1 Z");
            const size_t dwell = find_line(block, "G4 P1500");
            REQUIRE(away != std::string::npos);
            REQUIRE(dwell != std::string::npos);
            const size_t back  = find_line(block, "G1 Z", dwell);
            REQUIRE(back != std::string::npos);
            REQUIRE(z_of(block[away]) - z_of(block[back]) == Catch::Approx(2.5).margin(0.001));
        }
    }

    GIVEN("a transition depth that moves the first layers below Z = 0") {
        const std::string gcode = Slic3r::Test::slice({ Slic3r::Test::TestMesh::cube_20x20x20 }, sla_only_config({
            { "sla_material_transition_depth", "0,2.5" },
            { "sla_material_transition_time",  "0,1.5" }
        }));

        THEN("the move is emitted with a negative Z: the machine may travel below its Z origin") {
            const std::vector<std::string> block = sla_exposure_block(gcode);
            const size_t away  = find_line(block, "G1 Z");
            const size_t dwell = find_line(block, "G4 P1500");
            REQUIRE(away != std::string::npos);
            REQUIRE(dwell != std::string::npos);
            const size_t back  = find_line(block, "G1 Z", dwell);
            REQUIRE(back != std::string::npos);
            REQUIRE(z_of(block[away]) < 0.);
            REQUIRE(z_of(block[back]) - z_of(block[away]) == Catch::Approx(2.5).margin(0.001));
        }
    }

    GIVEN("a transition depth that would move the layers above the maximum print height") {
        THEN("G-code export fails instead of emitting a move the printer cannot make") {
            REQUIRE_THROWS_AS(
                Slic3r::Test::slice({ Slic3r::Test::TestMesh::cube_20x20x20 }, sla_only_config({
                    { "max_print_height",              30 },
                    { "sla_material_transition_depth", "0,-20" }
                })),
                Slic3r::ExportError);
        }
    }
}

SCENARIO("SLA layer transition move is a no-op when depth and time are left at defaults", "[Multi]")
{
    const std::string gcode = Slic3r::Test::slice({ Slic3r::Test::TestMesh::cube_20x20x20 }, sla_only_config({}));

    THEN("no motion or dwell appears between SLA_CLEAR and SLA_SHOW_FRAME") {
        for (const size_t occurrence : { 0, 1 }) {
            const std::vector<std::string> block = sla_exposure_block(gcode, occurrence);
            REQUIRE(find_line(block, "G1 Z") == std::string::npos);
            // The only dwell left is the delay before the exposure, right in front of the frame.
            const size_t dwell = find_line(block, "G4 ");
            REQUIRE((dwell == std::string::npos || dwell + 3 >= block.size()));
        }
    }

    THEN("every layer still makes its own Z move before the projector is cleared for it") {
        const std::vector<std::string> lines = gcode_lines(gcode);
        size_t layer = find_line(lines, ";LAYER_CHANGE");
        REQUIRE(layer != std::string::npos);
        layer = find_line(lines, ";LAYER_CHANGE", layer + 1);
        REQUIRE(layer != std::string::npos);
        const size_t z_move = find_line(lines, "G1 Z", layer);
        const size_t clear  = find_line(lines, "SLA_CLEAR", layer);
        REQUIRE(z_move != std::string::npos);
        REQUIRE(z_move < clear);
    }
}

SCENARIO("SLA native synthesis validates required path in ref mode", "[Multi]")
{
    auto config = Slic3r::DynamicPrintConfig::full_print_config_with({
        { "nozzle_diameter",                "0.6,0.6" },
        // See the comment in "SLA material metadata and placeholders are
        // emitted" above: all three roles must agree on technology, or this
        // region is "Mixed" and contributes no SLA content — meaning the
        // ref-mode path check below would never even run.
        { "perimeter_extruder",             2 },
        { "infill_extruder",                2 },
        { "solid_infill_extruder",          2 },
        { "skirts",                         0 },
        { "fill_density",                   15 },
        { "sla_material_extruder",          "0,1" },
        { "sla_material_video_synthesize",  "0,1" },
        { "sla_material_video_names",       "fdm;resin synth" },
        { "sla_material_video_paths",       ";" },
        { "sla_material_video_embed",       "0,0" }
    });

    THEN("slicing fails with ExportError because ref mode needs a concrete output path") {
        REQUIRE_THROWS_AS(
            Slic3r::Test::slice({ Slic3r::Test::TestMesh::cube_20x20x20 }, config),
            Slic3r::ExportError);
    }
}

SCENARIO("SLA metadata export rejects binary G-code", "[Multi]")
{
    auto config = Slic3r::DynamicPrintConfig::full_print_config_with({
        { "nozzle_diameter",         "0.6,0.6" },
        { "perimeter_extruder",      1 },
        { "infill_extruder",         2 },
        { "solid_infill_extruder",   2 },
        { "skirts",                  0 },
        { "fill_density",            15 },
        { "binary_gcode",            1 },
        { "sla_material_extruder",   "0,1" },
        { "sla_material_video_names", "fdm;resin A" },
        { "sla_material_video_paths", ";/tmp/resin_a.mkv" },
        { "sla_material_video_embed", "0,0" }
    });

    THEN("slicing fails with ExportError because SLA metadata is ASCII-only") {
        REQUIRE_THROWS_AS(
            Slic3r::Test::slice({ Slic3r::Test::TestMesh::cube_20x20x20 }, config),
            Slic3r::ExportError);
    }
}

SCENARIO("SLA native synthesis creates MKV and emits reference metadata", "[Multi]")
{
    REQUIRE(!boost::process::search_path("ffmpeg").empty());

    const boost::filesystem::path output_path =
        boost::filesystem::temp_directory_path() /
        boost::filesystem::unique_path("bioslicer_sla_synth_test_%%%%-%%%%-%%%%.mkv");

    auto config = Slic3r::DynamicPrintConfig::full_print_config_with({
        { "nozzle_diameter",                 "0.6,0.6" },
        // See the comment in "SLA material metadata and placeholders are
        // emitted" above: all three roles must agree on technology.
        { "perimeter_extruder",              2 },
        { "infill_extruder",                 2 },
        { "solid_infill_extruder",           2 },
        { "skirts",                          0 },
        { "fill_density",                    15 },
        { "sla_material_extruder",           "0,1" },
        { "sla_material_video_synthesize",   "0,1" },
        { "sla_material_video_names",        "fdm;resin synth" },
        { "sla_material_video_paths",        ";" + output_path.string() },
        { "sla_material_video_embed",        "0,0" },
        { "sla_material_video_synth_width",  64 },
        { "sla_material_video_synth_height", 64 },
        { "sla_material_video_synth_fps",    5 },
        { "sla_material_video_synth_lossless", 0 }
    });

    std::string gcode = Slic3r::Test::slice({ Slic3r::Test::TestMesh::cube_20x20x20 }, config);

    THEN("synthesis writes MKV and G-code references it") {
        REQUIRE(boost::filesystem::exists(output_path));
        REQUIRE(boost::filesystem::file_size(output_path) > 0);
        // External reference paths are emitted as path_b64=<...>, not a raw
        // path=<...>, so a path containing CR/LF can't inject G-code lines.
        REQUIRE(gcode.find("; bioslicer_sla_video ref name=resin_synth extruder=1 path_b64="
            + test_base64_encode(output_path.string())) != std::string::npos);
    }

    boost::system::error_code ec;
    boost::filesystem::remove(output_path, ec);
}

SCENARIO("SLA native synthesis with embed mode emits payload and cleans temp output", "[Multi]")
{
    REQUIRE(!boost::process::search_path("ffmpeg").empty());

    const std::string video_name = "resin_embed_temp_cleanup";
    // The synthesized-to-temp filename includes a random unique_path() suffix
    // (added to avoid collisions between concurrent exports — see C4-3), so
    // the exact final name can't be predicted; scan the temp directory for
    // anything matching the known prefix/suffix instead of one fixed path.
    const std::string temp_output_prefix = "bioslicer_" + video_name + "_t1_native-";
    const std::string temp_output_suffix = ".mkv";
    auto find_matching_temp_files = [&]() {
        std::vector<boost::filesystem::path> matches;
        boost::system::error_code dir_ec;
        for (boost::filesystem::directory_iterator it(boost::filesystem::temp_directory_path(), dir_ec), end;
                !dir_ec && it != end; it.increment(dir_ec)) {
            const std::string name = it->path().filename().string();
            if (name.size() >= temp_output_prefix.size() + temp_output_suffix.size()
                    && name.compare(0, temp_output_prefix.size(), temp_output_prefix) == 0
                    && name.compare(name.size() - temp_output_suffix.size(), temp_output_suffix.size(), temp_output_suffix) == 0)
                matches.push_back(it->path());
        }
        return matches;
    };
    for (const boost::filesystem::path &leftover : find_matching_temp_files()) {
        boost::system::error_code ec;
        boost::filesystem::remove(leftover, ec);
    }

    auto config = Slic3r::DynamicPrintConfig::full_print_config_with({
        { "nozzle_diameter",                 "0.6,0.6" },
        // See the comment in "SLA material metadata and placeholders are
        // emitted" above: all three roles must agree on technology.
        { "perimeter_extruder",              2 },
        { "infill_extruder",                 2 },
        { "solid_infill_extruder",           2 },
        { "skirts",                          0 },
        { "fill_density",                    15 },
        { "sla_material_extruder",           "0,1" },
        { "sla_material_video_synthesize",   "0,1" },
        { "sla_material_video_names",        "fdm;" + video_name },
        { "sla_material_video_paths",        ";" },
        { "sla_material_video_embed",        "0,1" },
        { "sla_material_video_synth_width",  64 },
        { "sla_material_video_synth_height", 64 },
        { "sla_material_video_synth_fps",    5 },
        { "sla_material_video_synth_lossless", 0 }
    });

    std::string gcode = Slic3r::Test::slice({ Slic3r::Test::TestMesh::cube_20x20x20 }, config);

    THEN("embedded payload markers are emitted and temp file is removed") {
        REQUIRE(gcode.find("; bioslicer_sla_material_map extruder=1 name=" + video_name + " embedded=1") != std::string::npos);
        REQUIRE(gcode.find("; bioslicer_sla_video begin name=" + video_name + " extruder=1") != std::string::npos);
        REQUIRE(gcode.find("; bioslicer_sla_video end name=" + video_name) != std::string::npos);
        REQUIRE(gcode.find("; bioslicer_sla_video ref name=" + video_name) == std::string::npos);
        REQUIRE(find_matching_temp_files().empty());
    }
}

std::string slice_stacked_cubes(const DynamicPrintConfig &config, const DynamicPrintConfig &volume1config, const DynamicPrintConfig &volume2config)
{
    Model        model;
    ModelObject *object = model.add_object();
    object->name = "object.stl";
    ModelVolume *v1 = object->add_volume(Test::mesh(Test::TestMesh::cube_20x20x20));
    v1->set_material_id("lower_material");
    v1->config.assign_config(volume1config);
    ModelVolume *v2 = object->add_volume(Test::mesh(Test::TestMesh::cube_20x20x20));
    v2->set_material_id("upper_material");
    v2->translate(0., 0., 20.);
    v2->config.assign_config(volume2config);
    object->add_instance();
    object->ensure_on_bed();
    Print print;
    print.auto_assign_extruders(object);
    THEN("auto_assign_extruders() assigned correct extruder to first volume") {
        REQUIRE(v1->config.extruder() == 1);
    }
    THEN("auto_assign_extruders() assigned correct extruder to second volume") {
        REQUIRE(v2->config.extruder() == 2);
    }
    print.apply(model, config);
    print.validate();
    return Test::gcode(print);
}

SCENARIO("Stacked cubes", "[Multi]")
{
    DynamicPrintConfig lower_config;
    lower_config.set_deserialize_strict({
        { "extruder",               1 },
        { "bottom_solid_layers",    0 },
        { "top_solid_layers",       1 },
    });

    DynamicPrintConfig upper_config;
    upper_config.set_deserialize_strict({
        { "extruder",               2 },
        { "bottom_solid_layers",    1 },
        { "top_solid_layers",       0 }
    });

    static constexpr const double solid_infill_speed = 99;
    auto config = Slic3r::DynamicPrintConfig::full_print_config_with({
        { "nozzle_diameter",        "0.6, 0.6, 0.6, 0.6" },
        { "fill_density",           0 },
        { "solid_infill_speed",     solid_infill_speed },
        { "top_solid_infill_speed", solid_infill_speed },
        // for preventing speeds from being altered
        { "cooling",                "0, 0, 0, 0" },
        // for preventing speeds from being altered
        { "first_layer_speed",      "100%" }
    });

    auto test_shells = [](const std::string &gcode) {
        GCodeReader       parser;
        int               tool = -1;
        // Scaled Z heights.
        std::set<coord_t> T0_shells, T1_shells;
        parser.parse_buffer(gcode, [&tool, &T0_shells, &T1_shells]
            (Slic3r::GCodeReader &self, const Slic3r::GCodeReader::GCodeLine &line)
        {
            if (boost::starts_with(line.cmd(), "T")) {
                tool = atoi(line.cmd().data() + 1);
            } else if (line.cmd() == "G1" && line.extruding(self) && line.dist_XY(self) > 0) {
                if (is_approx<double>(line.new_F(self), solid_infill_speed * 60.) && (tool == 0 || tool == 1))
                    (tool == 0 ? T0_shells : T1_shells).insert(scaled<coord_t>(self.z()));
            }
        });
        return std::make_pair(T0_shells, T1_shells);
    };
    
    WHEN("Interface shells disabled") {
        std::string gcode = slice_stacked_cubes(config, lower_config, upper_config);
        auto [t0, t1] = test_shells(gcode);
        THEN("no interface shells") {
            REQUIRE(t0.empty());
            REQUIRE(t1.empty());
        }
    }
    WHEN("Interface shells enabled") {
        config.set_deserialize_strict("interface_shells", "1");
        std::string gcode = slice_stacked_cubes(config, lower_config, upper_config);
        auto [t0, t1] = test_shells(gcode);
        THEN("top interface shells") {
            REQUIRE(t0.size() == lower_config.opt_int("top_solid_layers"));
        }
        THEN("bottom interface shells") {
            REQUIRE(t1.size() == upper_config.opt_int("bottom_solid_layers"));
        }
    }
    WHEN("Slicing with auto-assigned extruders") {
        auto config = Slic3r::DynamicPrintConfig::full_print_config_with({
            { "nozzle_diameter",        "0.6,0.6,0.6,0.6" },
            { "layer_height",           0.4 },
            { "first_layer_height",     0.4 },
            { "skirts",                 0 }
        });
        std::string gcode = slice_stacked_cubes(config, DynamicPrintConfig{}, DynamicPrintConfig{});
        GCodeReader       parser;
        int               tool = -1;
        // Scaled Z heights.
        std::set<coord_t> T0_shells, T1_shells;
        parser.parse_buffer(gcode, [&tool, &T0_shells, &T1_shells](Slic3r::GCodeReader &self, const Slic3r::GCodeReader::GCodeLine &line)
        {
            if (boost::starts_with(line.cmd(), "T")) {
                tool = atoi(line.cmd().data() + 1);
            } else if (line.cmd() == "G1" && line.extruding(self) && line.dist_XY(self) > 0) {
                if (tool == 0 && self.z() > 20)
                    // Layers incorrectly extruded with T0 at the top object.
                    T0_shells.insert(scaled<coord_t>(self.z()));
                else if (tool == 1 && self.z() < 20)
                    // Layers incorrectly extruded with T1 at the bottom object.
                    T1_shells.insert(scaled<coord_t>(self.z()));
            }
        });
        THEN("T0 is never used for upper object") {
            REQUIRE(T0_shells.empty());
        }
        THEN("T0 is never used for lower object") {
            REQUIRE(T1_shells.empty());
        }
    }
}

namespace {

// Extruder 1 for the walls and extruder 4 for the infill, so the print changes between the tools T0 and T3.
// The tools carry different filament diameters and fan settings, so a lost tool change shows up in the
// statistics and in the cooling output.
DynamicPrintConfig material_tool_change_config(bool omit_tool_command)
{
    DynamicPrintConfig config = DynamicPrintConfig::full_print_config();
    config.set_deserialize_strict({
        { "nozzle_diameter",                "0.6, 0.6, 0.6, 0.6" },
        { "filament_diameter",              "1.75, 3.0, 1.75, 2.85" },
        { "fan_always_on",                  "0, 1, 0, 1" },
        { "min_fan_speed",                  "0, 35, 0, 80" },
        { "max_fan_speed",                  "0, 35, 0, 80" },
        { "channel_type",                   "fff;fff;fff;fff" },
        { "filament_settings_id",           "Basic PLA @X;PCL BioInk @X;Basic PLA @X;GelMA 10% @X" },
        { "extruder",                       2 },
        { "infill_extruder",                4 },
        { "support_material_extruder",      0 },
        { "start_gcode",                    "SELECT_MATERIAL MATERIAL=[initial_material]" },
        { "toolchange_gcode",               "SELECT_MATERIAL MATERIAL=[next_material]" },
        { "toolchange_omit_tool_command",   omit_tool_command ? 1 : 0 }
    });
    return config;
}

struct SlicedToolChanges {
    std::string gcode;
    double      used_filament;
};

SlicedToolChanges slice_material_tool_changes(bool omit_tool_command)
{
    Print print;
    Slic3r::Test::init_and_process_print({ Slic3r::Test::TestMesh::cube_20x20x20 }, print, material_tool_change_config(omit_tool_command));
    std::string gcode = Slic3r::Test::gcode(print);
    return { std::move(gcode), print.print_statistics().total_used_filament };
}

std::vector<std::string> lines_with_prefix(const std::string &gcode, const std::vector<std::string> &prefixes)
{
    std::vector<std::string> out;
    std::istringstream stream(gcode);
    for (std::string line; std::getline(stream, line); )
        for (const std::string &prefix : prefixes)
            if (line.compare(0, prefix.size(), prefix) == 0) {
                out.push_back(line);
                break;
            }
    return out;
}

bool has_bare_tool_change(const std::string &gcode)
{
    std::istringstream stream(gcode);
    for (std::string line; std::getline(stream, line); )
        if (line.size() >= 2 && line[0] == 'T' && std::isdigit(static_cast<unsigned char>(line[1])))
            return true;
    return false;
}

size_t count_lines_equal(const std::string &gcode, const std::string &wanted)
{
    return lines_with_prefix(gcode, { wanted }).size();
}

} // namespace

SCENARIO("A tool change can name the material instead of emitting a T command", "[Multi]")
{
    const SlicedToolChanges classic = slice_material_tool_changes(false);
    const SlicedToolChanges named   = slice_material_tool_changes(true);

    THEN("Both sliced successfully") {
        REQUIRE(! classic.gcode.empty());
        REQUIRE(! named.gcode.empty());
    }
    THEN("A print that exposes nothing does not ask the printer to load SLA videos") {
        CHECK(classic.gcode.find("SLA_LOAD_GCODE_VIDEOS") == std::string::npos);
    }
    THEN("Without the option the T command is still emitted next to the custom G-code and there are no tags") {
        CHECK(has_bare_tool_change(classic.gcode));
        CHECK(classic.gcode.find(";TOOL_CHANGE:") == std::string::npos);
        CHECK(classic.gcode.find("SELECT_MATERIAL MATERIAL=FFF_GELMA_10_X") != std::string::npos);
    }
    THEN("With the option no bare T command is emitted") {
        CHECK(! has_bare_tool_change(named.gcode));
    }
    THEN("Each tool change is marked with the extruder that becomes active") {
        CHECK(named.gcode.find(";TOOL_CHANGE:0\n") != std::string::npos);
        CHECK(named.gcode.find(";TOOL_CHANGE:3\n") != std::string::npos);
    }
    THEN("The custom G-code names the material of the next tool") {
        CHECK(named.gcode.find("SELECT_MATERIAL MATERIAL=FFF_BASIC_PLA_X\n") != std::string::npos);
        CHECK(named.gcode.find("SELECT_MATERIAL MATERIAL=FFF_GELMA_10_X\n") != std::string::npos);
    }
    THEN("The start G-code selects the material the print starts with") {
        const auto selects = lines_with_prefix(named.gcode, { "SELECT_MATERIAL" });
        REQUIRE(! selects.empty());
        CHECK(selects.front() == "SELECT_MATERIAL MATERIAL=FFF_BASIC_PLA_X");
    }
    THEN("The extruder reset that goes with a tool change is kept") {
        CHECK(count_lines_equal(named.gcode, "G92 E0") == count_lines_equal(classic.gcode, "G92 E0"));
    }
    THEN("The statistics still follow the active extruder") {
        CHECK(named.used_filament > 0.);
        CHECK(named.used_filament == Catch::Approx(classic.used_filament));
    }
    THEN("The cooling buffer still switches fan settings with the tool") {
        const std::vector<std::string> fan_commands = { "M106", "M107" };
        REQUIRE(! lines_with_prefix(classic.gcode, fan_commands).empty());
        CHECK(lines_with_prefix(named.gcode, fan_commands) == lines_with_prefix(classic.gcode, fan_commands));
    }
}

namespace {

// One object made of two 20 mm cubes with explicit extruders (1-based): the second one stacked on
// top of the first, or placed next to it.
std::string slice_two_cubes(const DynamicPrintConfig &config, int first_extruder, int second_extruder, bool stacked)
{
    Model        model;
    ModelObject *object = model.add_object();
    object->name = "object.stl";
    ModelVolume *v1 = object->add_volume(Test::mesh(Test::TestMesh::cube_20x20x20));
    v1->config.set("extruder", first_extruder);
    ModelVolume *v2 = object->add_volume(Test::mesh(Test::TestMesh::cube_20x20x20));
    v2->config.set("extruder", second_extruder);
    if (stacked)
        v2->translate(0., 0., 20.);
    else
        v2->translate(20., 0., 0.);
    object->add_instance();
    object->ensure_on_bed();
    model.center_instances_around_point({100, 100});
    Print print;
    print.apply(model, config);
    REQUIRE(print.validate().empty());
    print.set_status_silent();
    return Test::gcode(print);
}

size_t count_lines_with_prefix(const std::string &gcode, const std::string &prefix)
{
    return lines_with_prefix(gcode, { prefix }).size();
}

} // namespace

SCENARIO("An SLA material is only exposed on the layers it has geometry on", "[Multi]")
{
    // Extruders 2 and 3 are SLA. The lower cube is cured with extruder 2, the upper one with extruder 3.
    // A video holds one frame per layer its material has geometry on, so the commands must match.
    auto config = Slic3r::DynamicPrintConfig::full_print_config_with({
        { "nozzle_diameter",              "0.6,0.6,0.6" },
        { "layer_height",                 0.4 },
        { "first_layer_height",           0.4 },
        { "skirts",                       0 },
        { "sla_material_extruder",        "0,1,1" },
        { "sla_material_video_names",     "fdm;lower;upper" },
        { "sla_material_video_paths",     "/tmp/fdm.mkv;/tmp/lower.mkv;/tmp/upper.mkv" },
        { "sla_material_video_embed",     "0,0,0" },
        { "sla_material_transition_depth", "0,0.2,0.2" },
        { "sla_material_transition_time",  "0,1.5,1.5" }
    });
    const std::string gcode = slice_two_cubes(config, 2, 3, true);

    THEN("each material gets one exposure per layer of its own cube, with frames counted from zero") {
        // 20 mm at 0.4 mm layers.
        CHECK(count_lines_with_prefix(gcode, "SLA_SHOW_FRAME NAME=1 ") == 50);
        CHECK(count_lines_with_prefix(gcode, "SLA_SHOW_FRAME NAME=2 ") == 50);
        const size_t last_lower  = gcode.find("SLA_SHOW_FRAME NAME=1 FRAME=49\n");
        const size_t first_upper = gcode.find("SLA_SHOW_FRAME NAME=2 FRAME=0\n");
        REQUIRE(last_lower != std::string::npos);
        REQUIRE(first_upper != std::string::npos);
        CHECK(last_lower < first_upper);
        CHECK(gcode.find("SLA_SHOW_FRAME NAME=1 FRAME=50\n") == std::string::npos);
    }

    THEN("each material is selected once, right before its first exposure") {
        CHECK(count_lines_with_prefix(gcode, "BIOSLICER_MATERIAL_SWAP MATERIAL=lower") == 1);
        CHECK(count_lines_with_prefix(gcode, "BIOSLICER_MATERIAL_SWAP MATERIAL=upper") == 1);
    }

    THEN("the layer transition of the second material happens in its own resin, after the material change") {
        // Exposure 50 is the first one of the upper cube.
        const std::vector<std::string> block = sla_exposure_block(gcode, 50);
        REQUIRE(block.back() == "SLA_SHOW_FRAME NAME=2 FRAME=0");
        const size_t swap = find_line(block, "BIOSLICER_MATERIAL_SWAP MATERIAL=upper");
        const size_t away = find_line(block, "G1 Z");
        REQUIRE(swap != std::string::npos);
        REQUIRE(away != std::string::npos);
        CHECK(swap < away);
    }
}

SCENARIO("A hybrid layer extrudes first and exposes afterwards", "[Multi]")
{
    // Extruder 1 extrudes one cube, extruder 2 cures the cube next to it.
    auto config = Slic3r::DynamicPrintConfig::full_print_config_with({
        { "nozzle_diameter",              "0.6,0.6" },
        { "layer_height",                 0.4 },
        { "first_layer_height",           0.4 },
        { "skirts",                       0 },
        { "sla_material_extruder",        "0,1" },
        { "sla_material_video_names",     "fdm;resin" },
        { "sla_material_video_paths",     "/tmp/fdm.mkv;/tmp/resin.mkv" },
        { "sla_material_video_embed",     "0,0" },
        { "sla_material_transition_depth", "0,-2" },
        { "sla_material_transition_time",  "0,1" }
    });
    const std::vector<std::string> lines = gcode_lines(slice_two_cubes(config, 1, 2, false));

    THEN("every layer moves to its height, extrudes, then makes the transition move and exposes") {
        size_t layers = 0;
        for (size_t layer = find_line(lines, ";LAYER_CHANGE"); layer != std::string::npos; ) {
            const size_t next_layer = find_line(lines, ";LAYER_CHANGE", layer + 1);
            const size_t layer_end  = next_layer == std::string::npos ? lines.size() : next_layer;
            const size_t z_line     = find_line(lines, ";Z:", layer);
            REQUIRE(z_line < layer_end);
            const double layer_z    = std::stod(lines[z_line].substr(3));

            const size_t clear = find_line(lines, "SLA_CLEAR", layer);
            const size_t frame = find_line(lines, "SLA_SHOW_FRAME", layer);
            REQUIRE(clear < layer_end);
            REQUIRE(frame < layer_end);
            REQUIRE(clear < frame);

            // Extrusions of this layer come before the projector is touched.
            size_t extrusions = 0;
            for (size_t i = layer; i < clear; ++ i)
                if (starts_with(lines[i], "G1 X") && lines[i].find(" E") != std::string::npos)
                    ++ extrusions;
            REQUIRE(extrusions > 0);
            for (size_t i = clear; i < layer_end; ++ i)
                REQUIRE_FALSE((starts_with(lines[i], "G1 X") && lines[i].find(" E") != std::string::npos));

            // The transition leaves the cure height by the configured distance and returns to it.
            const size_t away  = find_line(lines, "G1 Z", clear);
            const size_t dwell = find_line(lines, "G4 P1000", clear);
            REQUIRE(away < dwell);
            REQUIRE(dwell < frame);
            const size_t back  = find_line(lines, "G1 Z", dwell);
            REQUIRE(back < frame);
            CHECK(z_of(lines[away]) == Catch::Approx(layer_z + 2.).margin(0.001));
            CHECK(z_of(lines[back]) == Catch::Approx(layer_z).margin(0.001));

            // The projector is dark again before the next layer starts extruding.
            REQUIRE(find_line(lines, "SLA_CLEAR", frame) < layer_end);

            ++ layers;
            layer = next_layer;
        }
        CHECK(layers == 50);
    }
}

namespace {

// An SLA only print on a printer that selects its materials by name.
DynamicPrintConfig sla_by_material_config(bool omit_tool_command)
{
    return Slic3r::DynamicPrintConfig::full_print_config_with({
        { "nozzle_diameter",              "0.6,0.6" },
        { "perimeter_extruder",           2 },
        { "infill_extruder",              2 },
        { "solid_infill_extruder",        2 },
        { "skirts",                       0 },
        { "channel_type",                 "fff;sla" },
        { "filament_settings_id",         "Basic PLA @X;Resin A @X" },
        { "sla_material_extruder",        "0,1" },
        { "sla_material_video_names",     "fdm;resin A" },
        { "sla_material_video_paths",     "/tmp/fdm.mkv;/tmp/resin_a.mkv" },
        { "sla_material_video_embed",     "0,0" },
        { "start_gcode",                  "PRINT_START MATERIALS=[materials]\\nSELECT_MATERIAL MATERIAL=[initial_material]" },
        { "toolchange_gcode",             "SELECT_MATERIAL MATERIAL=[next_material]" },
        { "toolchange_omit_tool_command", omit_tool_command ? 1 : 0 }
    });
}

} // namespace

SCENARIO("An SLA material is selected by name like a tool is", "[Multi]")
{
    const std::string named   = Slic3r::Test::slice({ Slic3r::Test::TestMesh::cube_20x20x20 }, sla_by_material_config(true));
    const std::string classic = Slic3r::Test::slice({ Slic3r::Test::TestMesh::cube_20x20x20 }, sla_by_material_config(false));

    THEN("the start G-code lists only the material that is used") {
        CHECK(named.find("PRINT_START MATERIALS=SLA_RESIN_A_X\n") != std::string::npos);
    }
    THEN("a print without extrusions does not select the material of an extruder it does not use") {
        const auto selects = lines_with_prefix(named, { "SELECT_MATERIAL" });
        REQUIRE(selects.size() == 2);
        // [initial_material] is empty: extruder 1 is only a placeholder in an SLA only print.
        CHECK(selects.front() == "SELECT_MATERIAL MATERIAL=");
        CHECK(named.find("FFF_BASIC_PLA_X") == std::string::npos);
    }
    THEN("the SLA material is selected by its name before its first exposure") {
        const size_t select = named.find("SELECT_MATERIAL MATERIAL=SLA_RESIN_A_X\n");
        const size_t frame  = named.find("SLA_SHOW_FRAME NAME=1 FRAME=0\n");
        REQUIRE(select != std::string::npos);
        REQUIRE(frame != std::string::npos);
        CHECK(select < frame);
    }
    THEN("with the tool command omitted the slicer's own SLA material command is omitted as well") {
        CHECK(named.find("BIOSLICER_MATERIAL_SWAP") == std::string::npos);
    }
    THEN("without the option both the custom G-code and the slicer's own command are emitted") {
        const size_t select = classic.find("SELECT_MATERIAL MATERIAL=SLA_RESIN_A_X\n");
        const size_t swap   = classic.find("BIOSLICER_MATERIAL_SWAP MATERIAL=resin_A\n");
        REQUIRE(select != std::string::npos);
        REQUIRE(swap != std::string::npos);
        CHECK(select < swap);
    }
}

SCENARIO("Temperatures do not name a tool the printer does not know", "[Multi]")
{
    auto with_temperatures = [](bool omit_tool_command) {
        DynamicPrintConfig config = material_tool_change_config(omit_tool_command);
        config.set_deserialize_strict({
            { "first_layer_temperature", "205, 215, 205, 225" },
            { "temperature",             "200, 210, 200, 220" }
        });
        Print print;
        Slic3r::Test::init_and_process_print({ Slic3r::Test::TestMesh::cube_20x20x20 }, print, config);
        return lines_with_prefix(Slic3r::Test::gcode(print), { "M104", "M109" });
    };
    auto has = [](const std::vector<std::string> &lines, const std::string &prefix) {
        return std::any_of(lines.begin(), lines.end(), [&prefix](const std::string &line) {
            return line.compare(0, prefix.size(), prefix) == 0; });
    };

    const std::vector<std::string> classic = with_temperatures(false);
    const std::vector<std::string> named   = with_temperatures(true);

    THEN("with tool numbers every tool is addressed by its number") {
        CHECK(has(classic, "M104 S200 T0"));
        CHECK(has(classic, "M104 S220 T3"));
    }
    THEN("with tools selected by material no temperature command carries a tool number") {
        REQUIRE(! named.empty());
        for (const std::string &line : named)
            CHECK(line.find(" T") == std::string::npos);
    }
    THEN("each tool gets its temperature while it is the active one") {
        // The first tool is heated to its first layer temperature by the waiting M109 after the start
        // G-code. The infill tool first prints above the solid bottom layers, so it is set straight to
        // its other layers temperature when it becomes active.
        auto sets = [](const std::vector<std::string> &lines, const std::string &temperature) {
            return std::any_of(lines.begin(), lines.end(), [&temperature](const std::string &line) {
                return line.find(" S" + temperature + " ") != std::string::npos; });
        };
        INFO(boost::algorithm::join(named, "\n"));
        CHECK(has(named, "M109 S205"));
        CHECK(sets(named, "200"));
        CHECK(sets(named, "220"));
        CHECK(! sets(named, "215"));
        CHECK(! sets(named, "210"));
    }
}

SCENARIO("Tools selected by material must be distinguishable", "[Multi]")
{
    auto validate = [](std::initializer_list<Slic3r::ConfigBase::SetDeserializeItem> items) {
        DynamicPrintConfig config = material_tool_change_config(true);
        config.set_deserialize_strict(items);
        Print print;
        Model model;
        Slic3r::Test::init_print({ Slic3r::Test::TestMesh::cube_20x20x20 }, print, model, config);
        return print.validate();
    };

    THEN("two used extruders with different materials are accepted") {
        CHECK(validate({}).empty());
    }
    THEN("two used extruders with the same material are rejected") {
        const std::string error = validate({ { "filament_settings_id", "Basic PLA @X;PCL BioInk @X;Basic PLA @X;Basic PLA @X" } });
        CHECK(error.find("FFF_BASIC_PLA_X") != std::string::npos);
    }
    THEN("the same material on two extruders is fine while the printer is given tool numbers") {
        CHECK(validate({
            { "filament_settings_id",         "Basic PLA @X;PCL BioInk @X;Basic PLA @X;Basic PLA @X" },
            { "toolchange_omit_tool_command", 0 }
        }).empty());
    }
    THEN("ooze prevention, which heats tools that are not active, is rejected") {
        CHECK(! validate({ { "ooze_prevention", 1 } }).empty());
    }
}

SCENARIO("Turning an extruder into an SLA material slices the print again", "[Multi]")
{
    DynamicPrintConfig config = sla_only_config({ { "sla_material_extruder", "0,0" } });

    Print print;
    Model model;
    Slic3r::Test::init_print({ Slic3r::Test::TestMesh::cube_20x20x20 }, print, model, config);
    print.process();
    const PrintObject &object = *print.objects().front();
    REQUIRE(object.is_step_done(posPerimeters));
    REQUIRE(object.layers().front()->regions().front()->perimeters().items_count() > 0);

    WHEN("the extruder the object is printed with becomes an SLA material") {
        config.set_deserialize_strict("sla_material_extruder", "0,1");
        print.apply(model, config);

        THEN("the toolpaths made for it as an FFF extruder are discarded") {
            REQUIRE_FALSE(object.is_step_done(posPerimeters));
            REQUIRE_FALSE(object.is_step_done(posInfill));
        }
        THEN("the G-code exposes the object and extrudes nothing") {
            const std::vector<std::string> lines = gcode_lines(Slic3r::Test::gcode(print));
            CHECK(find_line(lines, "SLA_SHOW_FRAME") != std::string::npos);
            for (const std::string &line : lines)
                REQUIRE_FALSE((starts_with(line, "G1 X") && line.find(" E") != std::string::npos));
        }
    }
}

SCENARIO("An SLA-only print reports its layers and its progress", "[Multi]")
{
    // Two objects cured side by side share their layers: 20 mm at 0.4 mm layers, 9 s each.
    const std::string gcode = Slic3r::Test::slice(
        { Slic3r::Test::TestMesh::cube_20x20x20, Slic3r::Test::TestMesh::cube_20x20x20 },
        sla_only_config({
            { "layer_height",                       0.4 },
            { "first_layer_height",                 0.4 },
            { "gcode_flavor",                       "klipper" },
            { "remaining_times",                    1 },
            { "sla_material_delay_before_exposure", "0,3" },
            { "sla_material_exposure_time",         "0,6" },
            { "start_gcode",                        "; total layers [total_layer_count]" }
        }));
    const std::vector<std::string> lines = gcode_lines(gcode);

    THEN("the total layer count is the number of layers printed, not one per object") {
        CHECK(count_lines_with_prefix(gcode, ";LAYER_CHANGE") == 50);
        CHECK(find_line(lines, "; total layers 50") != std::string::npos);
        CHECK(find_line(lines, "SET_PRINT_STATS_INFO CURRENT_LAYER=50") != std::string::npos);
        CHECK(find_line(lines, "SET_PRINT_STATS_INFO CURRENT_LAYER=51") == std::string::npos);
    }

    THEN("M73 reports the progress and the remaining time of the exposures") {
        static const std::regex m73(R"(^M73 P([0-9]+) R([0-9]+)$)");
        const size_t first_layer = find_line(lines, ";LAYER_CHANGE");
        const size_t first_m73   = find_line(lines, "M73 ");
        REQUIRE(first_m73 < first_layer);
        // 50 layers of 9 s.
        CHECK(lines[first_m73] == "M73 P0 R7");

        int layer        = 0;
        int last_percent = 0;
        int reports      = 0;
        for (size_t i = first_layer; i < lines.size(); ++ i) {
            if (lines[i] == ";LAYER_CHANGE")
                ++ layer;
            std::smatch match;
            if (! std::regex_match(lines[i], match, m73))
                continue;
            const int percent = std::stoi(match[1]);
            CHECK(percent >= last_percent);
            last_percent = percent;
            if (percent < 100) {
                // Layers before this one are done. The estimator lags by up to one layer.
                CHECK(std::abs(percent - 2 * (layer - 1)) <= 3);
                ++ reports;
            }
        }
        CHECK(reports >= 20);
        CHECK(last_percent == 100);
        CHECK(lines[find_line(lines, "M73 P100")] == "M73 P100 R0");
    }
}
