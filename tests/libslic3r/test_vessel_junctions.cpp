#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_approx.hpp>

#include <cmath>
#include <random>
#include <sstream>

#include <libslic3r/AABBTreeIndirect.hpp>
#include <libslic3r/Geometry.hpp>
#include <libslic3r/MeshBoolean.hpp>
#include <libslic3r/TriangleMesh.hpp>
#include <libslic3r/Vessel/VesselTree.hpp>
#include <libslic3r/Vessel/VesselTube.hpp>

using namespace Slic3r;
using namespace Slic3r::vessel;
using Catch::Approx;

namespace {

// Analytic solid used as a reference that does not come from the mesh: cone frusta along the branch axes, a ball on
// every round end, and junction balls.
// A cone frustum from radius r at a to radius rb at b (rb < 0 means the same radius), with optional balls on its ends.
struct Capsule
{
    Vec3d  a, b;
    double r;
    bool   cap_a, cap_b;
    double rb = -1.;
    double radius_b() const { return rb < 0. ? r : rb; }

    bool inside(const Vec3d &p) const
    {
        const Vec3d  d = b - a;
        const double t = (p - a).dot(d) / d.squaredNorm();
        if ((t < 0. && !cap_a) || (t > 1. && !cap_b))
            return false;
        const double tc = std::clamp(t, 0., 1.);
        const double rt = r + (radius_b() - r) * tc;
        return (p - a - tc * d).squaredNorm() <= rt * rt;
    }
};

struct Ball
{
    Vec3d  c;
    double r;
    bool   inside(const Vec3d &p) const { return (p - c).squaredNorm() <= r * r; }
};

struct Reference
{
    std::vector<Capsule> tubes;
    std::vector<Ball>    balls;

    bool inside(const Vec3d &p) const
    {
        return std::any_of(balls.begin(), balls.end(), [&](const Ball &b) { return b.inside(p); }) ||
               std::any_of(tubes.begin(), tubes.end(), [&](const Capsule &c) { return c.inside(p); });
    }

    // Monte Carlo volume of the union with its standard error, deterministic. Each shape is sampled in a box around
    // it, and a point counts only if no shape before it contains it, so the shares add up to the union.
    std::pair<double, double> volume(int samples, uint32_t seed) const
    {
        struct Box { Vec3d origin, x, y, z; double volume; };
        std::vector<Box> boxes;
        for (const Capsule &c : tubes) {
            const double R = std::max(c.r, c.radius_b());
            const Vec3d  z = (c.b - c.a).normalized();
            const Vec3d  x = z.cross(std::abs(z.x()) < 0.9 ? Vec3d::UnitX() : Vec3d::UnitY()).normalized(), y = z.cross(x);
            const double before = c.cap_a ? c.r : 0., after = c.cap_b ? c.radius_b() : 0.;
            const Vec3d  origin = c.a - z * before - (x + y) * R;
            boxes.push_back({origin, 2. * R * x, 2. * R * y, ((c.b - c.a).norm() + before + after) * z, 0.});
        }
        for (const Ball &b : balls)
            boxes.push_back({b.c - Vec3d::Constant(b.r), Vec3d(2. * b.r, 0, 0), Vec3d(0, 2. * b.r, 0), Vec3d(0, 0, 2. * b.r), 0.});
        double total = 0.;
        for (Box &box : boxes)
            total += box.volume = box.x.norm() * box.y.norm() * box.z.norm();

        auto shape_inside = [&](size_t i, const Vec3d &p) { return i < tubes.size() ? tubes[i].inside(p) : balls[i - tubes.size()].inside(p); };
        std::mt19937                           gen(seed);
        std::uniform_real_distribution<double> u(0., 1.);
        double                                 volume = 0., variance = 0.;
        for (size_t i = 0; i < boxes.size(); ++i) {
            const Box &box = boxes[i];
            const int  n   = std::max(1000, int(samples * box.volume / total));
            int        in  = 0;
            for (int k = 0; k < n; ++k) {
                const Vec3d p = box.origin + u(gen) * box.x + u(gen) * box.y + u(gen) * box.z;
                if (!shape_inside(i, p))
                    continue;
                bool earlier = false;
                for (size_t j = 0; j < i && !earlier; ++j)
                    earlier = shape_inside(j, p);
                in += !earlier;
            }
            const double share = double(in) / n;
            volume += box.volume * share;
            variance += box.volume * box.volume * share * (1. - share) / n;
        }
        return {volume, std::sqrt(variance)};
    }
};

// Distances along rays to every surface crossing, nearest first, duplicates at shared edges removed.
class RayCaster
{
public:
    explicit RayCaster(const indexed_triangle_set &its)
        : m_its(its), m_tree(AABBTreeIndirect::build_aabb_tree_over_indexed_triangle_set(its.vertices, its.indices))
    {}

