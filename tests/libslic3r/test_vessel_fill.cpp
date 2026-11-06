#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_approx.hpp>

#include <algorithm>
#include <cmath>
#include <functional>
#include <random>
#include <sstream>

#include <libslic3r/MeshBoolean.hpp>
#include <libslic3r/TriangleMesh.hpp>
#include <libslic3r/Vessel/VesselTree.hpp>
#include <libslic3r/Vessel/VesselTube.hpp>

using namespace Slic3r;
using namespace Slic3r::vessel;
using Catch::Approx;

namespace {

// A closed mesh and the exact signed distance to the smooth surface it approximates (negative inside).
// The mesh has its corners on the smooth surface, so it lies inside it, by at most `chord` mm.
struct Container
{
    indexed_triangle_set                its;
    std::function<double(const Vec3d &)> distance;
    double                              chord;
};

Container sphere(double R, const Vec3d &centre)
{
    Container c;
    c.its = its_make_sphere(R, PI / 48.);
    for (Vec3f &v : c.its.vertices)
        v += centre.cast<float>();
    c.distance = [R, centre](const Vec3d &p) { return (p - centre).norm() - R; };
    // Sagitta of a facet edge spanning PI / 48 at radius R, twice over for the facet centre.
    c.chord = 2. * R * (1. - std::cos(PI / 96.));
    return c;
}

// A torus around the Z axis: ring radius R, tube radius a. It is not convex, and a straight vessel cannot cross its hole.
Container torus(double R, double a, int ring_steps, int tube_steps)
{
    Container c;
    for (int i = 0; i < ring_steps; ++i)
        for (int j = 0; j < tube_steps; ++j) {
            const double u = 2. * PI * i / ring_steps, v = 2. * PI * j / tube_steps;
            c.its.vertices.emplace_back(Vec3d((R + a * std::cos(v)) * std::cos(u), (R + a * std::cos(v)) * std::sin(u), a * std::sin(v)).cast<float>());
        }
    for (int i = 0; i < ring_steps; ++i)
        for (int j = 0; j < tube_steps; ++j) {
            const int i1 = (i + 1) % ring_steps, j1 = (j + 1) % tube_steps;
            const int v00 = i * tube_steps + j, v10 = i1 * tube_steps + j, v01 = i * tube_steps + j1, v11 = i1 * tube_steps + j1;
            c.its.indices.emplace_back(v00, v10, v11);
            c.its.indices.emplace_back(v00, v11, v01);
        }
    c.distance = [R, a](const Vec3d &p) { return std::hypot(std::hypot(p.x(), p.y()) - R, p.z()) - a; };
    c.chord    = 2. * (R + a) * (1. - std::cos(PI / ring_steps)) + 2. * a * (1. - std::cos(PI / tube_steps));
    return c;
}

// Largest signed distance from the smooth surface of any point of the skeleton's swept vessels, sampled along the
// polyline and along the spline through every branch, plus the radius there.
double worst_reach(const Skeleton &sk, const Container &c)
{
    double worst = -1e30;
    for (const Branch &b : sk.branches)
        for (bool spline : {false, true})
            for (const PathPoint &p : sample_centreline(b.path, spline, 0.05))
                worst = std::max(worst, c.distance(p.pos) + p.radius);
    return worst;
}

TreeParams fill_params(uint32_t seed)
{
    TreeParams tp;
    tp.mode           = GrowthMode::Fill;
    tp.seed           = seed;
    tp.root_radius    = 1.2;
    tp.root_length    = 6.;
    tp.fill_step      = 2.5;
    tp.fill_points    = 120;
    tp.fill_clearance = 0.8;
    return tp;
}

} // namespace

TEST_CASE("Filling a sphere keeps every vessel inside it with the clearance", "[Vessel][VesselFill]")
{
    const double    R = 15.;
    const Container ball = sphere(R, Vec3d(0, 0, 20));
    std::string     report;
    int             failures = 0;
    for (uint32_t seed = 1; seed <= 12; ++seed) {
        TreeParams tp    = fill_params(seed);
        tp.root_position = Vec3d(0, 0, 20. - R + 3.);
        tp.root_direction = Vec3d(0.1 * (seed % 3), 0, 1);
        const Skeleton sk = generate_tree(tp, &ball.its);
        INFO("seed " << seed);
        REQUIRE(validate_skeleton(sk).empty());

        // Murray's law with equal flow to every end: at every junction the children carry the parent's flow.
        size_t leaves = 0;
        for (const Branch &b : sk.branches) {
            leaves += b.is_leaf();
            if (b.is_leaf())
                continue;
            REQUIRE(b.children.size() <= 3);
            double sum = 0.;
            for (int c : b.children)
                sum += std::pow(sk.branches[c].start_radius(), 3);
            REQUIRE(sum == Approx(std::pow(b.end_radius(), 3)).epsilon(1e-12));
        }
        REQUIRE(sk.branches[0].start_radius() == Approx(tp.root_radius));
        // The leaves have the radius of one end's share: root radius * (1 / ends)^(1/3).
        for (const Branch &b : sk.branches)
            if (b.is_leaf())
                REQUIRE(b.start_radius() == Approx(tp.root_radius * std::pow(1. / double(leaves), 1. / 3.)).epsilon(1e-12));

        // Every vessel wall stays the clearance away from the smooth surface, less the facets of its mesh.
        const double reach = worst_reach(sk, ball);
        // The tree fills the sphere: most points of it lie within two growth steps of a vessel axis. Targets are only
        // placed the clearance plus half a step inside the surface, so that is the region measured.
        std::vector<Vec3d> axis_points;
        for (const Branch &b : sk.branches)
            for (const PathPoint &p : sample_centreline(b.path, false, 0.5))
                axis_points.emplace_back(p.pos);
        std::mt19937                           gen(seed);
        std::uniform_real_distribution<double> u(-1., 1.);
        int                                    inside = 0, near = 0;
        while (inside < 2000) {
            const Vec3d q = Vec3d(0, 0, 20) + (R - tp.fill_clearance) * Vec3d(u(gen), u(gen), u(gen));
            if (ball.distance(q) > -tp.fill_clearance - 0.5 * tp.fill_step)
                continue;
            ++inside;
            near += std::any_of(axis_points.begin(), axis_points.end(), [&](const Vec3d &a) { return (a - q).norm() <= 2. * tp.fill_step; });
        }
        const double coverage = double(near) / inside;
        if (leaves < 15 || reach > -tp.fill_clearance + ball.chord || coverage < 0.9) {
            ++failures;
            std::ostringstream line;
            line << "seed " << seed << ": " << sk.branches.size() << " vessels, " << leaves << " ends, wall comes within "
                 << -reach << " mm of the surface, " << 100. * coverage << " percent of the sphere within two steps of a vessel\n";
            report += line.str();
        }
    }
    INFO(report);
    CHECK(failures == 0);
}

