#include "ChannelDialog.hpp"

#include "I18N.hpp"
#include "GUI_App.hpp"
#include "GUI.hpp"

#include <wx/sizer.h>
#include <wx/spinctrl.h>
#include <wx/stattext.h>
#include <wx/textctrl.h>

namespace Slic3r {
namespace GUI {

ChannelDialog::ChannelDialog(wxWindow *parent, const std::string &default_points)
    : wxDialog(parent, wxID_ANY, _L("Subtract channel"), wxDefaultPosition, wxDefaultSize, wxDEFAULT_DIALOG_STYLE | wxRESIZE_BORDER)
{
    auto *sizer = new wxBoxSizer(wxVERTICAL);

    sizer->Add(new wxStaticText(this, wxID_ANY,
        _L("Channel path: one point per line as X Y Z in mm (bed coordinates).\n"
           "The channel runs through the points in order.")), 0, wxALL, 8);
    m_points = new wxTextCtrl(this, wxID_ANY, from_u8(default_points), wxDefaultPosition, wxSize(360, 120), wxTE_MULTILINE);
    sizer->Add(m_points, 1, wxEXPAND | wxLEFT | wxRIGHT, 8);

    auto *grid = new wxFlexGridSizer(2, 6, 8);
    auto add_row = [this, grid](const wxString &label, double value, double max_value, wxSpinCtrlDouble *&ctrl) {
        grid->Add(new wxStaticText(this, wxID_ANY, label), 0, wxALIGN_CENTER_VERTICAL);
        ctrl = new wxSpinCtrlDouble(this, wxID_ANY, wxEmptyString, wxDefaultPosition, wxDefaultSize,
                                    wxSP_ARROW_KEYS, 0., max_value, value, 0.1);
        ctrl->SetDigits(3);
        grid->Add(ctrl, 0, wxEXPAND);
    };
    add_row(_L("Channel diameter (mm)"), 1.0, 1000., m_diameter);
    m_diameter->SetRange(0.001, 1000.);
    add_row(_L("Inlet port length (mm), 0 = closed end"), 0., 1000., m_inlet);
    add_row(_L("Outlet port length (mm), 0 = closed end"), 0., 1000., m_outlet);
    sizer->Add(grid, 0, wxALL, 8);

    sizer->Add(new wxStaticText(this, wxID_ANY,
        _L("A port prolongs the channel past its first or last point.\n"
           "If it reaches the outside of the object, the channel opens there.")), 0, wxLEFT | wxRIGHT, 8);
    sizer->Add(CreateStdDialogButtonSizer(wxOK | wxCANCEL), 0, wxALL | wxALIGN_RIGHT, 8);

    SetSizerAndFit(sizer);
}

bool ChannelDialog::get_path(std::vector<Vec3d> &path, std::string &error) const
{
    return SolidEdit::parse_points(into_u8(m_points->GetValue()), path, error);
}

SolidEdit::ChannelParams ChannelDialog::get_params() const
{
    SolidEdit::ChannelParams p;
    p.diameter          = m_diameter->GetValue();
    p.inlet_extension   = m_inlet->GetValue();
    p.outlet_extension  = m_outlet->GetValue();
    return p;
}

}} // namespace Slic3r::GUI
