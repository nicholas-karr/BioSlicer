#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_approx.hpp>

#include <cmath>
#include <random>
#include <sstream>

#include <libslic3r/AABBTreeIndirect.hpp>
#include <libslic3r/BoundingBox.hpp>
#include <libslic3r/Geometry.hpp>
#include <libslic3r/MeshBoolean.hpp>
#include <libslic3r/TriangleMesh.hpp>
#include <libslic3r/Vessel/VesselTree.hpp>
#include <libslic3r/Vessel/VesselTube.hpp>

using namespace Slic3r;
using namespace Slic3r::vessel;
using Catch::Approx;

namespace {

// Area of a regular n-gon inscribed in a circle divided by the area of the circle.
double polygon_area_factor(int n) { return n * std::sin(2. * PI / n) / (2. * PI); }

// Distances along a ray to every surface crossing, nearest first, duplicates at shared edges removed.
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

std::pair<Vec3d, Vec3d> mesh_bounds(const indexed_triangle_set &its)
{
    Vec3d lo = Vec3d::Constant(1e30), hi = Vec3d::Constant(-1e30);
    for (const Vec3f &v : its.vertices) {
        lo = lo.cwiseMin(v.cast<double>());
        hi = hi.cwiseMax(v.cast<double>());
    }
    return {lo, hi};
}

TubeParams straight_params(int sides, CapStyle caps)
{
    TubeParams p;
    p.spline    = false;
    p.sides     = sides;
    p.cap_start = p.cap_end = caps;
    return p;
}

std::vector<PathPoint> straight_path(double radius, double length)
{
    return {{Vec3d(0, 0, 0), radius}, {Vec3d(0, 0, length), radius}};
}

} // namespace

TEST_CASE("Centreline sampling keeps control points and spacing", "[Vessel]")
{
    const std::vector<PathPoint> control = {
        {Vec3d(0, 0, 0), 1.}, {Vec3d(10, 0, 0), 2.}, {Vec3d(10, 10, 0), 3.}, {Vec3d(0, 10, 5), 1.}};
    const double spacing = 0.5;

    for (bool spline : {false, true}) {
        const auto pts = sample_centreline(control, spline, spacing);
        REQUIRE(pts.size() > control.size());
        REQUIRE((pts.front().pos - control.front().pos).norm() == Approx(0.).margin(1e-9));
        REQUIRE((pts.back().pos - control.back().pos).norm() == Approx(0.).margin(1e-9));

        // Every control point is in the output with its own radius.
        for (const PathPoint &c : control) {
            bool found = false;
            for (const PathPoint &p : pts)
                if ((p.pos - c.pos).norm() < 1e-9) {
                    found = true;
                    REQUIRE(p.radius == Approx(c.radius));
                }
            REQUIRE(found);
        }
        // Chord length between neighbours never exceeds the requested spacing (arc length can only be longer by the sagitta).
        for (size_t i = 1; i < pts.size(); ++i)
            REQUIRE((pts[i].pos - pts[i - 1].pos).norm() <= spacing * 1.001);
    }

    // A polyline sampled at 0.5 mm over a 10 mm segment gives exactly 20 pieces.
    const auto line = sample_centreline({{Vec3d(0, 0, 0), 1.}, {Vec3d(10, 0, 0), 1.}}, false, 0.5);
    REQUIRE(line.size() == 21);
    REQUIRE(polyline_length(line) == Approx(10.).margin(1e-9));
}

TEST_CASE("Straight solid tube volume and bounding box", "[Vessel]")
{
    const double radius = 2., length = 30.;

    SECTION("flat ends, volume is pi r^2 L within 0.5 percent")
    {
        const int sides = 64;
        const auto its  = make_tube(straight_path(radius, length), straight_params(sides, CapStyle::Flat));
        const auto chk  = check_mesh(its);
        REQUIRE(chk.watertight());
        REQUIRE(chk.degenerate_faces == 0);
        REQUIRE_FALSE(MeshBoolean::cgal::does_self_intersect(TriangleMesh(its)));
        const double expected = PI * radius * radius * length;
        REQUIRE(chk.volume == Approx(expected).epsilon(0.005));
        // The inscribed polygon accounts for the whole deviation.
        REQUIRE(chk.volume == Approx(expected * polygon_area_factor(sides)).epsilon(1e-5));

        const auto [lo, hi] = mesh_bounds(its);
        REQUIRE(lo.z() == Approx(0.).margin(1e-5));
        REQUIRE(hi.z() == Approx(length).margin(1e-5));
        REQUIRE(hi.x() - lo.x() == Approx(2. * radius).margin(1e-4));
        REQUIRE(hi.y() - lo.y() == Approx(2. * radius).margin(1e-4));
    }

    SECTION("round ends add a half ellipsoid on each side and reach one radius further")
    {
        TubeParams p = straight_params(64, CapStyle::Round);
        p.cap_rings  = 16;
        const auto its = make_tube(straight_path(radius, length), p);
        const auto chk = check_mesh(its);
        REQUIRE(chk.watertight());
        REQUIRE(chk.degenerate_faces == 0);
        const double expected = PI * radius * radius * length + 4. / 3. * PI * radius * radius * radius;
        REQUIRE(chk.volume == Approx(expected).epsilon(0.005));

        const auto [lo, hi] = mesh_bounds(its);
        REQUIRE(lo.z() == Approx(-radius).margin(1e-5));
        REQUIRE(hi.z() == Approx(length + radius).margin(1e-5));
    }

    SECTION("elliptical section")
    {
        TubeParams p = straight_params(64, CapStyle::Flat);
        p.aspect     = 0.5;
        const auto its = make_tube(straight_path(radius, length), p);
        const auto chk = check_mesh(its);
        REQUIRE(chk.watertight());
        REQUIRE(chk.volume == Approx(PI * radius * radius * p.aspect * length).epsilon(0.005));
        const auto [lo, hi] = mesh_bounds(its);
        const double width_x = hi.x() - lo.x(), width_y = hi.y() - lo.y();
        REQUIRE(std::max(width_x, width_y) == Approx(2. * radius).margin(1e-4));
        REQUIRE(std::min(width_x, width_y) == Approx(2. * radius * p.aspect).margin(1e-4));
    }

    SECTION("tapered tube is a truncated cone")
    {
        const std::vector<PathPoint> path = {{Vec3d(0, 0, 0), 3.}, {Vec3d(0, 0, 20), 1.}};
        const auto its = make_tube(path, straight_params(96, CapStyle::Flat));
        const auto chk = check_mesh(its);
        REQUIRE(chk.watertight());
        const double frustum = PI * 20. / 3. * (3. * 3. + 3. * 1. + 1. * 1.);
        REQUIRE(chk.volume == Approx(frustum).epsilon(0.005));
    }
}

