#include "MeshPrimitives.hpp"

#include <algorithm>
#include <cmath>
#include <utility>

#include "libslic3r/I18N.hpp"
#include <libslic3r/libslic3r.h>

namespace Slic3r {

namespace {

const std::vector<PrimitiveParamInfo> &torus_info()
{
    static const std::vector<PrimitiveParamInfo> v = {
        {"major_radius", L("Ring radius (centre of tube to centre of hole)"), "mm", 15., 0.01, 10000., false},
        {"minor_radius", L("Tube radius"), "mm", 4.,  0.01, 10000., false},
        {"major_segments", L("Segments around the ring"), "", 64., 3.,   2048.,  true},
        {"minor_segments", L("Segments around the tube"), "", 32., 3.,   2048.,  true}};
    return v;
}

const std::vector<PrimitiveParamInfo> &ellipsoid_info()
{
    static const std::vector<PrimitiveParamInfo> v = {
        {"radius_x", L("Radius along X"), "mm", 10., 0.01, 10000., false},
        {"radius_y", L("Radius along Y"), "mm", 8.,  0.01, 10000., false},
        {"radius_z", L("Radius along Z"), "mm", 6.,  0.01, 10000., false},
        {"segments", L("Segments around"), "", 64., 3.,   2048.,  true},
        {"rings", L("Rings pole to pole"), "", 32., 2.,   2048.,  true}};
    return v;
}

const std::vector<PrimitiveParamInfo> &capsule_info()
{
    static const std::vector<PrimitiveParamInfo> v = {
        {"radius", L("Radius"), "mm", 5.,  0.01, 10000., false},
        {"length", L("Straight section length"), "mm", 20., 0.,   10000., false},
        {"segments", L("Segments around"), "", 64., 3.,   2048.,  true},
        {"rings", L("Rings per rounded end"), "", 16., 1.,   1024.,  true}};
    return v;
}

const std::vector<PrimitiveParamInfo> &frustum_info()
{
    static const std::vector<PrimitiveParamInfo> v = {
        {"bottom_radius", L("Bottom radius"), "mm", 10., 0., 10000., false},
        {"top_radius", L("Top radius"), "mm", 5.,  0., 10000., false},
        {"height", L("Height"), "mm", 20., 0.01, 10000., false},
        {"segments", L("Segments around"), "", 64., 3., 2048., true}};
    return v;
}

const std::vector<PrimitiveParamInfo> &tube_info()
{
    static const std::vector<PrimitiveParamInfo> v = {
        {"outer_radius", L("Outer radius"), "mm", 10., 0.02, 10000., false},
        {"inner_radius", L("Inner radius"), "mm", 8.,  0.01, 10000., false},
        {"height", L("Height"), "mm", 30., 0.01, 10000., false},
        {"segments", L("Segments around"), "", 64., 3.,   2048.,  true}};
    return v;
}

const std::vector<PrimitiveParamInfo> &wedge_info()
{
    static const std::vector<PrimitiveParamInfo> v = {
        {"length", L("Length (X)"), "mm", 20., 0.01, 10000., false},
        {"width", L("Width (Y)"), "mm", 10., 0.01, 10000., false},
        {"height_high", L("Height at the tall end"), "mm", 10., 0.01, 10000., false},
        {"height_low", L("Height at the low end (0 = ramp)"), "mm", 0.,  0.,   10000., false}};
    return v;
}

// 1 - sin(a) / a with a = 2 pi / n. Relative area lost by a regular n-gon inscribed in a circle,
// normalised to the polygon having the same radius.
double polygon_deficit(double n)
{
    const double a = 2. * PI / n;
    return 1. - std::sin(a) / a;
}

// Area of a regular n-gon with circumradius 1 divided by pi.
double polygon_area_factor(double n)
{
    return 0.5 * n * std::sin(2. * PI / n) / PI;
}

std::string format_number(double v)
{
    std::string s = std::to_string(v);
    s.erase(s.find_last_not_of('0') + 1);
    if (!s.empty() && s.back() == '.')
        s.pop_back();
    return s;
}

using Profile = std::vector<std::pair<double, double>>; // (radius, z), bottom to top

// Volume of the solid obtained by revolving the polyline around Z with exact circles.
double profile_volume(const Profile &p)
{
    double v = 0.;
    for (size_t i = 0; i + 1 < p.size(); ++i) {
        const double r1 = p[i].first, r2 = p[i + 1].first, h = p[i + 1].second - p[i].second;
        v += PI * h / 3. * (r1 * r1 + r1 * r2 + r2 * r2);
    }
    return v;
}

Profile sphere_profile(size_t rings)
{
    Profile p;
    for (size_t j = 0; j <= rings; ++j) {
        const double t = PI * double(j) / double(rings);
        p.emplace_back(j == 0 || j == rings ? 0. : std::sin(t), -std::cos(t));
    }
    return p;
}

Profile capsule_profile(double r, double len, size_t rings)
{
    Profile p;
    for (size_t j = 0; j <= rings; ++j) {
        const double a = 0.5 * PI * double(j) / double(rings);
        p.emplace_back(j == 0 ? 0. : r * std::sin(a), -0.5 * len - r * std::cos(a));
    }
    for (size_t j = rings + 1; j-- > 0;) {
        const double a = 0.5 * PI * double(j) / double(rings);
        p.emplace_back(j == 0 ? 0. : r * std::sin(a), 0.5 * len + r * std::cos(a));
    }
    // Remove the duplicated pair when the straight section has no length.
    if (len <= 0.) {
        Profile q;
        for (const auto &pt : p)
            if (q.empty() || std::abs(q.back().first - pt.first) > 1e-12 || std::abs(q.back().second - pt.second) > 1e-12)
                q.push_back(pt);
        p = std::move(q);
    }
    return p;
}

// Revolve a profile around Z. A profile end with radius 0 becomes a pole, any other end
// gets a flat cap. Triangles face outward.
indexed_triangle_set revolve(const Profile &prof, int n)
{
    indexed_triangle_set its;
    std::vector<std::vector<int>> rings;
    for (const auto &pt : prof) {
        std::vector<int> ids;
        if (pt.first <= 0.) {
            ids.push_back(int(its.vertices.size()));
            its.vertices.emplace_back(0.f, 0.f, float(pt.second));
        } else {
            for (int i = 0; i < n; ++i) {
                const double a = 2. * PI * double(i) / double(n);
                ids.push_back(int(its.vertices.size()));
                its.vertices.emplace_back(float(pt.first * std::cos(a)), float(pt.first * std::sin(a)), float(pt.second));
            }
        }
        rings.push_back(std::move(ids));
    }

    for (size_t k = 0; k + 1 < rings.size(); ++k) {
        const auto &lo = rings[k];
        const auto &hi = rings[k + 1];
        for (int i = 0; i < n; ++i) {
            const int j = (i + 1) % n;
            if (lo.size() == 1) {
                its.indices.emplace_back(lo[0], hi[j], hi[i]);
            } else if (hi.size() == 1) {
                its.indices.emplace_back(lo[i], lo[j], hi[0]);
            } else {
                its.indices.emplace_back(lo[i], lo[j], hi[i]);
                its.indices.emplace_back(lo[j], hi[j], hi[i]);
            }
        }
    }

    // Flat caps for ends that are not poles.
    if (rings.front().size() > 1) {
        const int c = int(its.vertices.size());
        its.vertices.emplace_back(0.f, 0.f, float(prof.front().second));
        for (int i = 0; i < n; ++i)
            its.indices.emplace_back(c, rings.front()[(i + 1) % n], rings.front()[i]);
    }
    if (rings.back().size() > 1) {
        const int c = int(its.vertices.size());
        its.vertices.emplace_back(0.f, 0.f, float(prof.back().second));
        for (int i = 0; i < n; ++i)
            its.indices.emplace_back(c, rings.back()[i], rings.back()[(i + 1) % n]);
    }
    return its;
}

indexed_triangle_set make_torus(double R, double r, int n, int m)
{
    indexed_triangle_set its;
    its.vertices.reserve(size_t(n) * m);
    for (int i = 0; i < n; ++i) {
        const double t = 2. * PI * double(i) / double(n);
        for (int j = 0; j < m; ++j) {
            const double p = 2. * PI * double(j) / double(m);
            const double rho = R + r * std::cos(p);
            its.vertices.emplace_back(float(rho * std::cos(t)), float(rho * std::sin(t)), float(r * std::sin(p)));
        }
    }
    auto id = [n, m](int i, int j) { return (i % n) * m + (j % m); };
    for (int i = 0; i < n; ++i)
        for (int j = 0; j < m; ++j) {
            its.indices.emplace_back(id(i, j), id(i + 1, j), id(i, j + 1));
            its.indices.emplace_back(id(i + 1, j), id(i + 1, j + 1), id(i, j + 1));
        }
    return its;
}

indexed_triangle_set make_tube(double ro, double ri, double h, int n)
{
    indexed_triangle_set its;
    for (double r : {ro, ri})
        for (double z : {0., h})
            for (int i = 0; i < n; ++i) {
                const double a = 2. * PI * double(i) / double(n);
                its.vertices.emplace_back(float(r * std::cos(a)), float(r * std::sin(a)), float(z));
            }
    // Ring ids: outer bottom 0, outer top 1, inner bottom 2, inner top 3.
    auto id = [n](int ring, int i) { return ring * n + (i % n); };
    for (int i = 0; i < n; ++i) {
        // Outer wall faces outward.
        its.indices.emplace_back(id(0, i), id(0, i + 1), id(1, i));
        its.indices.emplace_back(id(0, i + 1), id(1, i + 1), id(1, i));
        // Inner wall faces the axis.
        its.indices.emplace_back(id(2, i), id(3, i), id(2, i + 1));
        its.indices.emplace_back(id(2, i + 1), id(3, i), id(3, i + 1));
        // Bottom annulus faces down.
        its.indices.emplace_back(id(0, i), id(2, i), id(2, i + 1));
        its.indices.emplace_back(id(0, i), id(2, i + 1), id(0, i + 1));
        // Top annulus faces up.
        its.indices.emplace_back(id(1, i), id(1, i + 1), id(3, i + 1));
        its.indices.emplace_back(id(1, i), id(3, i + 1), id(3, i));
    }
    return its;
}

indexed_triangle_set make_wedge(double L, double W, double hh, double hl)
{
    indexed_triangle_set its;
    const float x0 = float(-0.5 * L), x1 = float(0.5 * L), y0 = float(-0.5 * W), y1 = float(0.5 * W);
    its.vertices = {{x0, y0, 0.f}, {x1, y0, 0.f}, {x1, y1, 0.f}, {x0, y1, 0.f}, // A B C D
                    {x1, y0, float(hh)}, {x1, y1, float(hh)}};               // B' C'
    int A = 0, B = 1, C = 2, D = 3, Bt = 4, Ct = 5, At = 0, Dt = 3;
    if (hl > 0.) {
        At = int(its.vertices.size());
        its.vertices.emplace_back(x0, y0, float(hl));
        Dt = int(its.vertices.size());
        its.vertices.emplace_back(x0, y1, float(hl));
    }
    auto tri = [&its](int a, int b, int c) {
        if (a != b && b != c && a != c)
            its.indices.emplace_back(a, b, c);
    };
    tri(A, D, C);   tri(A, C, B);        // bottom
    tri(At, Bt, Ct); tri(At, Ct, Dt);    // top
    tri(A, B, Bt);  tri(A, Bt, At);      // y = -W/2
    tri(D, Ct, C);  tri(D, Dt, Ct);      // y = +W/2
    tri(A, Dt, D);  tri(A, At, Dt);      // x = -L/2
    tri(B, C, Ct);  tri(B, Ct, Bt);      // x = +L/2
    return its;
}

double ellipsoid_deficit(size_t rings, double n)
{
    const Profile p = sphere_profile(rings);
    const double  v = profile_volume(p) / (4. / 3. * PI);
    return 1. - polygon_area_factor(n) * v;
}

} // namespace

const char *primitive_name(PrimitiveType type)
{
    switch (type) {
    case PrimitiveType::Torus:     return L("Torus");
    case PrimitiveType::Ellipsoid: return L("Ellipsoid");
    case PrimitiveType::Capsule:   return L("Capsule");
    case PrimitiveType::Frustum:   return L("Cone / Frustum");
    case PrimitiveType::Tube:      return L("Tube");
    case PrimitiveType::Wedge:     return L("Wedge");
    default:                       return "";
    }
}

const std::vector<PrimitiveParamInfo> &primitive_param_info(PrimitiveType type)
{
    switch (type) {
    case PrimitiveType::Torus:     return torus_info();
    case PrimitiveType::Ellipsoid: return ellipsoid_info();
    case PrimitiveType::Capsule:   return capsule_info();
    case PrimitiveType::Frustum:   return frustum_info();
    case PrimitiveType::Tube:      return tube_info();
    default:                       return wedge_info();
    }
}

std::vector<double> primitive_default_params(PrimitiveType type)
{
    std::vector<double> ret;
    for (const PrimitiveParamInfo &i : primitive_param_info(type))
        ret.push_back(i.def);
    return ret;
}

size_t primitive_triangle_count(PrimitiveType type, const std::vector<double> &p)
{
    auto u = [&p](size_t i) { return size_t(p[i]); };
    switch (type) {
    case PrimitiveType::Torus:     return 2 * u(2) * u(3);
    case PrimitiveType::Ellipsoid: return 2 * u(3) * (u(4) - 1);
    case PrimitiveType::Capsule:   return 4 * u(2) * u(3) - (p[1] > 0. ? 0 : 2 * u(2));
    case PrimitiveType::Frustum:   return (p[0] > 0. && p[1] > 0.) ? 4 * u(3) : 2 * u(3);
    case PrimitiveType::Tube:      return 8 * u(3);
    default:                       return p[3] > 0. ? 12 : 8;
    }
}

std::string primitive_validate(PrimitiveType type, const std::vector<double> &p)
{
    const auto &info = primitive_param_info(type);
    if (p.size() != info.size())
        return L("Wrong number of parameters.");
    for (size_t i = 0; i < p.size(); ++i) {
        if (!std::isfinite(p[i]) || p[i] < info[i].min || p[i] > info[i].max)
            return std::string(info[i].label) + " must be between " + format_number(info[i].min) + " and " +
                   format_number(info[i].max) + ".";
        if (info[i].integer && p[i] != std::floor(p[i]))
            return std::string(info[i].label) + " must be a whole number.";
    }
    if (primitive_triangle_count(type, p) > primitive_max_triangles)
        return L("Too many segments: the shape would have more than 2 million triangles.");
    switch (type) {
    case PrimitiveType::Torus:
        if (p[1] >= p[0])
            return L("The tube radius must be smaller than the ring radius.");
        break;
    case PrimitiveType::Frustum:
        if (p[0] <= 0. && p[1] <= 0.)
            return L("At least one radius must be larger than 0.");
        break;
    case PrimitiveType::Tube:
        if (p[1] >= p[0])
            return L("The inner radius must be smaller than the outer radius.");
        break;
    case PrimitiveType::Wedge:
        if (p[3] > p[2])
            return L("The low end cannot be taller than the high end.");
        break;
    default: break;
    }
    return {};
}

indexed_triangle_set make_primitive(PrimitiveType type, const std::vector<double> &p)
{
    if (!primitive_validate(type, p).empty())
        return {};
    switch (type) {
    case PrimitiveType::Torus:
        return make_torus(p[0], p[1], int(p[2]), int(p[3]));
    case PrimitiveType::Ellipsoid: {
        indexed_triangle_set its = revolve(sphere_profile(size_t(p[4])), int(p[3]));
        for (Vec3f &v : its.vertices)
            v = Vec3f(float(v.x() * p[0]), float(v.y() * p[1]), float(v.z() * p[2]));
        return its;
    }
    case PrimitiveType::Capsule:
        return revolve(capsule_profile(p[0], p[1], size_t(p[3])), int(p[2]));
    case PrimitiveType::Frustum:
        return revolve({{p[0], 0.}, {p[1], p[2]}}, int(p[3]));
    case PrimitiveType::Tube:
        return make_tube(p[0], p[1], p[2], int(p[3]));
    case PrimitiveType::Wedge:
        return make_wedge(p[0], p[1], p[2], p[3]);
    default: return {};
    }
}

double primitive_volume(PrimitiveType type, const std::vector<double> &p)
{
    switch (type) {
    case PrimitiveType::Torus:     return 2. * PI * PI * p[0] * p[1] * p[1];
    case PrimitiveType::Ellipsoid: return 4. / 3. * PI * p[0] * p[1] * p[2];
    case PrimitiveType::Capsule:   return PI * p[0] * p[0] * p[1] + 4. / 3. * PI * p[0] * p[0] * p[0];
    case PrimitiveType::Frustum:   return PI * p[2] / 3. * (p[0] * p[0] + p[0] * p[1] + p[1] * p[1]);
    case PrimitiveType::Tube:      return PI * p[2] * (p[0] * p[0] - p[1] * p[1]);
    case PrimitiveType::Wedge:     return p[0] * p[1] * 0.5 * (p[2] + p[3]);
    default:                       return 0.;
    }
}

double primitive_volume_tolerance(PrimitiveType type, const std::vector<double> &p)
{
    constexpr double float_allowance = 1e-5;
    switch (type) {
    case PrimitiveType::Torus:
        // Both cross sections lose area like inscribed polygons.
        return polygon_deficit(p[2]) + polygon_deficit(p[3]) + float_allowance;
    case PrimitiveType::Ellipsoid:
        return ellipsoid_deficit(size_t(p[4]), p[3]) + float_allowance;
    case PrimitiveType::Capsule: {
        // Exact deficit of the revolved polyline for the actual proportions.
        const Profile prof = capsule_profile(p[0], p[1], size_t(p[3]));
        const double  v    = profile_volume(prof);
        return 1. - polygon_area_factor(p[2]) * v / primitive_volume(type, p) + float_allowance;
    }
    case PrimitiveType::Frustum:
    case PrimitiveType::Tube:
        return polygon_deficit(p[3]) + float_allowance;
    default:
        return float_allowance;
    }
}

std::vector<double> primitive_params_for_tolerance(PrimitiveType type, std::vector<double> params, double max_relative_loss)
{
    if (!primitive_validate(type, params).empty())
        return params;
    std::vector<size_t> count_idx;
    switch (type) {
    case PrimitiveType::Torus:     count_idx = {2, 3}; break;
    case PrimitiveType::Ellipsoid: count_idx = {3, 4}; break;
    case PrimitiveType::Capsule:   count_idx = {2, 3}; break;
    case PrimitiveType::Frustum:
    case PrimitiveType::Tube:      count_idx = {3}; break;
    default: return params;
    }
    const auto &info = primitive_param_info(type);
    auto round4 = [](double v) { return 4. * std::ceil(v / 4.); };
    for (int guard = 0; guard < 64 && primitive_volume_tolerance(type, params) > max_relative_loss; ++guard) {
        bool grew = false;
        for (size_t i : count_idx) {
            const double next = std::min(round4(params[i] * 1.25), 4. * std::floor(info[i].max / 4.));
            if (next > params[i]) {
                params[i] = next;
                grew = true;
            }
        }
        if (!grew)
            break;
        if (!primitive_validate(type, params).empty()) {
            // Past the triangle limit: go back to the last usable counts.
            for (size_t i : count_idx)
                params[i] = std::max(info[i].min, round4(params[i] / 1.25) - 4.);
            while (!primitive_validate(type, params).empty())
                for (size_t i : count_idx)
                    params[i] = std::max(info[i].min, params[i] - 4.);
            break;
        }
    }
    return params;
}

} // namespace Slic3r
