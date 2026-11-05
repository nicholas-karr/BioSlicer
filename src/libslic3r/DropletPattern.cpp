#include "DropletPattern.hpp"

#include <algorithm>
#include <cmath>
#include <memory>

#include "ClipperUtils.hpp"
#include "Emboss.hpp"
#include "Polygon.hpp"

namespace Slic3r {

DropletSet make_droplet_grid(const DropletGridParams &p)
{
    DropletSet set;
    set.name       = "Grid";
    set.keep_order = true;
    const int cols = std::max(1, p.cols);
    const int rows = std::max(1, p.rows);
    set.points.reserve(size_t(cols) * size_t(rows));
    for (int r = 0; r < rows; ++ r)
        for (int i = 0; i < cols; ++ i) {
            const int c = (p.serpentine && (r & 1)) ? cols - 1 - i : i;
            DropletPoint point;
            point.pos       = p.origin + Vec3d(c * p.pitch_x, r * p.pitch_y, 0.);
            // The volume is the same at a place whatever the order it is visited in.
            point.volume_nl = std::max(0., p.volume_nl + r * p.volume_step_row + c * p.volume_step_col);
            point.count     = p.count;
            set.points.emplace_back(point);
        }
    return set;
}

DropletSet make_droplet_line(const Vec3d &start, const Vec3d &end, int count, double volume_nl, int drops)
{
    DropletSet set;
    set.name       = "Line";
    set.keep_order = true;
    count = std::max(1, count);
    for (int i = 0; i < count; ++ i) {
        DropletPoint point;
        point.pos       = count == 1 ? start : start + (end - start) * (double(i) / double(count - 1));
        point.volume_nl = volume_nl;
        point.count     = drops;
        set.points.emplace_back(point);
    }
    return set;
}

ExPolygons wall_footprint(const Points &polyline, bool closed, double thickness_mm)
{
    const float half = float(scale_(thickness_mm) * 0.5);
    if (polyline.size() < 2 || half <= 0.f)
        return {};
    if (closed && polyline.size() >= 3) {
        const Polygon loop(polyline);
        return diff_ex(offset(loop, half), offset(loop, -half));
    }
    return union_ex(offset(Polyline(polyline), half, ClipperLib::jtMiter, 3., ClipperLib::etOpenSquare));
}

indexed_triangle_set make_wall_mesh(const ExPolygons &footprint, double z0, double z1)
{
    if (footprint.empty() || z1 <= z0)
        return {};
    // The shape is extruded along +z in scaled units.
    Emboss::ProjectScale projection(std::make_unique<Emboss::ProjectZ>((z1 - z0) / SCALING_FACTOR), SCALING_FACTOR);
    indexed_triangle_set mesh = Emboss::polygons2model(footprint, projection);
    // A mesh with its volume turned inside out would slice as an empty one.
    if (its_volume(mesh) < 0.f)
        for (stl_triangle_vertex_indices &face : mesh.indices)
            std::swap(face[1], face[2]);
    for (stl_vertex &v : mesh.vertices)
        v.z() += float(z0);
    return mesh;
}

namespace {

Polygon well_outline(const WellArrayParams &p, const Vec2d &center, double grow)
{
    const double ax = p.inner_x * 0.5 + grow;
    const double ay = p.inner_y * 0.5 + grow;
    if (p.shape == WellShape::Round) {
        // A circle has one diameter; an ellipse is not a well.
        const double radius = std::max(ax, 0.01);
        Polygon circle = make_circle_num_segments(scale_(radius), 96);
        circle.translate(scaled<coord_t>(center.x()), scaled<coord_t>(center.y()));
        return circle;
    }
    Polygon rect;
    rect.points = { scaled<coord_t>(Vec2d(center.x() - ax, center.y() - ay)), scaled<coord_t>(Vec2d(center.x() + ax, center.y() - ay)),
                    scaled<coord_t>(Vec2d(center.x() + ax, center.y() + ay)), scaled<coord_t>(Vec2d(center.x() - ax, center.y() + ay)) };
    return rect;
}

std::string well_name(int row, int col)
{
    // A..Z, then AA.. for plates with more than 26 rows.
    std::string letters;
    for (int r = row; ; r = r / 26 - 1) {
        letters.insert(letters.begin(), char('A' + r % 26));
        if (r < 26)
            break;
    }
    return letters + std::to_string(col + 1);
}

} // namespace

WellArray make_well_array(const WellArrayParams &p)
{
    WellArray out;
    const int cols = std::max(1, p.cols);
    const int rows = std::max(1, p.rows);

    Polygons inner, outer;
    for (int r = 0; r < rows; ++ r)
        for (int c = 0; c < cols; ++ c) {
            const Vec2d center(c * p.pitch_x, r * p.pitch_y);
            inner.emplace_back(well_outline(p, center, 0.));
            outer.emplace_back(well_outline(p, center, p.wall_thickness));
        }
    // Walls of neighbouring wells may touch or overlap: the wall is what the outline of all wells covers minus the wells.
    const ExPolygons outline = union_ex(outer);
    const ExPolygons walls   = diff_ex(outline, inner);
    const double floor_z     = std::max(0., p.floor_thickness);

    indexed_triangle_set mesh = make_wall_mesh(walls, 0., floor_z + std::max(0.1, p.wall_height));
    if (floor_z > 0.)
        its_merge(mesh, make_wall_mesh(outline, 0., floor_z));
    out.mesh = std::move(mesh);

    const int n = std::max(1, p.droplets_n);
    for (int r = 0; r < rows; ++ r)
        for (int c = 0; c < cols; ++ c) {
            DropletSet set;
            set.name = well_name(r, c);
            set.keep_order = true;
            // The droplets sit in the middle third of the well, so they land away from the wall.
            const double span_x = p.inner_x / 3.;
            const double span_y = (p.shape == WellShape::Round ? p.inner_x : p.inner_y) / 3.;
            for (int j = 0; j < n; ++ j)
                for (int i = 0; i < n; ++ i) {
                    DropletPoint point;
                    const double fx = n == 1 ? 0. : (double(i) / (n - 1) - 0.5) * span_x * 2.;
                    const double fy = n == 1 ? 0. : (double(j) / (n - 1) - 0.5) * span_y * 2.;
                    point.pos       = Vec3d(c * p.pitch_x + fx, r * p.pitch_y + fy, floor_z);
                    point.volume_nl = p.volume_nl;
                    point.count     = p.count;
                    set.points.emplace_back(point);
                }
            out.droplets.emplace_back(std::move(set));
        }
    return out;
}

} // namespace Slic3r
