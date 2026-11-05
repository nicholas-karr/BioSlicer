#include "VesselTree.hpp"

#include <algorithm>
#include <cmath>
#include <map>
#include <memory>
#include <numeric>
#include <sstream>
#include <tuple>

#include "libslic3r/AABBTreeIndirect.hpp"
#include "libslic3r/Exception.hpp"
#include "libslic3r/Geometry.hpp"
#include "libslic3r/MeshBoolean.hpp"
#include "libslic3r/libslic3r.h"

namespace Slic3r { namespace vessel {

namespace {

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

// Inside tests and free-path queries on a closed mesh.
class Bounds
{
public:
    explicit Bounds(const indexed_triangle_set &its)
        : m_its(its), m_tree(AABBTreeIndirect::build_aabb_tree_over_indexed_triangle_set(its.vertices, its.indices))
    {}

    bool inside(const Vec3d &p) const
    {
        // A direction that is unlikely to graze an edge of the mesh.
        const Vec3d                dir = Vec3d(0.5773, 0.3141, 0.7573).normalized();
        std::vector<igl::Hit>      hits;
        AABBTreeIndirect::intersect_ray_all_hits(m_its.vertices, m_its.indices, m_tree, p, dir, hits);
        return hits.size() % 2 == 1;
    }

    // Distance from `start` along `dir` that stays `margin` away from the surface, at most `max_length`.
    double free_length(const Vec3d &start, const Vec3d &dir, double max_length, double margin) const
    {
        igl::Hit hit;
        if (AABBTreeIndirect::intersect_ray_first_hit(m_its.vertices, m_its.indices, m_tree, start, dir, hit)
            && hit.t < max_length + margin)
            return hit.t - margin;
        return max_length;
    }

private:
    const indexed_triangle_set &m_its;
    AABBTreeIndirect::Tree<3, float> m_tree;
};

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

        const int index = int(m_skeleton.branches.size());
        {
            Branch b;
            b.parent = parent;
            b.depth  = depth;
            b.start  = start;
            b.end    = start + dir * length;
            b.radius = radius;
            m_skeleton.branches.emplace_back(b);
        }

        if (depth < m_params.depth) {
            const BifurcationGeometry g = compute_bifurcation(radius, m_params.rule);
            // All random numbers of this node are drawn up front so that their count does not depend on the outcome of the children.
            const double azimuth  = 2. * PI * m_random.unit();
            const double jitter_a = m_random.signed_unit(), jitter_b = m_random.signed_unit();
            const double len_a    = m_random.signed_unit(), len_b = m_random.signed_unit();
            const double jit      = Slic3r::Geometry::deg2rad(m_params.angle_jitter);

            const Vec3d  end    = m_skeleton.branches[index].end;
            const Vec3d  normal = rotate_about(any_perpendicular(dir), dir, azimuth);
            const double base   = nominal_length * m_params.length_ratio;

            int child_a = Branch::NONE, child_b = Branch::NONE;
            if (g.radius_a >= m_params.min_radius)
                child_a = grow(index, end, rotate_about(dir, normal, g.angle_a + jit * jitter_a).normalized(), g.radius_a,
                               base * (1. + m_params.length_jitter * len_a), depth + 1);
            if (g.radius_b >= m_params.min_radius)
                child_b = grow(index, end, rotate_about(dir, normal, -g.angle_b + jit * jitter_b).normalized(), g.radius_b,
                               base * (1. + m_params.length_jitter * len_b), depth + 1);
            m_skeleton.branches[index].child_a = child_a;
            m_skeleton.branches[index].child_b = child_b;
        }
        return index;
    }

