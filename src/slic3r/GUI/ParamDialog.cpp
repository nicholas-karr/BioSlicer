#include "ParamDialog.hpp"

#include <wx/button.h>
#include <wx/checkbox.h>
#include <wx/choice.h>
#include <wx/sizer.h>
#include <wx/spinctrl.h>
#include <wx/stattext.h>

#include "GUI_App.hpp"
#include "I18N.hpp"

namespace Slic3r {
namespace GUI {

ParamDialog::ParamDialog(wxWindow *parent, const wxString &title, const wxString &place_label)
    : wxDialog(parent, wxID_ANY, title, wxDefaultPosition, wxDefaultSize, wxDEFAULT_DIALOG_STYLE | wxRESIZE_BORDER)
{
    SetFont(wxGetApp().normal_font());
    m_grid = new wxFlexGridSizer(2, 8, 0);
    m_grid->AddGrowableCol(1);
    m_status = new wxStaticText(this, wxID_ANY, wxEmptyString);
    m_place  = new wxButton(this, wxID_OK, place_label);
    m_place->Bind(wxEVT_BUTTON, [this](wxCommandEvent &) {
        if (place())
            EndModal(wxID_OK);
    });
}

wxChoice *ParamDialog::add_choice(const wxString &label, const std::vector<wxString> &items, int selection)
{
    auto *ctrl = new wxChoice(this, wxID_ANY);
    for (const wxString &item : items)
        ctrl->Append(item);
    ctrl->SetSelection(selection);
    m_grid->Add(new wxStaticText(this, wxID_ANY, label), 0, wxALIGN_CENTER_VERTICAL | wxALL, 4);
    m_grid->Add(ctrl, 1, wxEXPAND | wxALL, 4);
    return ctrl;
}

wxSpinCtrlDouble *ParamDialog::add_value(const wxString &label, const wxString &unit, double min, double max, double value, double step, int digits)
{
    auto *ctrl = new wxSpinCtrlDouble(this, wxID_ANY, wxEmptyString, wxDefaultPosition, wxDefaultSize, wxSP_ARROW_KEYS, min, max, value, step);
    ctrl->SetDigits(digits);
    m_grid->Add(new wxStaticText(this, wxID_ANY, unit.empty() ? label : label + " (" + unit + "):"), 0, wxALIGN_CENTER_VERTICAL | wxALL, 4);
    m_grid->Add(ctrl, 1, wxEXPAND | wxALL, 4);
    return ctrl;
}

wxSpinCtrl *ParamDialog::add_count(const wxString &label, int min, int max, int value)
{
    auto *ctrl = new wxSpinCtrl(this, wxID_ANY, wxEmptyString, wxDefaultPosition, wxDefaultSize, wxSP_ARROW_KEYS, min, max, value);
    m_grid->Add(new wxStaticText(this, wxID_ANY, label + ":"), 0, wxALIGN_CENTER_VERTICAL | wxALL, 4);
    m_grid->Add(ctrl, 1, wxEXPAND | wxALL, 4);
    return ctrl;
}

wxCheckBox *ParamDialog::add_check(const wxString &label, bool value)
{
    auto *ctrl = new wxCheckBox(this, wxID_ANY, label);
    ctrl->SetValue(value);
    m_grid->Add(new wxStaticText(this, wxID_ANY, wxEmptyString), 0, wxALL, 4);
    m_grid->Add(ctrl, 1, wxEXPAND | wxALL, 4);
    return ctrl;
}

wxStaticText *ParamDialog::add_note(const wxString &text)
{
    auto *note = new wxStaticText(this, wxID_ANY, text);
    note->Wrap(360);
    m_grid->Add(new wxStaticText(this, wxID_ANY, wxEmptyString), 0, wxALL, 4);
    m_grid->Add(note, 1, wxEXPAND | wxALL, 4);
    return note;
}

void ParamDialog::set_status(const wxString &text)
{
    m_status->SetLabel(text);
    m_status->Wrap(400);
    Layout();
    Fit();
}

void ParamDialog::finish_layout()
{
    auto *buttons = new wxBoxSizer(wxHORIZONTAL);
    buttons->AddStretchSpacer(1);
    buttons->Add(m_place, 0, wxALL, 4);
    buttons->Add(new wxButton(this, wxID_CANCEL, _L("Cancel")), 0, wxALL, 4);

    auto *outer = new wxBoxSizer(wxVERTICAL);
    outer->Add(m_grid, 0, wxEXPAND | wxALL, 8);
    outer->Add(m_status, 0, wxEXPAND | wxLEFT | wxRIGHT, 12);
    outer->Add(buttons, 0, wxEXPAND | wxALL, 8);
    SetSizerAndFit(outer);
    CentreOnParent();
}

} // namespace GUI
} // namespace Slic3r
