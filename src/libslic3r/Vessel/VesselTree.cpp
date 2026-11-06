#include "VesselTree.hpp"

#include <algorithm>
#include <cmath>
#include <functional>
#include <iomanip>
#include <locale>
#include <map>
#include <memory>
#include <numeric>
#include <sstream>
#include <tuple>

#include "VesselBoolean.hpp"
#include "libslic3r/AABBTreeIndirect.hpp"
#include "libslic3r/Exception.hpp"
#include "libslic3r/Geometry.hpp"
#include "libslic3r/libslic3r.h"

namespace Slic3r { namespace vessel {

namespace {

// Children that leave a junction less than this far apart would make the same tube twice.
constexpr double MIN_CHILD_SEPARATION_DEG = 0.5;
// A child's first point may be this far from the end of its parent and is moved onto it.
constexpr double SNAP_DISTANCE = 0.01;
// Junction spheres are never wider than this times the widest vessel that meets there (the largest junction margin).
// The fill mode keeps this much room for them.
constexpr double MAX_JUNCTION_SCALE = 1.05;
// Largest number of corners of a cross section.
constexpr int MAX_SIDES = 256;

// Rotates v around the unit axis k by `angle` radians (Rodrigues' formula).
Vec3d rotate_about(const Vec3d &v, const Vec3d &k, double angle)
{
    const double c = std::cos(angle), s = std::sin(angle);
    return v * c + k.cross(v) * s + k * k.dot(v) * (1. - c);
}

Vec3d any_perpendicular(const Vec3d &v)
{
    const Vec3d axis = std::abs(v.x()) < 0.9 ? Vec3d::UnitX() : Vec3d::UnitY();
    return v.cross(axis).normalized();
}

double angle_between(const Vec3d &a, const Vec3d &b)
{
    return std::acos(std::clamp(a.normalized().dot(b.normalized()), -1., 1.));
}

// splitmix64: a small generator whose output is the same on every platform and standard library.
class Random
{
public:
    explicit Random(uint64_t seed) : m_state(seed) {}
    // Uniform in [-1, 1).
    double signed_unit() { return 2. * unit() - 1.; }
    // Uniform in [0, 1).
    double unit() { return double(next() >> 11) * (1. / 9007199254740992.); }

private:
    uint64_t next()
    {
        uint64_t z = (m_state += 0x9E3779B97F4A7C15ull);
        z          = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
        z          = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
        return z ^ (z >> 31);
    }
    uint64_t m_state;
};

// Shortest distance between the segments p0-p1 and q0-q1 (Ericson, Real-Time Collision Detection, 5.1.9).
double segment_distance(const Vec3d &p0, const Vec3d &p1, const Vec3d &q0, const Vec3d &q1)
{
    const Vec3d  d1 = p1 - p0, d2 = q1 - q0, r = p0 - q0;
    const double a = d1.squaredNorm(), e = d2.squaredNorm(), f = d2.dot(r);
    double       s = 0., t = 0.;
    if (a <= 1e-24 && e <= 1e-24)
        return r.norm();
    if (a <= 1e-24)
        t = std::clamp(f / e, 0., 1.);
    else {
        const double c = d1.dot(r);
        if (e <= 1e-24)
            s = std::clamp(-c / a, 0., 1.);
        else {
            const double b = d1.dot(d2), denom = a * e - b * b;
            s = denom > 1e-24 ? std::clamp((b * f - c * e) / denom, 0., 1.) : 0.;
            t = (b * s + f) / e;
            if (t < 0.) {
                t = 0.;
                s = std::clamp(-c / a, 0., 1.);
            } else if (t > 1.) {
                t = 1.;
                s = std::clamp((b - c) / a, 0., 1.);
            }
        }
    }
    return (p0 + d1 * s - (q0 + d2 * t)).norm();
}

// Inside tests, distance and free-path queries on a closed mesh.
class Bounds
{
public:
    explicit Bounds(const indexed_triangle_set &its)
        : m_its(its), m_tree(AABBTreeIndirect::build_aabb_tree_over_indexed_triangle_set(its.vertices, its.indices))
    {
        m_lo = Vec3d::Constant(std::numeric_limits<double>::max());
        m_hi = -m_lo;
        for (const Vec3f &v : its.vertices) {
            m_lo = m_lo.cwiseMin(v.cast<double>());
            m_hi = m_hi.cwiseMax(v.cast<double>());
        }
    }

    bool inside(const Vec3d &p) const
    {
        // A direction that is unlikely to graze an edge of the mesh.
        const Vec3d                dir = Vec3d(0.5773, 0.3141, 0.7573).normalized();
        std::vector<igl::Hit>      hits;
        AABBTreeIndirect::intersect_ray_all_hits(m_its.vertices, m_its.indices, m_tree, p, dir, hits);
        return hits.size() % 2 == 1;
    }

    // Distance from p to the nearest point of the surface.
    double distance(const Vec3d &p) const
    {
        size_t hit_idx;
        Vec3d  hit_point;
        return std::sqrt(AABBTreeIndirect::squared_distance_to_indexed_triangle_set(m_its.vertices, m_its.indices, m_tree, p, hit_idx, hit_point));
    }

    // True if the ball around p stays inside the mesh.
    bool contains_ball(const Vec3d &p, double radius) const { return inside(p) && distance(p) >= radius; }

    // Distance from `start` along `dir` that stays `margin` away from the surface, at most `max_length`.
    double free_length(const Vec3d &start, const Vec3d &dir, double max_length, double margin) const
    {
        igl::Hit hit;
        if (AABBTreeIndirect::intersect_ray_first_hit(m_its.vertices, m_its.indices, m_tree, start, dir, hit)
            && hit.t < max_length + margin)
            return hit.t - margin;
        return max_length;
    }

    const Vec3d &lo() const { return m_lo; }
    const Vec3d &hi() const { return m_hi; }

private:
    const indexed_triangle_set &m_its;
    AABBTreeIndirect::Tree<3, float> m_tree;
    Vec3d                            m_lo, m_hi;
};

// Grows a tree by the branching rules.
class Grower
{
public:
    Grower(const TreeParams &params, const Bounds *bounds) : m_params(params), m_bounds(bounds), m_random(params.seed) {}