TEST_CASE("Curved tubes are watertight and keep their volume", "[Vessel]")
{
    // A quarter circle of radius 15 mm in the XY plane.
    std::vector<PathPoint> arc;
    for (int i = 0; i <= 4; ++i) {
        const double a = 0.5 * PI * i / 4.;
        arc.push_back({Vec3d(15. * std::cos(a), 15. * std::sin(a), 0.), 2.});
    }
    TubeParams p;
    p.sides = 48;
    p.cap_start = p.cap_end = CapStyle::Flat;
    p.sample_spacing = 0.5;
    REQUIRE(validate_tube(arc, p).empty());

    const auto its = make_tube(arc, p);
    const auto chk = check_mesh(its);
    REQUIRE(chk.watertight());
    REQUIRE(chk.degenerate_faces == 0);
    // Pappus: a tube of radius r around an arc of length s has volume pi r^2 s.
    const double arc_length = 0.5 * PI * 15.;
    REQUIRE(chk.volume == Approx(PI * 4. * arc_length * polygon_area_factor(48)).epsilon(0.01));
}

TEST_CASE("Hollow tube wall thickness measured by ray casting", "[Vessel]")
{
    const double radius = 2., wall = 0.5, length = 30., layer_height = 0.2;
    TubeParams   p = straight_params(64, CapStyle::Flat);
    p.wall_thickness = wall;

    const auto its = make_tube(straight_path(radius, length), p);
    const auto chk = check_mesh(its);
    REQUIRE(chk.watertight());
    REQUIRE(chk.degenerate_faces == 0);
    const double inner = radius - wall;
    REQUIRE(chk.volume == Approx(PI * (radius * radius - inner * inner) * length * polygon_area_factor(64)).epsilon(1e-5));

    // Shoot rays from the axis outwards at several heights and angles, avoiding vertices and the diagonals of the quads.
    for (double z : {0.31 * length, 0.62 * length})
        for (int k = 0; k < 8; ++k) {
            const double a = 2. * PI * (k + 0.3) / 8.;
            const auto   t = ray_distances(its, Vec3d(0, 0, z), Vec3d(std::cos(a), std::sin(a), 0));
            REQUIRE(t.size() == 2);
            const double measured = t[1] - t[0];
            REQUIRE(std::abs(measured - wall) <= layer_height);
            // The mesh is much closer than a layer: the chord error of 64 sides is a few microns.
            REQUIRE(measured == Approx(wall).margin(0.01));
        }

    // The lumen is open at both ends: a ray along the axis leaves the mesh without a hit.
    REQUIRE(ray_distances(its, Vec3d(0, 0, 1), Vec3d(0, 0, 1)).empty());

    SECTION("wall thickness holds along a bend")
    {
        std::vector<PathPoint> arc;
        for (int i = 0; i <= 4; ++i) {
            const double a = 0.5 * PI * i / 4.;
            arc.push_back({Vec3d(15. * std::cos(a), 15. * std::sin(a), 0.), radius});
        }
        TubeParams bend;
        bend.sides = 64;
        bend.wall_thickness = wall;
        const auto curved = make_tube(arc, bend);
        REQUIRE(check_mesh(curved).watertight());
        // Radial ray from the centreline point at 45 degrees (a control point of the path).
        const double a = 0.25 * PI;
        const Vec3d  origin(15. * std::cos(a), 15. * std::sin(a), 0.);
        const Vec3d  dir(std::cos(a), std::sin(a), 0.);
        // Tilt the ray inside the cross section so that it avoids mesh edges.
        const Vec3d tilted = (dir + Vec3d(0, 0, 0.1)).normalized();
        const auto  t = ray_distances(curved, origin, tilted);
        REQUIRE(t.size() == 2);
        REQUIRE(std::abs((t[1] - t[0]) * tilted.head<2>().norm() - wall) <= layer_height);
    }
}

TEST_CASE("Tube input is validated with plain messages", "[Vessel]")
{
    TubeParams p = straight_params(32, CapStyle::Flat);

    REQUIRE(validate_tube(straight_path(2., 10.), p).empty());
    REQUIRE(!validate_tube({}, p).empty());
    REQUIRE(!validate_tube(straight_path(-1., 10.), p).empty());

    p.wall_thickness = 2.;
    REQUIRE(!validate_tube(straight_path(2., 10.), p).empty());
    REQUIRE_THROWS_AS(make_tube(straight_path(2., 10.), p), InvalidArgument);

    p = straight_params(32, CapStyle::Flat);
    p.aspect = 1.5;
    REQUIRE(!validate_tube(straight_path(2., 10.), p).empty());

    // A spline through a right angle corner bends more sharply than a 2 mm radius allows.
    p = straight_params(32, CapStyle::Flat);
    const std::vector<PathPoint> corner = {{Vec3d(0, 0, 0), 2.}, {Vec3d(10, 0, 0), 2.}, {Vec3d(10, 10, 0), 2.}};
    p.spline = true;
    REQUIRE(!validate_tube(corner, p).empty());

    // Straight pieces meet in a mitre joint, which reaches 2 mm * tan(45 degrees) = 2 mm along each piece. Two right
    // angle turns 3.9 mm apart would cut into each other, 4.1 mm apart they fit.
    p.spline = false;
    REQUIRE(validate_tube(corner, p).empty());
    const std::vector<PathPoint> tight = {{Vec3d(0, 0, 0), 2.}, {Vec3d(10, 0, 0), 2.}, {Vec3d(10, 3.9, 0), 2.}, {Vec3d(20, 3.9, 0), 2.}};
    REQUIRE(!validate_tube(tight, p).empty());
    const std::vector<PathPoint> fits = {{Vec3d(0, 0, 0), 2.}, {Vec3d(10, 0, 0), 2.}, {Vec3d(10, 4.1, 0), 2.}, {Vec3d(20, 4.1, 0), 2.}};
    REQUIRE(validate_tube(fits, p).empty());
}

