#pragma once

#include "ParamDialog.hpp"

namespace Slic3r {
namespace GUI {

// Makes copies of a test piece with one parameter stepped, arranges them on the bed with their index engraved,
// and saves a table of what each copy is as CSV.
class ParameterSweepDialog : public ParamDialog
{
public:
    explicit ParameterSweepDialog(wxWindow *parent);

private:
    bool place() override;

    wxChoice         *m_subject { nullptr };
    wxSpinCtrlDouble *m_start { nullptr };
    wxSpinCtrlDouble *m_step { nullptr };
    wxSpinCtrl       *m_count { nullptr };
    wxSpinCtrlDouble *m_spacing { nullptr };
    wxCheckBox       *m_label { nullptr };
    wxCheckBox       *m_csv { nullptr };
};

} // namespace GUI
} // namespace Slic3r
