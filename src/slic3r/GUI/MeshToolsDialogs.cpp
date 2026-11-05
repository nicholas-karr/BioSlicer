#include "MeshToolsDialogs.hpp"

#include <functional>
#include <map>

#include <wx/button.h>
#include <wx/choice.h>
#include <wx/dialog.h>
#include <wx/filedlg.h>
#include <wx/textctrl.h>
#include <wx/sizer.h>
#include <wx/spinctrl.h>
#include <wx/stattext.h>

#include "GUI_App.hpp"
#include "I18N.hpp"
#include "GUI.hpp"
#include "format.hpp"
#include "libslic3r/libslic3r.h"
#include "libslic3r/Utils.hpp"
#include <boost/nowide/fstream.hpp>

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

void show_dimensions_report_dialog(wxWindow *parent, const std::vector<DimensionsReport> &reports)
{
    wxString text;
    for (const DimensionsReport &r : reports) {
        text += from_u8(r.name) + "\n";
        text += wxString::Format(_L("  Size: %.3f x %.3f x %.3f mm\n"), r.size.x(), r.size.y(), r.size.z());
        text += wxString::Format(_L("  Bounding box: X %.3f to %.3f, Y %.3f to %.3f, Z %.3f to %.3f mm\n"),
                                 r.bbox_min.x(), r.bbox_max.x(), r.bbox_min.y(), r.bbox_max.y(), r.bbox_min.z(), r.bbox_max.z());
        text += wxString::Format(_L("  Volume: %.3f mm3 (%.4f ml)\n"), r.volume_mm3, r.volume_ml);
        text += wxString::Format(_L("  Surface area: %.3f mm2\n"), r.surface_area_mm2);
        if (r.thickness_valid)
            text += wxString::Format(_L("  Thinnest wall: %.3f mm at X %.2f, Y %.2f, Z %.2f mm\n"), r.min_wall_thickness_mm,
                                     r.min_wall_location.x(), r.min_wall_location.y(), r.min_wall_location.z());
        else
            text += _L("  Thinnest wall: not measurable (the surface is not closed)\n");
        if (r.open_edges > 0)
            text += wxString::Format(_L("  Warning: %d open edges, volume and wall thickness may be wrong\n"), int(r.open_edges));
    }

    wxDialog *dlg = make_dialog(parent, _L("Dimensions report"));
    auto *body    = new wxTextCtrl(dlg, wxID_ANY, text, wxDefaultPosition, wxSize(560, 260), wxTE_MULTILINE | wxTE_READONLY);
    auto *save    = new wxButton(dlg, wxID_ANY, _L("Save as CSV") + dots);
    auto *outer   = new wxBoxSizer(wxVERTICAL);
    outer->Add(body, 1, wxEXPAND | wxALL, 8);
    auto *row = new wxBoxSizer(wxHORIZONTAL);
    row->Add(save, 0, wxRIGHT, 8);
    row->AddStretchSpacer();
    row->Add(dlg->CreateStdDialogButtonSizer(wxCLOSE), 0);
    outer->Add(row, 0, wxEXPAND | wxALL, 8);
    dlg->SetSizerAndFit(outer);
    save->Bind(wxEVT_BUTTON, [&](wxCommandEvent &) {
        wxFileDialog fd(dlg, _L("Save dimensions report"), wxEmptyString, "dimensions.csv", "CSV (*.csv)|*.csv",
                        wxFD_SAVE | wxFD_OVERWRITE_PROMPT);
        if (fd.ShowModal() != wxID_OK)
            return;
        boost::nowide::ofstream file(into_u8(fd.GetPath()), std::ios::binary);
        file << dimensions_report_csv(reports);
        if (!file.good())
            show_error(dlg, _L("The file could not be written."));
    });
    dlg->CentreOnParent();
    dlg->ShowModal();
    dlg->Destroy();
}

} // namespace GUI
} // namespace Slic3r
