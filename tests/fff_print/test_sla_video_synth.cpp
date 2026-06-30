#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_approx.hpp>

#include <fstream>
#include <sstream>
#include <vector>

#include <boost/filesystem.hpp>
#include <boost/process/search_path.hpp>

#include "libslic3r/BoundingBox.hpp"
#include "libslic3r/ExPolygon.hpp"
#include "libslic3r/GCode/SLAVideoSynth.hpp"
#include "libslic3r/Point.hpp"
#include "libslic3r/libslic3r.h"

using namespace Slic3r;
using namespace Catch;

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

struct PGMFrame {
    int width  = 0;
    int height = 0;
    std::vector<unsigned char> pixels; // row-major, top-to-bottom

    unsigned char pixel(int row, int col) const
    {
        return pixels[size_t(row) * size_t(width) + size_t(col)];
    }
};

static PGMFrame parse_pgm(const std::string &data)
{
    // Minimal P5 PGM parser.
    PGMFrame f;
    std::istringstream ss(data);
    std::string magic;
    int maxval = 0;
    ss >> magic >> f.width >> f.height >> maxval;
    REQUIRE(magic   == "P5");
    REQUIRE(f.width  > 0);
    REQUIRE(f.height > 0);
    REQUIRE(maxval  == 255);
    ss.get(); // consume the single whitespace byte that follows maxval
    const size_t n = size_t(f.width) * size_t(f.height);
    f.pixels.resize(n);
    ss.read(reinterpret_cast<char *>(f.pixels.data()), std::streamsize(n));
    REQUIRE(size_t(ss.gcount()) == n);
    return f;
}

static BoundingBoxf make_bed(double x_min, double y_min, double x_max, double y_max)
{
    BoundingBoxf b;
    b.min     = Vec2d(x_min, y_min);
    b.max     = Vec2d(x_max, y_max);
    b.defined = true;
    return b;
}

// ---------------------------------------------------------------------------
// sla_proj_bbox
// ---------------------------------------------------------------------------

TEST_CASE("sla_proj_bbox: explicit dimensions centre on bed", "[SLAVideoSynth]")
{
    // BioTrident 250 bed: 77–177 x 79–134 mm → centre (127, 106.5)
    const BoundingBoxf bed = make_bed(77, 79, 177, 134);

    SECTION("100 x 100 mm projector")
    {
        const BoundingBoxf proj = sla_proj_bbox(bed, 100.0, 100.0);
        REQUIRE(proj.min.x() == Approx(77.0));
        REQUIRE(proj.min.y() == Approx(56.5));
        REQUIRE(proj.max.x() == Approx(177.0));
        REQUIRE(proj.max.y() == Approx(156.5));
    }

    SECTION("non-square projector")
    {
        const BoundingBoxf proj = sla_proj_bbox(bed, 80.0, 60.0);
        REQUIRE(proj.min.x() == Approx(87.0));
        REQUIRE(proj.min.y() == Approx(76.5));
        REQUIRE(proj.max.x() == Approx(167.0));
        REQUIRE(proj.max.y() == Approx(136.5));
    }
}

TEST_CASE("sla_proj_bbox: fallback uses max bed dimension as square", "[SLAVideoSynth]")
{
    SECTION("BioTrident 250 — fallback matches 100x100 explicit")
    {
        // bed is 100 mm wide, 55 mm tall → max dim = 100 → same as explicit 100x100
        const BoundingBoxf bed      = make_bed(77, 79, 177, 134);
        const BoundingBoxf explicit_ = sla_proj_bbox(bed, 100.0, 100.0);
        const BoundingBoxf fallback  = sla_proj_bbox(bed,   0.0,   0.0);
        REQUIRE(fallback.min.x() == Approx(explicit_.min.x()));
        REQUIRE(fallback.min.y() == Approx(explicit_.min.y()));
        REQUIRE(fallback.max.x() == Approx(explicit_.max.x()));
        REQUIRE(fallback.max.y() == Approx(explicit_.max.y()));
    }

    SECTION("tall bed — fallback squares on height")
    {
        // 50 x 200 mm bed → max dim = 200 → 200x200 square centred at (25, 100)
        const BoundingBoxf bed      = make_bed(0, 0, 50, 200);
        const BoundingBoxf fallback = sla_proj_bbox(bed, 0.0, 0.0);
        REQUIRE(fallback.min.x() == Approx(-75.0));
        REQUIRE(fallback.min.y() == Approx(0.0));
        REQUIRE(fallback.max.x() == Approx(125.0));
        REQUIRE(fallback.max.y() == Approx(200.0));
    }

    SECTION("square bed — fallback equals bed")
    {
        const BoundingBoxf bed      = make_bed(0, 0, 100, 100);
        const BoundingBoxf fallback = sla_proj_bbox(bed, 0.0, 0.0);
        REQUIRE(fallback.min.x() == Approx(0.0));
        REQUIRE(fallback.min.y() == Approx(0.0));
        REQUIRE(fallback.max.x() == Approx(100.0));
        REQUIRE(fallback.max.y() == Approx(100.0));
    }
}

