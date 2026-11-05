#pragma once

#include <vector>
#include <wx/dialog.h>

class wxCheckBox;
class wxChoice;
class wxFlexGridSizer;
class wxSpinCtrl;
class wxSpinCtrlDouble;
class wxStaticText;
class wxWindow;

namespace Slic3r {
namespace GUI {

// Generates droplet experiments: a grid or a line of droplets on the bed, or an array of SLA wells with their droplets.
class DropletGenDialog : public wxDialog
{
public:
    explicit DropletGenDialog(wxWindow *parent);
    ~DropletGenDialog() override;

private:
    enum Mode { Grid = 0, Line, Wells };

    // A row of the dialog, shown in the modes of `modes` (bit mask of 1 << Mode).
    struct Row
    {
        wxStaticText *label { nullptr };
        wxWindow     *control { nullptr };
        unsigned      modes { 0 };
    };

    wxSpinCtrlDouble *add_double(const wxString &label, unsigned modes, double min, double max, double value, double inc, int digits = 2);
    wxSpinCtrl       *add_int(const wxString &label, unsigned modes, int min, int max, int value);
    void              refresh_controls();
    void              on_place();
    // Builds the series. As a preview it is added to the scene as it would be placed, without an undo step.
    bool              apply(bool preview);
    void              rebuild_preview();
    void              clear_preview();
    std::vector<double> signature() const;

    std::vector<Row>  m_rows;
    wxFlexGridSizer  *m_grid { nullptr };

    wxChoice         *m_mode { nullptr };
    wxChoice         *m_droplet_extruder { nullptr };
    wxChoice         *m_wall_extruder { nullptr };
    wxChoice         *m_shape { nullptr };
    wxCheckBox       *m_center { nullptr };
    wxCheckBox       *m_serpentine { nullptr };
    wxCheckBox       *m_dip { nullptr };
    wxStaticText     *m_status { nullptr };

    wxSpinCtrlDouble *m_origin_x { nullptr };
    wxSpinCtrlDouble *m_origin_y { nullptr };
    wxSpinCtrlDouble *m_end_x { nullptr };
    wxSpinCtrlDouble *m_end_y { nullptr };
    wxSpinCtrlDouble *m_z { nullptr };
    wxSpinCtrlDouble *m_volume { nullptr };
    wxSpinCtrlDouble *m_volume_row { nullptr };
    wxSpinCtrlDouble *m_volume_col { nullptr };
    wxSpinCtrlDouble *m_pitch_x { nullptr };
    wxSpinCtrlDouble *m_pitch_y { nullptr };
    wxSpinCtrlDouble *m_inner_x { nullptr };
    wxSpinCtrlDouble *m_inner_y { nullptr };
    wxSpinCtrlDouble *m_wall { nullptr };
    wxSpinCtrlDouble *m_wall_height { nullptr };
    wxSpinCtrlDouble *m_floor { nullptr };
    wxSpinCtrl       *m_cols { nullptr };
    wxSpinCtrl       *m_rows_spin { nullptr };
    wxSpinCtrl       *m_count { nullptr };
    wxSpinCtrl       *m_line_count { nullptr };
    wxSpinCtrl       *m_per_well { nullptr };

    // The preview in the scene: wells are temporary objects, droplets are the sets from m_preview_from on.
    std::vector<size_t> m_preview_objects;
    size_t              m_preview_from { size_t(-1) };
    bool                m_placed { false };
    std::vector<double> m_signature;

    // 1-based extruders behind the choices.
    std::vector<int>  m_droplet_extruders;
    std::vector<int>  m_wall_extruders;
};

} // namespace GUI
} // namespace Slic3r