    std::vector<double> operator()(const Vec3d &origin, const Vec3d &dir) const
    {
        std::vector<igl::Hit> hits;
        AABBTreeIndirect::intersect_ray_all_hits(m_its.vertices, m_its.indices, m_tree, origin, dir, hits);
        std::vector<double> t;
        for (const igl::Hit &h : hits)
            if (t.empty() || h.t - t.back() > 1e-6)
                t.emplace_back(h.t);
        return t;
    }

private:
    const indexed_triangle_set      &m_its;
    AABBTreeIndirect::Tree<3, float> m_tree;
};

double point_segment_distance(const Vec3d &p, const Vec3d &a, const Vec3d &b)
{
    const Vec3d  d = b - a;
    const double t = std::clamp((p - a).dot(d) / d.squaredNorm(), 0., 1.);
    return (p - a - t * d).norm();
}

// A random junction with 2 or 3 children. Every third seed uses Murray's law with a random flow share and the optimal
// angles, the others a fixed child radius of 0.2 to 1 times the parent's and a fixed angle of 10 to 170 degrees between
// the children (with three children, each leaves at half of it from the parent axis). Vessels are as short as 1.05 parent radii.
struct RandomJunction
{
    BifurcationParams bp;
    double            angle_deg, ratio;
};

RandomJunction random_junction(uint32_t seed, int children)
{
    std::mt19937                           gen(seed);
    std::uniform_real_distribution<double> u(0., 1.);
    RandomJunction                         j;
    j.angle_deg = 10. + 160. * u(gen);
    j.ratio     = 0.2 + 0.8 * u(gen);
    BifurcationParams &bp = j.bp;
    bp.children           = children;
    bp.parent_radius      = 0.5 + 2.5 * u(gen);
    bp.parent_length      = bp.parent_radius * (1.05 + 6. * u(gen));
    bp.child_lengths.clear();
    for (int i = 0; i < children; ++i)
        bp.child_lengths.push_back(bp.parent_radius * (1.05 + 6. * u(gen)));
    bp.junction           = Vec3d(10. * u(gen) - 5., 10. * u(gen) - 5., 10. * u(gen));
    bp.parent_direction   = Vec3d(u(gen) - 0.5, u(gen) - 0.5, 1.).normalized();
    bp.plane_normal       = Vec3d(u(gen) - 0.5, 1., u(gen) - 0.5);
    if (seed % 3 == 0) {
        bp.rule.flow_split = 0.1 + 0.8 * u(gen);
        j.ratio            = std::pow(bp.rule.flow_split, 1. / 3.);
        j.angle_deg        = 0.;
    } else {
        bp.rule.radius_rule  = RadiusRule::FixedRatio;
        bp.rule.radius_ratio = j.ratio;
        bp.rule.angle_rule   = AngleRule::Fixed;
        bp.rule.total_angle  = j.angle_deg;
    }
    return j;
}

} // namespace

