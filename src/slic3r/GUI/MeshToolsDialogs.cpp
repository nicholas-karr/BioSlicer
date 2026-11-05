#include "MeshToolsDialogs.hpp"

#include <functional>
#include <map>

#include <wx/button.h>
#include <wx/choice.h>
#include <wx/dialog.h>
#include <wx/sizer.h>
#include <wx/spinctrl.h>
#include <wx/stattext.h>

#include "GUI_App.hpp"
#include "I18N.hpp"
#include "libslic3r/libslic3r.h"

namespace Slic3r {
namespace GUI {

namespace {

wxSpinCtrlDouble *make_mm_spin(wxWindow *parent, double value, double min, double max)
{
    auto *spin = new wxSpinCtrlDouble(parent, wxID_ANY, wxEmptyString, wxDefaultPosition, wxDefaultSize, wxSP_ARROW_KEYS, min, max, value, 1.);
    spin->SetDigits(4);
    return spin;
}

void add_row(wxWindow *parent, wxFlexGridSizer *grid, const wxString &label, wxWindow *ctrl)
{
    grid->Add(new wxStaticText(parent, wxID_ANY, label), 0, wxALIGN_CENTER_VERTICAL | wxALL, 4);
    grid->Add(ctrl, 1, wxEXPAND | wxALL, 4);
}

wxDialog *make_dialog(wxWindow *parent, const wxString &title)
{
    auto *dlg = new wxDialog(parent, wxID_ANY, title, wxDefaultPosition, wxDefaultSize, wxDEFAULT_DIALOG_STYLE);
    dlg->SetFont(wxGetApp().normal_font());
    return dlg;
}

bool run(wxDialog *dlg, wxFlexGridSizer *grid, wxStaticText *note, const std::function<void()> &before_show = {})
{
    auto *outer = new wxBoxSizer(wxVERTICAL);
    outer->Add(grid, 0, wxEXPAND | wxALL, 8);
    if (note)
        outer->Add(note, 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, 12);
    outer->Add(dlg->CreateStdDialogButtonSizer(wxOK | wxCANCEL), 0, wxEXPAND | wxALL, 8);
    dlg->SetSizerAndFit(outer);
    if (before_show)
        before_show();
    dlg->Fit();
    dlg->CentreOnParent();
    const bool ok = dlg->ShowModal() == wxID_OK;
    return ok;
}

// Last values typed per primitive, so repeated experiments start from the previous design.
std::map<PrimitiveType, std::vector<double>> s_last_primitive;

} // namespace

bool show_primitive_dialog(wxWindow *parent, PrimitiveType type, std::vector<double> &params)
{
    const auto &info = primitive_param_info(type);
    if (params.size() != info.size()) {
        auto it = s_last_primitive.find(type);
        params  = it != s_last_primitive.end() ? it->second : primitive_default_params(type);
    }

    wxDialog *dlg  = make_dialog(parent, _(primitive_name(type)));
    auto     *grid = new wxFlexGridSizer(2, 8, 8);
    grid->AddGrowableCol(1);
    std::vector<wxControl *> ctrls;
    for (size_t i = 0; i < info.size(); ++i) {
        const wxString label = _(info[i].label) + (*info[i].unit ? wxString(" (") + info[i].unit + ")" : wxString());
        wxControl *ctrl;
        if (info[i].integer)
            ctrl = new wxSpinCtrl(dlg, wxID_ANY, wxEmptyString, wxDefaultPosition, wxDefaultSize, wxSP_ARROW_KEYS,
                                  int(info[i].min), int(info[i].max), int(params[i]));
        else
            ctrl = make_mm_spin(dlg, params[i], info[i].min, info[i].max);
        ctrls.push_back(ctrl);
        add_row(dlg, grid, label, ctrl);
    }
    // Trade mesh size for accuracy: raise the segment counts until the volume loss is below the given limit.
    auto *loss_spin = make_mm_spin(dlg, 0.1, 0.0001, 50.);
    auto *raise_btn = new wxButton(dlg, wxID_ANY, _L("Raise segments"));
    auto *loss_row  = new wxBoxSizer(wxHORIZONTAL);
    loss_row->Add(new wxStaticText(dlg, wxID_ANY, _L("Largest volume loss (%)")), 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 6);
    loss_row->Add(loss_spin, 1, wxRIGHT, 6);
    loss_row->Add(raise_btn, 0);
    grid->AddSpacer(0);
    grid->Add(loss_row, 1, wxEXPAND | wxALL, 4);
    auto *note = new wxStaticText(dlg, wxID_ANY, wxEmptyString);

    auto read = [&]() {
        std::vector<double> v;
        for (size_t i = 0; i < info.size(); ++i)
            v.push_back(info[i].integer ? double(static_cast<wxSpinCtrl *>(ctrls[i])->GetValue())
                                        : static_cast<wxSpinCtrlDouble *>(ctrls[i])->GetValue());
        return v;
    };
    auto refresh = [&]() {
        const std::vector<double> v   = read();
        const std::string         err = primitive_validate(type, v);
        wxWindow                 *ok  = dlg->FindWindow(wxID_OK);
        if (ok)
            ok->Enable(err.empty());
        if (!err.empty())
            note->SetLabel(_(err.c_str()));
        else
            note->SetLabel(wxString::Format(_L("Volume %.3f mm3. The printed model is at most %.3f %% smaller because of the segment count."),
                                            primitive_volume(type, v), 100. * primitive_volume_tolerance(type, v)));
        dlg->Layout();
    };
    raise_btn->Bind(wxEVT_BUTTON, [&](wxCommandEvent &) {
        const std::vector<double> v = primitive_params_for_tolerance(type, read(), loss_spin->GetValue() / 100.);
        for (size_t i = 0; i < info.size() && i < v.size(); ++i) {
            if (info[i].integer)
                static_cast<wxSpinCtrl *>(ctrls[i])->SetValue(int(v[i]));
        }
        refresh();
    });
    for (wxControl *c : ctrls) {
        c->Bind(wxEVT_SPINCTRLDOUBLE, [&](wxSpinDoubleEvent &) { refresh(); });
        c->Bind(wxEVT_SPINCTRL, [&](wxCommandEvent &) { refresh(); });
        c->Bind(wxEVT_TEXT, [&](wxCommandEvent &) { refresh(); });
    }

    bool ok = run(dlg, grid, note, refresh);
    if (ok) {
        params = read();
        s_last_primitive[type] = params;
    }
    dlg->Destroy();
    return ok;
}

bool show_offset_dialog(wxWindow *parent, double &distance_mm)
{
    wxDialog *dlg  = make_dialog(parent, _L("Offset surface"));
    auto     *grid = new wxFlexGridSizer(2, 8, 8);
    grid->AddGrowableCol(1);
    auto *spin = make_mm_spin(dlg, distance_mm, -1000., 1000.);
    add_row(dlg, grid, _L("Distance (mm), positive grows, negative shrinks"), spin);
    bool ok = run(dlg, grid, nullptr);
    if (ok)
        distance_mm = spin->GetValue();
    dlg->Destroy();
    return ok && distance_mm != 0.;
}

bool show_shell_dialog(wxWindow *parent, double &wall_thickness_mm, int &ports, double &port_radius_mm)
{
    wxDialog *dlg  = make_dialog(parent, _L("Make hollow shell"));
    auto     *grid = new wxFlexGridSizer(2, 8, 8);
    grid->AddGrowableCol(1);
    auto *wall = make_mm_spin(dlg, wall_thickness_mm, 0.01, 1000.);
    add_row(dlg, grid, _L("Wall thickness (mm)"), wall);
    wxArrayString choices;
    choices.Add(_L("None (closed cavity)"));
    choices.Add(_L("One opening on top"));
    choices.Add(_L("Openings on top and bottom"));
    auto *port_choice = new wxChoice(dlg, wxID_ANY, wxDefaultPosition, wxDefaultSize, choices);
    port_choice->SetSelection(ports);
    add_row(dlg, grid, _L("Openings to the cavity"), port_choice);
    auto *radius = make_mm_spin(dlg, port_radius_mm, 0.01, 1000.);
    add_row(dlg, grid, _L("Opening radius (mm)"), radius);
    bool ok = run(dlg, grid, nullptr);
    if (ok) {
        wall_thickness_mm = wall->GetValue();
        ports             = port_choice->GetSelection();
        port_radius_mm    = radius->GetValue();
    }
    dlg->Destroy();
    return ok;
}

} // namespace GUI
} // namespace Slic3r
