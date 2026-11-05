#ifndef slic3r_GUI_ChannelDialog_hpp_
#define slic3r_GUI_ChannelDialog_hpp_

#include <string>
#include <vector>

#include <wx/dialog.h>

#include "libslic3r/Point.hpp"
#include "libslic3r/SolidEdit.hpp"

class wxSpinCtrlDouble;
class wxTextCtrl;

namespace Slic3r {
namespace GUI {

// Asks for the points, diameter and port lengths of a channel. All lengths are in millimetres.
class ChannelDialog : public wxDialog
{
public:
    ChannelDialog(wxWindow *parent, const std::string &default_points);

    // Parses the point list. Returns false and sets error if a line is not three numbers.
    bool get_path(std::vector<Vec3d> &path, std::string &error) const;
    SolidEdit::ChannelParams get_params() const;

private:
    wxTextCtrl       *m_points;
    wxSpinCtrlDouble *m_diameter;
    wxSpinCtrlDouble *m_inlet;
    wxSpinCtrlDouble *m_outlet;
};

}} // namespace Slic3r::GUI

#endif // slic3r_GUI_ChannelDialog_hpp_