TEST_CASE("Random bifurcations are watertight, clean and do not bulge", "[Vessel][VesselJunction]")
{
    constexpr int sides = 24;
    int           failures = 0;
    std::string   report;
    for (uint32_t seed = 1; seed <= 60; ++seed) {
        const int            children = seed % 2 == 0 ? 3 : 2;
        const RandomJunction j  = random_junction(seed, children);
        const Skeleton       sk = make_bifurcation_skeleton(j.bp);
        VesselMeshParams     mp;
        mp.sides = sides;
        const indexed_triangle_set its = make_tree_mesh(sk, mp);
        const MeshCheck            chk = check_mesh(its);

        // Every vertex lies within 5 percent of the local radius from the nearest branch axis: no bulge at the junction.
        double worst_bulge = 0.;
        for (const Vec3f &vf : its.vertices) {
            const Vec3d v = vf.cast<double>();
            double      best = 1e30;
            for (const Branch &b : sk.branches)
                best = std::min(best, point_segment_distance(v, b.start(), b.end()) / b.start_radius());
            worst_bulge = std::max(worst_bulge, best);
        }
        const bool self_intersects = MeshBoolean::cgal::does_self_intersect(TriangleMesh(its));
        const bool ok = chk.watertight() && chk.degenerate_faces == 0 && !self_intersects && worst_bulge <= 1.05;
        if (!ok) {
            ++failures;
            std::ostringstream line;
            line << "seed " << seed << " children " << children << " angle " << j.angle_deg << " ratio " << j.ratio << ": open " << chk.open_edges << " nonmanifold "
                                  << chk.nonmanifold_edges << " degenerate " << chk.degenerate_faces << " self-intersecting "
                                  << self_intersects << " bulge " << worst_bulge << "\n";
            report += line.str();
        }
    }
    INFO(report);
    CHECK(failures == 0);
}

TEST_CASE("Random bifurcation volume matches the analytic union", "[Vessel][VesselJunction]")
{
    constexpr int sides = 48;
    // A regular polygon of `sides` corners inscribed in the circle has this share of its area.
    const double area_factor = sides * std::sin(2. * PI / sides) / (2. * PI);
    int          failures    = 0;
    std::string  report;
    for (uint32_t seed = 100; seed < 120; ++seed) {
        const int            children = seed % 2 == 0 ? 3 : 2;
        const RandomJunction j  = random_junction(seed, children);
        const Skeleton       sk = make_bifurcation_skeleton(j.bp);
        VesselMeshParams     mp;
        mp.sides = sides;
        const double volume = check_mesh(make_tree_mesh(sk, mp)).volume;

        // The ideal vessel: the three tubes with round outer ends and a junction no wider than the parent.
        Reference ref;
        for (const Branch &b : sk.branches)
            ref.tubes.push_back({b.start(), b.end(), b.start_radius(), b.parent == Branch::NONE, b.is_leaf()});
        ref.balls.push_back({j.bp.junction, j.bp.parent_radius});
        const auto [expected, sigma] = ref.volume(400000, seed);
        const double rel = volume / (expected * area_factor) - 1.;
        if (std::abs(rel) > 0.01 + 3. * sigma / expected) {
            ++failures;
            std::ostringstream line;
            line << "seed " << seed << " children " << children << " angle " << j.angle_deg << " ratio " << j.ratio << ": mesh " << volume << " mm3, analytic "
                                  << expected * area_factor << " +- " << sigma << " mm3, off by " << 100. * rel << " percent" << "\n";
            report += line.str();
        }
    }
    INFO(report);
    CHECK(failures == 0);
}