    Skeleton run()
    {
        const Vec3d dir = m_params.root_direction.normalized();
        if (m_bounds && !m_bounds->inside(m_params.root_position))
            throw InvalidArgument("The root of the vessel tree is outside the volume it must stay inside.");
        if (grow(Branch::NONE, m_params.root_position, dir, m_params.root_radius, m_params.root_length, 0) == Branch::NONE)
            throw InvalidArgument("The root branch does not fit inside the volume it must stay inside.");
        return std::move(m_skeleton);
    }

private:
    int grow(int parent, const Vec3d &start, const Vec3d &dir, double radius, double nominal_length, int depth)
    {
        double length = nominal_length;
        if (m_bounds) {
            const double free = m_bounds->free_length(start, dir, length, radius);
            if (free < length) {
                ++m_skeleton.clipped_by_bounds;
                length = free;
            }
            if (length < radius)
                return Branch::NONE;
        }

        const int    index      = int(m_skeleton.branches.size());
        const double end_radius = radius * m_params.taper;
        {
            Branch b;
            b.parent = parent;
            b.depth  = depth;
            b.path   = {{start, radius}, {start + dir * length, end_radius}};
            m_skeleton.branches.emplace_back(b);
        }

        if (depth < m_params.depth) {
            const int              n = m_params.children;
            const JunctionGeometry g = compute_junction(end_radius, n, m_params.rule);
            // All random numbers of this node are drawn up front so that their count does not depend on the outcome of the children.
            const double        azimuth = 2. * PI * m_random.unit();
            std::vector<double> angle_jitter(n), length_jitter(n);
            for (double &j : angle_jitter)
                j = m_random.signed_unit();
            for (double &j : length_jitter)
                j = m_random.signed_unit();
            const double jit = Slic3r::Geometry::deg2rad(m_params.angle_jitter);

            const Vec3d  end    = m_skeleton.branches[index].end();
            const Vec3d  normal = rotate_about(any_perpendicular(dir), dir, azimuth);
            const double base   = nominal_length * m_params.length_ratio;

            std::vector<int> children;
            for (int i = 0; i < n; ++i) {
                if (g.radii[i] < m_params.min_radius)
                    continue;
                // The first child turns towards `normal`, the others are spread around the parent axis. Every child but the
                // first turns its jitter the other way, so that trees saved by earlier versions grow the same.
                const Vec3d  axis  = i == 0 ? normal : n == 2 ? Vec3d(-normal) : rotate_about(normal, dir, g.azimuths[i]);
                const double angle = g.angles[i] + (i == 0 ? 1. : -1.) * jit * angle_jitter[i];
                const int    child = grow(index, end, rotate_about(dir, axis, angle).normalized(), g.radii[i],
                                          base * (1. + m_params.length_jitter * length_jitter[i]), depth + 1);
                if (child != Branch::NONE)
                    children.emplace_back(child);
            }
            m_skeleton.branches[index].children = std::move(children);
        }
        return index;
    }

    const TreeParams &m_params;
    const Bounds     *m_bounds;
    Random            m_random;
    Skeleton          m_skeleton;
};

// Grows a tree towards target points spread through a closed mesh (space colonisation, Runions et al. 2007).
class Filler
{
public:
    Filler(const TreeParams &params, const Bounds &bounds) : m_params(params), m_bounds(bounds), m_random(params.seed) {}

    Skeleton run()
    {
        place_targets();
        place_trunk();
        grow();
        // Removing a vessel changes the radii of those that fed it, so this repeats until every vessel fits.
        for (;;) {
            assign_leaves();
            Skeleton  sk     = build_skeleton();
            const int victim = first_misfit(sk);
            if (victim < 0) {
                sk.clipped_by_bounds = m_pruned;
                return sk;
            }
            if (victim == 0)
                throw InvalidArgument("The root vessel does not fit inside the object with this clearance. Move it, shorten it or make it thinner.");
            kill_subtree(m_first_node[victim]);
            ++m_pruned;
        }
    }

private:
    struct Node
    {
        Vec3d            pos;
        int              parent = -1;
        std::vector<int> children;
        // Trunk nodes other than the last one do not sprout branches.
        bool             grows  = true;
        bool             alive  = true;
        int              leaves = 0;
    };

    double step() const { return m_params.fill_step; }
    double influence() const { return 5. * step(); }
    double kill_distance() const { return step(); }
    // Vessels never come closer to each other than this, measured between centreline nodes.
    double node_spacing() const { return 0.5 * step(); }

    void place_targets()
    {
        const Vec3d lo = m_bounds.lo(), hi = m_bounds.hi();
        const int   wanted = m_params.fill_points;
        for (int tries = 0; tries < 400 * wanted && int(m_targets.size()) < wanted; ++tries) {
            const Vec3d q(lo.x() + m_random.unit() * (hi.x() - lo.x()), lo.y() + m_random.unit() * (hi.y() - lo.y()),
                          lo.z() + m_random.unit() * (hi.z() - lo.z()));
            if (m_bounds.contains_ball(q, m_params.fill_clearance + node_spacing()))
                m_targets.emplace_back(q);
        }
        if (m_targets.empty())
            throw InvalidArgument("No room for vessels: the object is too thin for the clearance and the step length.");
    }

    void place_trunk()
    {
        const Vec3d  dir   = m_params.root_direction.normalized();
        const int    steps = std::max(1, int(std::ceil(m_params.root_length / step() - 1e-9)));
        for (int i = 0; i <= steps; ++i) {
            Node n;
            n.pos    = m_params.root_position + dir * (m_params.root_length * double(i) / steps);
            n.parent = i == 0 ? -1 : i - 1;
            n.grows  = i == steps;
            if (!m_bounds.contains_ball(n.pos, m_params.root_radius + m_params.fill_clearance))
                throw InvalidArgument("The root vessel does not fit inside the object with this clearance. Move it, shorten it or make it thinner.");
            if (i > 0)
                m_nodes[i - 1].children.emplace_back(i);
            m_nodes.emplace_back(n);
        }
        remove_reached_targets(0);
    }

    // Removes the targets within the kill distance of any node from `first_node` on.
    void remove_reached_targets(size_t first_node)
    {
        const double k2 = kill_distance() * kill_distance();
        m_targets.erase(std::remove_if(m_targets.begin(), m_targets.end(), [&](const Vec3d &t) {
            for (size_t n = first_node; n < m_nodes.size(); ++n)
                if ((m_nodes[n].pos - t).squaredNorm() < k2)
                    return true;
            return false;
        }), m_targets.end());
    }

    void grow()
    {
        const double       cos_separation = std::cos(Slic3r::Geometry::deg2rad(25.));
        std::vector<Vec3d> pull;
        std::vector<int>   count;
        // Every round adds nodes at least half a step apart inside a bounded volume, so the loop ends.
        while (!m_targets.empty()) {
            pull.assign(m_nodes.size(), Vec3d::Zero());
            count.assign(m_nodes.size(), 0);
            for (const Vec3d &t : m_targets) {
                int    best = -1;
                double best_d2 = influence() * influence();
                for (size_t n = 0; n < m_nodes.size(); ++n) {
                    const double d2 = (t - m_nodes[n].pos).squaredNorm();
                    if (d2 < best_d2) {
                        best_d2 = d2;
                        best    = int(n);
                    }
                }
                if (best >= 0 && m_nodes[best].grows) {
                    pull[best] += (t - m_nodes[best].pos).normalized();
                    ++count[best];
                }
            }

            const size_t before = m_nodes.size();
            for (size_t n = 0; n < before; ++n) {
                Node &node = m_nodes[n];
                // Pulls from opposite sides cancel out; three children are the most a junction gets.
                if (count[n] == 0 || node.children.size() >= 3 || pull[n].norm() < 0.1)
                    continue;
                const Vec3d dir = pull[n].normalized();
                const Vec3d q   = node.pos + step() * dir;
                if (!m_bounds.contains_ball(q, m_params.fill_clearance + node_spacing()))
                    continue;
                bool ok = true;
                for (int c : node.children)
                    ok &= (m_nodes[c].pos - node.pos).normalized().dot(dir) < cos_separation;
                if (node.parent >= 0)
                    ok &= (m_nodes[node.parent].pos - node.pos).normalized().dot(dir) < cos_separation;
                for (size_t m = 0; m < m_nodes.size() && ok; ++m)
                    ok &= m == n || (m_nodes[m].pos - q).squaredNorm() >= node_spacing() * node_spacing();
                if (!ok)
                    continue;
                Node child;
                child.pos    = q;
                child.parent = int(n);
                m_nodes.emplace_back(child);
                m_nodes[n].children.emplace_back(int(m_nodes.size()) - 1);
            }
            if (m_nodes.size() == before)
                break;
            remove_reached_targets(before);
        }
    }

