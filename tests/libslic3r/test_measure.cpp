#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_approx.hpp>

#include <algorithm>
#include <cmath>
#include <optional>

#include "libslic3r/Measure.hpp"
#include "libslic3r/TriangleMesh.hpp"

using namespace Slic3r;
using namespace Slic3r::Measure;
using Catch::Approx;

namespace {

constexpr double pi = 3.14159265358979323846;

// The whole-plane feature of the plane of `mesh` whose normal points along `direction`.
std::optional<SurfaceFeature> find_plane(const Measuring &measuring, const Vec3d &direction)
{
    for (int i = 0; i < measuring.get_num_of_planes(); ++i) {
        const SurfaceFeature &plane = measuring.get_plane_features(unsigned(i)).back();
        if (plane.get_type() == SurfaceFeatureType::Plane && std::get<1>(plane.get_plane()).dot(direction) > 0.999)
            return plane;
    }
    return std::nullopt;
}

std::vector<SurfaceFeature> features_of_type(const Measuring &measuring, const Vec3d &direction, SurfaceFeatureType type)
{
    std::vector<SurfaceFeature> out;
    for (int i = 0; i < measuring.get_num_of_planes(); ++i) {
        const auto &features = measuring.get_plane_features(unsigned(i));
        if (std::get<1>(features.back().get_plane()).dot(direction) > 0.999)
            for (const SurfaceFeature &f : features)
                if (f.get_type() == type)
                    out.emplace_back(f);
    }
    return out;
}

} // namespace

TEST_CASE("A box has six planes and the distances between them are its edge lengths", "[Measure]")
{
    // 20 x 10 x 5 mm.
    const indexed_triangle_set box = its_make_cube(20., 10., 5.);
    const Measuring measuring(box);
    REQUIRE(measuring.get_num_of_planes() == 6);

    // Every plane is made of two triangles; together they are all twelve faces.
    size_t faces = 0;
    for (int i = 0; i < measuring.get_num_of_planes(); ++i)
        faces += measuring.get_plane_triangle_indices(i).size();
    CHECK(faces == box.indices.size());

    const auto top    = find_plane(measuring, Vec3d::UnitZ());
    const auto bottom = find_plane(measuring, -Vec3d::UnitZ());
    const auto right  = find_plane(measuring, Vec3d::UnitX());
    const auto left   = find_plane(measuring, -Vec3d::UnitX());
    const auto back   = find_plane(measuring, Vec3d::UnitY());
    const auto front  = find_plane(measuring, -Vec3d::UnitY());
    REQUIRE((top && bottom && right && left && back && front));

    auto gap = [](const SurfaceFeature &a, const SurfaceFeature &b) {
        const MeasurementResult r = get_measurement(a, b);
        REQUIRE(r.distance_infinite.has_value());
        return r.distance_infinite->dist;
    };
    CHECK(gap(*top, *bottom) == Approx(5.).margin(1e-6));
    CHECK(gap(*left, *right) == Approx(20.).margin(1e-6));
    CHECK(gap(*front, *back) == Approx(10.).margin(1e-6));

    // Planes at a right angle give an angle and no distance.
    const MeasurementResult corner = get_measurement(*top, *right);
    REQUIRE(corner.angle.has_value());
    CHECK(corner.angle->angle == Approx(pi / 2.).margin(1e-6));
    CHECK_FALSE(corner.distance_infinite.has_value());
}

TEST_CASE("The edges and corners of a face are found with their lengths", "[Measure]")
{
    const indexed_triangle_set box = its_make_cube(20., 10., 5.);
    const Measuring measuring(box);

    const auto edges = features_of_type(measuring, Vec3d::UnitZ(), SurfaceFeatureType::Edge);
    REQUIRE(edges.size() == 4);
    std::vector<double> lengths;
    for (const SurfaceFeature &e : edges) {
        const auto [from, to] = e.get_edge();
        lengths.push_back((to - from).norm());
        CHECK(from.z() == Approx(5.).margin(1e-6));
        CHECK(to.z() == Approx(5.).margin(1e-6));
    }
    std::sort(lengths.begin(), lengths.end());
    CHECK(lengths[0] == Approx(10.).margin(1e-6));
    CHECK(lengths[1] == Approx(10.).margin(1e-6));
    CHECK(lengths[2] == Approx(20.).margin(1e-6));
    CHECK(lengths[3] == Approx(20.).margin(1e-6));

    // Two edges of the same face meeting at a corner are perpendicular.
    const SurfaceFeature *long_edge = nullptr, *short_edge = nullptr;
    for (const SurfaceFeature &e : edges) {
        const double len = (e.get_edge().second - e.get_edge().first).norm();
        (len > 15. ? long_edge : short_edge) = &e;
    }
    REQUIRE((long_edge && short_edge));
    CHECK(are_perpendicular(*long_edge, *short_edge));
    CHECK_FALSE(are_parallel(*long_edge, *short_edge));
    const MeasurementResult angle = get_measurement(*long_edge, *short_edge);
    REQUIRE(angle.angle.has_value());
    CHECK(angle.angle->angle == Approx(pi / 2.).margin(1e-6));
}