TEST_CASE("sla_proj_bbox: partial zeros fall back (both must be > 0)", "[SLAVideoSynth]")
{
    // Only width provided — should fall back because height is 0
    const BoundingBoxf bed      = make_bed(0, 0, 100, 50);
    const BoundingBoxf fallback = sla_proj_bbox(bed, 0.0, 0.0);
    const BoundingBoxf partial  = sla_proj_bbox(bed, 80.0, 0.0);
    REQUIRE(partial.min.x() == Approx(fallback.min.x()));
    REQUIRE(partial.min.y() == Approx(fallback.min.y()));
    REQUIRE(partial.max.x() == Approx(fallback.max.x()));
    REQUIRE(partial.max.y() == Approx(fallback.max.y()));
}

// ---------------------------------------------------------------------------
// render_sla_synth_pgm_frame
// ---------------------------------------------------------------------------

TEST_CASE("render_sla_synth_pgm_frame: all pixels are fully exposed", "[SLAVideoSynth]")
{
    SECTION("small frame")
    {
        std::ostringstream ss;
        render_sla_synth_pgm_frame(ss, 16, 16);
        const PGMFrame f = parse_pgm(ss.str());
        REQUIRE(f.width  == 16);
        REQUIRE(f.height == 16);
        for (unsigned char px : f.pixels)
            REQUIRE(px == 255);
    }

    SECTION("non-square frame")
    {
        std::ostringstream ss;
        render_sla_synth_pgm_frame(ss, 32, 20);
        const PGMFrame f = parse_pgm(ss.str());
        REQUIRE(f.width  == 32);
        REQUIRE(f.height == 20);
        for (unsigned char px : f.pixels)
            REQUIRE(px == 255);
    }
}

// ---------------------------------------------------------------------------
// render_sla_slice_pgm_frame
// ---------------------------------------------------------------------------

TEST_CASE("render_sla_slice_pgm_frame: empty expolygons → all dark", "[SLAVideoSynth]")
{
    BoundingBoxf proj = make_bed(0, 0, 10, 10);
    std::ostringstream ss;
    render_sla_slice_pgm_frame(ss, 10, 10, {}, proj);
    const PGMFrame f = parse_pgm(ss.str());
    for (unsigned char px : f.pixels)
        REQUIRE(px == 0);
}

TEST_CASE("render_sla_slice_pgm_frame: polygon covering interior lights up expected pixels", "[SLAVideoSynth]")
{
    // 10×10 pixel frame, proj = (0,0)–(10,10) mm.
    // Polygon from (1,1)–(9,9) mm (in internal coords = scale_(1)–scale_(9)).
    // Expected: pixel rows 1–8 and cols 1–8 lit; row/col 0 and 9 dark.
    //
    // mm_to_px_x(x) = x * 0.9          (for 10 px, span=10)
    // mm_to_px_y(y) = (10 - y) * 0.9   (y-axis flipped)
    // polygon px corners: (0.9, 8.1), (8.1, 8.1), (8.1, 0.9), (0.9, 0.9)
    // Scanlines between px_y 0.9–8.1, i.e. rows 1–7 have yy inside.
    // (yy=0.5 < 0.9 → dark;  yy=1.5, …, 7.5 → lit;  yy=8.5 > 8.1 → dark)
    // Cols: ceil(0.9)=1 to floor(8.1)=8 → cols 1–8.

    BoundingBoxf proj = make_bed(0, 0, 10, 10);
    ExPolygon sq;
    sq.contour = Polygon({
        Point(scale_(1), scale_(1)),
        Point(scale_(9), scale_(1)),
        Point(scale_(9), scale_(9)),
        Point(scale_(1), scale_(9)),
    });

    std::ostringstream ss;
    render_sla_slice_pgm_frame(ss, 10, 10, {sq}, proj);
    const PGMFrame f = parse_pgm(ss.str());

    // Corners of the frame must be dark (outside the polygon)
    REQUIRE(f.pixel(0, 0) == 0);
    REQUIRE(f.pixel(0, 9) == 0);
    REQUIRE(f.pixel(9, 0) == 0);
    REQUIRE(f.pixel(9, 9) == 0);

    // Interior pixels must be lit
    for (int row = 1; row <= 7; ++row)
        for (int col = 1; col <= 8; ++col)
            REQUIRE(f.pixel(row, col) == 255);

    // Edges of the frame must be dark
    for (int i = 0; i < 10; ++i) {
        REQUIRE(f.pixel(0, i) == 0);   // top row
        REQUIRE(f.pixel(9, i) == 0);   // bottom row (yy=9.5 > 8.1)
    }
}

