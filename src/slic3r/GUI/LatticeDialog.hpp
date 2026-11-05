#pragma once

#include "ParamDialog.hpp"

namespace Slic3r {
namespace GUI {

// Generates a gyroid-like lattice block (Schwarz P or diamond), optionally graded in density along an axis.
class LatticeDialog : public ParamDialog
{
public:
    explicit LatticeDialog(wxWindow *parent);

private:
    bool place() override;

    wxChoice         *m_type { nullptr };
    wxSpinCtrlDouble *m_x { nullptr };
    wxSpinCtrlDouble *m_y { nullptr };
    wxSpinCtrlDouble *m_z { nullptr };
    wxSpinCtrlDouble *m_cell { nullptr };
    wxSpinCtrlDouble *m_density { nullptr };
    wxCheckBox       *m_graded { nullptr };
    wxSpinCtrlDouble *m_density_end { nullptr };
    wxChoice         *m_axis { nullptr };
};

} // namespace GUI
} // namespace Slic3r