    const TreeParams &m_params;
    const Bounds     *m_bounds;
    Random            m_random;
    Skeleton          m_skeleton;
};

using CGALMeshPtr = MeshBoolean::cgal::CGALMeshPtr;

// Union of closed meshes, combined pairwise so that the operands stay small.
CGALMeshPtr union_all(const std::vector<indexed_triangle_set> &pieces)
{
    std::vector<CGALMeshPtr> meshes;
    meshes.reserve(pieces.size());
    for (const indexed_triangle_set &p : pieces)
        meshes.emplace_back(MeshBoolean::cgal::triangle_mesh_to_cgal(p));
    while (meshes.size() > 1) {
        std::vector<CGALMeshPtr> next;
        next.reserve((meshes.size() + 1) / 2);
        for (size_t i = 0; i < meshes.size(); i += 2) {
            if (i + 1 < meshes.size())
                MeshBoolean::cgal::plus(*meshes[i], *meshes[i + 1]);
            next.emplace_back(std::move(meshes[i]));
        }
        meshes = std::move(next);
    }
    return std::move(meshes.front());
}

// One solid piece per branch and per junction. Radii are reduced by `inset`. The root start and the
// leaf ends are moved outwards by `extension` and get `caps`.
std::vector<indexed_triangle_set> make_pieces(const Skeleton &skeleton, const VesselMeshParams &params,
                                              double inset, double extension, CapStyle caps)
{
    std::vector<indexed_triangle_set> pieces;
    for (size_t i = 0; i < skeleton.branches.size(); ++i) {
        const Branch &b      = skeleton.branches[i];
        const Vec3d   dir    = (b.end - b.start).normalized();
        const double  radius = b.radius - inset;

        TubeParams tp;
        tp.spline         = false;
        tp.sample_spacing = 1e9;
        tp.sides          = params.sides;
        tp.cap_rings      = params.cap_rings;
        tp.cap_start      = b.parent == Branch::NONE ? caps : CapStyle::Flat;
        tp.cap_end        = b.is_leaf() ? caps : CapStyle::Flat;

        const Vec3d start = b.parent == Branch::NONE ? Vec3d(b.start - dir * extension) : b.start;
        const Vec3d end   = b.is_leaf() ? Vec3d(b.end + dir * extension) : b.end;
        pieces.emplace_back(make_tube({{start, radius}, {end, radius}}, tp));

        if (!b.is_leaf()) {
            TubeParams ball;
            ball.sides     = params.sides;
            ball.cap_rings = params.cap_rings;
            pieces.emplace_back(make_tube({{b.end, params.joint_scale * radius}}, ball));
        }
    }
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

} // namespace

size_t Skeleton::leaf_count() const
{
    size_t n = 0;
    for (const Branch &b : branches)
        n += b.is_leaf();
    return n;
}

BifurcationGeometry compute_bifurcation(double parent_radius, const BifurcationRule &rule)
{
    BifurcationGeometry g;
    const double        rp = parent_radius;

    if (rule.radius_rule == RadiusRule::Murray) {
        const double e = std::max(rule.murray_exponent, 1e-3);
        const double f = std::clamp(rule.flow_split, 1e-6, 1. - 1e-6);
        g.radius_a     = rp * std::pow(f, 1. / e);
        g.radius_b     = rp * std::pow(1. - f, 1. / e);
    } else {
        g.radius_a = g.radius_b = rp * rule.radius_ratio;
    }

    if (rule.angle_rule == AngleRule::MurrayOptimal) {
        const double rp4 = std::pow(rp, 4), ra4 = std::pow(g.radius_a, 4), rb4 = std::pow(g.radius_b, 4);
        g.angle_a = std::acos(std::clamp((rp4 + ra4 - rb4) / (2. * rp * rp * g.radius_a * g.radius_a), -1., 1.));
        g.angle_b = std::acos(std::clamp((rp4 + rb4 - ra4) / (2. * rp * rp * g.radius_b * g.radius_b), -1., 1.));
    } else {
        const double total = Slic3r::Geometry::deg2rad(rule.total_angle);
        const double sum   = g.radius_a + g.radius_b;
        g.angle_a          = total * g.radius_b / sum;
        g.angle_b          = total * g.radius_a / sum;
    }
    return g;
}

Skeleton generate_tree(const TreeParams &params, const indexed_triangle_set *bounds)
{
    if (!(params.root_radius > 0.) || !(params.root_length > 0.))
        throw InvalidArgument("The root radius and the root length must be larger than 0 mm.");
    if (params.depth < 0 || params.depth > 12)
        throw InvalidArgument("The number of branching levels must be between 0 and 12.");
    if (!(params.length_ratio > 0.))
        throw InvalidArgument("The length ratio must be larger than 0.");
    if (params.root_direction.norm() < 1e-9)
        throw InvalidArgument("The root direction must not be zero.");

    if (bounds != nullptr && !bounds->indices.empty()) {
        const Bounds tester(*bounds);
        return Grower(params, &tester).run();
    }
    return Grower(params, nullptr).run();
}

std::string validate_mesh_params(const Skeleton &skeleton, const VesselMeshParams &params)
{
    if (skeleton.branches.empty())
        return "The tree has no branches.";
    if (params.sides < 3)
        return "A cross section needs at least 3 sides.";
    if (params.cap_rings < 1)
        return "A round cap needs at least 1 ring.";
    if (params.joint_scale < 1.02)
        return "The junction scale must be at least 1.02.";
    if (params.wall_thickness < 0.)
        return "The wall thickness cannot be negative.";
    double min_radius = std::numeric_limits<double>::max();
    for (const Branch &b : skeleton.branches) {
        if (!(b.radius > 0.) || b.length() <= 0.)
            return "Every branch needs a radius and a length larger than 0 mm.";
        min_radius = std::min(min_radius, b.radius);
    }
    if (params.wall_thickness > 0. && params.wall_thickness >= min_radius)
        return "The wall thickness must be smaller than the radius of the thinnest branch.";
    return {};
}

indexed_triangle_set make_tree_mesh(const Skeleton &skeleton, const VesselMeshParams &params)
{
    if (std::string err = validate_mesh_params(skeleton, params); !err.empty())
        throw InvalidArgument(err);

    const bool hollow = params.wall_thickness > 0.;
    CGALMeshPtr outer = union_all(make_pieces(skeleton, params, 0., 0., hollow ? CapStyle::Flat : params.caps));
    if (hollow) {
        // The lumen sticks out of the open ends so that the cut leaves no skin behind.
        CGALMeshPtr lumen = union_all(make_pieces(skeleton, params, params.wall_thickness, params.wall_thickness, CapStyle::Flat));
        MeshBoolean::cgal::minus(*outer, *lumen);
    }
    indexed_triangle_set result = MeshBoolean::cgal::cgal_to_indexed_triangle_set(*outer);
    canonicalize(result);
    return result;
}

Skeleton make_bifurcation_skeleton(const BifurcationParams &params)
{
    const Vec3d  dir = params.parent_direction.normalized();
    Vec3d        normal = params.plane_normal - dir * params.plane_normal.dot(dir);
    normal = normal.norm() < 1e-9 ? any_perpendicular(dir) : Vec3d(normal.normalized());

    const BifurcationGeometry g = compute_bifurcation(params.parent_radius, params.rule);

    Skeleton sk;
    Branch   root;
    root.start  = params.junction - dir * params.parent_length;
    root.end    = params.junction;
    root.radius = params.parent_radius;
    root.child_a = 1;
    root.child_b = 2;
    sk.branches.emplace_back(root);

    auto child = [&](double angle, double length, double radius) {
        Branch b;
        b.parent = 0;
        b.depth  = 1;
        b.start  = params.junction;
        b.end    = params.junction + rotate_about(dir, normal, angle).normalized() * length;
        b.radius = radius;
        sk.branches.emplace_back(b);
    };
    child(g.angle_a, params.child_length_a, g.radius_a);
    child(-g.angle_b, params.child_length_b, g.radius_b);
    return sk;
}

indexed_triangle_set make_bifurcation(const BifurcationParams &params, const VesselMeshParams &mesh_params)
{
    return make_tree_mesh(make_bifurcation_skeleton(params), mesh_params);
}

namespace {

const char *radius_rule_name(RadiusRule r) { return r == RadiusRule::Murray ? "murray" : "fixed_ratio"; }
const char *angle_rule_name(AngleRule r) { return r == AngleRule::MurrayOptimal ? "murray_optimal" : "fixed"; }

std::string num(double v) { return float_to_string_decimal_point(v, 9); }

} // namespace

std::string spec_to_text(const VesselSpec &s)
{
    const TreeParams &t = s.tree;
    std::ostringstream o;
    o << "vessel_tree_version = 1\n";
    o << "root_position = " << num(t.root_position.x()) << " " << num(t.root_position.y()) << " " << num(t.root_position.z()) << "\n";
    o << "root_direction = " << num(t.root_direction.x()) << " " << num(t.root_direction.y()) << " " << num(t.root_direction.z()) << "\n";
    o << "root_radius = " << num(t.root_radius) << "\n";
    o << "root_length = " << num(t.root_length) << "\n";
    o << "depth = " << t.depth << "\n";
    o << "length_ratio = " << num(t.length_ratio) << "\n";
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
    o << "sides = " << s.mesh.sides << "\n";
    o << "wall_thickness = " << num(s.mesh.wall_thickness) << "\n";
    o << "caps = " << (s.mesh.caps == CapStyle::Round ? "round" : "flat") << "\n";
    o << "cap_rings = " << s.mesh.cap_rings << "\n";
    o << "joint_scale = " << num(s.mesh.joint_scale) << "\n";
    o << "bounds_object = " << s.bounds_object << "\n";
    return o.str();
}

bool spec_from_text(const std::string &text, VesselSpec &spec)
{
    std::map<std::string, std::string> kv;
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
        kv[trim(line.substr(0, eq))] = trim(line.substr(eq + 1));
    }
    if (kv.find("vessel_tree_version") == kv.end())
        return false;