TEST_CASE("Random hollow bifurcations keep the wall and an open lumen", "[Vessel][VesselJunction]")
{
    constexpr int sides = 24;
    int           failures = 0;
    std::string   report;
    for (uint32_t seed = 200; seed < 230; ++seed) {
        const int            children = seed % 2 == 0 ? 3 : 2;
        const RandomJunction j  = random_junction(seed, children);
        const Skeleton       sk = make_bifurcation_skeleton(j.bp);
        double               r_min = 1e30;
        for (const Branch &b : sk.branches)
            r_min = std::min(r_min, b.start_radius());
        VesselMeshParams mp;
        mp.sides          = sides;
        mp.wall_thickness = 0.3 * r_min;
        const indexed_triangle_set its = make_tree_mesh(sk, mp);
        const MeshCheck            chk = check_mesh(its);
        const RayCaster            ray_distances(its);
        const double               w   = mp.wall_thickness;

        // The outer surface without the lumen, as analytic shapes: a ray only measures the outer wall when it ends up
        // outside all of them (0.03 mm past the mesh, which lies inside the circles by up to 0.026 mm), coming from the
        // lumen. A ray that leaves one lumen through the septum into a neighbouring lumen and out of its
        // open end measures the septum, which tapers to an edge where the lumens of two children part.
        Reference outer, lumen;
        for (const Branch &b : sk.branches) {
            outer.tubes.push_back({b.start(), b.end(), b.start_radius(), false, false});
            lumen.tubes.push_back({b.start(), b.end(), b.start_radius() - w, false, false});
        }
        outer.balls.push_back({j.bp.junction, j.bp.parent_radius});
        lumen.balls.push_back({j.bp.junction, j.bp.parent_radius - w});
        std::mt19937 gen(seed);
        std::normal_distribution<double> n(0., 1.);
        double thinnest = 1e30;
        int    measured = 0;
        auto   measure  = [&](const Vec3d &origin, const Vec3d &d) {
            const auto t = ray_distances(origin, d);
            // The ray must go from the lumen into the wall and from the wall to the outside, not graze a tube from outside.
            if (t.size() < 2 || outer.inside(origin + d * (t.back() + 0.03)) || !lumen.inside(origin + d * (t[t.size() - 2] - 0.03)))
                return;
            // The open ends are flat rings of width w; a ray that leaves through the rim of a ring cuts its corner.
            const Vec3d exit = origin + d * t.back();
            for (const Branch &b : sk.branches) {
                const Vec3d  axis = (b.end() - b.start()).normalized();
                const double s    = (exit - b.start()).dot(axis);
                if ((b.parent == Branch::NONE && s < w) || (b.is_leaf() && s > b.length() - w))
                    if ((exit - b.start() - s * axis).norm() < b.start_radius() + 0.03)
                        return;
            }
            thinnest = std::min(thinnest, t.back() - t[t.size() - 2]);
            ++measured;
        };
        // Rays from points on every axis (inside the lumen), perpendicular to the axis, and from the junction in all directions.
        for (const Branch &b : sk.branches) {
            const Vec3d axis = (b.end() - b.start()).normalized();
            for (int k = 0; k < 24; ++k) {
                Vec3d d(n(gen), n(gen), n(gen));
                measure(b.start() + (0.05 + 0.9 * (k + 0.5) / 24.) * (b.end() - b.start()), (d - d.dot(axis) * axis).normalized());
            }
        }
        for (int k = 0; k < 100; ++k)
            measure(j.bp.junction, Vec3d(n(gen), n(gen), n(gen)).normalized());
        // The lumen is open: a ray along each child axis from the junction meets no wall before the end of the child.
        bool open = true;
        for (size_t i = 1; i < sk.branches.size(); ++i) {
            const Branch &b = sk.branches[i];
            const auto    t = ray_distances(b.start(), (b.end() - b.start()).normalized());
            open &= t.empty() || t.front() >= b.length() - 1e-3;
        }
        const bool self_intersects = MeshBoolean::cgal::does_self_intersect(TriangleMesh(its));
        const bool ok = chk.watertight() && chk.degenerate_faces == 0 && !self_intersects && thinnest >= 0.98 * w && measured > 60 && open;
        if (!ok) {
            ++failures;
            std::ostringstream line;
            line << "seed " << seed << " children " << children << " angle " << j.angle_deg << " ratio " << j.ratio << " wall " << w << ": open edges " << chk.open_edges
                                  << " nonmanifold " << chk.nonmanifold_edges << " degenerate " << chk.degenerate_faces << " self-intersecting "
                                  << self_intersects << " thinnest " << thinnest << " of " << measured << " rays, lumen open " << open << "\n";
            report += line.str();
        }
    }
    INFO(report);
    CHECK(failures == 0);
}