TEST_CASE("Mitre joints keep the volume of the centreline", "[Vessel]")
{
    // A mitre joint adds as much volume outside the corner as it removes inside it: the volume is pi r^2 times the
    // length of the centreline, whatever the turn angle.
    const double radius = 2.;
    const int    sides  = 64;
    for (double turn_deg : {10., 45., 90., 135.}) {
        const double turn = turn_deg * PI / 180.;
        const std::vector<PathPoint> path = {{Vec3d(0, 0, 0), radius}, {Vec3d(15, 0, 0), radius},
                                             {Vec3d(15 + 15 * std::cos(turn), 15 * std::sin(turn), 0), radius}};
        TubeParams p = straight_params(sides, CapStyle::Flat);
        REQUIRE(validate_tube(path, p).empty());
        const auto its = make_tube(path, p);
        const auto chk = check_mesh(its);
        INFO("turn " << turn_deg << " degrees");
        REQUIRE(chk.watertight());
        REQUIRE(chk.degenerate_faces == 0);
        REQUIRE_FALSE(MeshBoolean::cgal::does_self_intersect(TriangleMesh(its)));
        REQUIRE(chk.volume == Approx(PI * radius * radius * 30. * polygon_area_factor(sides)).epsilon(1e-4));
    }
}

TEST_CASE("Murray's law for child radii and angles", "[Vessel]")
{
    BifurcationRule rule; // Murray, exponent 3, equal flow split

    for (double rp : {0.5, 1., 2., 3.7})
        for (double split : {0.5, 0.3, 0.1, 0.9}) {
            rule.flow_split = split;
            const auto g = compute_bifurcation(rp, rule);
            // r_parent^3 = r_a^3 + r_b^3
            REQUIRE(std::pow(g.radius_a, 3) + std::pow(g.radius_b, 3) == Approx(std::pow(rp, 3)).epsilon(1e-12));
            // The flow through each child is proportional to r^3.
            REQUIRE(std::pow(g.radius_a / rp, 3) == Approx(split).epsilon(1e-12));
        }

    // Symmetric case: each child is 2^(-1/3) of the parent and leaves at acos(2^(-1/3)) = 37.467 degrees.
    rule.flow_split = 0.5;
    const auto g = compute_bifurcation(2., rule);
    REQUIRE(g.radius_a == Approx(2. * std::pow(2., -1. / 3.)).epsilon(1e-12));
    REQUIRE(g.radius_a == Approx(1.5874).epsilon(1e-4));
    REQUIRE(Geometry::rad2deg(g.angle_a) == Approx(37.467).margin(0.001));
    REQUIRE(g.angle_a == Approx(g.angle_b).epsilon(1e-12));

    // The optimal angles satisfy the force balance transverse to the parent axis: ra^2 sin(a) = rb^2 sin(b)
    // and along it: rp^2 = ra^2 cos(a) + rb^2 cos(b).
    rule.flow_split = 0.2;
    const auto h = compute_bifurcation(1.5, rule);
    REQUIRE(h.radius_a * h.radius_a * std::sin(h.angle_a) == Approx(h.radius_b * h.radius_b * std::sin(h.angle_b)).epsilon(1e-9));
    REQUIRE(1.5 * 1.5 == Approx(h.radius_a * h.radius_a * std::cos(h.angle_a) + h.radius_b * h.radius_b * std::cos(h.angle_b)).epsilon(1e-9));
    REQUIRE(h.radius_a < h.radius_b);
    REQUIRE(h.angle_a > h.angle_b); // the thinner child turns further

    SECTION("trifurcations")
    {
        // Three equal children: each has 3^(-1/3) of the parent radius and leaves at acos(3^(-1/3)) = 46.10 degrees.
        rule.flow_split = 1. / 3.;
        const JunctionGeometry t = compute_junction(2., 3, rule);
        REQUIRE(t.radii.size() == 3);
        for (int i = 0; i < 3; ++i) {
            REQUIRE(t.radii[i] == Approx(2. * std::pow(3., -1. / 3.)).epsilon(1e-12));
            REQUIRE(Geometry::rad2deg(t.angles[i]) == Approx(Geometry::rad2deg(std::acos(std::pow(3., -1. / 3.)))).epsilon(1e-9));
            REQUIRE(Geometry::rad2deg(t.azimuths[i]) == Approx(120. * i).margin(1e-9));
        }
        REQUIRE(Geometry::rad2deg(t.angles[0]) == Approx(46.10).margin(0.005));

        // Uneven: the sideways pulls r^2 sin(a) are equal (they cancel at 120 degrees apart), and the pulls along the
        // axis add up to the parent's: sum of r^2 cos(a) = rp^2. Murray's law holds for the radii.
        rule.flow_split = 0.6;
        const JunctionGeometry u = compute_junction(1.5, 3, rule);
        double axial = 0., cubes = 0.;
        for (int i = 0; i < 3; ++i) {
            axial += u.radii[i] * u.radii[i] * std::cos(u.angles[i]);
            cubes += std::pow(u.radii[i], 3);
            REQUIRE(u.radii[i] * u.radii[i] * std::sin(u.angles[i]) == Approx(u.radii[0] * u.radii[0] * std::sin(u.angles[0])).epsilon(1e-9));
        }
        REQUIRE(axial == Approx(1.5 * 1.5).epsilon(1e-9));
        REQUIRE(cubes == Approx(std::pow(1.5, 3)).epsilon(1e-12));
        REQUIRE(u.angles[0] < u.angles[1]); // the wide child turns less
    }

    SECTION("other exponents")
    {
        rule.flow_split      = 0.5;
        rule.murray_exponent = 2.;
        const auto e2 = compute_bifurcation(1., rule);
        REQUIRE(std::pow(e2.radius_a, 2) + std::pow(e2.radius_b, 2) == Approx(1.).epsilon(1e-12));
    }

    SECTION("fixed ratio and fixed angle")
    {
        rule.radius_rule = RadiusRule::FixedRatio;
        rule.radius_ratio = 0.75;
        rule.angle_rule = AngleRule::Fixed;
        rule.total_angle = 90.;
        const auto f = compute_bifurcation(4., rule);
        REQUIRE(f.radius_a == Approx(3.));
        REQUIRE(f.radius_b == Approx(3.));
        REQUIRE(Geometry::rad2deg(f.angle_a + f.angle_b) == Approx(90.).epsilon(1e-12));
        REQUIRE(Geometry::rad2deg(f.angle_a) == Approx(45.).epsilon(1e-12));
    }
}

