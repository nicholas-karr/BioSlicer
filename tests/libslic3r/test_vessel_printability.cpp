#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_approx.hpp>

#include <cmath>

#include <libslic3r/TriangleMesh.hpp>
#include <libslic3r/Vessel/VesselPrintability.hpp>
#include <libslic3r/Vessel/VesselTree.hpp>

using namespace Slic3r;
using namespace Slic3r::vessel;
using Catch::Approx;

namespace {

Branch make_branch(int parent, const Vec3d &start, const Vec3d &end, double radius)
{
    Branch b;
    b.parent = parent;
    b.start  = start;
    b.end    = end;
    b.radius = radius;
    return b;
}

} // namespace

TEST_CASE("Segment distance", "[Vessel]")
{
    // Crossing at right angles, 3 mm apart.
    REQUIRE(segment_distance(Vec3d(-5, 0, 0), Vec3d(5, 0, 0), Vec3d(0, -5, 3), Vec3d(0, 5, 3)) == Approx(3.));
    // Parallel.
    REQUIRE(segment_distance(Vec3d(0, 0, 0), Vec3d(10, 0, 0), Vec3d(2, 4, 0), Vec3d(8, 4, 0)) == Approx(4.));
    // End point to end point.
    REQUIRE(segment_distance(Vec3d(0, 0, 0), Vec3d(1, 0, 0), Vec3d(4, 4, 0), Vec3d(5, 4, 0)) == Approx(5.));
    // A segment that is a point.
    REQUIRE(segment_distance(Vec3d(0, 0, 0), Vec3d(0, 0, 0), Vec3d(3, 4, 0), Vec3d(3, 4, 5)) == Approx(5.));
    REQUIRE(segment_distance(Vec3d(0, 0, 0), Vec3d(0, 0, 0), Vec3d(0, 0, 0), Vec3d(0, 0, 0)) == Approx(0.));
    // Touching.
    REQUIRE(segment_distance(Vec3d(0, 0, 0), Vec3d(2, 0, 0), Vec3d(1, -1, 0), Vec3d(1, 1, 0)) == Approx(0.).margin(1e-12));
}

