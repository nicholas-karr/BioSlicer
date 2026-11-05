#ifndef slic3r_DimensionsReport_hpp_
#define slic3r_DimensionsReport_hpp_

#include <string>
#include <vector>

#include "libslic3r/Point.hpp"

namespace Slic3r {

class ModelObject;

// What an object measures once placed on the bed: everything in millimetres, square millimetres or
// cubic millimetres, with the instance transform applied. Only model parts are counted.
struct DimensionsReport
{
    std::string name;
    size_t      instance        = 0;
    Vec3d       bbox_min        = Vec3d::Zero();
    Vec3d       bbox_max        = Vec3d::Zero();
    Vec3d       size            = Vec3d::Zero();
    double      volume_mm3      = 0.;
    double      volume_ml       = 0.;      // 1 ml = 1000 mm3
    double      surface_area_mm2 = 0.;
    bool        thickness_valid = false;   // false for open meshes where no wall could be measured
    double      min_wall_thickness_mm = 0.;
    Vec3d       min_wall_location     = Vec3d::Zero();
    size_t      triangles       = 0;
    size_t      open_edges      = 0;       // 0 for a closed surface
};

DimensionsReport make_dimensions_report(const ModelObject &object, size_t instance_idx = 0, bool measure_thickness = true);

// One header line and one line per report, comma separated, '.' as decimal separator whatever the
// locale, quoted names, 6 decimals.
std::string dimensions_report_csv(const std::vector<DimensionsReport> &reports);

} // namespace Slic3r

#endif // slic3r_DimensionsReport_hpp_