TEST_CASE("Filling a torus keeps the vessels in its curved tube", "[Vessel][VesselFill]")
{
    const Container ring = torus(20., 6., 96, 32);
    std::string     report;
    int             failures = 0;
    for (uint32_t seed = 1; seed <= 8; ++seed) {
        TreeParams tp     = fill_params(seed);
        tp.root_position  = Vec3d(20., 0., 0.);
        tp.root_direction = Vec3d(0, 1, 0);
        tp.root_length    = 3.;
        tp.fill_points    = 200;
        const Skeleton sk = generate_tree(tp, &ring.its);
        const double   reach = worst_reach(sk, ring);
        // The vessels spread round the ring: some reach the far side, half a turn away from the root.
        double farthest = 0.;
        for (const Branch &b : sk.branches)
            farthest = std::max(farthest, std::abs(std::atan2(b.end().y(), b.end().x())));
        if (reach > -tp.fill_clearance + ring.chord || farthest < 0.75 * PI) {
            ++failures;
            std::ostringstream line;
            line << "seed " << seed << ": " << sk.branches.size() << " vessels, wall comes within " << -reach
                 << " mm of the surface, farthest end at " << farthest * 180. / PI << " degrees\n";
            report += line.str();
        }
    }
    INFO(report);
    CHECK(failures == 0);
}

TEST_CASE("A filled tree meshes into a clean solid inside the container", "[Vessel][VesselFill]")
{
    const double    R = 12.;
    const Container ball = sphere(R, Vec3d::Zero());
    for (uint32_t seed : {3u, 4u}) {
        TreeParams tp     = fill_params(seed);
        tp.root_position  = Vec3d(0, 0, -R + 3.);
        tp.fill_points    = 60;
        const Skeleton sk = generate_tree(tp, &ball.its);
        for (double wall : {0., 0.2}) {
            VesselMeshParams mp;
            mp.sides          = 16;
            mp.wall_thickness = wall;
            INFO("seed " << seed << " wall " << wall);
            const indexed_triangle_set its = make_tree_mesh(sk, mp);
            const MeshCheck            chk = check_mesh(its);
            REQUIRE(chk.watertight());
            REQUIRE(chk.degenerate_faces == 0);
            REQUIRE(chk.volume > 0.);
            REQUIRE(its_number_of_patches(its) == 1);
            REQUIRE_FALSE(MeshBoolean::cgal::does_self_intersect(TriangleMesh(its)));
            double worst = -1e30;
            for (const Vec3f &v : its.vertices)
                worst = std::max(worst, ball.distance(v.cast<double>()));
            REQUIRE(worst <= -tp.fill_clearance + ball.chord);
        }
    }
}

TEST_CASE("Filling is deterministic and checks its input", "[Vessel][VesselFill]")
{
    const Container ball = sphere(10., Vec3d::Zero());
    TreeParams      tp   = fill_params(5);
    tp.root_position     = Vec3d(0, 0, -7.);
    REQUIRE(skeleton_to_csv(generate_tree(tp, &ball.its)) == skeleton_to_csv(generate_tree(tp, &ball.its)));
    TreeParams other = tp;
    other.seed       = 6;
    REQUIRE(skeleton_to_csv(generate_tree(tp, &ball.its)) != skeleton_to_csv(generate_tree(other, &ball.its)));

    // Filling needs an object, a root inside it, and room for the root vessel.
    REQUIRE_THROWS_AS(generate_tree(tp), InvalidArgument);
    TreeParams outside = tp;
    outside.root_position = Vec3d(0, 0, -30.);
    REQUIRE_THROWS_AS(generate_tree(outside, &ball.its), InvalidArgument);
    TreeParams fat = tp;
    fat.root_radius = 9.;
    REQUIRE_THROWS_AS(generate_tree(fat, &ball.its), InvalidArgument);
    TreeParams bad = tp;
    bad.fill_step = 0.;
    REQUIRE(!validate_tree_params(bad).empty());
}
