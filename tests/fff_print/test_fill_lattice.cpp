#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_approx.hpp>

#include <cmath>
#include <memory>

#include "libslic3r/ClipperUtils.hpp"
#include "libslic3r/ExPolygon.hpp"
#include "libslic3r/Fill/FillBase.hpp"
#include "libslic3r/Fill/FillGyroid.hpp"
#include "libslic3r/Surface.hpp"

using namespace Slic3r;
using Catch::Approx;

namespace {

constexpr double spacing = 0.45;

// The lines the pattern makes inside a square of the given side (mm) at height z (mm), not joined along the border.
Polylines pattern_lines(const char *type, double density, double z, double side, size_t layer_id = 0)
{
    std::unique_ptr<Fill> filler(Fill::new_from_type(type));
    const ExPolygon square(Polygon::new_scale({ { 0., 0. }, { side, 0. }, { side, side }, { 0., side } }));
    filler->bounding_box = get_extents(square);
    filler->angle        = 0.f;
    filler->spacing      = spacing;
    filler->z            = z;
    filler->layer_id     = layer_id;
    FillParams params;
    params.density           = float(density);
    params.anchor_length     = 0.f;
    params.anchor_length_max = 0.f;
    Surface surface(stInternal, square);
    return filler->fill_surface(&surface, params);
}

double total_length_mm(const Polylines &lines)
{
    double length = 0.;
    for (const Polyline &line : lines)
        length += unscaled(line.length());
    return length;
}


// Share of the square covered by lines of the width of the spacing, averaged over eight heights spread over a few cells.
double mean_share(const char *type, double density, double side = 60.)
{
    double sum = 0.;
    int    n   = 0;
    for (double z = 0.; z < 8.; z += 1.) {
        sum += total_length_mm(pattern_lines(type, density, z, side)) * spacing / (side * side);
        ++n;
    }
    return sum / n;
}

} // namespace

TEST_CASE("Gyroid lines lie inside the area and cover about the density", "[FillLattice]")
{
    const double side = 60.;
    const ExPolygon area(Polygon::new_scale({ { 0., 0. }, { side, 0. }, { side, side }, { 0., side } }));
    const ExPolygons grown = offset_ex(area, float(scale_(0.01)));
    for (double density : { 0.1, 0.2, 0.4 }) {
        const Polylines lines = pattern_lines("gyroid", density, 1.5, side);
        REQUIRE_FALSE(lines.empty());
        for (const Polyline &line : lines)
            for (const Point &pt : line.points)
                CHECK(grown.front().contains(pt));
        // The pattern is tuned so that lines of one spacing in width weigh the density; within 12 %.
        CHECK(mean_share("gyroid", density) == Approx(density).epsilon(0.12));
    }
}

TEST_CASE("Gyroid changes with height and repeats after one cell", "[FillLattice]")
{
    const Polylines a = pattern_lines("gyroid", 0.2, 0., 40.);
    const Polylines b = pattern_lines("gyroid", 0.2, 1., 40.);
    REQUIRE_FALSE(a.empty());
    REQUIRE_FALSE(b.empty());
    // One layer up the waves have moved.
    CHECK(a.front().points != b.front().points);

    // The cell is 2 pi * spacing / (density * 2.44) long in z.
    const double cell = 2. * 3.14159265358979323846 * spacing / (0.2 * FillGyroid::DensityAdjust);
    const Polylines c = pattern_lines("gyroid", 0.2, cell, 40.);
    REQUIRE(c.size() == a.size());
    for (size_t i = 0; i < a.size(); ++i) {
        REQUIRE(c[i].points.size() == a[i].points.size());
        for (size_t j = 0; j < a[i].points.size(); ++j)
            CHECK((c[i].points[j] - a[i].points[j]).cast<double>().norm() < scale_(0.01));
    }
}

TEST_CASE("3D honeycomb lines lie inside the area and grow with the density", "[FillLattice]")
{
    const double side = 60.;
    const ExPolygon area(Polygon::new_scale({ { 0., 0. }, { side, 0. }, { side, side }, { 0., side } }));
    const ExPolygons grown = offset_ex(area, float(scale_(0.01)));
    double previous = 0.;
    for (double density : { 0.1, 0.2, 0.4 }) {
        const Polylines lines = pattern_lines("3dhoneycomb", density, 1.5, side);
        REQUIRE_FALSE(lines.empty());
        for (const Polyline &line : lines)
            for (const Point &pt : line.points)
                CHECK(grown.front().contains(pt));
        const double share = mean_share("3dhoneycomb", density);
        // Lines of one spacing in width cover the density within 20 %, and a denser fill has more line.
        CHECK(share == Approx(density).epsilon(0.20));
        CHECK(share > previous);
        previous = share;
    }
}

TEST_CASE("3D honeycomb alternates its lines between layers and changes with height", "[FillLattice]")
{
    const Polylines layer0 = pattern_lines("3dhoneycomb", 0.2, 2., 40., 0);
    const Polylines layer1 = pattern_lines("3dhoneycomb", 0.2, 2., 40., 1);
    REQUIRE_FALSE(layer0.empty());
    REQUIRE_FALSE(layer1.empty());
    // Columns on one layer, rows on the next: the first line runs along y, then along x.
    auto extent = [](const Polyline &line) { return get_extents(line); };
    CHECK(extent(layer0.front()).size().y() > extent(layer0.front()).size().x());
    CHECK(extent(layer1.front()).size().x() > extent(layer1.front()).size().y());

    const Polylines higher = pattern_lines("3dhoneycomb", 0.2, 2.3, 40., 0);
    CHECK(total_length_mm(higher) != Approx(total_length_mm(layer0)).epsilon(1e-6));
}
