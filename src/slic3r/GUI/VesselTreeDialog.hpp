#pragma once

#include <vector>
#include <wx/dialog.h>

#include "libslic3r/Vessel/VesselTree.hpp"

class wxChoice;
class wxSpinCtrl;
class wxSpinCtrlDouble;
class wxStaticText;
class wxButton;

namespace Slic3r {
namespace GUI {

// Creates a branching vessel tree (or edits the one the selected object was made from).
// All values are in millimetres unless the label says otherwise.
class VesselTreeDialog : public wxDialog
{
public:
    // edit_object_idx >= 0 edits the vessel tree object with this index, -1 makes a new one.
    explicit VesselTreeDialog(wxWindow *parent, int edit_object_idx = -1);
    ~VesselTreeDialog();

private:
    void refresh_controls();
    vessel::VesselSpec read_controls() const;
    void write_controls(const vessel::VesselSpec &spec);
    void rebuild_preview();
    void remove_temp_objects();
    void on_place(wxCommandEvent &);
    void on_cancel(wxCommandEvent &);
    void on_export_csv(wxCommandEvent &);

    // Mesh of the object the tree has to stay inside, in bed coordinates. Empty if there is none.
    bool bounds_mesh(const std::string &object_name, indexed_triangle_set &out) const;

    wxSpinCtrlDouble *m_root_radius      { nullptr };
    wxSpinCtrlDouble *m_root_length      { nullptr };
    wxSpinCtrl       *m_levels           { nullptr };
    wxSpinCtrlDouble *m_length_ratio     { nullptr };
    wxChoice         *m_radius_rule      { nullptr };
    wxStaticText     *m_flow_split_label { nullptr };
    wxSpinCtrlDouble *m_flow_split       { nullptr };
    wxStaticText     *m_radius_ratio_label { nullptr };
    wxSpinCtrlDouble *m_radius_ratio     { nullptr };
    wxChoice         *m_angle_rule       { nullptr };
    wxStaticText     *m_total_angle_label { nullptr };
    wxSpinCtrlDouble *m_total_angle      { nullptr };
    wxSpinCtrlDouble *m_angle_jitter     { nullptr };
    wxSpinCtrlDouble *m_length_jitter    { nullptr };
    wxSpinCtrlDouble *m_min_radius       { nullptr };
    wxSpinCtrl       *m_seed             { nullptr };
    wxSpinCtrlDouble *m_wall             { nullptr };
    wxSpinCtrl       *m_sides            { nullptr };
    wxChoice         *m_bounds           { nullptr };
    wxStaticText     *m_root_x_label     { nullptr };
    wxSpinCtrlDouble *m_root_x           { nullptr };
    wxStaticText     *m_root_y_label     { nullptr };
    wxSpinCtrlDouble *m_root_y           { nullptr };
    wxStaticText     *m_root_z_label     { nullptr };
    wxSpinCtrlDouble *m_root_z           { nullptr };
    wxStaticText     *m_status           { nullptr };
    wxButton         *m_btn_place        { nullptr };

    std::vector<std::string> m_bounds_names; // names behind the entries of m_bounds, entry 0 is "none"
    std::vector<size_t>      m_temp_obj_idxs;
    int                      m_edit_object_idx { -1 };
    bool                     m_finished { false };
};

} // namespace GUI
} // namespace Slic3r