namespace {

struct BifurcationCase
{
    const char     *name;
    double          parent_radius;
    BifurcationRule rule;
    double          wall;
};

std::vector<BifurcationCase> bifurcation_cases()
{
    std::vector<BifurcationCase> cases;
    BifurcationRule murray;
    cases.push_back({"Murray symmetric", 2., murray, 0.});

    BifurcationRule lopsided;
    lopsided.flow_split = 0.15;
    cases.push_back({"Murray lopsided", 2., lopsided, 0.});

    BifurcationRule narrow;
    narrow.radius_rule  = RadiusRule::FixedRatio;
    narrow.radius_ratio = 0.7;
    narrow.angle_rule   = AngleRule::Fixed;
    narrow.total_angle  = 30.;
    cases.push_back({"Fixed narrow 30 degrees", 1.5, narrow, 0.});

    BifurcationRule wide = narrow;
    wide.total_angle     = 140.;
    wide.radius_ratio    = 0.9;
    cases.push_back({"Fixed wide 140 degrees", 1.5, wide, 0.});

    cases.push_back({"Murray symmetric hollow", 2., murray, 0.4});
    cases.push_back({"Murray lopsided hollow", 2., lopsided, 0.3});
    cases.push_back({"Fixed wide hollow", 1.5, wide, 0.3});
    return cases;
}

} // namespace

TEST_CASE("Bifurcations are watertight, manifold and correctly oriented", "[Vessel]")
{
    for (const BifurcationCase &c : bifurcation_cases()) {
        DYNAMIC_SECTION(c.name)
        {
            BifurcationParams bp;
            bp.parent_radius  = c.parent_radius;
            bp.rule           = c.rule;
            bp.parent_length  = 12.;
            bp.child_lengths  = {14., 10.};
            bp.junction       = Vec3d(3., -2., 5.);
            bp.parent_direction = Vec3d(0.3, 0.2, 1.).normalized();
            bp.plane_normal   = Vec3d(0, 1, 0);

            VesselMeshParams mp;
            mp.sides          = 32;
            mp.wall_thickness = c.wall;

            const indexed_triangle_set its = make_bifurcation(bp, mp);
            const MeshCheck            chk = check_mesh(its);
            INFO("open edges " << chk.open_edges << " non-manifold " << chk.nonmanifold_edges);
            REQUIRE(chk.watertight());
            REQUIRE(chk.degenerate_faces == 0);
            REQUIRE(chk.volume > 0.); // outward facing
            REQUIRE(its_number_of_patches(its) == 1);

            // The volume lies between the largest single tube and the sum of all three tubes (plus the junction ball).
            const Skeleton sk = make_bifurcation_skeleton(bp);
            double sum = 0., largest = 0.;
            for (const Branch &b : sk.branches) {
                const double v = PI * b.start_radius() * b.start_radius() * b.length();
                sum += v;
                largest = std::max(largest, v);
            }
            const double ball = 4. / 3. * PI * std::pow((1. + mp.junction_margin) * c.parent_radius, 3);
            if (c.wall == 0.) {
                REQUIRE(chk.volume > largest);
                REQUIRE(chk.volume < sum + ball);
            } else {
                REQUIRE(chk.volume > 0.);
                REQUIRE(chk.volume < sum + ball);
                // Shell of thickness w around a lumen: volume of the walls only.
                REQUIRE(chk.volume < sum * (1. - std::pow(1. - c.wall / sk.branches[2].start_radius(), 2)) + ball);
            }
        }
    }
}

TEST_CASE("Bifurcation geometry matches the radii and angles it was built from", "[Vessel]")
{
    BifurcationParams bp;
    bp.parent_radius    = 2.;
    bp.parent_length    = 15.;
    bp.child_lengths    = {15., 15.};
    bp.parent_direction = Vec3d::UnitZ();
    bp.plane_normal     = Vec3d::UnitY();

    const Skeleton sk = make_bifurcation_skeleton(bp);
    REQUIRE(sk.branches.size() == 3);
    const Branch &parent = sk.branches[0], &a = sk.branches[1], &b = sk.branches[2];

    REQUIRE(parent.start_radius() == Approx(2.));
    REQUIRE(std::pow(a.start_radius(), 3) + std::pow(b.start_radius(), 3) == Approx(8.).epsilon(1e-12));
    REQUIRE(parent.length() == Approx(15.));
    REQUIRE(a.length() == Approx(15.));

    const Vec3d da = (a.end() - a.start()).normalized(), db = (b.end() - b.start()).normalized();
    const double angle = std::acos(da.dot(db));
    REQUIRE(Geometry::rad2deg(angle) == Approx(74.935).margin(0.01)); // 2 acos(2^(-1/3))
    // The children lie in the XZ plane, on opposite sides of the parent axis.
    REQUIRE(da.y() == Approx(0.).margin(1e-12));
    REQUIRE(da.x() * db.x() < 0.);

    // Radius of each child measured on the solid mesh, half way along the child, perpendicular to its axis.
    VesselMeshParams mp;
    mp.sides = 64;
    const auto its = make_bifurcation(bp, mp);
    for (const Branch *c : {&a, &b}) {
        const Vec3d axis = (c->end() - c->start()).normalized();
        const Vec3d mid  = 0.5 * (c->start() + c->end());
        const Vec3d out  = axis.cross(Vec3d::UnitY()).normalized(); // perpendicular to the axis, in the branching plane
        // Rotate by 0.3 of a side step to stay clear of mesh edges.
        const Vec3d dir = (out * std::cos(0.3 * 2 * PI / 64) + Vec3d::UnitY() * std::sin(0.3 * 2 * PI / 64)).normalized();
        const auto  t = ray_distances(its, mid, dir);
        REQUIRE(!t.empty());
        REQUIRE(t[0] == Approx(c->start_radius()).epsilon(0.003));
    }
}