    // Number of vessel ends that each node feeds.
    void assign_leaves()
    {
        for (Node &n : m_nodes)
            n.leaves = 0;
        for (size_t i = m_nodes.size(); i-- > 0;) {
            Node &n = m_nodes[i];
            if (!n.alive)
                continue;
            if (n.leaves == 0)
                n.leaves = 1;
            if (n.parent >= 0)
                m_nodes[n.parent].leaves += n.leaves;
        }
    }

    // Radius of the vessel that ends in this node, by Murray's law with equal flow to every end.
    double radius(const Node &n) const
    {
        const double e = std::max(m_params.rule.murray_exponent, 1e-3);
        return m_params.root_radius * std::pow(double(n.leaves) / double(m_nodes[0].leaves), 1. / e);
    }

    // Index of the first branch that comes closer to the surface than the clearance, or that touches a vessel it does not
    // share a junction with; -1 if every branch fits. The branch is checked as make_tree_mesh() sweeps it: along the spline
    // through its points and along the straight pieces between them, whose mitre joints stick out by up to the largest
    // stretch of a joint, with its round ends and the junction sphere at its end.
    int first_misfit(const Skeleton &sk) const
    {
        const double clearance = m_params.fill_clearance;
        for (size_t i = 0; i < sk.branches.size(); ++i) {
            const Branch &b = sk.branches[i];
            const double  r = b.start_radius();
            double stretch = 1.;
            for (size_t k = 1; k + 1 < b.path.size(); ++k) {
                const Vec3d t_in = (b.path[k].pos - b.path[k - 1].pos).normalized(), t_out = (b.path[k + 1].pos - b.path[k].pos).normalized();
                stretch = std::max(stretch, 2. / std::max((t_in + t_out).norm(), 1e-9));
            }
            // Every point of the centreline is within half a spacing of a sample.
            const double spacing = std::min(0.2 * r, 0.25 * step());
            std::vector<PathPoint> c = sample_centreline(b.path, false, spacing);
            const std::vector<PathPoint> spline = sample_centreline(b.path, true, spacing);
            c.insert(c.end(), spline.begin(), spline.end());
            for (const PathPoint &p : c)
                if (!m_bounds.contains_ball(p.pos, stretch * r + clearance + 0.5 * spacing))
                    return int(i);
            if (!b.is_leaf()) {
                double r_max = b.end_radius();
                for (int ch : b.children)
                    r_max = std::max(r_max, sk.branches[ch].start_radius());
                if (!m_bounds.contains_ball(b.end(), MAX_JUNCTION_SCALE * r_max + clearance))
                    return int(i);
            }
        }
        // Vessels that do not meet at a junction keep apart.
        auto related = [&](size_t i, size_t j) {
            const Branch &a = sk.branches[i], &b = sk.branches[j];
            return a.parent == int(j) || b.parent == int(i) || (a.parent == b.parent && a.parent != Branch::NONE);
        };
        for (size_t j = 1; j < sk.branches.size(); ++j)
            for (size_t i = 0; i < j; ++i) {
                if (related(i, j))
                    continue;
                const std::vector<PathPoint> &p = sk.branches[i].path, &q = sk.branches[j].path;
                const double gap = sk.branches[i].start_radius() + sk.branches[j].start_radius();
                for (size_t k = 0; k + 1 < p.size(); ++k)
                    for (size_t m = 0; m + 1 < q.size(); ++m)
                        if (segment_distance(p[k].pos, p[k + 1].pos, q[m].pos, q[m + 1].pos) < gap)
                            return int(j);
            }
        return -1;
    }

    void kill_subtree(int index)
    {
        Node &n = m_nodes[index];
        n.alive = false;
        for (int c : n.children)
            kill_subtree(c);
        n.children.clear();
        if (n.parent >= 0) {
            std::vector<int> &siblings = m_nodes[n.parent].children;
            siblings.erase(std::remove(siblings.begin(), siblings.end(), index), siblings.end());
        }
    }

    Skeleton build_skeleton()
    {
        Skeleton sk;
        m_first_node.clear();
        // Chains of nodes with a single child become one branch each.
        std::function<void(int, int, int)> walk = [&](int first, int parent_branch, int depth) {
            Branch       b;
            b.parent = parent_branch;
            b.depth  = depth;
            int node = first;
            if (parent_branch != Branch::NONE)
                b.path.push_back({m_nodes[m_nodes[first].parent].pos, 0.});
            for (;;) {
                b.path.push_back({m_nodes[node].pos, 0.});
                if (m_nodes[node].children.size() != 1)
                    break;
                node = m_nodes[node].children.front();
            }
            const double r = radius(m_nodes[first]);
            for (PathPoint &p : b.path)
                p.radius = r;
            smooth(b.path);
            const int index = int(sk.branches.size());
            sk.branches.emplace_back(std::move(b));
            m_first_node.emplace_back(first);
            if (parent_branch != Branch::NONE)
                sk.branches[parent_branch].children.emplace_back(index);
            for (int c : m_nodes[node].children)
                walk(c, index, depth + 1);
        };
        walk(0, Branch::NONE, 0);
        return sk;
    }

    // Rounds off the corners of a chain so that it can be swept as a spline or as straight pieces, and keeps
    // every point at the clearance from the surface. Points that cannot be rounded off are left where they were.
    void smooth(std::vector<PathPoint> &path) const
    {
        if (path.size() < 3)
            return;
        const double           r = path.front().radius;
        std::vector<PathPoint> smoothed = path;
        for (int pass = 0; pass < 2; ++pass)
            for (size_t i = 1; i + 1 < smoothed.size(); ++i)
                smoothed[i].pos = 0.5 * smoothed[i].pos + 0.25 * (smoothed[i - 1].pos + smoothed[i + 1].pos);
        for (size_t i = 1; i + 1 < smoothed.size(); ++i)
            if (!m_bounds.contains_ball(smoothed[i].pos, r + m_params.fill_clearance))
                smoothed[i].pos = path[i].pos;
        // Pull a corner towards the line through its neighbours until its circle of curvature is twice the radius.
        for (int pass = 0; pass < 100; ++pass) {
            bool changed = false;
            for (size_t i = 1; i + 1 < smoothed.size(); ++i) {
                const Vec3d  a = smoothed[i - 1].pos, b = smoothed[i].pos, c = smoothed[i + 1].pos;
                const double cross = (b - a).cross(c - b).norm();
                if (cross < 1e-12)
                    continue;
                const double circumradius = (b - a).norm() * (c - b).norm() * (c - a).norm() / (2. * cross);
                if (circumradius >= 2. * r)
                    continue;
                const Vec3d moved = 0.5 * b + 0.25 * (a + c);
                if (m_bounds.contains_ball(moved, r + m_params.fill_clearance)) {
                    smoothed[i].pos = moved;
                    changed         = true;
                }
            }
            if (!changed)
                break;
        }
        TubeParams check;
        check.spline = true;
        if (validate_tube(smoothed, check).empty()) {
            check.spline = false;
            if (validate_tube(smoothed, check).empty())
                path = std::move(smoothed);
        }
    }