TEST_CASE("render_sla_slice_pgm_frame: hole punches dark region in lit polygon", "[SLAVideoSynth]")
{
    // Outer polygon: (0,0)–(10,10) mm (oversized, fills entire frame).
    // Hole: (3,3)–(7,7) mm.
    // Result: pixels outside the hole are lit; pixels inside the hole are dark.
    //
    // hole in px: (2.7, 2.7)–(6.3, 6.3)
    // hole rows (scanlines between 2.7–6.3): rows 3–5
    // hole cols: ceil(2.7)=3 to floor(6.3)=6

    BoundingBoxf proj = make_bed(0, 0, 10, 10);

    ExPolygon donut;
    donut.contour = Polygon({
        Point(scale_(-1), scale_(-1)),
        Point(scale_(11), scale_(-1)),
        Point(scale_(11), scale_(11)),
        Point(scale_(-1), scale_(11)),
    });
    // Hole in CCW (as seen from outside = CW winding inside ExPolygon)
    donut.holes.emplace_back(Polygon({
        Point(scale_(3), scale_(3)),
        Point(scale_(3), scale_(7)),
        Point(scale_(7), scale_(7)),
        Point(scale_(7), scale_(3)),
    }));

    std::ostringstream ss;
    render_sla_slice_pgm_frame(ss, 10, 10, {donut}, proj);
    const PGMFrame f = parse_pgm(ss.str());

    // Corners must be lit (inside outer polygon, outside hole)
    REQUIRE(f.pixel(0, 0) == 255);
    REQUIRE(f.pixel(0, 9) == 255);

    // Inside the hole must be dark
    REQUIRE(f.pixel(3, 3) == 0);
    REQUIRE(f.pixel(4, 4) == 0);
    REQUIRE(f.pixel(5, 5) == 0);
}

// ---------------------------------------------------------------------------
// encode_sla_video_with_ffmpeg_or_throw  (integration, requires ffmpeg)
// ---------------------------------------------------------------------------

TEST_CASE("encode_sla_video_with_ffmpeg_or_throw: synth frames produce valid MKV", "[SLAVideoSynth][ffmpeg]")
{
    if (boost::process::search_path("ffmpeg").empty())
        SKIP("ffmpeg not found in PATH");

    const boost::filesystem::path out =
        boost::filesystem::temp_directory_path() / "bioslicer_test_sla_synth.mkv";

    REQUIRE_NOTHROW(encode_sla_video_with_ffmpeg_or_throw(
        out,
        /*fps=*/5,
        /*lossless=*/false,
        /*frame_count=*/5,
        [](std::ostream &os, size_t) {
            render_sla_synth_pgm_frame(os, 16, 16);
        }
    ));

    REQUIRE(boost::filesystem::exists(out));
    REQUIRE(boost::filesystem::file_size(out) > 0);

    // Verify EBML magic bytes (MKV container header)
    std::ifstream f(out.string(), std::ios::binary);
    unsigned char header[4] = {};
    f.read(reinterpret_cast<char *>(header), 4);
    REQUIRE(header[0] == 0x1A);
    REQUIRE(header[1] == 0x45);
    REQUIRE(header[2] == 0xDF);
    REQUIRE(header[3] == 0xA3);

    boost::filesystem::remove(out);
}

TEST_CASE("encode_sla_video_with_ffmpeg_or_throw: slice frames produce valid MKV", "[SLAVideoSynth][ffmpeg]")
{
    if (boost::process::search_path("ffmpeg").empty())
        SKIP("ffmpeg not found in PATH");

    const boost::filesystem::path out =
        boost::filesystem::temp_directory_path() / "bioslicer_test_sla_slice.mkv";

    BoundingBoxf proj = make_bed(0, 0, 10, 10);
    ExPolygon sq;
    sq.contour = Polygon({
        Point(scale_(2), scale_(2)),
        Point(scale_(8), scale_(2)),
        Point(scale_(8), scale_(8)),
        Point(scale_(2), scale_(8)),
    });
    const ExPolygons expolys{sq};

    REQUIRE_NOTHROW(encode_sla_video_with_ffmpeg_or_throw(
        out,
        /*fps=*/5,
        /*lossless=*/false,
        /*frame_count=*/3,
        [&](std::ostream &os, size_t) {
            render_sla_slice_pgm_frame(os, 16, 16, expolys, proj);
        }
    ));

    REQUIRE(boost::filesystem::exists(out));
    REQUIRE(boost::filesystem::file_size(out) > 0);

    std::ifstream f(out.string(), std::ios::binary);
    unsigned char header[4] = {};
    f.read(reinterpret_cast<char *>(header), 4);
    REQUIRE(header[0] == 0x1A);
    REQUIRE(header[1] == 0x45);
    REQUIRE(header[2] == 0xDF);
    REQUIRE(header[3] == 0xA3);

    boost::filesystem::remove(out);
}