TEST_CASE("Vessel tree structure follows the parameters", "[Vessel]")
{
    TreeParams tp;
    tp.root_radius = 3.;
    tp.root_length = 20.;
    tp.length_ratio = 0.75;
    tp.depth = 4;

    const Skeleton sk = generate_tree(tp);
    REQUIRE(sk.branches.size() == 31); // 2^(4+1) - 1
    REQUIRE(sk.leaf_count() == 16);
    REQUIRE(sk.clipped_by_bounds == 0);

    for (const Branch &b : sk.branches) {
        REQUIRE(b.length() == Approx(20. * std::pow(0.75, b.depth)).epsilon(1e-9));
        if (!b.is_leaf()) {
            const Branch &ca = sk.branches[b.children[0]], &cb = sk.branches[b.children[1]];
            // Murray's law at every junction.
            REQUIRE(std::pow(ca.start_radius(), 3) + std::pow(cb.start_radius(), 3) == Approx(std::pow(b.start_radius(), 3)).epsilon(1e-12));
            REQUIRE((ca.start() - b.end()).norm() < 1e-12);
            REQUIRE((cb.start() - b.end()).norm() < 1e-12);
            REQUIRE(ca.depth == b.depth + 1);
        }
    }
    // The leaves have radius 3 * 2^(-4/3).
    for (const Branch &b : sk.branches)
        if (b.is_leaf())
            REQUIRE(b.start_radius() == Approx(3. * std::pow(2., -4. / 3.)).epsilon(1e-12));

    SECTION("minimum radius stops the growth")
    {
        tp.min_radius = 1.2;
        const Skeleton thin = generate_tree(tp);
        for (const Branch &b : thin.branches)
            REQUIRE(b.start_radius() >= 1.2);
        REQUIRE(thin.branches.size() < sk.branches.size());
    }

    SECTION("invalid parameters are rejected")
    {
        TreeParams bad = tp;
        bad.root_radius = 0.;
        REQUIRE_THROWS_AS(generate_tree(bad), InvalidArgument);
        bad = tp;
        bad.depth = -1;
        REQUIRE_THROWS_AS(generate_tree(bad), InvalidArgument);
    }
}

TEST_CASE("Vessel tree generation is deterministic by seed", "[Vessel]")
{
    TreeParams tp;
    tp.depth         = 4;
    tp.angle_jitter  = 10.;
    tp.length_jitter = 0.2;
    tp.seed          = 12345;

    auto same = [](const Skeleton &x, const Skeleton &y) {
        if (x.branches.size() != y.branches.size())
            return false;
        for (size_t i = 0; i < x.branches.size(); ++i)
            if (x.branches[i].start() != y.branches[i].start() || x.branches[i].end() != y.branches[i].end() ||
                x.branches[i].start_radius() != y.branches[i].start_radius() || x.branches[i].parent != y.branches[i].parent)
                return false;
        return true;
    };

    const Skeleton a = generate_tree(tp);
    const Skeleton b = generate_tree(tp);
    REQUIRE(same(a, b));

    TreeParams other = tp;
    other.seed       = 12346;
    REQUIRE_FALSE(same(a, generate_tree(other)));

    // The seed also picks the branching planes when there is no jitter.
    TreeParams calm = tp;
    calm.angle_jitter = calm.length_jitter = 0.;
    TreeParams calm2 = calm;
    calm2.seed       = 7;
    REQUIRE(same(generate_tree(calm), generate_tree(calm)));
    REQUIRE_FALSE(same(generate_tree(calm), generate_tree(calm2)));

    // Same seed, same mesh.
    TreeParams small = tp;
    small.depth      = 2;
    VesselMeshParams mp;
    mp.sides = 16;
    const auto m1 = make_tree_mesh(generate_tree(small), mp);
    const auto m2 = make_tree_mesh(generate_tree(small), mp);
    REQUIRE(m1.vertices == m2.vertices);
    REQUIRE(m1.indices == m2.indices);
}

TEST_CASE("Trees saved by the previous version grow the same", "[Vessel]")
{
    // The last rows of the table that the generator before trifurcations and tapering made for these settings.
    TreeParams tp;
    tp.seed            = 7;
    tp.depth           = 3;
    tp.angle_jitter    = 14.;
    tp.length_jitter   = 0.2;
    tp.rule.flow_split = 0.4;
    const std::string csv = skeleton_to_csv(generate_tree(tp));
    REQUIRE(csv.find("13,12,3,1,20.22903497776275,2.7788682285259076,51.92629617806146,1.0482965576835586\n"
                     "14,12,3,0,11.148485718739543,1.5388285622817741,46.50462929948938,1.1999999999999997\n"
                     "14,12,3,1,9.89510994723722,7.302951634829,56.87510191246608,1.1999999999999997\n") != std::string::npos);
}