TEST_CASE("Channel printability: diameters, walls between channels and walls to the block", "[Vessel]")
{
    SECTION("channel diameter")
    {
        Skeleton sk;
        sk.branches.push_back(make_branch(Branch::NONE, Vec3d(0, 0, 0), Vec3d(10, 0, 0), 0.5));
        REQUIRE(check_channel_printability(sk, 0.4).ok());
        const auto rep = check_channel_printability(sk, 1.2);
        REQUIRE(rep.min_channel_diameter == Approx(1.));
        REQUIRE(rep.narrowest_branch == 0);
        REQUIRE(rep.narrow_branches == std::vector<int>{0});
        REQUIRE_FALSE(rep.ok());
        // A hollow tree printed as it is has its lumen as the channel.
        REQUIRE(check_channel_printability(sk, 0.1, 0.2).min_channel_diameter == Approx(0.6));
        REQUIRE_THROWS_AS(check_channel_printability(sk, 0.1, 0.5), InvalidArgument);
        REQUIRE_THROWS_AS(check_channel_printability(sk, -1.), InvalidArgument);
    }

    SECTION("wall between two channels that are not neighbours")
    {
        // A U shape: the root and its grandchild run side by side 3 mm apart (axis to axis), radius 1 mm.
        Skeleton sk;
        sk.branches.push_back(make_branch(Branch::NONE, Vec3d(0, 0, 0), Vec3d(10, 0, 0), 1.));
        sk.branches.push_back(make_branch(0, Vec3d(10, 0, 0), Vec3d(10, 3, 0), 1.));
        sk.branches.push_back(make_branch(1, Vec3d(10, 3, 0), Vec3d(0, 3, 0), 1.));
        sk.branches[0].child_a = 1;
        sk.branches[1].child_a = 2;
        const auto rep = check_channel_printability(sk, 0.5);
        REQUIRE(rep.min_wall_between_channels == Approx(1.).margin(1e-9));
        REQUIRE(rep.wall_branch_a == 0);
        REQUIRE(rep.wall_branch_b == 2);
        REQUIRE(rep.ok());
        const auto thin = check_channel_printability(sk, 1.5);
        REQUIRE_FALSE(thin.ok());
        REQUIRE(thin.thin_walls.size() == 1);
        REQUIRE(thin.thin_walls[0].wall == Approx(1.).margin(1e-9));
        // Channels that overlap have a wall of zero or less.
        sk.branches[2].start = Vec3d(10, 1.5, 0);
        sk.branches[2].end   = Vec3d(0, 1.5, 0);
        sk.branches[1].end   = Vec3d(10, 1.5, 0);
        const auto touching = check_channel_printability(sk, 0.5);
        REQUIRE(touching.min_wall_between_channels == Approx(-0.5).margin(1e-9));
        const auto words = printability_warnings(touching, 0.5);
        REQUIRE(words.size() == 1);
        REQUIRE(words[0].find("touch or overlap") != std::string::npos);
    }

    SECTION("wall between a channel and the surface of the block")
    {
        // Block 20 x 20 x 10 mm, channel along x at z = 8.5 mm, radius 1 mm: 1.5 mm below the top, so the wall is 0.5 mm.
        indexed_triangle_set block = its_make_cube(20., 20., 10.);
        Skeleton sk;
        sk.branches.push_back(make_branch(Branch::NONE, Vec3d(0, 10, 8.5), Vec3d(20, 10, 8.5), 1.));
        const auto rep = check_channel_printability(sk, 0.4, 0., &block);
        REQUIRE(rep.min_wall_to_surface == Approx(0.5).margin(0.01)); // 48 directions: at most 0.3 percent of 1.5 mm too large
        REQUIRE(rep.min_wall_to_surface >= 0.5 - 1e-9);
        REQUIRE(rep.surface_wall_branch == 0);
        REQUIRE(rep.ok());
        const auto thin = check_channel_printability(sk, 0.6, 0., &block);
        REQUIRE_FALSE(thin.ok());
        REQUIRE(thin.thin_walls.size() == 1);
        REQUIRE(thin.thin_walls[0].branch_b == Branch::NONE);
        const auto words = printability_warnings(thin, 0.6);
        REQUIRE(words.size() == 1);
        REQUIRE(words[0].find("outside of the block") != std::string::npos);
        REQUIRE(words[0].find("0.50 mm") != std::string::npos);
        // A channel that opens through the end face is not a thin wall there: the same channel, shifted next to the end face.
        Skeleton shifted;
        shifted.branches.push_back(make_branch(Branch::NONE, Vec3d(0, 10, 5), Vec3d(20, 10, 5), 1.));
        REQUIRE(check_channel_printability(shifted, 0.4, 0., &block).min_wall_to_surface == Approx(4.).margin(0.02));
        // Without a block nothing is said about the surface.
        REQUIRE(check_channel_printability(sk, 0.6).min_wall_to_surface == PrintabilityReport::NONE);
    }

    SECTION("a grown tree")
    {
        TreeParams tp;
        tp.depth       = 4;
        tp.root_radius = 1.2;
        tp.root_length = 15.;
        tp.seed        = 9;
        const Skeleton sk = generate_tree(tp);
        double smallest = 1e9;
        for (const Branch &b : sk.branches)
            smallest = std::min(smallest, 2. * b.radius);
        const auto a = check_channel_printability(sk, 0.2), b = check_channel_printability(sk, 1.0), c = check_channel_printability(sk, 5.);
        REQUIRE(a.min_channel_diameter == Approx(smallest));
        // The leaves of this tree have a radius of 1.2 * 2^(-4/3) = 0.476 mm.
        REQUIRE(smallest == Approx(2. * 1.2 * std::pow(2., -4. / 3.)).epsilon(1e-9));
        REQUIRE(a.narrow_branches.empty());
        REQUIRE(b.narrow_branches.size() == 16); // the 16 leaves
        REQUIRE(c.narrow_branches.size() == sk.branches.size());
        REQUIRE(a.thin_walls.size() <= b.thin_walls.size());
        REQUIRE(b.thin_walls.size() <= c.thin_walls.size());
        REQUIRE(a.min_wall_between_channels == b.min_wall_between_channels);
        REQUIRE(printability_warnings(a, 0.2).empty());
        REQUIRE_FALSE(printability_warnings(c, 5.).empty());
    }
}
