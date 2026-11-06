#include "ShapeGen.hpp"

#include <algorithm>
#include <cmath>

namespace Slic3r {

namespace {
// ---------------------------------------------------------------------------
// Geometry helpers
// ---------------------------------------------------------------------------

void its_translate(indexed_triangle_set& its, float dz)
{
    for (auto& v : its.vertices)
        v.z() += dz;
}

void its_append(indexed_triangle_set& dst, const indexed_triangle_set& src)
{
    int base = (int)dst.vertices.size();
    for (const auto& v : src.vertices)
        dst.vertices.push_back(v);
    for (const auto& f : src.indices)
        dst.indices.push_back({f[0] + base, f[1] + base, f[2] + base});
}

// Pie-slice sector: radius r, heights [z_lo,z_hi], angles [a_lo,a_hi], n arc steps.
// All six faces have outward normals (verified analytically).
indexed_triangle_set make_wedge(float r, float z_lo, float z_hi,
                                        double a_lo, double a_hi, int n)
{
    indexed_triangle_set its;
    double step = (a_hi - a_lo) / n;
    int cb = (int)its.vertices.size(); its.vertices.push_back({0.f, 0.f, z_lo});
    int ct = (int)its.vertices.size(); its.vertices.push_back({0.f, 0.f, z_hi});
    int ab = (int)its.vertices.size();
    for (int i = 0; i <= n; ++i) { double a = a_lo+i*step; its.vertices.push_back({r*(float)std::cos(a), r*(float)std::sin(a), z_lo}); }
    int at = (int)its.vertices.size();
    for (int i = 0; i <= n; ++i) { double a = a_lo+i*step; its.vertices.push_back({r*(float)std::cos(a), r*(float)std::sin(a), z_hi}); }
    for (int i = 0; i < n; ++i) its.indices.push_back({cb, ab+i+1, ab+i});      // bottom -Z
    for (int i = 0; i < n; ++i) its.indices.push_back({ct, at+i, at+i+1});      // top +Z
    for (int i = 0; i < n; ++i) {                                                 // outer wall
        its.indices.push_back({ab+i, ab+i+1, at+i});
        its.indices.push_back({ab+i+1, at+i+1, at+i});
    }
    // Side walls: winding chosen so normals point away from sector interior
    its.indices.push_back({cb, ab,   ct}); its.indices.push_back({ab,   at,   ct}); // a_lo side
    its.indices.push_back({ct, ab+n, cb}); its.indices.push_back({ct, at+n, ab+n}); // a_hi side
    return its;
}

// Swept-tube helix coil. Generates geometry with z in [0, (t_hi-t_lo)*height]
// so make_zband's its_translate(seg, z_lo) places it correctly.
// phase_offset shifts the angular start position (radians) for multi-strand helices.
indexed_triangle_set make_helix_coil(float coil_r, float tube_r, float height,
                                             int turns, float t_lo, float t_hi,
                                             float phase_offset = 0.0f)
{
    const int path_steps = std::max(8, 32 * (int)std::ceil(turns * (t_hi - t_lo)));
    const int tube_steps = 16;
    indexed_triangle_set its;

    using V3 = Eigen::Matrix<float, 3, 1>;
    std::vector<V3> centers(path_steps + 1);
    std::vector<V3> nvec(path_steps + 1), bvec(path_steps + 1);

    for (int k = 0; k <= path_steps; ++k) {
        float tg    = t_lo + (float)k / path_steps * (t_hi - t_lo);
        float theta = 2.0f * (float)M_PI * turns * tg + phase_offset;
        float ct    = std::cos(theta), st = std::sin(theta);
        float z_loc = (float)k / path_steps * (t_hi - t_lo) * height;

        centers[k] = {coil_r * ct, coil_r * st, z_loc};

        float dtheta = 2.0f * (float)M_PI * turns;
        V3 tang{-coil_r * dtheta * st, coil_r * dtheta * ct, height};
        tang.normalize();

        V3 radial{-ct, -st, 0.0f};
        V3 n = radial - radial.dot(tang) * tang;
        float nlen = n.norm();
        if (nlen < 1e-6f) {
            V3 up{0.f, 0.f, 1.f};
            n = up - up.dot(tang) * tang;
            nlen = n.norm();
        }
        n /= nlen;
        nvec[k] = n;
        bvec[k] = tang.cross(n);
    }

    const int base = (int)its.vertices.size();
    auto vi = [&](int k, int j) { return base + k * tube_steps + j; };

    for (int k = 0; k <= path_steps; ++k)
        for (int j = 0; j < tube_steps; ++j) {
            float a = 2.0f * (float)M_PI * j / tube_steps;
            its.vertices.push_back(centers[k] + tube_r * (std::cos(a) * nvec[k] + std::sin(a) * bvec[k]));
        }

    for (int k = 0; k < path_steps; ++k)
        for (int j = 0; j < tube_steps; ++j) {
            int j1 = (j + 1) % tube_steps;
            its.indices.push_back({vi(k,j),  vi(k,j1),  vi(k+1,j)});
            its.indices.push_back({vi(k,j1), vi(k+1,j1), vi(k+1,j)});
        }

    // Start cap
    {
        int ctr = (int)its.vertices.size();
        its.vertices.push_back(centers[0]);
        for (int j = 0; j < tube_steps; ++j)
            its.indices.push_back({ctr, vi(0, (j+1) % tube_steps), vi(0, j)});
    }
    // End cap
    {
        int ctr = (int)its.vertices.size();
        its.vertices.push_back(centers[path_steps]);
        for (int j = 0; j < tube_steps; ++j)
            its.indices.push_back({ctr, vi(path_steps, j), vi(path_steps, (j+1) % tube_steps)});
    }

    return its;
}

// Solid hexagonal prism (flat-top orientation, vertices at 0 deg,60 deg,...).
// Outward normals on all six faces.
indexed_triangle_set make_hex_prism(float cx, float cy, float r, float z_lo, float z_hi)
{
    indexed_triangle_set its;
    int vbc = (int)its.vertices.size(); its.vertices.push_back({cx, cy, z_lo});
    int vtc = (int)its.vertices.size(); its.vertices.push_back({cx, cy, z_hi});
    int vb  = (int)its.vertices.size();
    for (int k = 0; k < 6; ++k) { float a = k*(float)(M_PI/3.0) + (float)(M_PI/6.0); its.vertices.push_back({cx+r*std::cos(a), cy+r*std::sin(a), z_lo}); }
    int vt  = (int)its.vertices.size();
    for (int k = 0; k < 6; ++k) { float a = k*(float)(M_PI/3.0) + (float)(M_PI/6.0); its.vertices.push_back({cx+r*std::cos(a), cy+r*std::sin(a), z_hi}); }
    for (int k = 0; k < 6; ++k) {
        int k1 = (k+1)%6;
        its.indices.push_back({vbc, vb+k1, vb+k});      // bottom cap (-Z)
        its.indices.push_back({vtc, vt+k,  vt+k1});     // top cap (+Z)
        its.indices.push_back({vb+k, vb+k1, vt+k});     // side wall
        its.indices.push_back({vb+k1, vt+k1, vt+k});
    }
    return its;
}

// Square frustum (or pyramid if s1==0): centered at XY origin, z in [z_lo, z_hi].
// Outward normals on all faces.
indexed_triangle_set make_square_frustum(float s0, float s1, float z_lo, float z_hi)
{
    indexed_triangle_set its;
    float h0 = s0 * 0.5f, h1 = s1 * 0.5f;
    // Bottom verts 0-3, top verts 4-7
    its.vertices.push_back({-h0, -h0, z_lo}); // 0
    its.vertices.push_back({ h0, -h0, z_lo}); // 1
    its.vertices.push_back({ h0,  h0, z_lo}); // 2
    its.vertices.push_back({-h0,  h0, z_lo}); // 3
    if (h1 <= 1e-4f) {
        // A pyramid has one apex. Four coincident top vertices create
        // zero-area edges and duplicate side triangles that confuse mesh
        // repair and downstream boolean operations.
        its.vertices.push_back({0.f, 0.f, z_hi}); // 4
        its.indices.push_back({0, 2, 1});
        its.indices.push_back({0, 3, 2});
        its.indices.push_back({0, 1, 4});
        its.indices.push_back({1, 2, 4});
        its.indices.push_back({2, 3, 4});
        its.indices.push_back({3, 0, 4});
        return its;
    }
    its.vertices.push_back({-h1, -h1, z_hi}); // 4
    its.vertices.push_back({ h1, -h1, z_hi}); // 5
    its.vertices.push_back({ h1,  h1, z_hi}); // 6
    its.vertices.push_back({-h1,  h1, z_hi}); // 7
    // Bottom cap (-Z)
    its.indices.push_back({0, 2, 1}); its.indices.push_back({0, 3, 2});
    // Top cap (+Z)
    its.indices.push_back({4, 5, 6}); its.indices.push_back({4, 6, 7});
    // Four side faces
    its.indices.push_back({0, 1, 5}); its.indices.push_back({0, 5, 4}); // front -Y
    its.indices.push_back({1, 2, 6}); its.indices.push_back({1, 6, 5}); // right +X
    its.indices.push_back({2, 3, 7}); its.indices.push_back({2, 7, 6}); // back  +Y
    its.indices.push_back({3, 0, 4}); its.indices.push_back({3, 4, 7}); // left  -X
    return its;
}

// ---------------------------------------------------------------------------
// Body mesh generators - bottom at z=0
// ---------------------------------------------------------------------------

indexed_triangle_set make_body(int shape_idx, float size, float height)
{
    const double step = 2.0 * M_PI / 512.0;
    float half = size * 0.5f;
    indexed_triangle_set body;
    switch (shape_idx) {
    case 0: body = its_make_cube(size, size, height);             break;
    case 1: body = its_make_cylinder(half, height, step);         break;
    case 2: body = its_make_cone(half, height, step);             break;
    case 3: body = its_make_sphere(half, step);                   break;
    case 4: body = its_make_pyramid(size, height);                break;
    // case 5 (Helix) is handled entirely in build_into_model; return cylinder as fallback
    default: body = its_make_cylinder(half, height, step);        break;
    }
    if (shape_idx == 3)
        its_translate(body, half);
    return body;
}

// Z-band segment for Stripes - uses full cross-section of the shape.
// Conservative (min-over-band) shape radius for [z_lo, z_hi]. Sampling only at
// z_mid lets a constant-radius band bulge outward past a tapered/curved
// boundary over half the band; the cone's radius is linear-decreasing and the
// sphere's is concave in z, so in both cases the minimum over the interval is
// attained at one of the two endpoints - using it keeps the band inscribed.
float shape_radius_over_zband(int shape_idx, float size, float full_height,
                                      float z_lo, float z_hi)
{
    float half = size * 0.5f;
    float fh   = std::max(full_height, 0.01f);
    switch (shape_idx) {
    case 2: { // Cone.
        float r_lo = half * std::max(0.f, 1.f - z_lo / fh);
        float r_hi = half * std::max(0.f, 1.f - z_hi / fh);
        return std::max(std::min(r_lo, r_hi), 0.5f);
    }
    case 3: { // Sphere, translated so its bottom is at Z=0.
        auto radius_at = [half](float z) {
            return std::sqrt(std::max(0.f, half*half - (z-half)*(z-half)));
        };
        return std::max(std::min(radius_at(z_lo), radius_at(z_hi)), 0.5f);
    }
    default: return half;
    }
}

indexed_triangle_set make_zband(int shape_idx, float size, float full_height,
                                       float z_lo, float z_hi, const ShapeGenParams& s_params)
{
    const double step = 2.0 * M_PI / 512.0;
    float seg_h = z_hi - z_lo;
    float half  = size * 0.5f;
    float fh    = std::max(full_height, 0.01f);

    indexed_triangle_set seg;
    switch (shape_idx) {
    case 0:  seg = its_make_cube(size, size, seg_h); break;
    case 1:  seg = its_make_cylinder(half, seg_h, step); break;
    case 2:  seg = its_make_cylinder(shape_radius_over_zband(shape_idx, size, full_height, z_lo, z_hi), seg_h, step); break;
    case 3:  seg = its_make_cylinder(shape_radius_over_zband(shape_idx, size, full_height, z_lo, z_hi), seg_h, step); break;
    case 4: {
        float s_lo = size * std::max(1.f - z_lo / fh, 0.f);
        float s_hi = size * std::max(1.f - z_hi / fh, 0.f);
        // Frustum already has z_lo/z_hi baked in - return directly.
        return make_square_frustum(s_lo, s_hi, z_lo, z_hi);
    }
    case 5: { // Helix: tube segment for this Z range; translate handles z_lo offset
        float tube_r = half * std::clamp(s_params.hs_tube_pct, 5, 40) / 100.0f;
        float path_r = half - tube_r;
        seg = make_helix_coil(path_r, tube_r, full_height, s_params.hs_turns,
                              z_lo / fh, z_hi / fh);
        its_translate(seg, z_lo);
        return seg;
    }
    default: seg = its_make_cylinder(half, seg_h, step); break;
    }
    its_translate(seg, z_lo);
    return seg;
}

// ---------------------------------------------------------------------------
// Per-cell mesh for Checkerboard / Helix / Honeycomb patterns
// ---------------------------------------------------------------------------
// Round shapes use angular wedge sectors.
// Cube/Pyramid use X-direction slabs so the geometry exactly matches the shape.

// One cell: Z-band [z_lo,z_hi], sector j out of n_a total.
indexed_triangle_set make_grid_cell(int shape_idx, float size, float height,
                                            float z_lo, float z_hi, int j, int n_a)
{
    float half  = size * 0.5f;
    float seg_h = z_hi - z_lo;

    if (shape_idx == 0) {   // Cube: X-slab of full Y depth
        float x_step = size / n_a;
        float x_lo   = -half + j * x_step;
        auto box = its_make_cube(x_step, size, seg_h);
        for (auto& v : box.vertices) { v.x() += x_lo; v.y() -= half; v.z() += z_lo; }
        return box;
    }
    if (shape_idx == 4) {   // Pyramid: X-slab tapering with height.
        // Scale is linear-decreasing in z, so the min over [z_lo,z_hi] (the
        // conservative - non-bulging - width for the whole band) is at z_hi;
        // computed from both endpoints for consistency with the cone/sphere case.
        float fh       = std::max(height, 0.01f);
        float scale_lo = std::max(1.f - z_lo / fh, 0.01f);
        float scale_hi = std::max(1.f - z_hi / fh, 0.01f);
        float scale    = std::min(scale_lo, scale_hi);
        float sw     = scale * size;
        float x_step = sw / n_a;
        float x_lo   = -sw * 0.5f + j * x_step;
        auto box = its_make_cube(x_step, sw, seg_h);
        for (auto& v : box.vertices) { v.x() += x_lo; v.y() -= sw * 0.5f; v.z() += z_lo; }
        return box;
    }

    const int arc_steps = std::max(4, 512 / n_a);  // ~0.7 deg per triangle
    double a_lo = j * (2.0 * M_PI / n_a);
    double a_hi = (j + 1) * (2.0 * M_PI / n_a);

    return make_wedge(shape_radius_over_zband(shape_idx, size, height, z_lo, z_hi),
                      z_lo, z_hi, a_lo, a_hi, arc_steps);
}

// ---------------------------------------------------------------------------
// Real hexagonal honeycomb
// ---------------------------------------------------------------------------
// Material 0 = the complete shape body (defines all geometry).
// Material 1 = inner hex prisms (R_in) overlapping the body - the slicer
//              resolves the overlap, making cell interiors material 1 and
//              the wall ring + inter-cell regions material 0.
std::vector<indexed_triangle_set>
build_honeycomb(int shape_idx, float size, float height, int n_cells, int wall_pct, int n_materials)
{
    std::vector<indexed_triangle_set> meshes(n_materials);

    const float half  = size * 0.5f;
    const float R     = size / (std::sqrt(3.f) * std::max(1, n_cells));
    const float R_in  = R * (1.f - std::clamp(wall_pct, 5, 50) / 100.f);
    const float dx    = std::sqrt(3.f) * R;
    const float dy    = 1.5f * R;

    // Full shape body is material 0 - same geometry as Single pattern.
    meshes[0] = make_body(shape_idx, size, height);
    if (shape_idx == 0)  // its_make_cube is corner-at-origin; center it to match hex coords
        for (auto& v : meshes[0].vertices) { v.x() -= half; v.y() -= half; }

    const int cols = (int)std::ceil(size / dx) + 2;
    const int rows = (int)std::ceil(size / dy) + 2;

    for (int row = -rows; row <= rows; ++row) {
        const float cy = row * dy;
        const float x_offset = (std::abs(row) % 2 == 1) ? dx * 0.5f : 0.f;
        for (int col = -cols; col <= cols; ++col) {
            const float cx = col * dx + x_offset;

            // Test the complete inner hex, not just its center. This keeps the
            // generated material volume inside the body's lateral boundary.
            bool contained = true;
            float cell_z_lo = 0.f;
            float cell_z_hi = height;
            for (int vertex = 0; vertex < 6; ++vertex) {
                const float angle = vertex * float(M_PI / 3.0) + float(M_PI / 6.0);
                const float x = cx + R_in * std::cos(angle);
                const float y = cy + R_in * std::sin(angle);
                const float radius = std::sqrt(x * x + y * y);
                const float square_radius = std::max(std::abs(x), std::abs(y));

                if ((shape_idx == 0 || shape_idx == 4)
                        ? square_radius > half
                        : radius > half) {
                    contained = false;
                    break;
                }

                switch (shape_idx) {
                case 2: // Cone.
                    cell_z_hi = std::min(
                        cell_z_hi, height * std::max(0.f, 1.f - radius / half));
                    break;
                case 3: { // Sphere, translated so its bottom is at Z=0.
                    const float dz = std::sqrt(std::max(
                        0.f, half * half - radius * radius));
                    cell_z_lo = std::max(cell_z_lo, half - dz);
                    cell_z_hi = std::min(cell_z_hi, half + dz);
                    break;
                }
                case 4: // Pyramid.
                    cell_z_hi = std::min(
                        cell_z_hi,
                        height * std::max(0.f, 1.f - square_radius / half));
                    break;
                default:
                    break;
                }
            }
            if (!contained || cell_z_hi - cell_z_lo < 0.1f)
                continue;

            its_append(meshes[1 % n_materials],
                make_hex_prism(cx, cy, R_in, cell_z_lo, cell_z_hi));
        }
    }
    return meshes;
}

// ---------------------------------------------------------------------------
// Pattern meshes
// ---------------------------------------------------------------------------
// pattern_idx (= UI pattern index - 1, 0-based):
//   0 = Stripes      Z-bands, n = stripe_rows rows
//   1 = Checkerboard angular grid, (i+j)%2 alternation
//   2 = Helix        16-sector helical boundary, configurable revolutions + strand width
//   3 = Honeycomb    real hexagonal tiling viewed from top
//
// Returns n_materials meshes, one per material.
std::vector<indexed_triangle_set>
build_pattern(int shape_idx, float size, float height, int pattern_idx,
              const ShapeGenParams& p, int n_materials)
{
    std::vector<indexed_triangle_set> meshes(n_materials);

    auto add_band = [&](float z_lo, float z_hi, int mat_idx) {
        auto seg = make_zband(shape_idx, size, height, z_lo, z_hi, p);
        its_append(meshes[mat_idx % n_materials], seg);
    };

    auto add_cell = [&](int i, int n_z, int j, int n_a, int mat_idx) {
        float z_lo = (float)i / n_z * height;
        float z_hi = (float)(i + 1) / n_z * height;
        auto seg = make_grid_cell(shape_idx, size, height, z_lo, z_hi, j, n_a);
        its_append(meshes[mat_idx % n_materials], seg);
    };

    switch (pattern_idx) {
    case 0: { // Stripes - n equal Z-bands cycling through n_materials
        int n = std::max(1, p.stripe_rows);
        float bh = height / n;
        for (int i = 0; i < n; ++i)
            add_band(i * bh, (i + 1) * bh, i % n_materials);
        break;
    }
    case 1: { // Checkerboard - (i+j) % n_materials
        const int n_a = std::max(4, p.cb_columns);
        const int n_z = std::max(2, p.cb_rows);
        for (int i = 0; i < n_z; ++i)
            for (int j = 0; j < n_a; ++j)
                add_cell(i, n_z, j, n_a, (i + j) % n_materials);
        break;
    }
    case 2: { // Helix - boundary rotates 1 sector per Z-band, configurable revolutions + width
        const int n_a  = 32;  // angular resolution (sectors / X-columns per Z-band)
        const int n_rev = std::max(1, (int)std::round(p.hx_revolutions));
        const int n_z  = n_a * n_rev;
        // Divide sectors into n_materials groups rotating with the helix phase.
        // For n_materials==2 the first group uses hx_width% of sectors (mat 0),
        // rest goes to mat 1 - matches the original strand-width semantics.
        const int width_cells = std::max(1, std::min(n_a - 1,
                                    (int)std::round(n_a * p.hx_width / 100.0)));
        for (int i = 0; i < n_z; ++i) {
            const int phase = i % n_a;
            for (int j = 0; j < n_a; ++j) {
                const int rank = (j - phase + n_a) % n_a;
                const int mat_idx = (n_materials == 2)
                    ? (rank < width_cells ? 0 : 1)
                    : (rank * n_materials / n_a);
                add_cell(i, n_z, j, n_a, mat_idx);
            }
        }
        break;
    }
    case 3: { // Honeycomb - real hexagonal tiling
        auto hc = build_honeycomb(shape_idx, size, height, p.hc_sectors, p.hc_bands, n_materials);
        for (int m = 0; m < n_materials; ++m)
            its_append(meshes[m], hc[m]);
        break;
    }
    }
    return meshes;
}

} // namespace

int shape_gen_material_count(const ShapeGenParams &p)
{
    if (p.shape == 5)
        return std::max(1, p.hs_strands) + 1;
    return p.pattern == 0 ? 1 : std::max(2, p.n_materials);
}

std::vector<indexed_triangle_set> generate_shape(const ShapeGenParams &p)
{
    const float size   = (float)p.size;
    const float height = (float)p.sla_h;
    const int   n_mat  = shape_gen_material_count(p);

    if (p.shape == 5) {
        const int   n_strands = n_mat - 1;
        const float coil_r    = size * 0.5f;
        const float tube_r    = coil_r * std::clamp(p.hs_tube_pct, 5, 40) / 100.0f;
        const float path_r    = coil_r - tube_r;
        std::vector<indexed_triangle_set> out;
        out.push_back(its_make_cylinder(coil_r, height, 2.0 * M_PI / 512.0));
        for (int s = 0; s < n_strands; ++s) {
            const float phase = 2.0f * (float)M_PI * s / n_strands;
            auto tube = make_helix_coil(path_r, tube_r, height, p.hs_turns, 0.0f, 1.0f, phase);
            for (auto &v : tube.vertices)
                v.z() = std::clamp(v.z(), 0.0f, height);
            out.push_back(std::move(tube));
        }
        return out;
    }

    if (p.pattern == 0)
        return { make_body(p.shape, size, height) };

    return build_pattern(p.shape, size, height, p.pattern - 1, p, n_mat);
}

} // namespace Slic3r