TEST_CASE("Random trees with tapering, trifurcations and short vessels are clean and true to size", "[Vessel][VesselJunction]")
{
    constexpr int sides       = 32;
    const double  area_factor = sides * std::sin(2. * PI / sides) / (2. * PI);
    int           failures    = 0;
    std::string   report;
    for (uint32_t seed = 1; seed <= 24; ++seed) {
        std::mt19937                           gen(seed * 7919u);
        std::uniform_real_distribution<double> u(0., 1.);
        TreeParams tp;
        tp.seed          = seed;
        tp.depth         = 2 + int(seed % 2);
        tp.children      = seed % 3 == 0 ? 3 : 2;
        tp.root_radius   = 1. + 2. * u(gen);
        // Every fourth tree has vessels about as short as they are wide.
        tp.root_length   = tp.root_radius * (seed % 4 == 0 ? 1.6 : 4. + 4. * u(gen));
        tp.length_ratio  = 0.7 + 0.3 * u(gen);
        tp.taper         = 0.6 + 0.4 * u(gen);
        tp.angle_jitter  = 10. * u(gen);
        tp.length_jitter = 0.2 * u(gen);
        tp.rule.flow_split = 0.3 + 0.4 * u(gen);
        INFO("seed " << seed);
        REQUIRE(validate_tree_params(tp).empty());
        const Skeleton sk = generate_tree(tp);

        // Murray's law at every junction of the skeleton, from the radius where the parent ends.
        for (const Branch &b : sk.branches) {
            if (b.is_leaf())
                continue;
            double sum = 0.;
            for (int c : b.children)
                sum += std::pow(sk.branches[c].start_radius(), 3);
            REQUIRE(sum == Approx(std::pow(b.end_radius(), 3)).epsilon(1e-12));
            REQUIRE(b.end_radius() == Approx(tp.taper * b.start_radius()).epsilon(1e-12));
        }

        VesselMeshParams mp;
        mp.sides = sides;
        double r_min = 1e30;
        for (const Branch &b : sk.branches)
            r_min = std::min(r_min, b.end_radius());
        if (seed % 3 == 1)
            mp.wall_thickness = 0.3 * r_min;
        const indexed_triangle_set its = make_tree_mesh(sk, mp);
        const MeshCheck            chk = check_mesh(its);
        const RayCaster            ray_distances(its);
        const bool                 self_intersects = MeshBoolean::cgal::does_self_intersect(TriangleMesh(its));
        bool                       ok = chk.watertight() && chk.degenerate_faces == 0 && !self_intersects && chk.volume > 0. &&
                  its_number_of_patches(its) == 1;

        // Radius in the middle of every vessel, measured perpendicular to it, is the mean of its end radii. A ray that
        // runs into a neighbouring vessel within 1.5 radii is not used.
        double worst_radius = 0.;
        for (const Branch &b : sk.branches) {
            const Vec3d  axis = (b.end() - b.start()).normalized();
            const Vec3d  mid  = 0.5 * (b.start() + b.end());
            const double r    = 0.5 * (b.start_radius() + b.end_radius());
            const Vec3d  u0   = axis.cross(std::abs(axis.x()) < 0.9 ? Vec3d::UnitX() : Vec3d::UnitY()).normalized();
            for (int k = 0; k < 6; ++k) {
                const double a = 2. * PI * (k + 0.37) / 6.;
                const Vec3d  d = u0 * std::cos(a) + axis.cross(u0) * std::sin(a);
                bool         crowded = false;
                for (const Branch &o : sk.branches)
                    if (&o != &b && point_segment_distance(mid + d * 1.5 * r, o.start(), o.end()) < 1.5 * std::max(o.start_radius(), r))
                        crowded = true;
                if (crowded)
                    continue;
                const auto   t     = ray_distances(mid, d);
                const size_t outer = mp.wall_thickness > 0. ? 1 : 0;
                if (t.size() <= outer)
                    continue;
                // The polygon lies inside the circle by 0 to 1 - cos(pi / 32) = 0.48 percent, 0.24 percent on average.
                worst_radius = std::max(worst_radius, std::abs(t[outer] / r - (1. - 0.0024)));
            }
        }
        ok &= worst_radius < 0.006;

        double rel = 0.;
        if (mp.wall_thickness == 0.) {
            Reference ref;
            for (const Branch &b : sk.branches) {
                ref.tubes.push_back({b.start(), b.end(), b.start_radius(), b.parent == Branch::NONE, b.is_leaf(), b.end_radius()});
                if (!b.is_leaf())
                    ref.balls.push_back({b.end(), b.end_radius()});
            }
            const auto [expected, sigma] = ref.volume(300000, seed);
            rel = chk.volume / (expected * area_factor) - 1.;
            ok &= std::abs(rel) < 0.015 + 3. * sigma / expected;
        }
        if (!ok) {
            ++failures;
            std::ostringstream line;
            line << "seed " << seed << " children " << tp.children << " taper " << tp.taper << " wall " << mp.wall_thickness << ": open "
                                  << chk.open_edges << " nonmanifold " << chk.nonmanifold_edges << " degenerate " << chk.degenerate_faces
                                  << " self-intersecting " << self_intersects << " worst radius error " << worst_radius
                                  << " volume off by " << 100. * rel << " percent" << "\n";
            report += line.str();
        }
    }
    INFO(report);
    CHECK(failures == 0);
}
