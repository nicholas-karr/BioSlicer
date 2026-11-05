#pragma once

#include <vector>

#include "TriangleMesh.hpp"

namespace Slic3r {

// Parametric multi-material test shapes. All lengths are in millimetres.
// Shape indices: 0 Cube, 1 Cylinder, 2 Cone, 3 Sphere, 4 Pyramid, 5 Helix.
// Pattern indices: 0 Single, 1 Stripes, 2 Checkerboard, 3 Helix, 4 Honeycomb.
struct ShapeGenParams {
    int    shape          = 1;     // Cylinder
    int    pattern        = 1;     // Stripes
    double size           = 20.0;  // mm, edge length or diameter
    double sla_h          = 15.0;  // mm, height
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

// Number of material meshes generate_shape() returns for the given parameters.
int shape_gen_material_count(const ShapeGenParams &p);

// One mesh per material, bottom at z = 0.
// Helix shape: mesh 0 is the cylinder, meshes 1.. are the strands.
std::vector<indexed_triangle_set> generate_shape(const ShapeGenParams &p);

} // namespace Slic3r