    const TreeParams  &m_params;
    const Bounds      &m_bounds;
    Random             m_random;
    std::vector<Vec3d> m_targets;
    std::vector<Node>  m_nodes;
    // The node each branch of the last skeleton starts with.
    std::vector<int>   m_first_node;
    size_t             m_pruned = 0;
};

// What make_tree_mesh() builds: one swept tube per branch and a ball at every junction that needs one.
struct TubeJob
{
    std::vector<PathPoint> samples;
    CapStyle               cap_start;
    CapStyle               cap_end;
};

struct BallJob
{
    Vec3d  center;
    double radius;
    Vec3d  axis;
};

struct MeshPlan
{
    std::vector<TubeJob> tubes;
    std::vector<BallJob> balls;
};

// Radius of the junction sphere at the end of a branch, or 0 when the round end of the parent is the junction.
double junction_sphere_radius(const Skeleton &skeleton, const Branch &b, const VesselMeshParams &params)
{
    const double r_parent = b.end_radius();
    double       r_max    = 0.;
    for (int c : b.children)
        r_max = std::max(r_max, skeleton.branches[c].start_radius());
    if (r_max <= (1. - params.junction_margin) * r_parent)
        return 0.;
    return (1. + params.junction_margin) * std::max(r_parent, r_max);
}

// How far the lumen may stick out of an open end so that the cut leaves no skin over the opening: the wall thickness,
// or less where another vessel lies just beyond the end, so that the lumen never cuts into that vessel's wall.
double open_end_extension(const MeshPlan &outer, size_t tube, bool at_start, double wall)
{
    const std::vector<PathPoint> &own = outer.tubes[tube].samples;
    const Vec3d  end = at_start ? own.front().pos : own.back().pos;
    const Vec3d  dir = at_start ? Vec3d((own[0].pos - own[1].pos).normalized()) : Vec3d((own.back().pos - own[own.size() - 2].pos).normalized());
    const double r   = (at_start ? own.front().radius : own.back().radius) - wall;
    for (double ext = wall; ext > wall / 100.; ext *= 0.5) {
        const Vec3d a = end + dir * (0.01 * wall), b = end + dir * ext;
        bool        clear = true;
        for (size_t i = 0; i < outer.tubes.size() && clear; ++i) {
            if (i == tube)
                continue;
            const std::vector<PathPoint> &s = outer.tubes[i].samples;
            for (size_t k = 0; k + 1 < s.size() && clear; ++k)
                clear = segment_distance(a, b, s[k].pos, s[k + 1].pos) > r + std::max(s[k].radius, s[k + 1].radius);
        }
        for (const BallJob &ball : outer.balls)
            clear &= segment_distance(a, b, ball.center, ball.center) > r + ball.radius;
        if (clear)
            return ext;
    }
    return wall / 100.;
}

MeshPlan plan_tree(const Skeleton &skeleton, const VesselMeshParams &params, bool lumen)
{
    const double inset  = lumen ? params.wall_thickness : 0.;
    const bool   hollow = params.wall_thickness > 0.;
    // A hollow tree is open at the root and at the leaves; the lumen sticks out of the open ends so that the cut leaves no skin.
    const CapStyle end_cap = hollow ? CapStyle::Flat : params.caps;
    const MeshPlan outer   = lumen ? plan_tree(skeleton, params, false) : MeshPlan();

    MeshPlan plan;
    for (size_t i = 0; i < skeleton.branches.size(); ++i) {
        const Branch &b = skeleton.branches[i];
        TubeJob job;
        job.samples = b.path.size() > 2 && params.spline ? sample_centreline(b.path, true, params.sample_spacing) : b.path;
        for (PathPoint &p : job.samples)
            p.radius -= inset;
        const bool   root   = b.parent == Branch::NONE;
        const double sphere = b.is_leaf() ? 0. : junction_sphere_radius(skeleton, b, params);
        job.cap_start       = root ? end_cap : CapStyle::Flat;
        job.cap_end         = b.is_leaf() ? end_cap : (sphere > 0. ? CapStyle::Flat : CapStyle::Round);
        if (lumen && root) {
            const double ext = open_end_extension(outer, i, true, params.wall_thickness);
            job.samples.insert(job.samples.begin(), PathPoint{job.samples.front().pos - b.start_direction() * ext, job.samples.front().radius});
        }
        if (lumen && b.is_leaf()) {
            const double ext = open_end_extension(outer, i, false, params.wall_thickness);
            job.samples.push_back(PathPoint{job.samples.back().pos + b.end_direction() * ext, job.samples.back().radius});
        }
        plan.tubes.emplace_back(std::move(job));
        if (sphere > 0.)
            plan.balls.push_back({b.end(), sphere - inset, b.end_direction()});
    }
    return plan;
}

// Rotation of the cross section vertices of piece `index`, within one side step, so that no two pieces share a vertex.
double ring_offset(size_t index, int sides, int attempt)
{
    const double golden = 0.6180339887498949;
    const double f      = std::fmod(golden * double(index + 1) + 0.2371 * attempt, 1.);
    return f * 2. * PI / sides;
}

// Sphere with `sides` meridians and 2 * rings latitude bands around `axis`, outward facing.
indexed_triangle_set make_ball(const Vec3d &center, double radius, const Vec3d &axis_in, int sides, int rings, double offset)
{
    const Vec3d axis     = axis_in.normalized();
    const Vec3d normal   = any_perpendicular(axis);
    const Vec3d binormal = axis.cross(normal);
    const int   bands    = 2 * rings;

    indexed_triangle_set its;
    its.vertices.emplace_back((center + radius * axis).cast<float>());
    for (int k = 1; k < bands; ++k) {
        const double phi = PI * double(k) / bands;
        for (int j = 0; j < sides; ++j) {
            const double th = 2. * PI * double(j) / sides + offset;
            its.vertices.emplace_back((center + radius * (std::sin(phi) * (std::cos(th) * normal + std::sin(th) * binormal) + std::cos(phi) * axis)).cast<float>());
        }
    }
    its.vertices.emplace_back((center - radius * axis).cast<float>());
    const int top = 0, bottom = int(its.vertices.size()) - 1;
    auto ring_vertex = [&](int k, int j) { return 1 + (k - 1) * sides + (j % sides); };
    for (int j = 0; j < sides; ++j)
        its.indices.emplace_back(top, ring_vertex(1, j), ring_vertex(1, j + 1));
    for (int k = 1; k + 1 < bands; ++k)
        for (int j = 0; j < sides; ++j) {
            its.indices.emplace_back(ring_vertex(k, j), ring_vertex(k + 1, j), ring_vertex(k + 1, j + 1));
            its.indices.emplace_back(ring_vertex(k, j), ring_vertex(k + 1, j + 1), ring_vertex(k, j + 1));
        }
    for (int j = 0; j < sides; ++j)
        its.indices.emplace_back(bottom, ring_vertex(bands - 1, j + 1), ring_vertex(bands - 1, j));
    return its;
}

std::vector<indexed_triangle_set> make_pieces(const MeshPlan &plan, const VesselMeshParams &params, int attempt)
{
    std::vector<indexed_triangle_set> pieces;
    size_t                            index = 0;
    for (const TubeJob &job : plan.tubes) {
        TubeParams tp;
        tp.spline      = false;
        tp.sides       = params.sides;
        tp.cap_rings   = params.cap_rings;
        tp.cap_start   = job.cap_start;
        tp.cap_end     = job.cap_end;
        tp.ring_offset = ring_offset(index++, params.sides, attempt);
        pieces.emplace_back(make_tube(job.samples, tp));
    }
    for (const BallJob &ball : plan.balls)
        pieces.emplace_back(make_ball(ball.center, ball.radius, ball.axis, params.sides, params.cap_rings, ring_offset(index++, params.sides, attempt)));
    return pieces;
}

// The boolean operations return vertices and faces in an order that varies between runs.
// Sorting them makes the same skeleton always give the same mesh, bit for bit.
void canonicalize(indexed_triangle_set &its)
{
    std::vector<int> order(its.vertices.size());
    std::iota(order.begin(), order.end(), 0);
    std::stable_sort(order.begin(), order.end(), [&](int a, int b) {
        const Vec3f &va = its.vertices[a], &vb = its.vertices[b];
        return std::tie(va.x(), va.y(), va.z()) < std::tie(vb.x(), vb.y(), vb.z());
    });
    std::vector<int> rank(order.size());
    std::vector<Vec3f> vertices(order.size());
    for (size_t i = 0; i < order.size(); ++i) {
        rank[order[i]] = int(i);
        vertices[i]    = its.vertices[order[i]];
    }
    for (stl_triangle_vertex_indices &f : its.indices) {
        Vec3i t(rank[f(0)], rank[f(1)], rank[f(2)]);
        // Rotate so that the smallest index comes first; the winding stays the same.
        int first = 0;
        for (int k = 1; k < 3; ++k)
            if (t(k) < t(first))
                first = k;
        f = Vec3i(t(first), t((first + 1) % 3), t((first + 2) % 3));
    }
    std::sort(its.indices.begin(), its.indices.end(), [](const Vec3i &a, const Vec3i &b) {
        return std::tie(a(0), a(1), a(2)) < std::tie(b(0), b(1), b(2));
    });
    its.vertices = std::move(vertices);
}

std::string ordinal(size_t i) { return "vessel " + std::to_string(i); }

} // namespace

std::string validate_rule(const BifurcationRule &rule, int children)
{
    if (children < 2 || children > 3)
        return "A junction needs 2 or 3 children.";
    if (!(rule.murray_exponent > 0.))
        return "The Murray exponent must be larger than 0.";
    if (rule.radius_rule == RadiusRule::Murray && !(rule.flow_split > 0. && rule.flow_split < 1.))
        return "The flow share of the first child must be between 0 and 1.";
    if (rule.radius_rule == RadiusRule::FixedRatio && !(rule.radius_ratio > 0. && rule.radius_ratio <= 1.))
        return "The radius ratio must be above 0 and at most 1.";
    if (rule.angle_rule == AngleRule::Fixed && !(rule.total_angle >= 1. && rule.total_angle <= 178.))
        return "The angle between the children must be between 1 and 178 degrees.";
    if (rule.angle_rule == AngleRule::MurrayOptimal) {
        // The children can only balance the pull of the parent when their cross sections add up to more than its own.
        double sum = 0.;
        if (rule.radius_rule == RadiusRule::Murray) {
            const double f = rule.flow_split;
            sum += std::pow(f, 2. / rule.murray_exponent);
            for (int i = 1; i < children; ++i)
                sum += std::pow((1. - f) / (children - 1), 2. / rule.murray_exponent);
        } else
            sum = children * rule.radius_ratio * rule.radius_ratio;
        if (sum <= 1. + 1e-9)
            return "Murray's optimal angles need children whose cross sections add up to more than the parent's. "
                   "Use Murray's law for the radii, a larger radius ratio, or a fixed angle.";
    }
    return {};
}

JunctionGeometry compute_junction(double parent_radius, int children, const BifurcationRule &rule)
{
    if (children < 2 || children > 3)
        throw InvalidArgument("A junction needs 2 or 3 children.");

    JunctionGeometry g;
    const double     rp = parent_radius;
    g.radii.resize(children);
    g.angles.resize(children);
    g.azimuths.resize(children);
    for (int i = 0; i < children; ++i)
        g.azimuths[i] = 2. * PI * double(i) / children;

    if (rule.radius_rule == RadiusRule::Murray) {
        const double e = std::max(rule.murray_exponent, 1e-3);
        const double f = std::clamp(rule.flow_split, 1e-6, 1. - 1e-6);
        g.radii[0]     = rp * std::pow(f, 1. / e);
        for (int i = 1; i < children; ++i)
            g.radii[i] = rp * std::pow((1. - f) / (children - 1), 1. / e);
    } else
        for (double &r : g.radii)
            r = rp * rule.radius_ratio;

    if (rule.angle_rule == AngleRule::MurrayOptimal) {
        if (children == 2) {
            const double rp4 = std::pow(rp, 4), ra4 = std::pow(g.radii[0], 4), rb4 = std::pow(g.radii[1], 4);
            g.angles[0] = std::acos(std::clamp((rp4 + ra4 - rb4) / (2. * rp * rp * g.radii[0] * g.radii[0]), -1., 1.));
            g.angles[1] = std::acos(std::clamp((rp4 + rb4 - ra4) / (2. * rp * rp * g.radii[1] * g.radii[1]), -1., 1.));
        } else {
            // Every child pulls sideways with the same force F = r^2 sin(angle), which cancels out around the axis,
            // and the pulls along the axis add up to the parent's: sum of r^2 cos(angle) = rp^2. Solved for F.
            std::vector<double> r2(children);
            double              f_max = std::numeric_limits<double>::max();
            for (int i = 0; i < children; ++i) {
                r2[i] = g.radii[i] * g.radii[i];
                f_max = std::min(f_max, r2[i]);
            }
            auto axial = [&](double f) {
                double sum = 0.;
                for (double r : r2)
                    sum += r * std::sqrt(std::max(0., 1. - (f / r) * (f / r)));
                return sum - rp * rp;
            };
            double lo = 0., hi = f_max;
            if (axial(hi) > 0.)
                lo = hi; // even the thinnest child at a right angle cannot balance the parent
            for (int it = 0; it < 200; ++it) {
                const double mid = 0.5 * (lo + hi);
                (axial(mid) > 0. ? lo : hi) = mid;
            }
            const double f = 0.5 * (lo + hi);
            for (int i = 0; i < children; ++i)
                g.angles[i] = std::asin(std::clamp(f / r2[i], 0., 1.));
        }
    } else {
        const double total = Slic3r::Geometry::deg2rad(rule.total_angle);
        if (children == 2) {
            const double sum = g.radii[0] + g.radii[1];
            g.angles[0]      = total * g.radii[1] / sum;
            g.angles[1]      = total * g.radii[0] / sum;
        } else
            for (double &a : g.angles)
                a = 0.5 * total;
    }
    return g;
}

BifurcationGeometry compute_bifurcation(double parent_radius, const BifurcationRule &rule)
{
    const JunctionGeometry g = compute_junction(parent_radius, 2, rule);
    return {g.radii[0], g.radii[1], g.angles[0], g.angles[1]};
}

size_t Skeleton::leaf_count() const
{
    size_t n = 0;
    for (const Branch &b : branches)
        n += b.is_leaf();
    return n;
}

size_t Skeleton::junction_count() const
{
    size_t n = 0;
    for (const Branch &b : branches)
        n += !b.is_leaf();
    return n;
}

std::string validate_skeleton(const Skeleton &skeleton)
{
    const std::vector<Branch> &br = skeleton.branches;
    if (br.empty())
        return "The tree has no vessels.";
    if (br[0].parent != Branch::NONE)
        return "The first vessel is the root and cannot have a parent.";
    for (size_t i = 0; i < br.size(); ++i) {
        const Branch &b = br[i];
        if (b.path.size() < 2)
            return "A vessel needs at least two centreline points (" + ordinal(i) + ").";
        for (const PathPoint &p : b.path)
            if (!(p.radius > 0.))
                return "Every radius must be larger than 0 mm (" + ordinal(i) + ").";
        for (size_t k = 1; k < b.path.size(); ++k)
            if (!((b.path[k].pos - b.path[k - 1].pos).norm() > 1e-6))
                return "Two neighbouring centreline points coincide (" + ordinal(i) + ").";
        if (i > 0) {
            if (b.parent < 0 || b.parent >= int(i))
                return "The parent of " + ordinal(i) + " must come before it.";
            const Branch &p = br[b.parent];
            if (std::find(p.children.begin(), p.children.end(), int(i)) == p.children.end())
                return ordinal(i) + " is missing from the children of its parent.";
            if ((b.start() - p.end()).norm() > 1e-6)
                return ordinal(i) + " does not start where its parent ends.";
            if (angle_between(b.start_direction(), -p.end_direction()) < Slic3r::Geometry::deg2rad(MIN_CHILD_SEPARATION_DEG))
                return ordinal(i) + " runs straight back into its parent.";
        }
        for (size_t k = 0; k < b.children.size(); ++k) {
            const int c = b.children[k];
            if (c <= int(i) || c >= int(br.size()) || br[c].parent != int(i))
                return "The children of " + ordinal(i) + " do not name it as their parent.";
            for (size_t m = 0; m < k; ++m)
                if (angle_between(br[c].start_direction(), br[b.children[m]].start_direction()) < Slic3r::Geometry::deg2rad(MIN_CHILD_SEPARATION_DEG))
                    return "Two children of " + ordinal(i) + " leave it in the same direction.";
        }
    }
    return {};
}

Skeleton make_skeleton(const std::vector<BranchPath> &paths)
{
    if (paths.empty())
        throw InvalidArgument("There are no centrelines.");
    Skeleton sk;
    for (size_t i = 0; i < paths.size(); ++i) {
        const BranchPath &in = paths[i];
        Branch            b;
        b.parent = in.parent;
        for (const PathPoint &p : in.points)
            if (b.path.empty() || (p.pos - b.path.back().pos).norm() > 1e-6)
                b.path.emplace_back(p);
        if (b.path.size() < 2)
            throw InvalidArgument("A vessel needs at least two different centreline points (" + ordinal(i) + ").");
        if (i == 0) {
            if (b.parent != Branch::NONE)
                throw InvalidArgument("The first vessel is the root and cannot have a parent.");
        } else {
            if (b.parent < 0 || b.parent >= int(i))
                throw InvalidArgument("The parent of " + ordinal(i) + " must be a vessel that comes before it.");
            Branch      &p   = sk.branches[b.parent];
            const double gap = (b.path.front().pos - p.end()).norm();
            if (gap > SNAP_DISTANCE)
                throw InvalidArgument(ordinal(i) + " starts " + std::to_string(gap) + " mm away from the end of " + ordinal(b.parent) + ".");
            b.path.front().pos = p.end();
            if ((b.path[1].pos - b.path[0].pos).norm() <= 1e-6)
                throw InvalidArgument("A vessel needs at least two different centreline points (" + ordinal(i) + ").");
            b.depth = p.depth + 1;
            p.children.emplace_back(int(i));
        }
        sk.branches.emplace_back(std::move(b));
    }
    if (std::string err = validate_skeleton(sk); !err.empty())
        throw InvalidArgument(err);
    return sk;
}

std::string validate_tree_params(const TreeParams &params)
{
    if (!(params.root_radius > 0.) || !(params.root_length > 0.))
        return "The root radius and the root length must be larger than 0 mm.";
    if (params.root_direction.norm() < 1e-9)
        return "The root direction must not be zero.";
    if (!(params.min_radius >= 0.))
        return "The smallest radius cannot be negative.";
    if (params.mode == GrowthMode::Branching) {
        if (params.depth < 0 || params.depth > 12)
            return "The number of branching levels must be between 0 and 12.";
        if (!(params.length_ratio > 0.))
            return "The length ratio must be larger than 0.";
        if (params.children < 2 || params.children > 3)
            return "A junction needs 2 or 3 children.";
        if (!(params.taper >= 0.2 && params.taper <= 1.))
            return "The narrowing along a vessel (end radius divided by start radius) must be between 0.2 and 1.";
        if (!(params.angle_jitter >= 0. && params.angle_jitter <= 90.))
            return "The angle variation must be between 0 and 90 degrees.";
        if (!(params.length_jitter >= 0. && params.length_jitter < 1.))
            return "The length variation must be between 0 and less than 1.";
        if (params.depth > 0)
            if (std::string err = validate_rule(params.rule, params.children); !err.empty())
                return err;
    } else {
        if (!(params.fill_step > 0.))
            return "The step length must be larger than 0 mm.";
        if (params.fill_points < 1 || params.fill_points > 100000)
            return "The number of target points must be between 1 and 100000.";
        if (!(params.fill_clearance >= 0.))
            return "The clearance cannot be negative.";
        if (!(params.rule.murray_exponent > 0.))
            return "The Murray exponent must be larger than 0.";
    }
    return {};
}

Skeleton generate_tree(const TreeParams &params, const indexed_triangle_set *bounds)
{
    if (std::string err = validate_tree_params(params); !err.empty())
        throw InvalidArgument(err);

    const bool have_bounds = bounds != nullptr && !bounds->indices.empty();
    if (params.mode == GrowthMode::Fill) {
        if (!have_bounds)
            throw InvalidArgument("Filling needs an object to fill. Choose the object the vessels must stay inside.");
        const Bounds tester(*bounds);
        return Filler(params, tester).run();
    }
    if (have_bounds) {
        const Bounds tester(*bounds);
        return Grower(params, &tester).run();
    }
    return Grower(params, nullptr).run();
}

std::string validate_mesh_params(const Skeleton &skeleton, const VesselMeshParams &params)
{
    if (std::string err = validate_skeleton(skeleton); !err.empty())
        return err;
    if (params.sides < 3 || params.sides > MAX_SIDES)
        return "A cross section needs between 3 and " + std::to_string(MAX_SIDES) + " sides.";
    if (params.cap_rings < 1)
        return "A round cap needs at least 1 ring.";
    if (!(params.junction_margin >= 0.005 && params.junction_margin <= MAX_JUNCTION_SCALE - 1.))
        return "The junction margin must be between 0.005 (0.5 percent) and 0.05 (5 percent).";
    if (!(params.sample_spacing > 0.))
        return "The sample spacing must be larger than 0 mm.";
    if (params.wall_thickness < 0.)
        return "The wall thickness cannot be negative.";
    double min_radius = std::numeric_limits<double>::max();
    for (const Branch &b : skeleton.branches)
        for (const PathPoint &p : b.path)
            min_radius = std::min(min_radius, p.radius);
    if (params.wall_thickness > 0. && params.wall_thickness >= min_radius)
        return "The wall thickness must be smaller than the radius of the thinnest vessel.";

    const MeshPlan plan = plan_tree(skeleton, params, false);
    for (size_t i = 0; i < plan.tubes.size(); ++i) {
        TubeParams tp;
        tp.spline    = false;
        tp.sides     = params.sides;
        tp.cap_rings = params.cap_rings;
        tp.cap_start = plan.tubes[i].cap_start;
        tp.cap_end   = plan.tubes[i].cap_end;
        if (std::string err = validate_tube(plan.tubes[i].samples, tp); !err.empty())
            return ordinal(i) + ": " + err;
    }
    return {};
}

indexed_triangle_set make_tree_mesh(const Skeleton &skeleton, const VesselMeshParams &params)
{
    if (std::string err = validate_mesh_params(skeleton, params); !err.empty())
        throw InvalidArgument(err);

    const bool     hollow = params.wall_thickness > 0.;
    const MeshPlan outer  = plan_tree(skeleton, params, false);
    const MeshPlan lumen  = hollow ? plan_tree(skeleton, params, true) : MeshPlan();

    // Exact coincidences between pieces are what can make the union fail. Each attempt turns the vertices of
    // every piece a little further around its axis, which changes no dimension.
    constexpr int attempts = 3;
    for (int attempt = 0;; ++attempt) {
        try {
            indexed_triangle_set result = combine_meshes(make_pieces(outer, params, attempt),
                                                         hollow ? make_pieces(lumen, params, attempt) : std::vector<indexed_triangle_set>());
            canonicalize(result);
            return result;
        } catch (const RuntimeError &) {
            if (attempt + 1 == attempts)
                throw;
        }
    }
}

Skeleton make_bifurcation_skeleton(const BifurcationParams &params)
{
    const Vec3d dir    = params.parent_direction.normalized();
    Vec3d       normal = params.plane_normal - dir * params.plane_normal.dot(dir);
    normal = normal.norm() < 1e-9 ? any_perpendicular(dir) : Vec3d(normal.normalized());

    const JunctionGeometry g = compute_junction(params.parent_radius, params.children, params.rule);

    Skeleton sk;
    Branch   root;
    root.path = {{params.junction - dir * params.parent_length, params.parent_radius}, {params.junction, params.parent_radius}};
    sk.branches.emplace_back(root);

    for (int i = 0; i < params.children; ++i) {
        const double length = params.child_lengths.empty() ? 10. : params.child_lengths[std::min(size_t(i), params.child_lengths.size() - 1)];
        const Vec3d  axis   = rotate_about(normal, dir, g.azimuths[i]);
        Branch       b;
        b.parent = 0;
        b.depth  = 1;
        b.path   = {{params.junction, g.radii[i]}, {params.junction + rotate_about(dir, axis, g.angles[i]).normalized() * length, g.radii[i]}};
        sk.branches[0].children.emplace_back(int(sk.branches.size()));
        sk.branches.emplace_back(b);
    }
    return sk;
}

indexed_triangle_set make_bifurcation(const BifurcationParams &params, const VesselMeshParams &mesh_params)
{
    return make_tree_mesh(make_bifurcation_skeleton(params), mesh_params);
}

namespace {

const char *radius_rule_name(RadiusRule r) { return r == RadiusRule::Murray ? "murray" : "fixed_ratio"; }
const char *angle_rule_name(AngleRule r) { return r == AngleRule::MurrayOptimal ? "murray_optimal" : "fixed"; }
const char *mode_name(GrowthMode m) { return m == GrowthMode::Fill ? "fill" : "branching"; }

// Shortest text with a decimal point that reads back as exactly the same double.
std::string num(double v)
{
    for (int digits : {15, 16, 17}) {
        std::ostringstream o;
        o.imbue(std::locale::classic());
        o << std::setprecision(digits) << v;
        if (digits == 17 || string_to_double_decimal_point(o.str()) == v)
            return o.str();
    }
    return {};
}

// Reads a number in the format written by num(); false if the text is not a number.
bool read_num(const std::string &text, double &out)
{
    const size_t first = text.find_first_not_of(" \t");
    if (first == std::string::npos || text[first] == '+')
        return false;
    const std::string s   = text.substr(first);
    size_t            pos = 0;
    const double      v   = string_to_double_decimal_point(s, &pos);
    if (pos == 0 || s.find_first_not_of(" \t\r", pos) != std::string::npos || !std::isfinite(v))
        return false;
    out = v;
    return true;
}

std::vector<std::string> split(const std::string &line, char sep)
{
    std::vector<std::string> cells;
    std::string              cell;
    std::istringstream       in(line);
    while (std::getline(in, cell, sep))
        cells.emplace_back(cell);
    return cells;
}

} // namespace

std::string skeleton_to_csv(const Skeleton &skeleton)
{
    std::string out = "branch,parent,depth,point,x_mm,y_mm,z_mm,radius_mm\n";
    for (size_t i = 0; i < skeleton.branches.size(); ++i) {
        const Branch &b = skeleton.branches[i];
        for (size_t point = 0; point < b.path.size(); ++point) {
            const PathPoint &p = b.path[point];
            out += std::to_string(i) + "," + std::to_string(b.parent) + "," + std::to_string(b.depth) + "," + std::to_string(point) + "," +
                   num(p.pos.x()) + "," + num(p.pos.y()) + "," + num(p.pos.z()) + "," + num(p.radius) + "\n";
        }
    }
    return out;
}

Skeleton skeleton_from_csv(const std::string &csv)
{
    std::vector<BranchPath> paths;
    std::istringstream      in(csv);
    std::string             line;
    size_t                  line_no = 0;
    while (std::getline(in, line)) {
        ++line_no;
        if (!line.empty() && line.back() == '\r')
            line.pop_back();
        if (line.find_first_not_of(" \t") == std::string::npos)
            continue;
        const std::vector<std::string> cells = split(line, ',');
        const std::string where = "Line " + std::to_string(line_no) + ": ";
        if (cells.size() < 8)
            throw InvalidArgument(where + "expected 8 values: branch,parent,depth,point,x_mm,y_mm,z_mm,radius_mm.");
        double v[8];
        bool   numbers = true;
        for (int k = 0; k < 8; ++k)
            numbers &= read_num(cells[k], v[k]);
        if (!numbers) {
            if (line_no == 1)
                continue; // the header
            throw InvalidArgument(where + "every value must be a number with a decimal point.");
        }
        const int branch = int(v[0]), parent = int(v[1]);
        if (branch < 0 || branch > int(paths.size()))
            throw InvalidArgument(where + "branch " + std::to_string(branch) + " comes too early, branches must be numbered 0, 1, 2 ... in order.");
        if (branch == int(paths.size())) {
            BranchPath bp;
            bp.parent = parent;
            paths.emplace_back(bp);
        } else if (branch + 1 != int(paths.size()))
            throw InvalidArgument(where + "the points of branch " + std::to_string(branch) + " must be in one block.");
        else if (paths[branch].parent != parent)
            throw InvalidArgument(where + "branch " + std::to_string(branch) + " names different parents.");
        if (!(v[7] > 0.))
            throw InvalidArgument(where + "the radius must be larger than 0 mm.");
        paths[branch].points.push_back({Vec3d(v[4], v[5], v[6]), v[7]});
    }
    if (paths.empty())
        throw InvalidArgument("The table has no points.");
    return make_skeleton(paths);
}

std::string spec_to_text(const VesselSpec &s)
{
    const TreeParams &t = s.tree;
    std::ostringstream o;
    o << "vessel_tree_version = 2\n";
    o << "mode = " << mode_name(t.mode) << "\n";
    o << "root_position = " << num(t.root_position.x()) << " " << num(t.root_position.y()) << " " << num(t.root_position.z()) << "\n";
    o << "root_direction = " << num(t.root_direction.x()) << " " << num(t.root_direction.y()) << " " << num(t.root_direction.z()) << "\n";
    o << "root_radius = " << num(t.root_radius) << "\n";
    o << "root_length = " << num(t.root_length) << "\n";
    o << "depth = " << t.depth << "\n";
    o << "length_ratio = " << num(t.length_ratio) << "\n";
    o << "children = " << t.children << "\n";
    o << "taper = " << num(t.taper) << "\n";
    o << "radius_rule = " << radius_rule_name(t.rule.radius_rule) << "\n";
    o << "murray_exponent = " << num(t.rule.murray_exponent) << "\n";
    o << "flow_split = " << num(t.rule.flow_split) << "\n";
    o << "radius_ratio = " << num(t.rule.radius_ratio) << "\n";
    o << "angle_rule = " << angle_rule_name(t.rule.angle_rule) << "\n";
    o << "total_angle = " << num(t.rule.total_angle) << "\n";
    o << "angle_jitter = " << num(t.angle_jitter) << "\n";
    o << "length_jitter = " << num(t.length_jitter) << "\n";
    o << "min_radius = " << num(t.min_radius) << "\n";
    o << "seed = " << t.seed << "\n";
    o << "fill_step = " << num(t.fill_step) << "\n";
    o << "fill_points = " << t.fill_points << "\n";
    o << "fill_clearance = " << num(t.fill_clearance) << "\n";
    o << "sides = " << s.mesh.sides << "\n";
    o << "wall_thickness = " << num(s.mesh.wall_thickness) << "\n";
    o << "caps = " << (s.mesh.caps == CapStyle::Round ? "round" : "flat") << "\n";
    o << "cap_rings = " << s.mesh.cap_rings << "\n";
    o << "junction_margin = " << num(s.mesh.junction_margin) << "\n";
    o << "spline = " << (s.mesh.spline ? 1 : 0) << "\n";
    o << "sample_spacing = " << num(s.mesh.sample_spacing) << "\n";
    o << "bounds_object = " << s.bounds_object << "\n";
    o << "min_feature_size = " << num(s.min_feature_size) << "\n";
    for (size_t i = 0; i < s.centreline.branches.size(); ++i)
        for (const PathPoint &p : s.centreline.branches[i].path)
            o << "centreline = " << i << " " << s.centreline.branches[i].parent << " " << num(p.pos.x()) << " " << num(p.pos.y()) << " "
              << num(p.pos.z()) << " " << num(p.radius) << "\n";
    return o.str();
}

bool spec_from_text(const std::string &text, VesselSpec &spec)
{
    std::map<std::string, std::string> kv;
    std::vector<std::string>           centreline;
    std::istringstream                 in(text);
    for (std::string line; std::getline(in, line);) {
        const size_t eq = line.find('=');
        if (eq == std::string::npos)
            continue;
        auto trim = [](std::string s) {
            const size_t a = s.find_first_not_of(" \t\r");
            const size_t b = s.find_last_not_of(" \t\r");
            return a == std::string::npos ? std::string() : s.substr(a, b - a + 1);
        };
        const std::string key = trim(line.substr(0, eq)), value = trim(line.substr(eq + 1));
        if (key == "centreline")
            centreline.emplace_back(value);
        else
            kv[key] = value;
    }
    if (kv.find("vessel_tree_version") == kv.end())
        return false;

    // Any value that is present but not a proper number rejects the whole text instead of giving a random value.
    bool valid   = true;
    auto read_int = [&](const std::string &text, int &out) {
        double v = 0.;
        if (!read_num(text, v) || v != std::floor(v) || std::abs(v) > 1e9)
            return false;
        out = int(v);
        return true;
    };
    auto get_double = [&](const char *key, double &out) {
        if (auto it = kv.find(key); it != kv.end() && !read_num(it->second, out))
            valid = false;
    };
    auto get_int = [&](const char *key, int &out) {
        if (auto it = kv.find(key); it != kv.end() && !read_int(it->second, out))
            valid = false;
    };
    auto get_vec = [&](const char *key, Vec3d &out) {
        if (auto it = kv.find(key); it != kv.end()) {
            std::istringstream v(it->second);
            std::string        a, b, c, extra;
            double             x = 0., y = 0., z = 0.;
            if (v >> a >> b >> c && !(v >> extra) && read_num(a, x) && read_num(b, y) && read_num(c, z))
                out = Vec3d(x, y, z);
            else
                valid = false;
        }
    };

    VesselSpec r;
    TreeParams &t = r.tree;
    if (auto it = kv.find("mode"); it != kv.end())
        t.mode = it->second == "fill" ? GrowthMode::Fill : GrowthMode::Branching;
    get_vec("root_position", t.root_position);
    get_vec("root_direction", t.root_direction);
    get_double("root_radius", t.root_radius);
    get_double("root_length", t.root_length);
    get_int("depth", t.depth);
    get_double("length_ratio", t.length_ratio);
    get_int("children", t.children);
    get_double("taper", t.taper);
    get_double("murray_exponent", t.rule.murray_exponent);
    get_double("flow_split", t.rule.flow_split);
    get_double("radius_ratio", t.rule.radius_ratio);
    get_double("total_angle", t.rule.total_angle);
    get_double("angle_jitter", t.angle_jitter);
    get_double("length_jitter", t.length_jitter);
    get_double("min_radius", t.min_radius);
    if (auto it = kv.find("radius_rule"); it != kv.end())
        t.rule.radius_rule = it->second == "fixed_ratio" ? RadiusRule::FixedRatio : RadiusRule::Murray;
    if (auto it = kv.find("angle_rule"); it != kv.end())
        t.rule.angle_rule = it->second == "fixed" ? AngleRule::Fixed : AngleRule::MurrayOptimal;
    if (auto it = kv.find("seed"); it != kv.end()) {
        double seed = 0.;
        if (read_num(it->second, seed) && seed >= 0. && seed <= 4294967295. && seed == std::floor(seed))
            t.seed = uint32_t(seed);
        else
            valid = false;
    }
    get_double("fill_step", t.fill_step);
    get_int("fill_points", t.fill_points);
    get_double("fill_clearance", t.fill_clearance);
    get_int("sides", r.mesh.sides);
    get_double("wall_thickness", r.mesh.wall_thickness);
    get_int("cap_rings", r.mesh.cap_rings);
    get_double("junction_margin", r.mesh.junction_margin);
    get_double("sample_spacing", r.mesh.sample_spacing);
    get_double("min_feature_size", r.min_feature_size);
    if (auto it = kv.find("caps"); it != kv.end())
        r.mesh.caps = it->second == "flat" ? CapStyle::Flat : CapStyle::Round;
    if (auto it = kv.find("spline"); it != kv.end())
        r.mesh.spline = it->second != "0";
    if (auto it = kv.find("bounds_object"); it != kv.end())
        r.bounds_object = it->second;

    if (!valid)
        return false;

    if (!centreline.empty()) {
        std::vector<BranchPath> paths;
        for (const std::string &value : centreline) {
            std::istringstream v(value);
            std::string        f[6], extra;
            int                branch = 0, parent = 0;
            double             x = 0., y = 0., z = 0., radius = 0.;
            if (!(v >> f[0] >> f[1] >> f[2] >> f[3] >> f[4] >> f[5]) || (v >> extra) || !read_int(f[0], branch) ||
                !read_int(f[1], parent) || !read_num(f[2], x) || !read_num(f[3], y) || !read_num(f[4], z) || !read_num(f[5], radius))
                return false;
            if (branch < 0 || branch > int(paths.size()))
                return false;
            if (branch == int(paths.size()))
                paths.push_back({parent, {}});
            paths[branch].points.push_back({Vec3d(x, y, z), radius});
        }
        try {
            r.centreline = make_skeleton(paths);
        } catch (const InvalidArgument &) {
            return false;
        }
    }
    spec = std::move(r);
    return true;
}

}} // namespace Slic3r::vessel