TEST_CASE("Vessel tree meshes are watertight", "[Vessel]")
{
    TreeParams tp;
    tp.depth         = 3;
    tp.root_radius   = 2.5;
    tp.root_length   = 20.;
    tp.angle_jitter  = 8.;
    tp.length_jitter = 0.15;
    tp.seed          = 99;
    const Skeleton sk = generate_tree(tp);

    VesselMeshParams mp;
    mp.sides = 24;

    SECTION("solid with round ends")
    {
        const auto its = make_tree_mesh(sk, mp);
        const auto chk = check_mesh(its);
        REQUIRE(chk.watertight());
        REQUIRE(chk.degenerate_faces == 0);
        REQUIRE(chk.volume > 0.);
        REQUIRE(its_number_of_patches(its) == 1);
        double centreline = 0.;
        for (const Branch &b : sk.branches)
            centreline += PI * b.start_radius() * b.start_radius() * b.length();
        // Overlap at the junctions is small compared to the volume of the branches.
        REQUIRE(chk.volume == Approx(centreline).epsilon(0.15));
    }

    SECTION("hollow with open ends")
    {
        mp.wall_thickness = 0.4;
        const auto its = make_tree_mesh(sk, mp);
        const auto chk = check_mesh(its);
        REQUIRE(chk.watertight());
        REQUIRE(chk.degenerate_faces == 0);
        REQUIRE(chk.volume > 0.);
        REQUIRE(its_number_of_patches(its) == 1);

        // Wall thickness on the root, measured by a ray from the axis.
        const Branch &root = sk.branches[0];
        const Vec3d   axis = (root.end() - root.start()).normalized();
        const Vec3d   perp = axis.cross(Vec3d::UnitX()).normalized();
        const Vec3d   dir  = (perp * std::cos(0.3) + axis.cross(perp) * std::sin(0.3)).normalized();
        const auto    t = ray_distances(its, root.start() + 0.4 * (root.end() - root.start()), dir);
        REQUIRE(t.size() >= 2);
        REQUIRE(std::abs((t[1] - t[0]) - 0.4) <= 0.2);

        // The lumen runs through the open root: a ray down the axis from just outside the root start meets no wall
        // before the junction, so the root end is open and the lumen is not capped.
        const auto along = ray_distances(its, root.start() - axis * 0.5, axis);
        REQUIRE(!along.empty());
        REQUIRE(along[0] > 0.5 + 0.9 * root.length());
    }

    SECTION("vertex and triangle counts are kept moderate by the sides setting")
    {
        VesselMeshParams coarse = mp;
        coarse.sides            = 12;
        REQUIRE(make_tree_mesh(sk, coarse).indices.size() < make_tree_mesh(sk, mp).indices.size());
    }
}

TEST_CASE("Vessel tree stops inside a bounding mesh", "[Vessel]")
{
    // Sphere of radius 25 mm centred at (0, 0, 20).
    const double R = 25.;
    const Vec3d  centre(0, 0, 20);
    indexed_triangle_set bounds = its_make_sphere(R, PI / 24.);
    for (Vec3f &v : bounds.vertices)
        v += centre.cast<float>();

    TreeParams tp;
    tp.root_position = Vec3d::Zero();
    tp.root_radius   = 1.5;
    tp.root_length   = 12.;
    tp.length_ratio  = 1.0;
    tp.depth         = 5;
    tp.angle_jitter  = 5.;
    tp.seed          = 3;

    const Skeleton free_tree = generate_tree(tp);
    const Skeleton bounded   = generate_tree(tp, &bounds);

    REQUIRE(bounded.clipped_by_bounds > 0);
    REQUIRE(bounded.branches.size() < free_tree.branches.size());
    for (const Branch &b : bounded.branches) {
        REQUIRE((b.start() - centre).norm() < R);
        REQUIRE((b.end() - centre).norm() < R);
        // The round end of the branch reaches the wall at most.
        const Vec3d tip = b.end() + (b.end() - b.start()).normalized() * b.start_radius();
        REQUIRE((tip - centre).norm() <= R + 1e-3);
    }

    // The mesh of the bounded tree is still a valid solid.
    VesselMeshParams mp;
    mp.sides = 12;
    TreeParams small = tp;
    small.depth      = 3;
    const auto its   = make_tree_mesh(generate_tree(small, &bounds), mp);
    REQUIRE(check_mesh(its).watertight());

    SECTION("a root outside the volume is refused")
    {
        TreeParams outside = tp;
        outside.root_position = Vec3d(100, 0, 0);
        REQUIRE_THROWS_AS(generate_tree(outside, &bounds), InvalidArgument);
    }
}

TEST_CASE("Vessel parameters survive a text round trip", "[Vessel]")
{
    VesselSpec in;
    in.tree.root_position  = Vec3d(1.5, -2.25, 3.125);
    in.tree.root_direction = Vec3d(0, 0.6, 0.8);
    in.tree.root_radius    = 2.75;
    in.tree.root_length    = 18.5;
    in.tree.depth          = 5;
    in.tree.length_ratio   = 0.7;
    in.tree.rule.radius_rule = RadiusRule::FixedRatio;
    in.tree.rule.radius_ratio = 0.81;
    in.tree.rule.angle_rule = AngleRule::Fixed;
    in.tree.rule.total_angle = 66.5;
    in.tree.rule.flow_split = 0.4;
    in.tree.angle_jitter   = 4.;
    in.tree.length_jitter  = 0.1;
    in.tree.min_radius     = 0.3;
    in.tree.seed           = 4000000000u;
    in.mesh.sides          = 40;
    in.mesh.wall_thickness = 0.35;
    in.mesh.caps           = CapStyle::Flat;
    in.mesh.junction_margin = 0.05;
    in.bounds_object       = "Heart wall";

    VesselSpec out;
    REQUIRE(spec_from_text(spec_to_text(in), out));
    REQUIRE(out.tree.root_position.isApprox(in.tree.root_position, 1e-9));
    REQUIRE(out.tree.root_direction.isApprox(in.tree.root_direction, 1e-9));
    REQUIRE(out.tree.root_radius == Approx(2.75));
    REQUIRE(out.tree.root_length == Approx(18.5));
    REQUIRE(out.tree.depth == 5);
    REQUIRE(out.tree.length_ratio == Approx(0.7));
    REQUIRE(out.tree.rule.radius_rule == RadiusRule::FixedRatio);
    REQUIRE(out.tree.rule.radius_ratio == Approx(0.81));
    REQUIRE(out.tree.rule.angle_rule == AngleRule::Fixed);
    REQUIRE(out.tree.rule.total_angle == Approx(66.5));
    REQUIRE(out.tree.rule.flow_split == Approx(0.4));
    REQUIRE(out.tree.seed == 4000000000u);
    REQUIRE(out.mesh.sides == 40);
    REQUIRE(out.mesh.wall_thickness == Approx(0.35));
    REQUIRE(out.mesh.caps == CapStyle::Flat);
    REQUIRE(out.mesh.junction_margin == Approx(0.05));
    REQUIRE(out.bounds_object == "Heart wall");

    // Regenerating from the saved text gives the same tree.
    const Skeleton a = generate_tree(in.tree), b = generate_tree(out.tree);
    REQUIRE(a.branches.size() == b.branches.size());
    for (size_t i = 0; i < a.branches.size(); ++i)
        REQUIRE((a.branches[i].end() - b.branches[i].end()).norm() < 1e-6);

    VesselSpec junk;
    REQUIRE_FALSE(spec_from_text("hello = world\n", junk));
}