TEST_CASE("Measuring between points gives the euclidean distance and its x y z parts", "[Measure]")
{
    const SurfaceFeature a(Vec3d(0., 0., 0.));
    const SurfaceFeature b(Vec3d(20., 10., 5.));
    const MeasurementResult r = get_measurement(a, b);
    REQUIRE(r.distance_strict.has_value());
    CHECK(r.distance_strict->dist == Approx(std::sqrt(20. * 20. + 10. * 10. + 5. * 5.)).margin(1e-9));
    REQUIRE(r.distance_xyz.has_value());
    CHECK(r.distance_xyz->x() == Approx(20.));
    CHECK(r.distance_xyz->y() == Approx(10.));
    CHECK(r.distance_xyz->z() == Approx(5.));

    // The order of the features does not matter.
    CHECK(get_measurement(b, a).distance_strict->dist == Approx(r.distance_strict->dist));
}

TEST_CASE("A point is measured to an edge and to a plane", "[Measure]")
{
    const SurfaceFeature edge(SurfaceFeatureType::Edge, Vec3d(0., 0., 0.), Vec3d(10., 0., 0.));

    SECTION("Beside the middle of the edge the distance is perpendicular") {
        const MeasurementResult r = get_measurement(SurfaceFeature(Vec3d(4., 3., 0.)), edge);
        REQUIRE(r.distance_strict.has_value());
        CHECK(r.distance_strict->dist == Approx(3.).margin(1e-9));
    }
    SECTION("Beyond the end of the edge the strict distance is to the end point") {
        const MeasurementResult r = get_measurement(SurfaceFeature(Vec3d(13., 4., 0.)), edge);
        REQUIRE(r.distance_strict.has_value());
        CHECK(r.distance_strict->dist == Approx(5.).margin(1e-9));
        REQUIRE(r.distance_infinite.has_value());
        CHECK(r.distance_infinite->dist == Approx(4.).margin(1e-9));
    }
    SECTION("A point above a plane is at its height") {
        const indexed_triangle_set box = its_make_cube(20., 10., 5.);
        const Measuring measuring(box);
        const auto top = find_plane(measuring, Vec3d::UnitZ());
        REQUIRE(top.has_value());
        const MeasurementResult r = get_measurement(SurfaceFeature(Vec3d(3., 3., 12.)), *top);
        REQUIRE(r.distance_infinite.has_value());
        CHECK(r.distance_infinite->dist == Approx(7.).margin(1e-6));
    }
}

TEST_CASE("The round end of a cylinder is measured as a circle of its radius", "[Measure]")
{
    // 8 mm diameter, 10 mm tall, 128 sides.
    const indexed_triangle_set cylinder = its_make_cylinder(4., 10., 2. * pi / 128.);
    const Measuring measuring(cylinder);

    const auto circles = features_of_type(measuring, Vec3d::UnitZ(), SurfaceFeatureType::Circle);
    REQUIRE(circles.size() == 1);
    const auto [center, radius, normal] = circles.front().get_circle();
    CHECK(radius == Approx(4.).epsilon(1e-3));
    CHECK(center.x() == Approx(0.).margin(1e-3));
    CHECK(center.y() == Approx(0.).margin(1e-3));
    CHECK(center.z() == Approx(10.).margin(1e-3));
    CHECK(std::abs(normal.z()) == Approx(1.).margin(1e-6));

    // The distance between the two end circles' centres is the height.
    const auto bottom = features_of_type(measuring, -Vec3d::UnitZ(), SurfaceFeatureType::Circle);
    REQUIRE(bottom.size() == 1);
    const MeasurementResult r = get_measurement(SurfaceFeature(std::get<0>(circles.front().get_circle())),
                                                SurfaceFeature(std::get<0>(bottom.front().get_circle())));
    REQUIRE(r.distance_strict.has_value());
    CHECK(r.distance_strict->dist == Approx(10.).margin(1e-3));
}
