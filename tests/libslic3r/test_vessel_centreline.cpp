#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_approx.hpp>

#include <cmath>
#include <string>

#include <libslic3r/AABBTreeIndirect.hpp>
#include <libslic3r/MeshBoolean.hpp>
#include <libslic3r/TriangleMesh.hpp>
#include <libslic3r/Vessel/VesselTree.hpp>
#include <libslic3r/Vessel/VesselTube.hpp>

using namespace Slic3r;
using namespace Slic3r::vessel;
using Catch::Approx;

namespace {

double polygon_area_factor(int n) { return n * std::sin(2. * PI / n) / (2. * PI); }

// Points of a helix of radius `a` around the Z axis rising `pitch` mm per turn, every `step` radians from angle 0 to `turns`.
std::vector<PathPoint> helix(double a, double pitch, double turns, double step, double radius, const Vec3d &offset = Vec3d::Zero())
{
    std::vector<PathPoint> pts;
    const int n = int(std::round(2. * PI * turns / step));
    for (int i = 0; i <= n; ++i) {
        const double t = 2. * PI * turns * i / n;
        pts.push_back({offset + Vec3d(a * std::cos(t), a * std::sin(t), pitch * t / (2. * PI)), radius});
    }
    return pts;
}

std::vector<double> ray_distances(const indexed_triangle_set &its, const Vec3d &origin, const Vec3d &dir)
{
    const auto            tree = AABBTreeIndirect::build_aabb_tree_over_indexed_triangle_set(its.vertices, its.indices);
    std::vector<igl::Hit> hits;
    AABBTreeIndirect::intersect_ray_all_hits(its.vertices, its.indices, tree, origin, dir, hits);
    std::vector<double> t;
    for (const igl::Hit &h : hits)
        if (t.empty() || h.t - t.back() > 1e-6)
            t.emplace_back(h.t);
    return t;
}

// A tree from point lists: a curved root that tapers from 2 to 1.6 mm, a straight child and a curved child.
std::vector<BranchPath> curved_tree_paths()
{
    std::vector<BranchPath> paths(3);
    paths[0].points = {{Vec3d(0, 0, 0), 2.}, {Vec3d(2, 0, 8), 1.9}, {Vec3d(6, 1, 15), 1.75}, {Vec3d(8, 2, 22), 1.6}};
    paths[1].parent = 0;
    paths[1].points = {{Vec3d(8, 2, 22), 1.27}, {Vec3d(16, 2, 30), 1.27}};
    paths[2].parent = 0;
    paths[2].points = {{Vec3d(8, 2, 22), 1.27}, {Vec3d(4, 3, 28), 1.2}, {Vec3d(3, 8, 34), 1.1}, {Vec3d(-2, 12, 38), 1.}};
    return paths;
}

} // namespace

TEST_CASE("A tube along a helix has the volume of its length", "[Vessel][VesselCentreline]")
{
    // Helix of radius 10 mm and pitch 12 mm: curvature radius a + p^2 / (4 pi^2 a) = 10.36 mm, far more than the 1.5 mm tube.
    const double a = 10., pitch = 12., turns = 1.25, r = 1.5;
    const double length = turns * std::hypot(2. * PI * a, pitch);
    const auto   path   = helix(a, pitch, turns, PI / 6., r);
    for (bool spline : {true, false}) {
        TubeParams p;
        p.spline         = spline;
        p.sides          = 48;
        p.sample_spacing = 0.4;
        p.cap_start = p.cap_end = CapStyle::Flat;
        INFO((spline ? "spline" : "straight pieces"));
        REQUIRE(validate_tube(path, p).empty());
        const auto its = make_tube(path, p);
        const auto chk = check_mesh(its);
        REQUIRE(chk.watertight());
        REQUIRE(chk.degenerate_faces == 0);
        REQUIRE_FALSE(MeshBoolean::cgal::does_self_intersect(TriangleMesh(its)));
        if (spline) {
            // Pappus: pi r^2 times the length of the helix; the spline through points 30 degrees apart follows it closely.
            REQUIRE(chk.volume == Approx(PI * r * r * length * polygon_area_factor(48)).epsilon(0.005));
        } else {
            // Mitre joints are exact: pi r^2 times the length of the polyline through the points.
            REQUIRE(chk.volume == Approx(PI * r * r * polyline_length(path) * polygon_area_factor(48)).epsilon(1e-4));
            // Each chord spans 30 degrees: (2 a sin(15 degrees))^2 + (pitch / 12)^2, squared.
            const double chord = std::hypot(2. * a * std::sin(PI / 12.), pitch / 12.);
            REQUIRE(polyline_length(path) == Approx(15. * chord).epsilon(1e-12));
        }
    }
}