namespace {

// The path used for tapered spline tubes: a quarter circle of radius 20 mm in the XY plane whose radius
// falls linearly from 3 mm to 1 mm over the five control points.
std::vector<PathPoint> tapered_arc()
{
    std::vector<PathPoint> arc;
    for (int i = 0; i <= 4; ++i) {
        const double a = 0.5 * PI * i / 4.;
        arc.push_back({Vec3d(20. * std::cos(a), 20. * std::sin(a), 0.), 3. - 0.5 * i});
    }
    return arc;
}

Vec3d random_direction(std::mt19937 &gen)
{
    std::normal_distribution<double> n(0., 1.);
    return Vec3d(n(gen), n(gen), n(gen)).normalized();
}

} // namespace

TEST_CASE("Tapered spline tube keeps its volume", "[Vessel]")
{
    const auto arc = tapered_arc();
    TubeParams p;
    p.sides          = 64;
    p.sample_spacing = 0.4;
    p.cap_start = p.cap_end = CapStyle::Flat;
    REQUIRE(validate_tube(arc, p).empty());

    const auto samples = sample_centreline(arc, true, p.sample_spacing);
    // Radius runs from 3 to 1 mm along the path.
    REQUIRE(samples.front().radius == Approx(3.));
    REQUIRE(samples.back().radius == Approx(1.));
    for (size_t i = 1; i < samples.size(); ++i)
        REQUIRE(samples[i].radius <= samples[i - 1].radius + 1e-12);

    // Sum of frusta between neighbouring samples: the volume of a tube that follows the path with a radius that changes linearly.
    double expected = 0.;
    for (size_t i = 1; i < samples.size(); ++i) {
        const double l = (samples[i].pos - samples[i - 1].pos).norm(), r0 = samples[i - 1].radius, r1 = samples[i].radius;
        expected += PI * l / 3. * (r0 * r0 + r0 * r1 + r1 * r1);
    }
    const auto its = make_tube(arc, p);
    const auto chk = check_mesh(its);
    REQUIRE(chk.watertight());
    REQUIRE(chk.degenerate_faces == 0);
    REQUIRE_FALSE(MeshBoolean::cgal::does_self_intersect(TriangleMesh(its)));
    // The bend moves volume from the inside to the outside of the curve and nearly cancels; polygon factor of 64 sides applies.
    REQUIRE(chk.volume == Approx(expected * polygon_area_factor(64)).epsilon(0.01));
}

TEST_CASE("Hollow tapered spline tube has the same wall along its length", "[Vessel]")
{
    const double wall = 0.4, layer_height = 0.2;
    const auto   arc  = tapered_arc();
    TubeParams   p;
    p.sides          = 64;
    p.sample_spacing = 0.4;
    p.wall_thickness = wall;
    const auto its   = make_tube(arc, p);
    REQUIRE(check_mesh(its).watertight());
    REQUIRE(check_mesh(its).volume > 0.);

    // At the three inner control points the centreline lies on the circle, so the radial direction is the cross-section normal.
    for (int i = 1; i <= 3; ++i) {
        const double a = 0.5 * PI * i / 4.;
        const Vec3d  origin(20. * std::cos(a), 20. * std::sin(a), 0.);
        const Vec3d  tilted = (Vec3d(std::cos(a), std::sin(a), 0.) + Vec3d(0, 0, 0.1)).normalized();
        const auto   t = ray_distances(its, origin, tilted);
        REQUIRE(t.size() == 2);
        const double measured = (t[1] - t[0]) * tilted.head<2>().norm();
        INFO("station " << i << " radius " << 3. - 0.5 * i << " measured wall " << measured);
        REQUIRE(std::abs(measured - wall) <= layer_height);
        REQUIRE(measured == Approx(wall).margin(0.02));
        // The outer surface is at the radius of this station.
        REQUIRE(t[1] * tilted.head<2>().norm() == Approx(3. - 0.5 * i).epsilon(0.01));
    }
}

TEST_CASE("The wall of a hollow bifurcation is never thinner than asked, also at the junction", "[Vessel]")
{
    const double wall = 0.4;
    BifurcationParams bp;
    bp.parent_radius = 2.;
    VesselMeshParams mp;
    mp.sides          = 48;
    mp.wall_thickness = wall;
    const auto its = make_bifurcation(bp, mp);
    REQUIRE(check_mesh(its).watertight());

    // From the junction centre, which is inside the lumen, shoot rays in all directions. Each ray that meets the wall passes
    // through it: the distance between the first two crossings is at least the perpendicular wall thickness.
    std::mt19937 gen(7);
    int          measured = 0;
    double       thinnest = 1e9;
    for (int k = 0; k < 400; ++k) {
        const auto t = ray_distances(its, bp.junction, random_direction(gen));
        if (t.size() < 2)
            continue;
        ++measured;
        thinnest = std::min(thinnest, t[1] - t[0]);
    }
    INFO("rays that crossed the wall " << measured << ", thinnest crossing " << thinnest);
    REQUIRE(measured > 100);
    REQUIRE(thinnest >= wall * 0.97);
}

