#include "DimensionsReport.hpp"

#include <cmath>
#include <iomanip>
#include <locale>
#include <sstream>

#include "libslic3r/Model.hpp"
#include "libslic3r/MeshThickness.hpp"

namespace Slic3r {

namespace {

double signed_volume(const indexed_triangle_set &its)
{
    double v = 0.;
    for (const Vec3i &f : its.indices)
        v += its.vertices[f[0]].cast<double>().dot(its.vertices[f[1]].cast<double>().cross(its.vertices[f[2]].cast<double>())) / 6.;
    return v;
}

double surface_area(const indexed_triangle_set &its)
{
    double a = 0.;
    for (const Vec3i &f : its.indices) {
        const Vec3d p = its.vertices[f[0]].cast<double>();
        a += 0.5 * (its.vertices[f[1]].cast<double>() - p).cross(its.vertices[f[2]].cast<double>() - p).norm();
    }
    return a;
}

} // namespace

DimensionsReport make_dimensions_report(const ModelObject &object, size_t instance_idx, bool measure_thickness)
{
    DimensionsReport r;
    r.name     = object.name;
    r.instance = instance_idx;
    if (instance_idx >= object.instances.size())
        return r;
    const Transform3d instance = object.instances[instance_idx]->get_matrix();

    BoundingBoxf3 bb;
    for (const ModelVolume *volume : object.volumes) {
        if (!volume->is_model_part())
            continue;
        indexed_triangle_set its = volume->mesh().its;
        // A mirrored instance flips the triangles; fix_left_handed restores outward orientation.
        its_transform(its, Transform3d(instance * volume->get_matrix()), true);
        if (its.indices.empty())
            continue;
        r.volume_mm3       += std::abs(signed_volume(its));
        r.surface_area_mm2 += surface_area(its);
        r.triangles        += its.indices.size();
        r.open_edges       += its_num_open_edges(its);
        bb.merge(bounding_box(its));
        if (measure_thickness) {
            const WallThicknessResult w = analyze_wall_thickness(its);
            if (w.valid && (!r.thickness_valid || w.min_thickness_mm < r.min_wall_thickness_mm)) {
                r.thickness_valid       = true;
                r.min_wall_thickness_mm = w.min_thickness_mm;
                r.min_wall_location     = w.min_location;
            }
        }
    }
    if (bb.defined) {
        r.bbox_min = bb.min;
        r.bbox_max = bb.max;
        r.size     = bb.size();
    }
    r.volume_ml = r.volume_mm3 / 1000.;
    return r;
}

std::string dimensions_report_csv(const std::vector<DimensionsReport> &reports)
{
    std::ostringstream out;
    out.imbue(std::locale::classic());
    out << std::fixed << std::setprecision(6);
    out << "name,instance,bbox_min_x_mm,bbox_min_y_mm,bbox_min_z_mm,bbox_max_x_mm,bbox_max_y_mm,bbox_max_z_mm,"
           "size_x_mm,size_y_mm,size_z_mm,volume_mm3,volume_ml,surface_area_mm2,min_wall_thickness_mm,"
           "min_wall_x_mm,min_wall_y_mm,min_wall_z_mm,triangles,open_edges\n";
    for (const DimensionsReport &r : reports) {
        std::string name = "\"";
        for (char c : r.name)
            name += c == '"' ? std::string("\"\"") : std::string(1, c);
        name += '"';
        out << name << ',' << r.instance;
        for (const Vec3d *v : {&r.bbox_min, &r.bbox_max, &r.size})
            out << ',' << (*v)[0] << ',' << (*v)[1] << ',' << (*v)[2];
        out << ',' << r.volume_mm3 << ',' << r.volume_ml << ',' << r.surface_area_mm2;
        if (r.thickness_valid)
            out << ',' << r.min_wall_thickness_mm << ',' << r.min_wall_location[0] << ',' << r.min_wall_location[1] << ',' << r.min_wall_location[2];
        else
            out << ",,,,";
        out << ',' << r.triangles << ',' << r.open_edges << '\n';
    }
    return out.str();
}

} // namespace Slic3r