TEST_CASE("A tree from point lists keeps its shape, radii and wall", "[Vessel][VesselCentreline]")
{
    const Skeleton sk = make_skeleton(curved_tree_paths());
    REQUIRE(sk.branches.size() == 3);
    REQUIRE(sk.branches[0].children == std::vector<int>{1, 2});
    REQUIRE(sk.branches[2].depth == 1);
    REQUIRE(sk.branches[2].path.size() == 4);

    VesselMeshParams mp;
    mp.sides = 32;
    for (bool spline : {true, false}) {
        mp.spline = spline;
        INFO((spline ? "spline" : "straight pieces"));
        const auto its = make_tree_mesh(sk, mp);
        const auto chk = check_mesh(its);
        REQUIRE(chk.watertight());
        REQUIRE(chk.degenerate_faces == 0);
        REQUIRE(its_number_of_patches(its) == 1);
        REQUIRE_FALSE(MeshBoolean::cgal::does_self_intersect(TriangleMesh(its)));

        // The surface passes through the radius given at every inner point of the curved child, measured in the plane
        // that bisects the two pieces meeting there.
        const Branch &c = sk.branches[2];
        for (size_t k = 1; k + 1 < c.path.size(); ++k) {
            const Vec3d n  = ((c.path[k].pos - c.path[k - 1].pos).normalized() + (c.path[k + 1].pos - c.path[k].pos).normalized()).normalized();
            const Vec3d u  = n.cross(Vec3d::UnitX()).normalized();
            const Vec3d d  = (u * std::cos(0.37) + n.cross(u) * std::sin(0.37)).normalized();
            const auto  t  = ray_distances(its, c.path[k].pos, d);
            REQUIRE(!t.empty());
            // A straight-piece mitre stretches the section along the bend; a spline section is a circle.
            REQUIRE(t[0] >= c.path[k].radius * (spline ? 0.99 : 0.99));
            REQUIRE(t[0] <= c.path[k].radius * (spline ? 1.005 : 1.2));
        }
    }

    SECTION("hollow: the wall is as thick as asked along the curves")
    {
        mp.spline         = true;
        mp.wall_thickness = 0.4;
        const auto its    = make_tree_mesh(sk, mp);
        const auto chk    = check_mesh(its);
        REQUIRE(chk.watertight());
        REQUIRE(chk.degenerate_faces == 0);
        REQUIRE_FALSE(MeshBoolean::cgal::does_self_intersect(TriangleMesh(its)));
        int measured = 0;
        for (const Branch &b : sk.branches) {
            const std::vector<PathPoint> s = sample_centreline(b.path, true, 0.5);
            // Stations away from the junction and the open ends.
            for (size_t k = s.size() / 4; k < 3 * s.size() / 4; k += 3) {
                const Vec3d tangent = (s[k + 1].pos - s[k - 1].pos).normalized();
                const Vec3d u       = tangent.cross(Vec3d::UnitY()).normalized();
                for (double angle : {0.4, 2.5, 4.6}) {
                    const Vec3d d = u * std::cos(angle) + tangent.cross(u) * std::sin(angle);
                    const auto  t = ray_distances(its, s[k].pos, d);
                    REQUIRE(t.size() >= 2);
                    REQUIRE(t[0] == Approx(s[k].radius - 0.4).epsilon(0.01));
                    REQUIRE(t[1] - t[0] == Approx(0.4).margin(0.02));
                    ++measured;
                }
            }
        }
        REQUIRE(measured > 40);
    }
}