TEST_CASE("Branch radii measured on the mesh follow Murray's law", "[Vessel]")
{
    for (uint32_t seed : {1u, 2u, 3u}) {
        TreeParams tp;
        tp.depth       = 2;
        tp.root_radius = 2.5;
        tp.root_length = 25.;
        tp.seed        = seed;
        const Skeleton sk = generate_tree(tp);
        VesselMeshParams mp;
        mp.sides = 48;
        const auto its = make_tree_mesh(sk, mp);
        REQUIRE(check_mesh(its).watertight());

        // Distance from the middle of each branch to the surface, perpendicular to the branch.
        std::vector<double> measured;
        for (const Branch &b : sk.branches) {
            const Vec3d axis = (b.end() - b.start()).normalized();
            const Vec3d u    = axis.cross(std::abs(axis.x()) < 0.9 ? Vec3d::UnitX() : Vec3d::UnitY()).normalized();
            const Vec3d v    = axis.cross(u);
            const double a   = 0.3 * 2. * PI / 48.;
            const auto   t   = ray_distances(its, 0.5 * (b.start() + b.end()), u * std::cos(a) + v * std::sin(a));
            REQUIRE(!t.empty());
            // The polygon lies inside the circle by at most 1 - cos(pi / 48) = 0.2 percent.
            REQUIRE(t[0] <= b.start_radius() * (1. + 1e-3));
            REQUIRE(t[0] >= b.start_radius() * 0.99);
            measured.push_back(t[0]);
        }
        for (size_t i = 0; i < sk.branches.size(); ++i)
            if (!sk.branches[i].is_leaf()) {
                const double rp = measured[i], ra = measured[sk.branches[i].children[0]], rb = measured[sk.branches[i].children[1]];
                REQUIRE(ra * ra * ra + rb * rb * rb == Approx(rp * rp * rp).epsilon(0.02));
            }
    }
}

TEST_CASE("Many seeds give watertight, manifold, deterministic trees", "[Vessel]")
{
    int checked = 0;
    for (uint32_t seed = 1; seed <= 30; ++seed) {
        TreeParams tp;
        tp.seed          = seed;
        tp.depth         = 1 + int(seed % 3);
        tp.root_radius   = 1.5 + 0.1 * (seed % 7);
        tp.root_length   = 14. + seed % 5;
        tp.length_ratio  = 0.7 + 0.02 * (seed % 6);
        tp.angle_jitter  = 6.;
        tp.length_jitter = 0.1;
        if (seed % 4 == 0) {
            tp.rule.radius_rule  = RadiusRule::FixedRatio;
            tp.rule.radius_ratio = 0.7 + 0.02 * (seed % 5);
            tp.rule.angle_rule   = AngleRule::Fixed;
            tp.rule.total_angle  = 50. + seed;
        } else {
            tp.rule.flow_split = 0.35 + 0.01 * (seed % 30);
        }
        INFO("seed " << seed);

        const Skeleton sk = generate_tree(tp);
        REQUIRE(skeleton_to_csv(sk) == skeleton_to_csv(generate_tree(tp)));
        if (tp.rule.radius_rule == RadiusRule::Murray)
            for (const Branch &b : sk.branches)
                if (!b.is_leaf())
                    REQUIRE(std::pow(sk.branches[b.children[0]].start_radius(), 3) + std::pow(sk.branches[b.children[1]].start_radius(), 3) ==
                            Approx(std::pow(b.start_radius(), 3)).epsilon(1e-12));

        VesselMeshParams mp;
        mp.sides = 12;
        const auto its = make_tree_mesh(sk, mp);
        const auto chk = check_mesh(its);
        REQUIRE(chk.watertight());
        REQUIRE(chk.volume > 0.);
        REQUIRE(its_number_of_patches(its) == 1);

        if (seed % 5 == 0) {
            mp.wall_thickness = 0.25;
            const auto hollow = make_tree_mesh(sk, mp);
            const auto hchk   = check_mesh(hollow);
            REQUIRE(hchk.watertight());
            REQUIRE(hchk.volume > 0.);
            REQUIRE(hchk.volume < chk.volume);
            REQUIRE(its_number_of_patches(hollow) == 1);
        }
        ++checked;
    }
    REQUIRE(checked == 30);
}

TEST_CASE("Centreline CSV lists every branch end with its radius", "[Vessel]")
{
    TreeParams tp;
    tp.depth        = 3;
    tp.root_radius  = 2. / 3.; // not a short decimal
    tp.root_length  = 10. / 7.;
    tp.root_position = Vec3d(1. / 3., -2. / 3., 0.1);
    tp.angle_jitter = 5.;
    tp.seed         = 21;
    const Skeleton sk = generate_tree(tp);
    const std::string csv = skeleton_to_csv(sk);

    std::istringstream in(csv);
    std::string        line;
    REQUIRE(std::getline(in, line));
    REQUIRE(line == "branch,parent,depth,point,x_mm,y_mm,z_mm,radius_mm");
    size_t rows = 0;
    while (std::getline(in, line)) {
        std::vector<std::string> f;
        std::istringstream       row(line);
        for (std::string cell; std::getline(row, cell, ',');)
            f.push_back(cell);
        REQUIRE(f.size() == 8);
        const Branch &b = sk.branches[std::stoul(f[0])];
        REQUIRE(std::stoi(f[1]) == b.parent);
        REQUIRE(std::stoi(f[2]) == b.depth);
        const Vec3d &p = f[3] == "0" ? b.start() : b.end();
        // Exactly the same numbers, not just close.
        REQUIRE(std::stod(f[4]) == p.x());
        REQUIRE(std::stod(f[5]) == p.y());
        REQUIRE(std::stod(f[6]) == p.z());
        REQUIRE(std::stod(f[7]) == b.start_radius());
        ++rows;
    }
    REQUIRE(rows == 2 * sk.branches.size());
    REQUIRE(sk.branches[0].parent == -1);

    SECTION("parameters with awkward numbers read back as the same numbers")
    {
        VesselSpec in_spec;
        in_spec.tree = tp;
        VesselSpec out_spec;
        REQUIRE(spec_from_text(spec_to_text(in_spec), out_spec));
        REQUIRE(out_spec.tree.root_radius == tp.root_radius);
        REQUIRE(out_spec.tree.root_length == tp.root_length);
        REQUIRE(out_spec.tree.root_position == tp.root_position);
        REQUIRE(skeleton_to_csv(generate_tree(out_spec.tree)) == csv);
    }
}
