#pragma once

#include <string>
#include <wx/dialog.h>

class wxChoice;
class wxSpinCtrl;
class wxSpinCtrlDouble;
class wxStaticText;
class wxButton;

namespace Slic3r {
namespace GUI {

class ShapeGenDialog : public wxDialog
{
public:
    explicit ShapeGenDialog(wxWindow* parent);
    ~ShapeGenDialog();

private:
    void rebuild_preview();
    void refresh_controls();
    void remove_temp_object();
    void on_place(wxCommandEvent&);
    void on_cancel(wxCommandEvent&);

    // Always-visible controls
    wxChoice*         m_choice_shape      { nullptr };
    wxStaticText*     m_pattern_label     { nullptr };
    wxChoice*         m_choice_pattern    { nullptr };
    wxSpinCtrlDouble* m_spin_size         { nullptr };
    wxSpinCtrlDouble* m_spin_sla_h        { nullptr };
    wxStaticText*     m_n_mat_label       { nullptr };
    wxSpinCtrl*       m_spin_n_materials  { nullptr };

    // Helix shape controls (shown/hidden by refresh_controls)
    wxStaticText*     m_hs_turns_label    { nullptr };
    wxSpinCtrl*       m_spin_hs_turns     { nullptr };
    wxStaticText*     m_hs_tube_label     { nullptr };
    wxSpinCtrl*       m_spin_hs_tube      { nullptr };
    wxStaticText*     m_hs_strands_label  { nullptr };
    wxSpinCtrl*       m_spin_hs_strands   { nullptr };

    wxStaticText*     m_status_label      { nullptr };
    wxButton*         m_btn_place         { nullptr };

    // Pattern-specific controls (shown/hidden by refresh_controls)
    wxStaticText*     m_stripe_rows_label { nullptr };
    wxSpinCtrl*       m_spin_stripe_rows  { nullptr };

    wxStaticText*     m_cb_col_label    { nullptr };
    wxSpinCtrl*       m_spin_cb_columns { nullptr };
    wxStaticText*     m_cb_row_label    { nullptr };
    wxSpinCtrl*       m_spin_cb_rows    { nullptr };

    wxStaticText*     m_hx_rev_label    { nullptr };
    wxSpinCtrlDouble* m_spin_hx_rev     { nullptr };
    wxStaticText*     m_hx_wid_label    { nullptr };
    wxSpinCtrl*       m_spin_hx_width   { nullptr };

    wxStaticText*     m_hc_sec_label    { nullptr };
    wxSpinCtrl*       m_spin_hc_sectors { nullptr };
    wxStaticText*     m_hc_ban_label    { nullptr };
    wxSpinCtrl*       m_spin_hc_bands   { nullptr };

    std::vector<size_t> m_temp_obj_idxs;
    bool m_placed { false };
};

} // namespace GUI
} // namespace Slic3r
