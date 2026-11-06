#ifndef slic3r_GUI_MeshToolsDialogs_hpp_
#define slic3r_GUI_MeshToolsDialogs_hpp_

#include <vector>

#include "libslic3r/DimensionsReport.hpp"
#include "libslic3r/MeshPrimitives.hpp"

class wxWindow;

namespace Slic3r {
namespace GUI {

// Each dialog returns true when the user confirmed. All lengths are in millimetres.

// Fields for every parameter of the primitive. Values typed in are used exactly.
bool show_primitive_dialog(wxWindow *parent, PrimitiveType type, std::vector<double> &params);

// Grow (positive) or shrink (negative) the surface.
bool show_offset_dialog(wxWindow *parent, double &distance_mm);

// ports: 0 = closed cavity, 1 = one opening on top, 2 = openings on top and bottom.
bool show_shell_dialog(wxWindow *parent, double &wall_thickness_mm, int &ports, double &port_radius_mm);

// Shows the numbers with their units and offers to save them as a CSV file.
void show_dimensions_report_dialog(wxWindow *parent, const std::vector<DimensionsReport> &reports);

} // namespace GUI
} // namespace Slic3r

#endif
