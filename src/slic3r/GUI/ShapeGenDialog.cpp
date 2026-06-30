#include "ShapeGenDialog.hpp"

#include <algorithm>
#include <cmath>
#include <vector>
#include <string>

#include <wx/sizer.h>
#include <wx/stattext.h>
#include <wx/button.h>
#include <wx/choice.h>
#include <wx/spinctrl.h>

#include "GUI_App.hpp"
#include "Plater.hpp"
#include "GLCanvas3D.hpp"
#include "I18N.hpp"
#include "libslic3r/Model.hpp"
#include "libslic3r/TriangleMesh.hpp"
#include "libslic3r/PrintConfig.hpp"
#include "libslic3r/PresetBundle.hpp"

namespace Slic3r {
namespace GUI {

// ---------------------------------------------------------------------------
// Persistent parameters
// ---------------------------------------------------------------------------

struct ShapeGenParams {
    int    shape          = 1;     // Cylinder
    int    pattern        = 1;     // Stripes
    double size           = 20.0;
    double sla_h          = 15.0;
    // Stripes
    int    stripe_rows    = 4;
    // Checkerboard
    int    cb_columns     = 8;
    int    cb_rows        = 4;
    // Helix pattern
    double hx_revolutions = 3.0;
    int    hx_width       = 50;   // strand % (10-90)
    // Honeycomb
    int    hc_sectors     = 6;    // hex cells across the diameter
    int    hc_bands       = 20;   // wall thickness as % of cell circumradius
    // Multi-material
    int    n_materials    = 2;    // number of distinct material bands
    // Helix shape
    int    hs_turns       = 3;    // helix revolutions
    int    hs_tube_pct    = 20;   // tube radius as % of cylinder radius (5-40)
    int    hs_strands     = 1;    // number of parallel helix strands
};

static ShapeGenParams s_params;

// ---------------------------------------------------------------------------
// Geometry helpers
// ---------------------------------------------------------------------------

static void its_translate(indexed_triangle_set& its, float dz)
{
    for (auto& v : its.vertices)
        v.z() += dz;
}

static void its_append(indexed_triangle_set& dst, const indexed_triangle_set& src)
{
    int base = (int)dst.vertices.size();
    for (const auto& v : src.vertices)
        dst.vertices.push_back(v);
    for (const auto& f : src.indices)
        dst.indices.push_back({f[0] + base, f[1] + base, f[2] + base});
}

// Pie-slice sector: radius r, heights [z_lo,z_hi], angles [a_lo,a_hi], n arc steps.
// All six faces have outward normals (verified analytically).
static indexed_triangle_set make_wedge(float r, float z_lo, float z_hi,
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
static indexed_triangle_set make_helix_coil(float coil_r, float tube_r, float height,
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

// Solid hexagonal prism (flat-top orientation, vertices at 0°,60°,...).
// Outward normals on all six faces.
static indexed_triangle_set make_hex_prism(float cx, float cy, float r, float z_lo, float z_hi)
{
    indexed_triangle_set its;
    int vbc = (int)its.vertices.size(); its.vertices.push_back({cx, cy, z_lo});
    int vtc = (int)its.vertices.size(); its.vertices.push_back({cx, cy, z_hi});
    int vb  = (int)its.vertices.size();
    for (int k = 0; k < 6; ++k) { float a = k*(float)(M_PI/3.0); its.vertices.push_back({cx+r*std::cos(a), cy+r*std::sin(a), z_lo}); }
    int vt  = (int)its.vertices.size();
    for (int k = 0; k < 6; ++k) { float a = k*(float)(M_PI/3.0); its.vertices.push_back({cx+r*std::cos(a), cy+r*std::sin(a), z_hi}); }
    for (int k = 0; k < 6; ++k) {
        int k1 = (k+1)%6;
        its.indices.push_back({vbc, vb+k1, vb+k});      // bottom cap (-Z)
        its.indices.push_back({vtc, vt+k,  vt+k1});     // top cap (+Z)
        its.indices.push_back({vb+k, vb+k1, vt+k});     // side wall
        its.indices.push_back({vb+k1, vt+k1, vt+k});
    }
    return its;
}

// ---------------------------------------------------------------------------
// Body mesh generators — bottom at z=0
// ---------------------------------------------------------------------------

static indexed_triangle_set make_body(int shape_idx, float size, float height)
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

// Z-band segment for Stripes — uses full cross-section of the shape.
static indexed_triangle_set make_zband(int shape_idx, float size, float full_height,
                                       float z_lo, float z_hi)
{
    const double step = 2.0 * M_PI / 512.0;
    float seg_h = z_hi - z_lo;
    float half  = size * 0.5f;
    float z_mid = (z_lo + z_hi) * 0.5f;
    float fh    = std::max(full_height, 0.01f);

    indexed_triangle_set seg;
    switch (shape_idx) {
    case 0:  seg = its_make_cube(size, size, seg_h); break;
    case 1:  seg = its_make_cylinder(half, seg_h, step); break;
    case 2:  seg = its_make_cylinder(std::max(half*(1.f-z_mid/fh), 0.5f), seg_h, step); break;
    case 3:  seg = its_make_cylinder(std::max(std::sqrt(std::max(0.f, half*half-(z_mid-half)*(z_mid-half))), 0.5f), seg_h, step); break;
    case 4:  { float s=std::max(size*(1.f-z_mid/fh),0.5f); seg=its_make_cube(s,s,seg_h); break; }
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

static float shape_radius_at_z(int shape_idx, float size, float full_height, float z_mid)
{
    float half = size * 0.5f;
    float fh   = std::max(full_height, 0.01f);
    switch (shape_idx) {
    case 2:  return std::max(half * (1.f - z_mid / fh), 0.5f);
    case 3:  return std::max(std::sqrt(std::max(0.f, half*half-(z_mid-half)*(z_mid-half))), 0.5f);
    default: return half;
    }
}

// One cell: Z-band [z_lo,z_hi], sector j out of n_a total.
static indexed_triangle_set make_grid_cell(int shape_idx, float size, float height,
                                            float z_lo, float z_hi, int j, int n_a)
{
    float z_mid = (z_lo + z_hi) * 0.5f;
    float half  = size * 0.5f;
    float seg_h = z_hi - z_lo;

    if (shape_idx == 0) {   // Cube: X-slab of full Y depth
        float x_step = size / n_a;
        float x_lo   = -half + j * x_step;
        auto box = its_make_cube(x_step, size, seg_h);
        for (auto& v : box.vertices) { v.x() += x_lo; v.y() -= half; v.z() += z_lo; }
        return box;
    }
    if (shape_idx == 4) {   // Pyramid: X-slab tapering with height
        float scale  = std::max(1.f - z_mid / std::max(height, 0.01f), 0.01f);
        float sw     = scale * size;
        float x_step = sw / n_a;
        float x_lo   = -sw * 0.5f + j * x_step;
        auto box = its_make_cube(x_step, sw, seg_h);
        for (auto& v : box.vertices) { v.x() += x_lo; v.y() -= sw * 0.5f; v.z() += z_lo; }
        return box;
    }

    const int arc_steps = std::max(4, 512 / n_a);  // ~0.7° per triangle
    double a_lo = j * (2.0 * M_PI / n_a);
    double a_hi = (j + 1) * (2.0 * M_PI / n_a);

    return make_wedge(shape_radius_at_z(shape_idx, size, height, z_mid),
                      z_lo, z_hi, a_lo, a_hi, arc_steps);
}

// ---------------------------------------------------------------------------
// Real hexagonal honeycomb
// ---------------------------------------------------------------------------
// Tiles the shape footprint with flat-top hexagonal prisms.
// Each cell gets one full-size hex prism (ext1 / FFF wall) plus one inner
// hex prism (ext5 / SLA cell interior) added on top of it. Because ext5 is
// loaded second, the slicer assigns it to the overlapping interior region,
// leaving only the ring between R and R_inner as FFF wall material.
static std::vector<indexed_triangle_set>
build_honeycomb(int shape_idx, float size, float height, int n_cells, int wall_pct, int n_materials)
{
    std::vector<indexed_triangle_set> meshes(n_materials);

    const float half  = size * 0.5f;
    const float R     = size / (std::sqrt(3.f) * std::max(1, n_cells));
    const float R_in  = R * (1.f - std::clamp(wall_pct, 5, 50) / 100.f);
    const float dx    = std::sqrt(3.f) * R;
    const float dy    = 1.5f * R;

    const int cols = (int)std::ceil(size / dx) + 2;
    const int rows = (int)std::ceil(size / dy) + 2;

    for (int row = -rows; row <= rows; ++row) {
        const float cy = row * dy;
        const float x_offset = (std::abs(row) % 2 == 1) ? dx * 0.5f : 0.f;
        for (int col = -cols; col <= cols; ++col) {
            const float cx = col * dx + x_offset;
            const float r2 = cx*cx + cy*cy;

            bool in_shape = false;
            switch (shape_idx) {
            case 0: case 4:  in_shape = std::abs(cx) < half && std::abs(cy) < half; break;
            default:         in_shape = r2 < half*half; break;
            }
            if (!in_shape) continue;

            its_append(meshes[0],               make_hex_prism(cx, cy, R,    0.f, height));
            its_append(meshes[1 % n_materials], make_hex_prism(cx, cy, R_in, 0.f, height));
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
static std::vector<indexed_triangle_set>
build_pattern(int shape_idx, float size, float height, int pattern_idx,
              const ShapeGenParams& p, int n_materials)
{
    std::vector<indexed_triangle_set> meshes(n_materials);

    auto add_band = [&](float z_lo, float z_hi, int mat_idx) {
        auto seg = make_zband(shape_idx, size, height, z_lo, z_hi);
        its_append(meshes[mat_idx % n_materials], seg);
    };

    auto add_cell = [&](int i, int n_z, int j, int n_a, int mat_idx) {
        float z_lo = (float)i / n_z * height;
        float z_hi = (float)(i + 1) / n_z * height;
        auto seg = make_grid_cell(shape_idx, size, height, z_lo, z_hi, j, n_a);
        its_append(meshes[mat_idx % n_materials], seg);
    };

    switch (pattern_idx) {
    case 0: { // Stripes — n equal Z-bands cycling through n_materials
        int n = std::max(1, p.stripe_rows);
        float bh = height / n;
        for (int i = 0; i < n; ++i)
            add_band(i * bh, (i + 1) * bh, i % n_materials);
        break;
    }
    case 1: { // Checkerboard — (i+j) % n_materials
        const int n_a = std::max(4, p.cb_columns);
        const int n_z = std::max(2, p.cb_rows);
        for (int i = 0; i < n_z; ++i)
            for (int j = 0; j < n_a; ++j)
                add_cell(i, n_z, j, n_a, (i + j) % n_materials);
        break;
    }
    case 2: { // Helix — boundary rotates 1 sector per Z-band, configurable revolutions + width
        const int n_a  = 16;  // fixed angular resolution
        const int n_rev = std::max(1, (int)std::round(p.hx_revolutions));
        const int n_z  = n_a * n_rev;
        // Divide sectors into n_materials groups rotating with the helix phase.
        // For n_materials==2 the first group uses hx_width% of sectors (mat 0),
        // rest goes to mat 1 — matches the original strand-width semantics.
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
    case 3: { // Honeycomb — real hexagonal tiling
        auto hc = build_honeycomb(shape_idx, size, height, p.hc_sectors, p.hc_bands, n_materials);
        for (int m = 0; m < n_materials; ++m)
            its_append(meshes[m], hc[m]);
        break;
    }
    }
    return meshes;
}

// ---------------------------------------------------------------------------
// Build the full ModelObject into a Model
// ---------------------------------------------------------------------------
// pattern_idx 0 = Single  (solid body, first SLA extruder)
// pattern_idx 1-4 map to build_pattern indices 0-3

// Build interleaved [FFF1, SLA1, FFF2, SLA2, ...] extruder list from the
// current printer preset.  Falls back to {1, 5} if the config is unavailable.
static std::vector<int> make_extruder_list(int n_materials)
{
    std::vector<int> fff_exts, sla_exts;
    const DynamicPrintConfig& cfg =
        wxGetApp().preset_bundle->printers.get_edited_preset().config;
    const auto* sla_opt = cfg.option<ConfigOptionBools>("sla_material_extruder");
    int n_ext = (int)cfg.option<ConfigOptionFloats>("nozzle_diameter")->values.size();
    for (int i = 0; i < n_ext; ++i) {
        bool is_sla = sla_opt && i < (int)sla_opt->values.size() && sla_opt->values[i];
        (is_sla ? sla_exts : fff_exts).push_back(i + 1);
    }
    if (fff_exts.empty()) fff_exts.push_back(1);

    if (sla_exts.empty()) {
        // No SLA extruders — cycle through FFF extruders so each material has a distinct color.
        std::vector<int> result;
        result.reserve(n_materials);
        for (int i = 0; i < n_materials; ++i)
            result.push_back(fff_exts[i % (int)fff_exts.size()]);
        return result;
    }

    std::vector<int> result;
    result.reserve(n_materials);
    int fi = 0, si = 0;
    while ((int)result.size() < n_materials) {
        if (fi < (int)fff_exts.size()) result.push_back(fff_exts[fi++]);
        if ((int)result.size() < n_materials && si < (int)sla_exts.size())
            result.push_back(sla_exts[si++]);
        if (fi >= (int)fff_exts.size() && si >= (int)sla_exts.size()) { fi = 0; si = 0; }
    }
    return result;
}

// Helix shape: cylinder body (FFF) + N helical strand tubes (SLA, FFF2, ...).
// Strands are evenly phase-offset and sized so their outer edge touches the cylinder wall.
static void build_helix_into_model(Model& out_model, float size, float height,
                                    const ShapeGenParams& p)
{
    const int   n_strands = std::max(1, p.hs_strands);
    const int   n_mat     = n_strands + 1;
    const auto  extruders = make_extruder_list(n_mat);
    const float coil_r    = size * 0.5f;
    const float tube_r    = coil_r * std::clamp(p.hs_tube_pct, 5, 40) / 100.0f;
    const float path_r    = coil_r - tube_r;
    const double step     = 2.0 * M_PI / 512.0;

    ModelObject* obj = out_model.add_object();
    obj->name = "Helix Shape";

    // Volume 0: background cylinder
    {
        auto cyl = its_make_cylinder(coil_r, height, step);
        ModelVolume* vol = obj->add_volume(TriangleMesh(std::move(cyl)));
        vol->name = "Cylinder";
        vol->config.set_key_value("extruder", new ConfigOptionInt(extruders[0]));
    }

    // Volumes 1..n_strands: helical tubes
    for (int s = 0; s < n_strands; ++s) {
        float phase = 2.0f * (float)M_PI * s / n_strands;
        auto tube = make_helix_coil(path_r, tube_r, height, p.hs_turns, 0.0f, 1.0f, phase);
        if (tube.vertices.empty())
            continue;
        for (auto& v : tube.vertices)
            v.z() = std::clamp(v.z(), 0.0f, height);
        ModelVolume* vol = obj->add_volume(TriangleMesh(std::move(tube)));
        vol->name = "Strand " + std::to_string(s + 1);
        vol->config.set_key_value("extruder", new ConfigOptionInt(extruders[s + 1]));
    }
}

static void build_into_model(Model& out_model, int shape_idx, float size,
                              float height, int pattern_idx, const ShapeGenParams& p)
{
    // Helix shape has its own multi-volume construction
    if (shape_idx == 5) {
        build_helix_into_model(out_model, size, height, p);
        return;
    }

    const int n_mat = (pattern_idx == 0) ? 1 : std::max(2, p.n_materials);
    const std::vector<int> extruders = make_extruder_list(n_mat);

    if (pattern_idx == 0) {
        ModelObject* obj = out_model.add_object();
        obj->name = "Generated Shape";
        indexed_triangle_set body = make_body(shape_idx, size, height);
        if (!body.vertices.empty()) {
            ModelVolume* vol = obj->add_volume(TriangleMesh(std::move(body)));
            vol->name = "Body";
            // Single always uses the first SLA extruder
            vol->config.set_key_value("extruder", new ConfigOptionInt(extruders.empty() ? 5 : extruders[0]));
        }
        return;
    }

    ModelObject* obj = out_model.add_object();
    obj->name = "Generated Shape";

    auto meshes = build_pattern(shape_idx, size, height, pattern_idx - 1, p, n_mat);
    for (int m = 0; m < n_mat; ++m) {
        if (meshes[m].vertices.empty())
            continue;
        ModelVolume* vol = obj->add_volume(TriangleMesh(std::move(meshes[m])));
        vol->name = "Material " + std::to_string(m + 1);
        vol->config.set_key_value("extruder", new ConfigOptionInt(extruders[m]));
    }
}

// ---------------------------------------------------------------------------
// ShapeGenDialog
// ---------------------------------------------------------------------------

ShapeGenDialog::ShapeGenDialog(wxWindow* parent)
    : wxDialog(parent, wxID_ANY, _L("Generate SLA Shape"),
               wxDefaultPosition, wxDefaultSize,
               wxDEFAULT_DIALOG_STYLE | wxRESIZE_BORDER)
{
    SetFont(wxGetApp().normal_font());

    auto* outer = new wxBoxSizer(wxVERTICAL);

    // Main grid — 0 vgap so hidden rows collapse fully; per-item wxALL,4 provides spacing.
    auto* grid = new wxFlexGridSizer(2, 8, 0);
    grid->AddGrowableCol(1);

    auto add_row = [&](const wxString& label, wxWindow* ctrl) {
        grid->Add(new wxStaticText(this, wxID_ANY, label),
                  0, wxALIGN_CENTER_VERTICAL | wxALL, 4);
        grid->Add(ctrl, 1, wxEXPAND | wxALL, 4);
    };

    // Always-visible rows
    wxArrayString shapes;
    for (const char* s : {"Cube", "Cylinder", "Cone", "Sphere", "Pyramid", "Helix"})
        shapes.Add(s);
    m_choice_shape = new wxChoice(this, wxID_ANY, wxDefaultPosition, wxDefaultSize, shapes);
    m_choice_shape->SetSelection(s_params.shape);
    add_row(_L("Shape:"), m_choice_shape);

    m_spin_size = new wxSpinCtrlDouble(this, wxID_ANY, wxEmptyString,
        wxDefaultPosition, wxDefaultSize, wxSP_ARROW_KEYS, 5.0, 200.0, s_params.size, 1.0);
    m_spin_size->SetDigits(1);
    add_row(_L("Size (mm):"), m_spin_size);

    m_spin_sla_h = new wxSpinCtrlDouble(this, wxID_ANY, wxEmptyString,
        wxDefaultPosition, wxDefaultSize, wxSP_ARROW_KEYS, 1.0, 200.0, s_params.sla_h, 1.0);
    m_spin_sla_h->SetDigits(1);
    add_row(_L("Height (mm):"), m_spin_sla_h);

    wxArrayString patterns;
    for (const char* p : {"Single", "Stripes", "Checkerboard", "Helix", "Honeycomb"})
        patterns.Add(p);
    m_choice_pattern = new wxChoice(this, wxID_ANY, wxDefaultPosition, wxDefaultSize, patterns);
    m_choice_pattern->SetSelection(s_params.pattern);
    m_pattern_label = new wxStaticText(this, wxID_ANY, _L("Pattern:"));
    grid->Add(m_pattern_label, 0, wxALIGN_CENTER_VERTICAL | wxALL, 4);
    grid->Add(m_choice_pattern, 1, wxEXPAND | wxALL, 4);

    // Materials row (hidden for Single pattern)
    m_n_mat_label      = new wxStaticText(this, wxID_ANY, _L("Materials:"));
    m_spin_n_materials = new wxSpinCtrl(this, wxID_ANY, wxEmptyString,
        wxDefaultPosition, wxDefaultSize, wxSP_ARROW_KEYS, 2, 8, s_params.n_materials);
    grid->Add(m_n_mat_label,      0, wxALIGN_CENTER_VERTICAL | wxALL, 4);
    grid->Add(m_spin_n_materials, 1, wxEXPAND | wxALL, 4);

    // Stripes row
    m_stripe_rows_label = new wxStaticText(this, wxID_ANY, _L("Rows:"));
    m_spin_stripe_rows  = new wxSpinCtrl(this, wxID_ANY, wxEmptyString,
        wxDefaultPosition, wxDefaultSize, wxSP_ARROW_KEYS, 2, 32, s_params.stripe_rows);
    grid->Add(m_stripe_rows_label, 0, wxALIGN_CENTER_VERTICAL | wxALL, 4);
    grid->Add(m_spin_stripe_rows,  1, wxEXPAND | wxALL, 4);

    // Checkerboard rows
    m_cb_col_label    = new wxStaticText(this, wxID_ANY, _L("Columns:"));
    m_spin_cb_columns = new wxSpinCtrl(this, wxID_ANY, wxEmptyString,
        wxDefaultPosition, wxDefaultSize, wxSP_ARROW_KEYS, 4, 32, s_params.cb_columns);
    grid->Add(m_cb_col_label,    0, wxALIGN_CENTER_VERTICAL | wxALL, 4);
    grid->Add(m_spin_cb_columns, 1, wxEXPAND | wxALL, 4);

    m_cb_row_label = new wxStaticText(this, wxID_ANY, _L("Rows:"));
    m_spin_cb_rows = new wxSpinCtrl(this, wxID_ANY, wxEmptyString,
        wxDefaultPosition, wxDefaultSize, wxSP_ARROW_KEYS, 2, 16, s_params.cb_rows);
    grid->Add(m_cb_row_label, 0, wxALIGN_CENTER_VERTICAL | wxALL, 4);
    grid->Add(m_spin_cb_rows, 1, wxEXPAND | wxALL, 4);

    // Helix rows
    m_hx_rev_label = new wxStaticText(this, wxID_ANY, _L("Revolutions:"));
    m_spin_hx_rev  = new wxSpinCtrlDouble(this, wxID_ANY, wxEmptyString,
        wxDefaultPosition, wxDefaultSize, wxSP_ARROW_KEYS, 1.0, 8.0, s_params.hx_revolutions, 0.5);
    m_spin_hx_rev->SetDigits(1);
    grid->Add(m_hx_rev_label, 0, wxALIGN_CENTER_VERTICAL | wxALL, 4);
    grid->Add(m_spin_hx_rev,  1, wxEXPAND | wxALL, 4);

    m_hx_wid_label  = new wxStaticText(this, wxID_ANY, _L("Strand (%):"));
    m_spin_hx_width = new wxSpinCtrl(this, wxID_ANY, wxEmptyString,
        wxDefaultPosition, wxDefaultSize, wxSP_ARROW_KEYS, 10, 90, s_params.hx_width);
    grid->Add(m_hx_wid_label,  0, wxALIGN_CENTER_VERTICAL | wxALL, 4);
    grid->Add(m_spin_hx_width, 1, wxEXPAND | wxALL, 4);

    // Honeycomb rows
    m_hc_sec_label    = new wxStaticText(this, wxID_ANY, _L("Cells across:"));
    m_spin_hc_sectors = new wxSpinCtrl(this, wxID_ANY, wxEmptyString,
        wxDefaultPosition, wxDefaultSize, wxSP_ARROW_KEYS, 2, 20, s_params.hc_sectors);
    grid->Add(m_hc_sec_label,    0, wxALIGN_CENTER_VERTICAL | wxALL, 4);
    grid->Add(m_spin_hc_sectors, 1, wxEXPAND | wxALL, 4);

    m_hc_ban_label = new wxStaticText(this, wxID_ANY, _L("Wall (%):"));
    m_spin_hc_bands = new wxSpinCtrl(this, wxID_ANY, wxEmptyString,
        wxDefaultPosition, wxDefaultSize, wxSP_ARROW_KEYS, 5, 50, s_params.hc_bands);
    grid->Add(m_hc_ban_label, 0, wxALIGN_CENTER_VERTICAL | wxALL, 4);
    grid->Add(m_spin_hc_bands, 1, wxEXPAND | wxALL, 4);

    // Helix shape rows (shown when shape == Helix, hidden otherwise)
    m_hs_turns_label = new wxStaticText(this, wxID_ANY, _L("Turns:"));
    m_spin_hs_turns  = new wxSpinCtrl(this, wxID_ANY, wxEmptyString,
        wxDefaultPosition, wxDefaultSize, wxSP_ARROW_KEYS, 1, 10, s_params.hs_turns);
    grid->Add(m_hs_turns_label, 0, wxALIGN_CENTER_VERTICAL | wxALL, 4);
    grid->Add(m_spin_hs_turns,  1, wxEXPAND | wxALL, 4);

    m_hs_tube_label = new wxStaticText(this, wxID_ANY, _L("Tube (%):"));
    m_spin_hs_tube  = new wxSpinCtrl(this, wxID_ANY, wxEmptyString,
        wxDefaultPosition, wxDefaultSize, wxSP_ARROW_KEYS, 5, 40, s_params.hs_tube_pct);
    grid->Add(m_hs_tube_label, 0, wxALIGN_CENTER_VERTICAL | wxALL, 4);
    grid->Add(m_spin_hs_tube,  1, wxEXPAND | wxALL, 4);

    m_hs_strands_label = new wxStaticText(this, wxID_ANY, _L("Strands:"));
    m_spin_hs_strands  = new wxSpinCtrl(this, wxID_ANY, wxEmptyString,
        wxDefaultPosition, wxDefaultSize, wxSP_ARROW_KEYS, 1, 6, s_params.hs_strands);
    grid->Add(m_hs_strands_label, 0, wxALIGN_CENTER_VERTICAL | wxALL, 4);
    grid->Add(m_spin_hs_strands,  1, wxEXPAND | wxALL, 4);

    outer->Add(grid, 0, wxEXPAND | wxALL, 8);

    m_status_label = new wxStaticText(this, wxID_ANY, wxEmptyString,
        wxDefaultPosition, wxDefaultSize, wxST_NO_AUTORESIZE);
    outer->Add(m_status_label, 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, 10);

    m_btn_place      = new wxButton(this, wxID_OK,     _L("Place on Bed"));
    auto* btn_cancel = new wxButton(this, wxID_CANCEL, _L("Cancel"));
    auto* btn_row    = new wxBoxSizer(wxHORIZONTAL);
    btn_row->AddStretchSpacer();
    btn_row->Add(m_btn_place, 0, wxRIGHT, 6);
    btn_row->Add(btn_cancel,  0);
    outer->Add(btn_row, 0, wxEXPAND | wxALL, 10);

    SetSizerAndFit(outer);
    SetMinSize(wxSize(420, -1));

    // Event bindings
    m_choice_shape->Bind(wxEVT_CHOICE, [this](wxCommandEvent&) {
        s_params.shape = m_choice_shape->GetSelection();
        refresh_controls();
        rebuild_preview();
    });
    m_choice_pattern->Bind(wxEVT_CHOICE, [this](wxCommandEvent&) {
        s_params.pattern = m_choice_pattern->GetSelection();
        refresh_controls();
        rebuild_preview();
    });

    // Direct rebuild on spin events (no debounce — timer events are unreliable
    // inside a modal dialog's event loop on GTK).
    auto trigger = [this](auto&) { rebuild_preview(); };
    m_spin_size->Bind(wxEVT_SPINCTRLDOUBLE,    trigger);
    m_spin_sla_h->Bind(wxEVT_SPINCTRLDOUBLE,   trigger);
    m_spin_stripe_rows->Bind(wxEVT_SPINCTRL, trigger);
    m_spin_cb_columns->Bind(wxEVT_SPINCTRL,    trigger);
    m_spin_cb_rows->Bind(wxEVT_SPINCTRL,       trigger);
    m_spin_hx_rev->Bind(wxEVT_SPINCTRLDOUBLE,  trigger);
    m_spin_hx_width->Bind(wxEVT_SPINCTRL,      trigger);
    m_spin_hc_sectors->Bind(wxEVT_SPINCTRL,    trigger);
    m_spin_hc_bands->Bind(wxEVT_SPINCTRL,      trigger);
    m_spin_n_materials->Bind(wxEVT_SPINCTRL,   trigger);
    m_spin_hs_turns->Bind(wxEVT_SPINCTRL,      trigger);
    m_spin_hs_tube->Bind(wxEVT_SPINCTRL,       trigger);
    m_spin_hs_strands->Bind(wxEVT_SPINCTRL,    trigger);

    m_btn_place->Bind(wxEVT_BUTTON,   &ShapeGenDialog::on_place,  this);
    btn_cancel->Bind(wxEVT_BUTTON,    &ShapeGenDialog::on_cancel, this);

    Bind(wxEVT_IDLE, [this](wxIdleEvent&) {
        if (m_spin_size->GetValue()           != s_params.size           ||
            m_spin_sla_h->GetValue()          != s_params.sla_h          ||
            m_spin_n_materials->GetValue()    != s_params.n_materials    ||
            m_spin_stripe_rows->GetValue()    != s_params.stripe_rows    ||
            m_spin_cb_columns->GetValue()     != s_params.cb_columns     ||
            m_spin_cb_rows->GetValue()        != s_params.cb_rows        ||
            m_spin_hx_rev->GetValue()         != s_params.hx_revolutions ||
            m_spin_hx_width->GetValue()       != s_params.hx_width       ||
            m_spin_hc_sectors->GetValue()     != s_params.hc_sectors     ||
            m_spin_hc_bands->GetValue()       != s_params.hc_bands       ||
            m_spin_hs_turns->GetValue()       != s_params.hs_turns       ||
            m_spin_hs_tube->GetValue()        != s_params.hs_tube_pct    ||
            m_spin_hs_strands->GetValue()     != s_params.hs_strands     ||
            m_choice_shape->GetSelection()    != s_params.shape          ||
            m_choice_pattern->GetSelection()  != s_params.pattern)
            rebuild_preview();
    });

    refresh_controls();
    rebuild_preview();
}

ShapeGenDialog::~ShapeGenDialog()
{
    if (!m_placed)
        remove_temp_object();
}

void ShapeGenDialog::refresh_controls()
{
    const int pat     = s_params.pattern;
    const bool helix  = (s_params.shape == 5);

    // When Helix shape is selected, the shape itself defines the material layout;
    // pattern and material controls are not applicable.
    m_pattern_label->Show(!helix);
    m_choice_pattern->Show(!helix);

    m_n_mat_label->Show(!helix && pat != 0);
    m_spin_n_materials->Show(!helix && pat != 0);

    m_stripe_rows_label->Show(!helix && pat == 1);
    m_spin_stripe_rows->Show(!helix && pat == 1);

    m_cb_col_label->Show(!helix && pat == 2);
    m_spin_cb_columns->Show(!helix && pat == 2);
    m_cb_row_label->Show(!helix && pat == 2);
    m_spin_cb_rows->Show(!helix && pat == 2);

    m_hx_rev_label->Show(!helix && pat == 3);
    m_spin_hx_rev->Show(!helix && pat == 3);
    m_hx_wid_label->Show(!helix && pat == 3);
    m_spin_hx_width->Show(!helix && pat == 3);

    m_hc_sec_label->Show(!helix && pat == 4);
    m_spin_hc_sectors->Show(!helix && pat == 4);
    m_hc_ban_label->Show(!helix && pat == 4);
    m_spin_hc_bands->Show(!helix && pat == 4);

    // Helix shape parameters
    m_hs_turns_label->Show(helix);
    m_spin_hs_turns->Show(helix);
    m_hs_tube_label->Show(helix);
    m_spin_hs_tube->Show(helix);
    m_hs_strands_label->Show(helix);
    m_spin_hs_strands->Show(helix);

    Layout();
    Fit();
}

void ShapeGenDialog::remove_temp_object()
{
    if (m_temp_obj_idxs.empty())
        return;
    // Remove highest index first so earlier indices stay valid.
    std::vector<size_t> sorted = m_temp_obj_idxs;
    std::sort(sorted.begin(), sorted.end(), std::greater<size_t>());
    for (size_t idx : sorted)
        wxGetApp().plater()->remove(idx);
    m_temp_obj_idxs.clear();
}

void ShapeGenDialog::rebuild_preview()
{
    // Always read from controls — spin events on GTK can fire before the
    // internal value is committed, so GetValue() here is the ground truth.
    s_params.size           = m_spin_size->GetValue();
    s_params.sla_h          = m_spin_sla_h->GetValue();
    s_params.n_materials    = m_spin_n_materials->GetValue();
    s_params.stripe_rows    = m_spin_stripe_rows->GetValue();
    s_params.cb_columns     = m_spin_cb_columns->GetValue();
    s_params.cb_rows        = m_spin_cb_rows->GetValue();
    s_params.hx_revolutions = m_spin_hx_rev->GetValue();
    s_params.hx_width       = m_spin_hx_width->GetValue();
    s_params.hc_sectors     = m_spin_hc_sectors->GetValue();
    s_params.hc_bands       = m_spin_hc_bands->GetValue();
    s_params.hs_turns       = m_spin_hs_turns->GetValue();
    s_params.hs_tube_pct    = m_spin_hs_tube->GetValue();
    s_params.hs_strands     = m_spin_hs_strands->GetValue();

    Plater::SuppressSnapshots suppress(wxGetApp().plater());

    const int   shape   = s_params.shape;
    const int   pattern = s_params.pattern;
    const float size    = (float)s_params.size;
    const float sla_h   = (float)s_params.sla_h;

    remove_temp_object();

    Model temp;
    build_into_model(temp, shape, size, sla_h, pattern, s_params);

    m_temp_obj_idxs = wxGetApp().plater()->add_model_objects(temp.objects);

    if (s_params.shape == 5) {
        m_status_label->SetLabel(wxString::Format(
            _L("Preview: Helix  |  %d strand(s)  |  %.1f mm"),
            s_params.hs_strands, sla_h));
    } else {
        m_status_label->SetLabel(wxString::Format(
            _L("Preview: %s  |  %s  |  %.1f mm"),
            m_choice_shape->GetStringSelection(),
            m_choice_pattern->GetStringSelection(),
            sla_h));
    }
    Layout();

    // Force the GL canvas to repaint immediately — idle events are suppressed
    // while a modal dialog runs its own event loop on GTK.
    wxGetApp().plater()->canvas3D()->force_repaint();
}

void ShapeGenDialog::on_place(wxCommandEvent&)
{
    rebuild_preview();
    m_placed = true;
    Plater::TakeSnapshot snapshot(wxGetApp().plater(), _L("Place SLA Shape"));
    EndModal(wxID_OK);
}

void ShapeGenDialog::on_cancel(wxCommandEvent&)
{
    remove_temp_object();
    EndModal(wxID_CANCEL);
}

} // namespace GUI
} // namespace Slic3r