    auto get_double = [&](const char *key, double &out) {
        if (auto it = kv.find(key); it != kv.end())
            out = string_to_double_decimal_point(it->second);
    };
    auto get_int = [&](const char *key, int &out) {
        if (auto it = kv.find(key); it != kv.end())
            out = int(string_to_double_decimal_point(it->second));
    };
    auto get_vec = [&](const char *key, Vec3d &out) {
        if (auto it = kv.find(key); it != kv.end()) {
            std::istringstream v(it->second);
            std::string        a, b, c;
            if (v >> a >> b >> c)
                out = Vec3d(string_to_double_decimal_point(a), string_to_double_decimal_point(b), string_to_double_decimal_point(c));
        }
    };

    VesselSpec r;
    TreeParams &t = r.tree;
    get_vec("root_position", t.root_position);
    get_vec("root_direction", t.root_direction);
    get_double("root_radius", t.root_radius);
    get_double("root_length", t.root_length);
    get_int("depth", t.depth);
    get_double("length_ratio", t.length_ratio);
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
    if (auto it = kv.find("seed"); it != kv.end())
        t.seed = uint32_t(std::stoull(it->second));
    get_int("sides", r.mesh.sides);
    get_double("wall_thickness", r.mesh.wall_thickness);
    get_int("cap_rings", r.mesh.cap_rings);
    get_double("joint_scale", r.mesh.joint_scale);
    if (auto it = kv.find("caps"); it != kv.end())
        r.mesh.caps = it->second == "flat" ? CapStyle::Flat : CapStyle::Round;
    if (auto it = kv.find("bounds_object"); it != kv.end())
        r.bounds_object = it->second;
    spec = std::move(r);
    return true;
}

}} // namespace Slic3r::vessel