TEST_CASE("Centreline tables read back as the same tree", "[Vessel][VesselCentreline]")
{
    const Skeleton    sk  = make_skeleton(curved_tree_paths());
    const std::string csv = skeleton_to_csv(sk);
    const Skeleton    back = skeleton_from_csv(csv);
    REQUIRE(skeleton_to_csv(back) == csv);
    REQUIRE(back.branches.size() == sk.branches.size());
    for (size_t i = 0; i < sk.branches.size(); ++i) {
        REQUIRE(back.branches[i].parent == sk.branches[i].parent);
        REQUIRE(back.branches[i].children == sk.branches[i].children);
        REQUIRE(back.branches[i].path.size() == sk.branches[i].path.size());
        for (size_t k = 0; k < sk.branches[i].path.size(); ++k) {
            REQUIRE(back.branches[i].path[k].pos == sk.branches[i].path[k].pos);
            REQUIRE(back.branches[i].path[k].radius == sk.branches[i].path[k].radius);
        }
    }

    // A grown tree with awkward numbers, too.
    TreeParams tp;
    tp.depth        = 3;
    tp.children     = 3;
    tp.taper        = 0.85;
    tp.root_radius  = 2. / 3.;
    tp.angle_jitter = 7.;
    tp.seed         = 11;
    const Skeleton grown = generate_tree(tp);
    REQUIRE(skeleton_to_csv(skeleton_from_csv(skeleton_to_csv(grown))) == skeleton_to_csv(grown));

    SECTION("a table typed by hand: Windows line ends, no header, a child that starts a hair off its parent's end")
    {
        const std::string hand = "0,-1,0,0,0,0,0,2\r\n0,-1,0,1,0,0,10,1.5\r\n1,0,1,0,0.004,0,10,1\r\n1,0,1,1,5,0,15,1\r\n";
        const Skeleton    t    = skeleton_from_csv(hand);
        REQUIRE(t.branches.size() == 2);
        REQUIRE(t.branches[1].start() == t.branches[0].end());
        REQUIRE(t.branches[0].end_radius() == Approx(1.5));
    }

    SECTION("mistakes are reported with the line they are on")
    {
        auto message = [](const std::string &text) {
            try {
                skeleton_from_csv(text);
            } catch (const InvalidArgument &e) {
                return std::string(e.what());
            }
            return std::string();
        };
        const std::string head = "branch,parent,depth,point,x_mm,y_mm,z_mm,radius_mm\n0,-1,0,0,0,0,0,2\n0,-1,0,1,0,0,10,2\n";
        REQUIRE(message(head + "1,0,1,0,0,0,10\n").find("Line 4") != std::string::npos);
        REQUIRE(message(head + "1,0,1,0,0,0,ten,1\n").find("Line 4") != std::string::npos);
        REQUIRE(message(head + "2,0,1,0,0,0,10,1\n").find("Line 4") != std::string::npos);
        REQUIRE(message(head + "1,0,1,0,0,0,10,-1\n").find("Line 4") != std::string::npos);
        // A child that starts 1 mm away from the end of its parent.
        REQUIRE(message(head + "1,0,1,0,1,0,10,1\n1,0,1,1,5,0,15,1\n").find("away from the end") != std::string::npos);
        // A single point is no vessel.
        REQUIRE(!message(head + "1,0,1,0,0,0,10,1\n").empty());
        REQUIRE(!message("").empty());
    }
}

TEST_CASE("Skeletons that cannot be meshed are refused with a reason", "[Vessel][VesselCentreline]")
{
    std::vector<BranchPath> paths = curved_tree_paths();

    // Two children that leave the junction in the same direction.
    std::vector<BranchPath> same = paths;
    same[2].points = {same[1].points[0], {Vec3d(24, 2, 38), 1.}};
    REQUIRE_THROWS_AS(make_skeleton(same), InvalidArgument);

    // A child that runs straight back into its parent.
    std::vector<BranchPath> back = paths;
    back[1].points = {{Vec3d(8, 2, 22), 1.}, {Vec3d(6, 1, 15), 1.}};
    REQUIRE_THROWS_AS(make_skeleton(back), InvalidArgument);

    // A parent that comes after its child.
    std::vector<BranchPath> order = paths;
    order[1].parent = 2;
    REQUIRE_THROWS_AS(make_skeleton(order), InvalidArgument);

    // A curve tighter than the vessel is wide cannot be swept.
    std::vector<BranchPath> tight(1);
    tight[0].points = {{Vec3d(0, 0, 0), 2.}, {Vec3d(3, 0, 0), 2.}, {Vec3d(3, 3, 0), 2.}, {Vec3d(0, 3, 0), 2.}};
    const Skeleton sk = make_skeleton(tight);
    VesselMeshParams mp;
    REQUIRE(!validate_mesh_params(sk, mp).empty());
    REQUIRE_THROWS_AS(make_tree_mesh(sk, mp), InvalidArgument);
}

TEST_CASE("Imported centrelines are saved with the vessel parameters", "[Vessel][VesselCentreline]")
{
    VesselSpec in;
    in.centreline          = make_skeleton(curved_tree_paths());
    in.mesh.wall_thickness = 0.3;
    in.mesh.spline         = false;
    VesselSpec out;
    REQUIRE(spec_from_text(spec_to_text(in), out));
    REQUIRE(skeleton_to_csv(out.centreline) == skeleton_to_csv(in.centreline));
    REQUIRE_FALSE(out.mesh.spline);
    REQUIRE(out.mesh.wall_thickness == Approx(0.3));

    // Grown trees keep no table.
    VesselSpec grown;
    REQUIRE(spec_from_text(spec_to_text(grown), out));
    REQUIRE(out.centreline.branches.empty());
}
