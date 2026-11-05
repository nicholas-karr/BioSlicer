#pragma once

#include "ParamDialog.hpp"

namespace Slic3r {
namespace GUI {

// Generates a test coupon (wall thickness, channel width, overhang or resolution) and places it on the bed.
class TestCouponDialog : public ParamDialog
{
public:
    explicit TestCouponDialog(wxWindow *parent);

private:
    bool place() override;

    wxChoice         *m_kind { nullptr };
    wxChoice         *m_profile { nullptr };
    wxSpinCtrlDouble *m_first { nullptr };
    wxSpinCtrlDouble *m_step { nullptr };
    wxSpinCtrl       *m_count { nullptr };
    wxSpinCtrlDouble *m_wall { nullptr };
    wxSpinCtrlDouble *m_height { nullptr };
};

} // namespace GUI
} // namespace Slic3r
