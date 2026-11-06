#include "VesselTube.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>
#include <string>
#include <unordered_map>

#include "libslic3r/Exception.hpp"
#include "libslic3r/libslic3r.h"

namespace Slic3r { namespace vessel {

namespace {

constexpr double EPS_DUPLICATE = 1e-9;
// Number of straight pieces used to measure the length of one spline segment.
constexpr int    SPLINE_TABLE_STEPS = 64;

// Centripetal Catmull-Rom spline between p1 (t = 0) and p2 (t = 1).
Vec3d catmull_rom(const Vec3d &p0, const Vec3d &p1, const Vec3d &p2, const Vec3d &p3, double t)
{
    auto next_knot = [](double ti, const Vec3d &a, const Vec3d &b) {
        return ti + std::sqrt(std::max((b - a).norm(), EPS_DUPLICATE));
    };
    const double t0 = 0.;
    const double t1 = next_knot(t0, p0, p1);
    const double t2 = next_knot(t1, p1, p2);
    const double t3 = next_knot(t2, p2, p3);
    const double u  = t1 + t * (t2 - t1);

    const Vec3d a1 = (t1 - u) / (t1 - t0) * p0 + (u - t0) / (t1 - t0) * p1;
    const Vec3d a2 = (t2 - u) / (t2 - t1) * p1 + (u - t1) / (t2 - t1) * p2;
    const Vec3d a3 = (t3 - u) / (t3 - t2) * p2 + (u - t2) / (t3 - t2) * p3;
    const Vec3d b1 = (t2 - u) / (t2 - t0) * a1 + (u - t0) / (t2 - t0) * a2;
    const Vec3d b2 = (t3 - u) / (t3 - t1) * a2 + (u - t1) / (t3 - t1) * a3;
    return (t2 - u) / (t2 - t1) * b1 + (u - t1) / (t2 - t1) * b2;
}

std::vector<PathPoint> remove_duplicates(const std::vector<PathPoint> &in)
{
    std::vector<PathPoint> out;
    out.reserve(in.size());
    for (const PathPoint &p : in)
        if (out.empty() || (p.pos - out.back().pos).norm() > EPS_DUPLICATE)
            out.emplace_back(p);
    return out;
}

// Smallest radius of curvature of the sampled centreline, per sample, as the
// circumradius of each triple of neighbouring samples.
double min_curvature_margin(const std::vector<PathPoint> &pts)
{
    double margin = std::numeric_limits<double>::max();
    for (size_t i = 1; i + 1 < pts.size(); ++i) {
        const Vec3d  a = pts[i - 1].pos, b = pts[i].pos, c = pts[i + 1].pos;
        const double cross = ((b - a).cross(c - b)).norm();
        if (cross < 1e-12)
            continue;
        const double curvature_radius = (b - a).norm() * (c - b).norm() * (c - a).norm() / (2. * cross);
        margin = std::min(margin, curvature_radius / pts[i].radius);
    }
    return margin;
}

// How far the mitre joint at an interior point of a polyline reaches along the axis beyond the
// perpendicular cross section: the tube radius times the tangent of half the turn angle.
double mitre_extent(const std::vector<PathPoint> &pts, size_t i)
{
    if (i == 0 || i + 1 >= pts.size())
        return 0.;
    const Vec3d  t_in  = (pts[i].pos - pts[i - 1].pos).normalized();
    const Vec3d  t_out = (pts[i + 1].pos - pts[i].pos).normalized();
    const double c     = std::clamp(t_in.dot(t_out), -1., 1.);
    // tan(theta / 2) = sqrt((1 - cos) / (1 + cos))
    if (1. + c < 1e-12)
        return std::numeric_limits<double>::infinity();
    return pts[i].radius * std::sqrt((1. - c) / (1. + c));
}

// Index of the first straight piece of a polyline whose two mitre joints cut into each other, or -1.
int mitre_conflict(const std::vector<PathPoint> &pts)
{
    for (size_t i = 0; i + 1 < pts.size(); ++i)
        if (mitre_extent(pts, i) + mitre_extent(pts, i + 1) >= (pts[i + 1].pos - pts[i].pos).norm() * (1. - 1e-9))
            return int(i);
    return -1;
}

struct Ring
{
    Vec3d  center;
    Vec3d  normal;
    Vec3d  binormal;
    double a; // semi-axis along normal
    double b; // semi-axis along binormal
    // Mitre joint: the section is stretched by `stretch` along the unit vector `stretch_dir`,
    // which lies in the plane of the ring. 1 for a section perpendicular to a straight piece.
    Vec3d  stretch_dir;
    double stretch;
};

Vec3d initial_normal(const Vec3d &tangent)
{
    Vec3d axis = Vec3d::UnitX();
    if (std::abs(tangent.y()) < std::abs(tangent.x()) && std::abs(tangent.y()) <= std::abs(tangent.z()))
        axis = Vec3d::UnitY();
    else if (std::abs(tangent.z()) < std::abs(tangent.x()) && std::abs(tangent.z()) < std::abs(tangent.y()))
        axis = Vec3d::UnitZ();
    return tangent.cross(axis).normalized();
}

// Rotation minimising frames along the centreline (double reflection method).
void compute_frames(const std::vector<PathPoint> &pts, std::vector<Vec3d> &tangents, std::vector<Vec3d> &normals)
{
    const size_t n = pts.size();
    tangents.assign(n, Vec3d::UnitZ());
    normals.assign(n, Vec3d::UnitX());
    if (n == 1)
        return;

    for (size_t i = 0; i < n; ++i) {
        Vec3d t;
        if (i == 0)
            t = pts[1].pos - pts[0].pos;
        else if (i + 1 == n)
            t = pts[i].pos - pts[i - 1].pos;
        else
            t = (pts[i].pos - pts[i - 1].pos).normalized() + (pts[i + 1].pos - pts[i].pos).normalized();
        tangents[i] = t.normalized();
    }

    normals[0] = initial_normal(tangents[0]);
    for (size_t i = 0; i + 1 < n; ++i) {
        const Vec3d  v1 = pts[i + 1].pos - pts[i].pos;
        const double c1 = v1.dot(v1);
        Vec3d        r  = normals[i];
        Vec3d        tl = tangents[i];
        if (c1 > 0.) {
            r  = r - (2. / c1) * v1.dot(r) * v1;
            tl = tl - (2. / c1) * v1.dot(tl) * v1;
        }
        const Vec3d  v2 = tangents[i + 1] - tl;
        const double c2 = v2.dot(v2);
        if (c2 > 1e-24)
            r = r - (2. / c2) * v2.dot(r) * v2;
        // Remove any drift from the tangent direction.
        r = r - r.dot(tangents[i + 1]) * tangents[i + 1];
        normals[i + 1] = r.normalized();
    }
}

// Stretch of the cross section at an interior point so that it becomes the mitre cut of the two straight
// pieces that meet there: the circle in the bisecting plane becomes the ellipse in which both pieces cut
// that plane, with the vertices on the generators of the pieces.
void mitre_stretch(const std::vector<PathPoint> &pts, size_t i, Vec3d &dir, double &stretch)
{
    dir     = Vec3d::UnitX();
    stretch = 1.;
    if (i == 0 || i + 1 >= pts.size())
        return;
    const Vec3d t_in  = (pts[i].pos - pts[i - 1].pos).normalized();
    const Vec3d t_out = (pts[i + 1].pos - pts[i].pos).normalized();
    const Vec3d u     = t_in - t_out;
    const double half_cos = 0.5 * (t_in + t_out).norm();
    if (u.norm() < 1e-9 || half_cos < 1e-9)
        return;
    dir     = u.normalized();
    stretch = 1. / half_cos;
}

} // namespace

double polyline_length(const std::vector<PathPoint> &points)
{
    double len = 0.;
    for (size_t i = 1; i < points.size(); ++i)
        len += (points[i].pos - points[i - 1].pos).norm();
    return len;
}

std::vector<PathPoint> sample_centreline(const std::vector<PathPoint> &control_in, bool spline, double spacing)
{
    const std::vector<PathPoint> ctrl = remove_duplicates(control_in);
    if (ctrl.size() < 2 || spacing <= 0.)
        return ctrl;

    const size_t           n = ctrl.size();
    std::vector<PathPoint> out;

    for (size_t i = 0; i + 1 < n; ++i) {
        const Vec3d &p1 = ctrl[i].pos;
        const Vec3d &p2 = ctrl[i + 1].pos;
        const Vec3d  p0 = i > 0 ? ctrl[i - 1].pos : Vec3d(2. * p1 - p2);
        const Vec3d  p3 = i + 2 < n ? ctrl[i + 2].pos : Vec3d(2. * p2 - p1);

        auto eval = [&](double t) { return spline ? catmull_rom(p0, p1, p2, p3, t) : Vec3d(p1 + t * (p2 - p1)); };

        // Table of cumulative arc length against the spline parameter.
        std::vector<double> cumulative(SPLINE_TABLE_STEPS + 1, 0.);
        Vec3d               prev = p1;
        for (int k = 1; k <= SPLINE_TABLE_STEPS; ++k) {
            const Vec3d q = eval(double(k) / SPLINE_TABLE_STEPS);
            cumulative[k] = cumulative[k - 1] + (q - prev).norm();
            prev          = q;
        }
        const double length = cumulative.back();
        const int    count  = std::max(1, int(std::ceil(length / spacing - 1e-9)));

        for (int m = 0; m < count; ++m) {
            const double s = length * double(m) / count;
            double       t = 0.;
            if (m > 0) {
                const size_t k = std::upper_bound(cumulative.begin(), cumulative.end(), s) - cumulative.begin() - 1;
                const double f = (s - cumulative[k]) / std::max(cumulative[k + 1] - cumulative[k], EPS_DUPLICATE);
                t              = (double(k) + f) / SPLINE_TABLE_STEPS;
            }
            const double w = length > 0. ? s / length : 0.;
            out.push_back({eval(t), (1. - w) * ctrl[i].radius + w * ctrl[i + 1].radius});
        }
    }
    out.push_back(ctrl.back());
    return out;
}

std::string validate_tube(const std::vector<PathPoint> &control, const TubeParams &params)
{
    if (control.empty())
        return "The path has no points.";
    double min_radius = std::numeric_limits<double>::max();
    for (const PathPoint &p : control) {
        if (!(p.radius > 0.))
            return "Every radius must be larger than 0 mm.";
        min_radius = std::min(min_radius, p.radius);
    }
    if (params.sides < 3)
        return "A cross section needs at least 3 sides.";
    if (!(params.aspect > 0.) || params.aspect > 1.)
        return "The aspect ratio (minor axis divided by major axis) must be in the range above 0 and up to 1.";
    if (!(params.sample_spacing > 0.))
        return "The sample spacing must be larger than 0 mm.";
    if (params.cap_rings < 1)
        return "A round cap needs at least 1 ring.";
    if (params.wall_thickness < 0.)
        return "The wall thickness cannot be negative.";
    if (params.wall_thickness > 0. && params.wall_thickness >= min_radius * params.aspect)
        return "The wall thickness must be smaller than the smallest radius (the minor radius for an elliptical section).";

    if (control.size() > 1) {
        if (params.spline) {
            const std::vector<PathPoint> pts = sample_centreline(control, true, params.sample_spacing);
            if (pts.size() < 2)
                return "The path points coincide, the path has no length.";
            if (min_curvature_margin(pts) < 1.)
                return "The path bends more sharply than the tube radius allows. Use a smaller radius or a gentler path.";
        } else {
            const std::vector<PathPoint> pts = remove_duplicates(control);
            if (pts.size() < 2)
                return "The path points coincide, the path has no length.";
            if (const int piece = mitre_conflict(pts); piece >= 0)
                return "The path turns more sharply than the tube radius allows between points " + std::to_string(piece + 1) + " and " +
                       std::to_string(piece + 2) + ". Use a smaller radius, a gentler turn or points further apart.";
        }
    }
    return {};
}

indexed_triangle_set make_tube(const std::vector<PathPoint> &control, const TubeParams &params_in)
{
    if (std::string err = validate_tube(control, params_in); !err.empty())
        throw InvalidArgument(err);

    TubeParams params = params_in;
    const bool ball   = control.size() == 1;
    if (ball) {
        params.cap_start = params.cap_end = CapStyle::Round;
        params.wall_thickness = 0.;
    }

    const std::vector<PathPoint> pts = ball ? control :
                                       params.spline ? sample_centreline(control, true, params.sample_spacing) : remove_duplicates(control);
    std::vector<Vec3d> tangents, normals;
    compute_frames(pts, tangents, normals);

    const bool hollow = params.wall_thickness > 0.;
    const int  sides  = params.sides;

    // Cross sections from the start to the end of the tube, offset inwards by `inset`.
    auto make_rings = [&](double inset, bool with_caps) {
        std::vector<Ring> rings;
        auto body_ring = [&](size_t i, const Vec3d &center, double scale) {
            const double a = pts[i].radius - inset;
            const double b = pts[i].radius * params.aspect - inset;
            Ring         r{center, normals[i], tangents[i].cross(normals[i]), a * scale, b * scale, Vec3d::UnitX(), 1.};
            if (!params.spline)
                mitre_stretch(pts, i, r.stretch_dir, r.stretch);
            return r;
        };
        const size_t last = pts.size() - 1;
        if (with_caps && params.cap_start == CapStyle::Round)
            for (int k = params.cap_rings - 1; k >= 1; --k) {
                const double phi = 0.5 * PI * double(k) / params.cap_rings;
                rings.push_back(body_ring(0, pts[0].pos - tangents[0] * pts[0].radius * std::sin(phi), std::cos(phi)));
            }
        for (size_t i = 0; i < pts.size(); ++i)
            rings.push_back(body_ring(i, pts[i].pos, 1.));
        if (with_caps && params.cap_end == CapStyle::Round)
            for (int k = 1; k < params.cap_rings; ++k) {
                const double phi = 0.5 * PI * double(k) / params.cap_rings;
                rings.push_back(body_ring(last, pts[last].pos + tangents[last] * pts[last].radius * std::sin(phi), std::cos(phi)));
            }
        return rings;
    };

    indexed_triangle_set its;
    auto add_rings = [&](const std::vector<Ring> &rings) {
        const int first = int(its.vertices.size());
        for (const Ring &r : rings)
            for (int j = 0; j < sides; ++j) {
                const double th = 2. * PI * double(j) / sides + params.ring_offset;
                Vec3d        v  = r.a * std::cos(th) * r.normal + r.b * std::sin(th) * r.binormal;
                if (r.stretch != 1.)
                    v += (r.stretch - 1.) * v.dot(r.stretch_dir) * r.stretch_dir;
                its.vertices.emplace_back((r.center + v).cast<float>());
            }
        return first;
    };
    // Quads between consecutive rings. Rings are outward facing unless `reversed`.
    auto add_strip = [&](int first, size_t ring_count, bool reversed) {
        for (size_t i = 0; i + 1 < ring_count; ++i)
            for (int j = 0; j < sides; ++j) {
                const int j1  = (j + 1) % sides;
                const int v00 = first + int(i) * sides + j, v01 = first + int(i) * sides + j1;
                const int v10 = first + int(i + 1) * sides + j, v11 = first + int(i + 1) * sides + j1;
                if (!reversed) {
                    its.indices.emplace_back(v00, v01, v10);
                    its.indices.emplace_back(v01, v11, v10);
                } else {
                    its.indices.emplace_back(v00, v10, v01);
                    its.indices.emplace_back(v01, v10, v11);
                }
            }
    };

    const std::vector<Ring> outer = make_rings(0., !hollow);
    const int               o0    = add_rings(outer);
    add_strip(o0, outer.size(), false);

    if (!hollow) {
        const size_t last = pts.size() - 1;
        const int    rows = int(outer.size());
        auto add_fan = [&](const Vec3d &apex, int ring_first, bool start) {
            const int c = int(its.vertices.size());
            its.vertices.emplace_back(apex.cast<float>());
            for (int j = 0; j < sides; ++j) {
                const int a = ring_first + j, b = ring_first + (j + 1) % sides;
                if (start)
                    its.indices.emplace_back(c, b, a);
                else
                    its.indices.emplace_back(c, a, b);
            }
        };
        const Vec3d start_apex = params.cap_start == CapStyle::Round ? Vec3d(pts[0].pos - tangents[0] * pts[0].radius) : pts[0].pos;
        const Vec3d end_apex   = params.cap_end == CapStyle::Round ? Vec3d(pts[last].pos + tangents[last] * pts[last].radius) : pts[last].pos;
        add_fan(start_apex, o0, true);
        add_fan(end_apex, o0 + (rows - 1) * sides, false);
    } else {
        const std::vector<Ring> inner = make_rings(params.wall_thickness, false);
        const int               i0    = add_rings(inner);
        add_strip(i0, inner.size(), true);
        // Flat annular ends join the outer and the inner surface.
        const int rows = int(outer.size());
        for (int j = 0; j < sides; ++j) {
            const int j1 = (j + 1) % sides;
            const int os = o0 + j, os1 = o0 + j1, is = i0 + j, is1 = i0 + j1;
            const int oe = o0 + (rows - 1) * sides + j, oe1 = o0 + (rows - 1) * sides + j1;
            const int ie = i0 + (rows - 1) * sides + j, ie1 = i0 + (rows - 1) * sides + j1;
            its.indices.emplace_back(os, is, os1);
            its.indices.emplace_back(os1, is, is1);
            its.indices.emplace_back(oe, oe1, ie);
            its.indices.emplace_back(oe1, ie1, ie);
        }
    }
    return its;
}

double mesh_volume(const indexed_triangle_set &its)
{
    double v = 0.;
    for (const stl_triangle_vertex_indices &f : its.indices) {
        const Vec3d a = its.vertices[f(0)].cast<double>(), b = its.vertices[f(1)].cast<double>(), c = its.vertices[f(2)].cast<double>();
        v += a.dot(b.cross(c));
    }
    return v / 6.;
}

MeshCheck check_mesh(const indexed_triangle_set &its)
{
    MeshCheck                                  res;
    std::unordered_map<uint64_t, unsigned int> edges;
    edges.reserve(its.indices.size() * 3);
    auto key = [](int a, int b) { return (uint64_t(uint32_t(a)) << 32) | uint64_t(uint32_t(b)); };

    for (const stl_triangle_vertex_indices &f : its.indices) {
        for (int k = 0; k < 3; ++k)
            ++edges[key(f(k), f((k + 1) % 3))];
        const Vec3d a = its.vertices[f(0)].cast<double>(), b = its.vertices[f(1)].cast<double>(), c = its.vertices[f(2)].cast<double>();
        if ((b - a).cross(c - a).norm() < 1e-12)
            ++res.degenerate_faces;
    }
    for (const auto &[k, count] : edges) {
        if (count > 1)
            res.nonmanifold_edges += count - 1;
        const uint64_t reverse = (k << 32) | (k >> 32);
        if (edges.find(reverse) == edges.end())
            res.open_edges += count;
    }
    res.volume = mesh_volume(its);
    return res;
}

}} // namespace Slic3r::vessel
